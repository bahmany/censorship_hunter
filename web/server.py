"""Hunter Web Admin Dashboard Server.

Runs a Flask web server alongside Hunter, providing:
- Real-time log streaming via SSE
- System status API (sources, balancers, benchmark)
- Telegram authentication via web form
- Command execution
"""

import asyncio
import collections
import json
import logging
import os
import platform
import queue
import re
import socket
import subprocess
import sys
import threading
import time
from typing import Dict, List, Optional, Tuple

from flask import Flask, Response, jsonify, render_template, request

from web.auth_bridge import AuthBridge

logger = logging.getLogger(__name__)

# Global references (set by start_server)
_dashboard = None       # HunterDashboard instance
_orchestrator = None    # HunterOrchestrator instance
_auth_bridge = AuthBridge()

# Log subscriber queues for SSE
_log_subscribers = []
_log_lock = threading.Lock()

# Traffic stats collector
_traffic_history: collections.deque = collections.deque(maxlen=120)  # 2 min at 1s
_traffic_lock = threading.Lock()


class WebLogHandler(logging.Handler):
    """Logging handler that broadcasts log entries to SSE subscribers."""

    def __init__(self, max_buffer=500):
        super().__init__()
        self.buffer = collections.deque(maxlen=max_buffer)

    def emit(self, record):
        try:
            entry = {
                'timestamp': time.strftime('%H:%M:%S', time.localtime(record.created)),
                'level': record.levelname,
                'message': record.getMessage(),
                'time': record.created
            }

            self.buffer.append(entry)

            with _log_lock:
                dead = []
                for i, q in enumerate(_log_subscribers):
                    try:
                        q.put_nowait(entry)
                    except queue.Full:
                        dead.append(i)
                for i in reversed(dead):
                    _log_subscribers.pop(i)
        except Exception:
            pass


_web_log_handler = WebLogHandler()
_web_log_handler.setLevel(logging.INFO)
_web_log_handler.setFormatter(logging.Formatter(
    '%(asctime)s | %(levelname)-7s | %(message)s', datefmt='%H:%M:%S'
))


