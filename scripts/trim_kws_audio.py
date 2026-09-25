#!/usr/bin/env python3
"""Trims leading/trailing silence of the 16 kHz mono WAV files in a directory
(helper of generate_kws_test_audio.ps1). Keeps 120 ms of margin around the speech."""

import array
import sys
import wave
from pathlib import Path

MARGIN_S = 0.12
FRAME = 160  # 10 ms at 16 kHz


def trim(path):
    with wave.open(str(path), "rb") as w:
        assert (
            w.getnchannels() == 1
            and w.getsampwidth() == 2
            and w.getframerate() == 16000
        )
        data = array.array("h", w.readframes(w.getnframes()))

    energies = []
    for i in range(0, len(data) - FRAME + 1, FRAME):
        chunk = data[i : i + FRAME]
        energies.append(sum(abs(s) for s in chunk) / FRAME)
    peak = max(energies) if energies else 0
    active = [i for i, e in enumerate(energies) if e > max(peak * 0.04, 30)]
    if not active:
        return
    margin = int(MARGIN_S * 16000)
    start = max(0, active[0] * FRAME - margin)
    end = min(len(data), (active[-1] + 1) * FRAME + margin)
    trimmed = data[start:end]

    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(trimmed.tobytes())
    print(f"{path.name}: {len(data) / 16000:.2f} s -> {len(trimmed) / 16000:.2f} s")


if __name__ == "__main__":
    for wav in sorted(Path(sys.argv[1]).glob("*.wav")):
        trim(wav)
