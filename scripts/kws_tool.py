#!/usr/bin/env python3
"""
Try out the stop-word recognition on a frame with a microphone (first step
towards switching a ringing alarm off by voice; not connected to the alarm yet).

    python kws_tool.py 192.168.1.20 enroll --times 3   # speak the word 3 times
    python kws_tool.py 192.168.1.20 test               # listen 10 s, count detections
    python kws_tool.py 192.168.1.20 status
    python kws_tool.py 192.168.1.20 clear

Enrolling: each round records for a few seconds - say the word once, shortly
after "Speak now". Three enrolments at your normal speaking speed work best;
the acceptance threshold is derived from how much your repetitions differ.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

ENROLL_STATUS = {
    0: "added",
    -1: "no usable word heard (too quiet, too short, or not like the earlier ones)",
    -2: "too long - say just the one word",
    -3: "the frame could not record (out of memory or microphone busy)",
}


def call(host, path, method="GET"):
    req = urllib.request.Request(f"http://{host}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="replace")
        raise SystemExit(f"{path}: HTTP {e.code} {detail}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise SystemExit(f"{host}: not reachable ({e})") from None


def wait_idle(host, max_wait_s):
    deadline = time.time() + max_wait_s
    while time.time() < deadline:
        status = call(host, "/api/kws/status")
        if status["mode"] == "idle":
            return status
        time.sleep(0.5)
    raise SystemExit("the frame did not finish in time")


def show(status):
    print(
        f"templates: {status['templates']}/{status['max_templates']}, "
        f"threshold: {status['threshold']}"
    )
    test = status["test"]
    if test["utterances"]:
        print(
            f"last test: {test['utterances']} utterances, {test['detections']} "
            f"detections, best distance {test['best_score']}"
        )


def cmd_enroll(host, times, seconds):
    for i in range(times):
        print(f"[{i + 1}/{times}] Speak the word now ...", flush=True)
        call(host, f"/api/kws/enroll?seconds={seconds}", "POST")
        status = wait_idle(host, seconds + 15)
        result = status["enroll"]
        print(f"    {ENROLL_STATUS.get(result['status'], result['status'])}")
        if result["status"] == 0:
            print(f"    template {status['templates']}, {result['frames']} frames")
        time.sleep(1)
    show(call(host, "/api/kws/status"))


def cmd_test(host, seconds):
    print(f"Listening for {seconds} s - say the word (and some other words) ...")
    call(host, f"/api/kws/test?seconds={seconds}", "POST")
    status = wait_idle(host, seconds + 15)
    show(status)
    test = status["test"]
    print(f"keyword heard {test['detections']} time(s)")
    return 0 if test["detections"] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("host", help="host/IP of the frame")
    sub = parser.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("enroll")
    p.add_argument("--times", type=int, default=3)
    p.add_argument("--seconds", type=int, default=3)
    p = sub.add_parser("test")
    p.add_argument("--seconds", type=int, default=10)
    sub.add_parser("status")
    sub.add_parser("clear")
    args = parser.parse_args()

    if args.cmd == "enroll":
        cmd_enroll(args.host, args.times, args.seconds)
    elif args.cmd == "test":
        return cmd_test(args.host, args.seconds)
    elif args.cmd == "status":
        show(call(args.host, "/api/kws/status"))
    elif args.cmd == "clear":
        call(args.host, "/api/kws/templates", "DELETE")
        print("templates cleared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
