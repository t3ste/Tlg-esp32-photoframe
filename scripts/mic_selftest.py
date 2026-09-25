#!/usr/bin/env python3
"""
Speaker + microphone self-test for boards with an onboard microphone.

A tone sequence (four beeps with pauses, played at 100 % volume) is played on
one frame's speaker while a frame's microphone listens. The two can be the same
frame (its own speaker and microphone) or two different frames - in that case
the microphone frame listens to the speaker frame, so put them next to each
other.

Usage:
    python mic_selftest.py 192.168.1.20                     # same frame
    python mic_selftest.py 192.168.1.20 --speaker 192.168.1.21

Exit code 0 when the microphone heard the tones, 1 when it did not, 2 when a
frame could not be reached or refused the request.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

# The tone sequence is ~5.2 s; the listener must cover its lead-in, the wait
# before the speaker frame is triggered, and the tail.
LISTEN_SECONDS = 10
SPEAKER_START_DELAY_S = 1.5
SAME_FRAME_SECONDS = 8


def request(host, path, method="GET", timeout=15):
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", errors="replace")
        raise SystemExit(f"{host}{path}: HTTP {e.code} {detail}") from None
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise SystemExit(f"{host}: not reachable ({e})") from None


def wait_for_result(mic_host, max_wait_s):
    """Polls the microphone frame until its monitor has finished."""
    deadline = time.time() + max_wait_s
    while time.time() < deadline:
        status = request(mic_host, "/api/mic/level")
        if not status["running"] and status.get("result"):
            return status["result"]
        time.sleep(1)
    raise SystemExit(f"{mic_host}: the microphone monitor did not finish in time")


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("mic", help="host/IP of the frame whose microphone listens")
    parser.add_argument(
        "--speaker",
        help="host/IP of the frame that plays the tones (default: the microphone frame itself)",
    )
    args = parser.parse_args()

    speaker = args.speaker or args.mic
    same_frame = speaker == args.mic

    info = request(args.mic, "/api/mic/level")
    if not info["available"]:
        print(f"{args.mic} has no microphone", file=sys.stderr)
        return 2

    if same_frame:
        print(f"Same frame {args.mic}: playing at 100 % while listening ...")
        request(
            args.mic, f"/api/mic/level?seconds={SAME_FRAME_SECONDS}&tones=1", "POST"
        )
        result = wait_for_result(args.mic, SAME_FRAME_SECONDS + 10)
    else:
        print(f"Microphone on {args.mic}, speaker on {speaker} at 100 % ...")
        request(args.mic, f"/api/mic/level?seconds={LISTEN_SECONDS}", "POST")
        time.sleep(SPEAKER_START_DELAY_S)
        request(speaker, "/api/mic/tones?volume=100", "POST")
        result = wait_for_result(args.mic, LISTEN_SECONDS + 10)

    print(
        f"  microphone: floor {result['baseline_dbfs']:.1f} dBFS, "
        f"threshold {result['threshold_dbfs']:.1f} dBFS, "
        f"peak {result['mic_peak_dbfs']:.1f} dBFS, "
        f"{result['mic_bursts']}/{result['expected_bursts']} tone bursts"
    )
    print(
        f"  second microphone: peak {result['mic2_peak_dbfs']:.1f} dBFS, "
        f"{result['mic2_bursts']} bursts"
    )
    if result["heard"]:
        print("PASS: the microphone hears the tones")
        return 0
    print("FAIL: no (or too few) tone bursts detected")
    return 1


if __name__ == "__main__":
    sys.exit(main())