def create_app():
    """Create and configure the Flask application."""
    app = Flask(
        __name__,
        template_folder=os.path.join(os.path.dirname(__file__), 'templates')
    )
    app.config['SECRET_KEY'] = os.urandom(24).hex()

    @app.route('/')
    def index():
        return render_template('dashboard.html')

    @app.route('/api/status')
    def api_status():
        state = {}
        if _dashboard:
            with _dashboard._lock:
                state = dict(_dashboard._state)
                state['sources'] = dict(state.get('sources', {}))
            state['uptime'] = _dashboard._uptime()

        # Auth status
        pending = _auth_bridge.get_pending()
        state['auth_pending'] = pending

        return jsonify(state)

    @app.route('/api/logs/stream')
    def log_stream():
        def generate():
            q = queue.Queue(maxsize=1000)
            with _log_lock:
                _log_subscribers.append(q)

            # Send buffered logs first
            for entry in list(_web_log_handler.buffer)[-100:]:
                yield f"data: {json.dumps(entry)}\n\n"

            try:
                while True:
                    try:
                        entry = q.get(timeout=30)
                        yield f"data: {json.dumps(entry)}\n\n"
                    except queue.Empty:
                        yield ": keepalive\n\n"
            finally:
                with _log_lock:
                    if q in _log_subscribers:
                        _log_subscribers.remove(q)

        return Response(
            generate(),
            mimetype='text/event-stream',
            headers={
                'Cache-Control': 'no-cache',
                'X-Accel-Buffering': 'no',
                'Connection': 'keep-alive'
            }
        )

    @app.route('/api/logs/history')
    def log_history():
        """Return buffered log entries as JSON array."""
        entries = list(_web_log_handler.buffer)[-200:]
        return jsonify(entries)

    @app.route('/api/auth/pending')
    def auth_pending():
        pending = _auth_bridge.get_pending()
        return jsonify({'pending': pending})

    @app.route('/api/auth/submit', methods=['POST'])
    def auth_submit():
        data = request.get_json() or {}
        value = data.get('value', '').strip()
        if not value:
            return jsonify({'ok': False, 'error': 'Empty value'}), 400
        ok = _auth_bridge.submit_response(value)
        return jsonify({'ok': ok})

    @app.route('/api/auth/cancel', methods=['POST'])
    def auth_cancel():
        _auth_bridge.cancel()
        return jsonify({'ok': True})

    @app.route('/api/telegram/status')
    def telegram_status():
        """Check if Telegram session is valid."""
        session_file = None
        try:
            if _orchestrator and getattr(_orchestrator, 'telegram_scraper', None):
                scraper = _orchestrator.telegram_scraper
                base = scraper._session_base_path()
                session_file = base + '.session'
        except Exception:
            session_file = None
        if not session_file:
            runtime_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'runtime')
            name = (
                os.environ.get('HUNTER_SESSION')
                or os.environ.get('TELEGRAM_SESSION')
                or os.environ.get('HUNTER_SESSION_NAME')
                or 'hunter_session'
            )
            session_file = os.path.join(runtime_dir, name + '.session')
        has_session = os.path.exists(session_file)
        return jsonify({
            'has_session': has_session,
            'session_file': os.path.basename(session_file),
            'auth_pending': _auth_bridge.get_pending() is not None
        })

    @app.route('/api/telegram/login', methods=['POST'])
    def telegram_login():
        """Trigger Telegram login flow via web auth bridge."""
        try:
            from telethon import TelegramClient
            from telethon.errors import SessionPasswordNeededError
        except ImportError:
            return jsonify({'ok': False, 'error': 'Telethon not installed'}), 500

        api_id = os.environ.get('HUNTER_API_ID') or os.environ.get('TELEGRAM_API_ID')
        api_hash = os.environ.get('HUNTER_API_HASH') or os.environ.get('TELEGRAM_API_HASH')
        phone = os.environ.get('HUNTER_PHONE') or os.environ.get('TELEGRAM_PHONE')
        session_name = (
            os.environ.get('HUNTER_SESSION')
            or os.environ.get('TELEGRAM_SESSION')
            or os.environ.get('HUNTER_SESSION_NAME')
            or 'hunter_session'
        )

        if not api_id or not api_hash or not phone:
            return jsonify({'ok': False, 'error': 'Missing HUNTER_API_ID, HUNTER_API_HASH, or HUNTER_PHONE in env'}), 400

        def _do_login():
            import asyncio
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            try:
                return loop.run_until_complete(_async_login(
                    session_name, int(api_id), api_hash, phone
                ))
            finally:
                loop.close()

        async def _async_login(sess, aid, ahash, ph):
            proxy = None
            proxy_method = 'direct'
            lock_obj = None
            acquired_lock = False
            client = None

            try:
                if not os.path.isabs(sess) and ('/' not in sess and '\\' not in sess):
                    runtime_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'runtime')
                    sess = os.path.join(runtime_dir, sess)
            except Exception:
                pass

            try:
                import socks as _socks_mod

                # 1) Try SSH tunnel from orchestrator's scraper
                if _orchestrator and getattr(_orchestrator, 'telegram_scraper', None):
                    scraper = _orchestrator.telegram_scraper

                    try:
                        lock_obj = getattr(scraper, '_telegram_lock', None)
                        if lock_obj is not None:
                            acquired_lock = lock_obj.acquire(blocking=False)
                            if not acquired_lock:
                                logger.info("[WebLogin] Another Telegram operation in progress, waiting...")
                                while not lock_obj.acquire(blocking=False):
                                    await asyncio.sleep(0.5)
                                acquired_lock = True
                    except Exception:
                        lock_obj = None
                        acquired_lock = False

                    try:
                        sess = scraper._session_base_path()
                    except Exception:
                        pass

                    ssh_port = scraper.establish_ssh_tunnel()
                    if ssh_port:
                        proxy = (_socks_mod.SOCKS5, '127.0.0.1', ssh_port)
                        proxy_method = f'SSH tunnel (port {ssh_port})'
                        logger.info(f"[WebLogin] Using SSH tunnel proxy on port {ssh_port}")

                # 2) Try balancer SOCKS port
                if proxy is None and _orchestrator and getattr(_orchestrator, 'balancer', None):
                    bal = _orchestrator.balancer
                    if bal and getattr(bal, '_running', False) and getattr(bal, '_backends', None):
                        proxy = (_socks_mod.SOCKS5, '127.0.0.1', bal.port)
                        proxy_method = f'Balancer SOCKS (port {bal.port})'
                        logger.info(f"[WebLogin] Using balancer proxy on port {bal.port}")

                # 3) Try env proxy
                if proxy is None:
                    ph_env = os.environ.get('HUNTER_TELEGRAM_PROXY_HOST')
                    pp_env = os.environ.get('HUNTER_TELEGRAM_PROXY_PORT')
                    if ph_env and pp_env:
                        proxy = (_socks_mod.SOCKS5, ph_env, int(pp_env))
                        proxy_method = f'Env proxy ({ph_env}:{pp_env})'

            except ImportError:
                pass

            try:
                logger.info(f"[WebLogin] Connecting via {proxy_method}...")
                client = TelegramClient(
                    sess,
                    aid,
                    ahash,
                    proxy=proxy,
                    connection_retries=2,
                    auto_reconnect=False,
                )
                await client.connect()

                if await client.is_user_authorized():
                    me = await client.get_me()
                    return {
                        'ok': True,
                        'message': f'Already logged in as {me.first_name} ({me.phone}) via {proxy_method}'
                    }

                logger.info(f"[WebLogin] Sending verification code to {ph}...")
                await client.send_code_request(ph)

                logger.info("[WebLogin] Waiting for code via web dashboard...")
                code = _auth_bridge.request_code(ph, timeout=300)
                if not code:
                    return {'ok': False, 'error': 'No verification code received (timeout or cancelled)'}

                try:
                    await client.sign_in(phone=ph, code=code)
                except SessionPasswordNeededError:
                    logger.info("[WebLogin] 2FA required, waiting for password via web dashboard...")
                    password = _auth_bridge.request_2fa(timeout=300)
                    if not password:
                        return {'ok': False, 'error': 'No 2FA password received (timeout or cancelled)'}
                    await client.sign_in(password=password)

                if await client.is_user_authorized():
                    me = await client.get_me()
                    logger.info(
                        f"[WebLogin] Logged in as {me.first_name} ({me.phone}) via {proxy_method}. Session saved."
                    )
                    return {
                        'ok': True,
                        'message': f'Logged in as {me.first_name} ({me.phone}) via {proxy_method}. Session saved.'
                    }

                return {'ok': False, 'error': 'Authentication failed'}

            finally:
                if client is not None:
                    try:
                        await client.disconnect()
                    except Exception:
                        pass
                    try:
                        sess_obj = getattr(client, 'session', None)
                        if sess_obj is not None:
                            try:
                                sess_obj.close()
                            except Exception:
                                pass
                    except Exception:
                        pass

                if lock_obj is not None and acquired_lock:
                    try:
                        lock_obj.release()
                    except Exception:
                        pass

        # Run login in background thread (it blocks waiting for user input via bridge)
        result_holder = [None]
        error_holder = [None]
        done_event = threading.Event()

        def _worker():
            try:
                result_holder[0] = _do_login()
            except Exception as e:
                logger.error(f"[WebLogin] Error: {e}")
                error_holder[0] = str(e)
            finally:
                done_event.set()

        t = threading.Thread(target=_worker, daemon=True, name='telegram-login')
        t.start()

        # Wait briefly — if already logged in, returns fast
        done_event.wait(timeout=3)
        if done_event.is_set():
            if error_holder[0]:
                return jsonify({'ok': False, 'error': error_holder[0]}), 500
            return jsonify(result_holder[0])

        # Still running = waiting for code/2FA via bridge
        return jsonify({
            'ok': True,
            'message': 'Verification code sent. Please enter it in the auth dialog.',
            'waiting': True
        })

    @app.route('/api/command', methods=['POST'])
    def api_command():
        data = request.get_json() or {}
        cmd = data.get('command', '')

        if cmd == 'status':
            return jsonify({'ok': True, 'message': 'Running'})

        elif cmd == 'force_cycle':
            if _orchestrator:
                logger.info("[WebAdmin] Force cycle requested by user")
                def _run_cycle():
                    loop = asyncio.new_event_loop()
                    asyncio.set_event_loop(loop)
                    try:
                        loop.run_until_complete(_orchestrator.run_cycle())
                    except Exception as e:
                        logger.error(f"[WebAdmin] Force cycle failed: {e}")
                    finally:
                        loop.close()
                threading.Thread(target=_run_cycle, daemon=True, name='force-cycle').start()
                return jsonify({'ok': True, 'message': 'Force cycle started in background'})
            return jsonify({'ok': False, 'message': 'Orchestrator not available'})

        elif cmd == 'clear_pid':
            pid_file = os.path.join(
                os.path.dirname(os.path.dirname(__file__)), 'runtime', 'hunter_service.pid'
            )
            if os.path.exists(pid_file):
                os.remove(pid_file)
                return jsonify({'ok': True, 'message': 'PID file removed'})
            return jsonify({'ok': True, 'message': 'No PID file found'})

        return jsonify({'ok': False, 'message': f'Unknown command: {cmd}'})

    # --- Balancer Backend Management ---
    @app.route('/api/balancer/details')
    def api_balancer_details():
        """Get detailed backend info for both balancers."""
        result = {'main': None, 'gemini': None}
        if _orchestrator:
            bal = getattr(_orchestrator, 'balancer', None)
            if bal:
                result['main'] = {
                    'status': bal.get_status(),
                    'backends': bal.get_all_backends_detail(),
                    'pool': bal.get_available_configs_list(),
                }
            gbal = getattr(_orchestrator, 'gemini_balancer', None)
            if gbal:
                result['gemini'] = {
                    'status': gbal.get_status(),
                    'backends': gbal.get_all_backends_detail(),
                    'pool': gbal.get_available_configs_list(),
                }
        return jsonify(result)

    @app.route('/api/balancer/force', methods=['POST'])
    def api_balancer_force():
        """Force a specific backend for a balancer."""
        data = request.get_json() or {}
        uri = data.get('uri', '').strip()
        permanent = data.get('permanent', False)
        target = data.get('balancer', 'main')  # 'main' or 'gemini'
        if not uri:
            return jsonify({'ok': False, 'error': 'No URI provided'}), 400
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'}), 503
        bal = getattr(_orchestrator, 'balancer', None) if target == 'main' else getattr(_orchestrator, 'gemini_balancer', None)
        if not bal:
            return jsonify({'ok': False, 'error': f'Balancer "{target}" not available'}), 404
        ok = bal.set_forced_backend(uri, permanent=permanent)
        if ok:
            return jsonify({'ok': True, 'message': f'Backend forced ({"permanent" if permanent else "temporary"})'})
        return jsonify({'ok': False, 'error': 'Invalid or unsupported config URI'}), 400

    @app.route('/api/balancer/unforce', methods=['POST'])
    def api_balancer_unforce():
        """Remove forced backend."""
        data = request.get_json() or {}
        target = data.get('balancer', 'main')
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'}), 503
        bal = getattr(_orchestrator, 'balancer', None) if target == 'main' else getattr(_orchestrator, 'gemini_balancer', None)
        if not bal:
            return jsonify({'ok': False, 'error': f'Balancer "{target}" not available'}), 404
        bal.clear_forced_backend()
        return jsonify({'ok': True, 'message': 'Force cleared'})

    @app.route('/api/balancer/set-backends', methods=['POST'])
    def api_balancer_set_backends():
        """Manually set backend URIs for a balancer."""
        data = request.get_json() or {}
        uris = data.get('uris', [])
        target = data.get('balancer', 'main')
        if not uris or not isinstance(uris, list):
            return jsonify({'ok': False, 'error': 'No URIs provided'}), 400
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'}), 503
        bal = getattr(_orchestrator, 'balancer', None) if target == 'main' else getattr(_orchestrator, 'gemini_balancer', None)
        if not bal:
            return jsonify({'ok': False, 'error': f'Balancer "{target}" not available'}), 404
        count = bal.set_manual_backends(uris)
        if count > 0:
            return jsonify({'ok': True, 'count': count, 'message': f'{count} backends set'})
        return jsonify({'ok': False, 'error': 'No valid backends accepted'}), 400

    # --- Config Categories ---
    @app.route('/api/configs/categories')
    def api_config_categories():
        """Get configs categorized by service access."""
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'})
        categories = _orchestrator.categorize_backends()
        return jsonify({'ok': True, 'categories': categories})

    @app.route('/api/configs/test-access', methods=['POST'])
    def api_test_config_access():
        """Test which services a specific config can access."""
        data = request.get_json() or {}
        uri = data.get('uri', '').strip()
        if not uri:
            return jsonify({'ok': False, 'error': 'No URI provided'}), 400
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'}), 503
        # Run in thread to avoid blocking
        result_holder = [None]
        done = threading.Event()
        def _test():
            try:
                result_holder[0] = _orchestrator.test_config_access(uri)
            except Exception as e:
                result_holder[0] = {'error': str(e)}
            done.set()
        t = threading.Thread(target=_test, daemon=True)
        t.start()
        done.wait(timeout=60)
        if result_holder[0] is None:
            return jsonify({'ok': False, 'error': 'Test timed out'}), 504
        if 'error' in result_holder[0]:
            return jsonify({'ok': False, 'error': result_holder[0]['error']}), 500
        return jsonify({'ok': True, 'access': result_holder[0]})

    # --- Manual Engine Commands ---
    @app.route('/api/engine/rescan', methods=['POST'])
    def api_engine_rescan():
        """Re-run the config finder engine."""
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'}), 503
        logger.info('[WebAdmin] Manual rescan triggered')
        def _run():
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            try:
                return loop.run_until_complete(_orchestrator.manual_rescan())
            finally:
                loop.close()
        # Run async in background thread
        threading.Thread(target=_run, daemon=True, name='manual-rescan').start()
        return jsonify({'ok': True, 'message': 'Rescan started in background'})

    @app.route('/api/telegram/fetch-configs', methods=['POST'])
    def api_telegram_fetch():
        """Manually fetch configs from Telegram."""
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'}), 503
        result_holder = [None]
        done = threading.Event()
        def _run():
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            try:
                result_holder[0] = loop.run_until_complete(_orchestrator.manual_telegram_fetch())
            except Exception as e:
                result_holder[0] = {'ok': False, 'error': str(e)}
            finally:
                loop.close()
                done.set()
        threading.Thread(target=_run, daemon=True, name='manual-tg-fetch').start()
        done.wait(timeout=130)
        if result_holder[0] is None:
            return jsonify({'ok': False, 'error': 'Telegram fetch timed out'}), 504
        return jsonify(result_holder[0])

    @app.route('/api/telegram/publish', methods=['POST'])
    def api_telegram_publish():
        """Publish current configs to Telegram group."""
        if not _orchestrator:
            return jsonify({'ok': False, 'error': 'Orchestrator not available'}), 503
        result_holder = [None]
        done = threading.Event()
        def _run():
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            try:
                result_holder[0] = loop.run_until_complete(_orchestrator.manual_publish_telegram())
            except Exception as e:
                result_holder[0] = {'ok': False, 'error': str(e)}
            finally:
                loop.close()
                done.set()
        threading.Thread(target=_run, daemon=True, name='manual-tg-publish').start()
        done.wait(timeout=30)
        if result_holder[0] is None:
            return jsonify({'ok': False, 'error': 'Publish timed out'}), 504
        return jsonify(result_holder[0])

    # --- Traffic stats ---
    @app.route('/api/traffic')
    def api_traffic():
        with _traffic_lock:
            data = list(_traffic_history)
        return jsonify(data)

    # --- Connected clients (who is using the proxy) ---
    @app.route('/api/clients')
    def api_clients():
        clients = _get_proxy_clients()
        return jsonify(clients)

    # --- Top configs ---
    @app.route('/api/configs/top')
    def api_top_configs():
        configs = []
        if _orchestrator:
            bal = getattr(_orchestrator, 'balancer', None)
            if bal:
                with bal._lock:
                    for b in bal._backends:
                        configs.append({
                            'uri_short': _shorten_uri(b.get('uri', '')),
                            'latency': round(b.get('latency', 0)),
                            'healthy': b.get('healthy', False),
                        })
        return jsonify(configs)

    # --- Config export ---
    @app.route('/api/configs/export')
    def api_export_configs():
        tier = request.args.get('tier', 'gold')
        base = os.path.dirname(os.path.dirname(__file__))
        fname = f'HUNTER_{tier}.txt'
        fpath = os.path.join(base, 'runtime', fname)
        if os.path.exists(fpath):
            try:
                with open(fpath, 'r', encoding='utf-8') as f:
                    content = f.read()
                lines = [l.strip() for l in content.splitlines() if l.strip()]
                return jsonify({'ok': True, 'file': fname, 'count': len(lines), 'configs': lines})
            except Exception as e:
                return jsonify({'ok': False, 'error': str(e)})
        return jsonify({'ok': True, 'file': fname, 'count': 0, 'configs': []})

    # --- System info ---
    @app.route('/api/system')
    def api_system():
        info = {
            'platform': platform.platform(),
            'python': platform.python_version(),
            'cpu_count': os.cpu_count(),
            'hostname': socket.gethostname(),
            'pid': os.getpid(),
        }
        try:
            import psutil
            mem = psutil.virtual_memory()
            info['memory_total_gb'] = round(mem.total / (1024**3), 1)
            info['memory_used_gb'] = round(mem.used / (1024**3), 1)
            info['memory_pct'] = mem.percent
            info['cpu_pct'] = psutil.cpu_percent(interval=0.5)
            disk = psutil.disk_usage('.')
            info['disk_total_gb'] = round(disk.total / (1024**3), 1)
            info['disk_used_gb'] = round(disk.used / (1024**3), 1)
            info['disk_pct'] = disk.percent
            net = psutil.net_io_counters()
            info['net_sent_mb'] = round(net.bytes_sent / (1024**2), 1)
            info['net_recv_mb'] = round(net.bytes_recv / (1024**2), 1)
        except ImportError:
            pass
        return jsonify(info)

    return app


