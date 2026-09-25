# Next steps and ideas

## Next (recommended)
- Real-voice tuning from the test results: margin/threshold, maybe a "sensitivity" setting for the stop word
- Snooze by voice (second word) and/or a snooze button
- Weekday choice for the button-set alarm (today Mon-Fri only)
- Own alarm volume (today shared with the Chimes volume), optional soft-start ramp
- Publish v218.6.0 as stable once the manual tests pass

## Robustness
- Templates file: checksum + atomic write (temp file, rename)
- Host tests for `kws_service` logic (load/validate/calibrate) with a fake storage
- CI: run `kws_test` / `alarm_pattern_test` (needs `host_tests/data/kws`)
- Thread-safe lazy init of the matcher (first use from HTTP task vs alarm task)

## Ideas
- Listen during the notes too (quieter tones, or echo suppression) → faster stop
- Learn the noise of the room at ring start to adapt the VAD threshold
- Two users / two stop words
- Stop word check on the frame's display ("heard: 4.1 / threshold 6.4")
- German user guide
- More boards with speaker + microphone → voice comes automatically (`BOARD_HAL_HAS_MICROPHONE`)

## Housekeeping
- Untracked files in the repo root (logs, JPGs, PDFs, `Versuch*.log` ...) → move out or ignore
- Delete `feature/voice-stop` (local + remote) once released; check leftover stashes/worktrees
- Update or remove the old plan `docs/CROP_EDITOR_PLAN.md` (untracked)
