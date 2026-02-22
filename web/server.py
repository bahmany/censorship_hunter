"""Hunter Web Admin Dashboard Server.

Runs a Flask web server alongside Hunter, providing:
- Real-time log streaming via SSE
- System status API (sources, balancers, benchmark)
- Telegram authentication via web form
- Command execution
"""

import collections
import json
import logging
import os
import queue
import threading
import time
from typing import Optional

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
            except GeneratorExit:
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
        session_file = os.path.join(
            os.path.dirname(os.path.dirname(__file__)),
            os.environ.get('HUNTER_SESSION_NAME', 'hunter_session') + '.session'
        )
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
        session_name = os.environ.get('HUNTER_SESSION_NAME', 'hunter_session')

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
            client = TelegramClient(sess, aid, ahash)
            await client.connect()

            if await client.is_user_authorized():
                me = await client.get_me()
                await client.disconnect()
                return {'ok': True, 'message': f'Already logged in as {me.first_name} ({me.phone})'}

            logger.info(f"[WebLogin] Sending verification code to {ph}...")
            await client.send_code_request(ph)

            logger.info("[WebLogin] Waiting for code via web dashboard...")
            code = _auth_bridge.request_code(ph, timeout=300)
            if not code:
                await client.disconnect()
                return {'ok': False, 'error': 'No verification code received (timeout or cancelled)'}

            try:
                await client.sign_in(phone=ph, code=code)
            except SessionPasswordNeededError:
                logger.info("[WebLogin] 2FA required, waiting for password via web dashboard...")
                password = _auth_bridge.request_2fa(timeout=300)
                if not password:
                    await client.disconnect()
                    return {'ok': False, 'error': 'No 2FA password received (timeout or cancelled)'}
                await client.sign_in(password=password)

            if await client.is_user_authorized():
                me = await client.get_me()
                logger.info(f"[WebLogin] Successfully logged in as {me.first_name} ({me.phone})")
                await client.disconnect()
                return {'ok': True, 'message': f'Logged in as {me.first_name} ({me.phone}). Session saved.'}
            else:
                await client.disconnect()
                return {'ok': False, 'error': 'Authentication failed'}

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
                return jsonify({'ok': True, 'message': 'Force cycle signal sent'})
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

    return app


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