def _shorten_uri(uri: str) -> str:
    """Shorten a proxy URI for display."""
    if not uri:
        return '-'
    proto = uri.split('://', 1)[0] if '://' in uri else '?'
    # Extract host from URI
    try:
        rest = uri.split('://', 1)[1] if '://' in uri else uri
        # For base64 URIs, just show protocol + first chars
        if len(rest) > 60:
            return f"{proto}://...{rest[-20:]}"
        return f"{proto}://{rest[:40]}"
    except Exception:
        return uri[:50]


def _get_proxy_clients() -> List[Dict]:
    """Get list of IPs connected to balancer SOCKS ports."""
    clients = {}
    ports_to_check = set()
    if _orchestrator:
        bal = getattr(_orchestrator, 'balancer', None)
        if bal:
            ports_to_check.add(bal.port)
            ports_to_check.add(bal.port + 100)  # HTTP port
        gbal = getattr(_orchestrator, 'gemini_balancer', None)
        if gbal:
            ports_to_check.add(gbal.port)
    if not ports_to_check:
        ports_to_check = {10808, 10908}

    try:
        if sys.platform == 'win32':
            result = subprocess.run(
                ['netstat', '-an'],
                capture_output=True, text=True, timeout=5,
                creationflags=subprocess.CREATE_NO_WINDOW
            )
        else:
            result = subprocess.run(
                ['netstat', '-an'], capture_output=True, text=True, timeout=5
            )
        for line in result.stdout.splitlines():
            if 'ESTABLISHED' not in line:
                continue
            parts = line.split()
            if len(parts) < 3:
                continue
            local = parts[1] if sys.platform != 'win32' else parts[1]
            remote = parts[2] if sys.platform != 'win32' else parts[2]
            # Check if local port matches our proxy ports
            try:
                local_port = int(local.rsplit(':', 1)[-1])
            except (ValueError, IndexError):
                continue
            if local_port in ports_to_check:
                remote_ip = remote.rsplit(':', 1)[0].strip('[]')
                if remote_ip in ('127.0.0.1', '::1', '0.0.0.0'):
                    continue
                if remote_ip not in clients:
                    clients[remote_ip] = {'ip': remote_ip, 'connections': 0, 'port': local_port}
                clients[remote_ip]['connections'] += 1
    except Exception:
        pass
    return list(clients.values())


