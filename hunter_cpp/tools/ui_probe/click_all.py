#!/usr/bin/env python3
"""Real-click interaction test for the Hunter GUI under a virtual X display.

Needs a -DHUNTER_UI_PROBE=ON build (widget rectangles are dumped to
$HUNTER_UI_PROBE_OUT every 0.25 s) plus Xvfb, xdotool, xwd and ffmpeg.
Clicks every labelled widget once (destructive ones last), follows widgets
that appear afterwards (tabs, popups, About), hammers a few buttons, runs a
Stop->Start race, resizes the window, and reports crashes / sanitizer errors.
"""
import argparse, json, os, re, signal, subprocess, sys, time

DESTRUCTIVE = re.compile(r"^(Stop|Exit|Quit|Close|Disconnect|Clear)", re.I)
SKIP = re.compile(r"^(#|\s*$)")
ERR = re.compile(r"ERROR: AddressSanitizer|runtime error:|terminate called|Segmentation|Aborted|TERMINATE")


def sh(args, env, timeout=10):
    return subprocess.run(args, env=env, capture_output=True, text=True, timeout=timeout)


def read_rects(path):
    items = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                if len(p) < 6:
                    continue
                x, y, w, h = map(float, p[:4])
                label = p[5].split("##")[0].strip()
                if w < 4 or h < 4 or SKIP.match(label):
                    continue
                items.append({"label": label, "x": x, "y": y, "w": w, "h": h, "win": p[4]})
    except FileNotFoundError:
        pass
    return items


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bin", default="hunter_cpp/build_probe/hunter")
    ap.add_argument("--size", default="1280x800")
    ap.add_argument("--dpi", type=int, default=96)
    ap.add_argument("--out", default="ui_probe_out")
    ap.add_argument("--settle", type=float, default=1.2)
    ap.add_argument("--startup", type=float, default=25)
    ap.add_argument("--display", default=":93")
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    binary = os.path.abspath(a.bin)
    w, h = map(int, a.size.split("x"))
    env = dict(os.environ, DISPLAY=a.display, HUNTER_UI_PROBE_OUT=os.path.join(out, "rects.tsv"),
               ASAN_OPTIONS="detect_leaks=0:halt_on_error=1", UBSAN_OPTIONS="print_stacktrace=1")
    rects = env["HUNTER_UI_PROBE_OUT"]
    if os.path.exists(rects):
        os.remove(rects)
    xvfb = subprocess.Popen(["Xvfb", a.display, "-screen", "0", f"{w}x{h}x24"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    wm = subprocess.Popen(["openbox"], env=dict(os.environ, DISPLAY=a.display),
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)
    if a.dpi != 96:
        subprocess.run(["xrdb", "-merge"], input=f"Xft.dpi: {a.dpi}\n", text=True, env=env, timeout=10)
    log = open(os.path.join(out, "app.log"), "w")
    app = subprocess.Popen([binary], cwd=os.path.dirname(binary), env=env, stdout=log, stderr=subprocess.STDOUT,
                           start_new_session=True)
    results, shot_n = [], [0]
    report = {"size": a.size, "dpi": a.dpi, "results": results}

    def log_errors():
        with open(os.path.join(out, "app.log"), errors="replace") as f:
            return [l.rstrip() for l in f if ERR.search(l)]

    def shot(name):
        shot_n[0] += 1
        png = os.path.join(out, f"{shot_n[0]:02d}_{re.sub(r'[^A-Za-z0-9]+', '_', name)[:40]}.png")
        xwd = subprocess.run(["xwd", "-root", "-silent"], env=env, capture_output=True, timeout=10).stdout
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "xwd_pipe", "-i", "-", png], input=xwd, timeout=20)
        return png

    def alive():
        return app.poll() is None

    def click(x, y, n=1, gap=0.0):
        for _ in range(n):
            sh(["xdotool", "mousemove", str(int(x)), str(int(y)), "click", "1"], env)
            if gap:
                time.sleep(gap)

    try:
        t0 = time.time()
        while time.time() - t0 < a.startup and not os.path.exists(rects) and alive():
            time.sleep(0.5)
        time.sleep(3)
        wid = sh(["xdotool", "search", "--onlyvisible", "--name", "."], env).stdout.split()
        if wid:
            sh(["xdotool", "windowmove", wid[0], "0", "0"], env)
            sh(["xdotool", "windowsize", wid[0], str(w), str(h)], env)
        time.sleep(a.settle)
        shot("start")
        clicked, queue = set(), []
        key = lambda i: f"{re.sub(r'[(]\d+[)]', '', i['label']).strip()}@{int(i['y'] // 12)}"
        baseline_errors = len(log_errors())

        def enqueue():
            for it in sorted(read_rects(rects), key=lambda i: (bool(DESTRUCTIVE.match(i["label"])), i["y"], i["x"])):
                if it["label"] == it["win"].split("/")[-1].split("##")[0]:
                    continue  # window/child bounding boxes, not controls
                if key(it) not in clicked and all(key(q) != key(it) for q in queue):
                    queue.append(it)
            queue.sort(key=lambda i: bool(DESTRUCTIVE.match(i["label"])))

        enqueue()
        while queue and alive() and len(results) < 80:
            it = queue.pop(0)
            clicked.add(key(it))
            cx, cy = it["x"] + it["w"] / 2, it["y"] + it["h"] / 2
            click(cx, cy)
            time.sleep(a.settle)
            png = shot(it["label"])
            sh(["xdotool", "key", "Escape"], env)
            time.sleep(0.3)
            errs = log_errors()[baseline_errors:]
            results.append({"label": it["label"], "x": int(cx), "y": int(cy), "alive": alive(), "errors": errs,
                            "shot": os.path.basename(png)})
            if not alive() or errs:
                break
            enqueue()

        # Rapid interaction on the first few safe buttons.
        if alive():
            safe = [r for r in results if not DESTRUCTIVE.match(r["label"])][:3]
            for r in safe:
                click(r["x"], r["y"], n=5, gap=0.05)
            time.sleep(a.settle)
            results.append({"label": "rapid-clicks:" + ",".join(r["label"] for r in safe), "alive": alive(),
                            "errors": log_errors()[baseline_errors:]})

        # Stop -> Start race (the 1.9.0 crash): toggle the hunt button quickly.
        if alive():
            btn = next((i for i in read_rects(rects) if i["label"] in ("Start", "Stop")), None)
            if btn:
                cx, cy = btn["x"] + btn["w"] / 2, btn["y"] + btn["h"] / 2
                for _ in range(3):
                    click(cx, cy)
                    time.sleep(0.4)
                time.sleep(6)
                for _ in range(2):
                    click(cx, cy)
                    time.sleep(0.4)
                time.sleep(a.settle)
                results.append({"label": "stop-start-race", "alive": alive(), "errors": log_errors()[baseline_errors:],
                                "shot": os.path.basename(shot("after_race"))})

        # Live resize through narrow / wide layouts.
        if alive() and wid:
            for rw, rh in ((560, 760), (900, 600), (w, h)):
                sh(["xdotool", "windowsize", wid[0], str(rw), str(rh)], env)
                time.sleep(a.settle)
                results.append({"label": f"resize {rw}x{rh}", "alive": alive(), "errors": log_errors()[baseline_errors:],
                                "shot": os.path.basename(shot(f"resize_{rw}x{rh}"))})

        # Clean shutdown the way a user does it: the window manager's close (Alt+F4).
        if alive():
            if wid:
                sh(["xdotool", "windowactivate", "--sync", wid[0]], env)
                sh(["xdotool", "key", "--window", wid[0], "alt+F4"], env)
            try:
                code = app.wait(timeout=40)
            except subprocess.TimeoutExpired:
                code = "timeout"
            report["shutdown_exit"] = code
            report["shutdown_errors"] = log_errors()[baseline_errors:]
    finally:
        if app.poll() is None:
            os.killpg(app.pid, signal.SIGKILL)
        report["final_exit"] = app.poll()
        wm.terminate()
        xvfb.terminate()
        log.close()
        with open(os.path.join(out, "app.log"), errors="replace") as f:
            report["log_tail"] = f.readlines()[-40:]
        json.dump(report, open(os.path.join(out, "report.json"), "w"), indent=1)

    bad = [r for r in results if not r.get("alive") or r.get("errors")]
    for r in results:
        print(f"{'OK ' if r.get('alive') and not r.get('errors') else 'BAD'} {r['label']}")
    print("shutdown_exit:", report.get("shutdown_exit"), "errors:", len(report.get("shutdown_errors", [])))
    sys.exit(1 if bad or report.get("shutdown_errors") or report.get("shutdown_exit") not in (0,) else 0)


if __name__ == "__main__":
    main()
