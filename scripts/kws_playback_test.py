#!/usr/bin/env python3
"""
End-to-end test of the stop-word recognition on a real frame, without a person:
this PC plays the test words (host_tests/data/kws, spoken by text-to-speech)
through its speakers, and the frame's microphone enrols the word and then
listens to a sequence of "Stopp" variants mixed with other words.

    python kws_playback_test.py 192.168.1.20

Put the frame near the PC speakers. The samples are compressed to a high level
first (the frame's microphone is quiet compared with a person talking next to
it). Windows only (plays through PowerShell's SoundPlayer). Exit code 0 when
every "Stopp" was heard and nothing else was accepted.
"""

import argparse
import array
import json
import math
import os
import subprocess
import sys
import tempfile
import time
import urllib.request
import wave
from pathlib import Path

DATA_DIR = Path(__file__).resolve().parent.parent / "host_tests" / "data" / "kws"
ENROLL_WORDS = ["stopp_hedda_r0", "stopp_hedda_r2", "stopp_hedda_r0", "stopp_hedda_r2"]
SEQUENCE = [
    ("neg_danke", False),
    ("stopp_hedda_r-2", True),
    ("neg_stock", False),
    ("neg_guten_morgen", False),
    ("stopp_hedda_r3", True),
    ("neg_spott", False),
    ("stopp_zira_r0", False),  # another speaker/language: must not pass
    ("neg_stoppuhr", False),
    ("stopp_hedda_r0", True),
    ("neg_hallo", False),
]


def prepare_audio(dst):
    """Level-compressed copies with a little silence around each word."""
    for src in DATA_DIR.glob("*.wav"):
        with wave.open(str(src), "rb") as w:
            data = array.array("h", w.readframes(w.getnframes()))
        rms = math.sqrt(sum(x * x for x in data) / len(data))
        gain = 9000.0 / rms
        out = array.array("h", (int(32000 * math.tanh(x * gain / 32000)) for x in data))
        pad = array.array("h", [0] * 3200)
        with wave.open(str(dst / src.name), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(16000)
            w.writeframes((pad + out + pad).tobytes())


class Frame:
    def __init__(self, host):
        self.host = host

    def call(self, path, method="GET"):
        req = urllib.request.Request(f"http://{self.host}{path}", method=method)
        with urllib.request.urlopen(req, timeout=15) as resp:
            return json.loads(resp.read().decode("utf-8"))

    def wait_idle(self, limit=90):
        end = time.time() + limit
        while time.time() < end:
            status = self.call("/api/kws/status")
            if status["mode"] == "idle":
                return status
            time.sleep(0.5)
        raise SystemExit("the frame did not finish in time")

    def log(self, marker):
        req = urllib.request.Request(f"http://{self.host}/api/debug/log")
        with urllib.request.urlopen(req, timeout=30) as resp:
            text = resp.read().decode("utf-8", errors="replace")
        lines = [ln for ln in text.splitlines() if " kws: " in ln]
        start = max((i for i, ln in enumerate(lines) if marker in ln), default=0)
        return [ln.split(" kws: ", 1)[1] for ln in lines[start:]]


def play(audio_dir, names, gap_ms):
    """Plays the words one after another in one PowerShell process."""
    words = ",".join(f"'{n}'" for n in names)
    script = (
        f"$d='{audio_dir}'; foreach($n in @({words})){{ "
        "(New-Object Media.SoundPlayer (Join-Path $d ($n+'.wav'))).PlaySync(); "
        f"Start-Sleep -Milliseconds {gap_ms} }}"
    )
    return subprocess.Popen(["powershell.exe", "-NoProfile", "-Command", script])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("host", help="host/IP of a frame with a microphone")
    args = parser.parse_args()
    if os.name != "nt":
        raise SystemExit("this test plays audio through Windows PowerShell")

    frame = Frame(args.host)
    if not frame.call("/api/kws/status")["available"]:
        raise SystemExit("this board has no microphone")

    with tempfile.TemporaryDirectory() as tmp:
        prepare_audio(Path(tmp))
        frame.call("/api/kws/templates", "DELETE")
        for word in ENROLL_WORDS:
            frame.call("/api/kws/enroll?seconds=4", "POST")
            proc = play(tmp, [word], 0)
            status = frame.wait_idle()
            proc.wait()
            result = status["enroll"]
            print(
                f"enrol {word}: {'ok' if result['status'] == 0 else result['status']}"
                f", threshold now {status['threshold']}"
            )
            if result["status"] != 0:
                raise SystemExit(
                    "enrolment failed - is the frame close to the speakers?"
                )

        frame.call("/api/kws/test?seconds=40", "POST")
        time.sleep(1)
        play(tmp, [w for w, _ in SEQUENCE], 1500).wait()
        time.sleep(2)
        status = frame.wait_idle(90)

    lines = frame.log("Listening for the stop word")
    scores = [
        float(ln.split("distance ")[1].split(" ")[0])
        for ln in lines
        if ln.startswith("Utterance")
    ]
    threshold = status["threshold"]
    print(
        f"\nthreshold {threshold}; heard {len(scores)} utterances of {len(SEQUENCE)} played\n"
    )
    ok = len(scores) >= len(SEQUENCE)  # a trailing extra sound at the end is fine
    for (word, should_match), score in zip(SEQUENCE, scores):
        matched = score < threshold
        good = matched == should_match
        ok = ok and good
        print(
            f"  {word:18s} distance {score:6.2f}  "
            f"{'accepted' if matched else 'rejected':8s} "
            f"{'ok' if good else '<-- WRONG'}"
        )
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