def _collect_traffic_stats():
    """Background thread: collect network I/O stats every second."""
    try:
        import psutil
    except ImportError:
        return
    prev = psutil.net_io_counters()
    while True:
        time.sleep(1)
        try:
            cur = psutil.net_io_counters()
            sent_rate = (cur.bytes_sent - prev.bytes_sent)  # bytes/sec
            recv_rate = (cur.bytes_recv - prev.bytes_recv)
            entry = {
                't': int(time.time()),
                'up': round(sent_rate / 1024, 1),    # KB/s
                'down': round(recv_rate / 1024, 1),
            }
            with _traffic_lock:
                _traffic_history.append(entry)
            prev = cur
        except Exception:
            pass


def start_server(dashboard=None, orchestrator=None, host='0.0.0.0', port=8585):
    """Start the web dashboard server in a background thread.

    Args:
        dashboard: HunterDashboard instance for reading state
        orchestrator: HunterOrchestrator instance for commands
        host: Bind address (default 0.0.0.0)
        port: Bind port (default 8585)

    Returns:
        The background thread running the server
    """
    global _dashboard, _orchestrator
    _dashboard = dashboard
    _orchestrator = orchestrator

    # Attach web log handler to root logger
    root_logger = logging.getLogger()
    root_logger.addHandler(_web_log_handler)

    # Mark auth bridge as active
    _auth_bridge.active = True

    # Start traffic stats collector
    threading.Thread(target=_collect_traffic_stats, daemon=True, name='traffic-stats').start()

    app = create_app()

    def _run():
        # Suppress Flask request logging
        wlog = logging.getLogger('werkzeug')
        wlog.setLevel(logging.WARNING)

        logger.info(f"Web admin dashboard: http://localhost:{port}")
        try:
            app.run(host=host, port=port, debug=False, use_reloader=False, threaded=True)
        except Exception as e:
            logger.error(f"Web server error: {e}")

    thread = threading.Thread(target=_run, daemon=True, name='web-dashboard')
    thread.start()
    return thread


def get_auth_bridge() -> AuthBridge:
    """Get the global auth bridge instance."""
    return _auth_bridge
