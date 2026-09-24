# Chimes/Climate Flash Overhead

Measured via `idf.py size-files` (reads the linker `.map` file), comparing
`waveshare_photopainter_73` (has speaker + SHTC3 climate sensor) against
`seeedstudio_xiao_ee02` (has neither).

## Per-object-file sizes

| Object file | waveshare_photopainter_73 | xiao_ee02 |
|---|---:|---:|
| `audio_chime.c.obj` | 3120 B | 15 B |
| `shtc3_sensor.c.obj` | 1267 B | *(not in build)* |
| `climate_history.c.obj` | 1158 B | 34 B |
| `chime.c.obj` | 483 B | 12 B |
| `climate.c.obj` | 471 B | 37 B |
| **Total** | **6499 B (~6.3 KB)** | **98 B** |

Total saved on a board with neither capability: **6401 B (~6.25 KB)**.

## Where the savings come from

Two independent mechanisms contribute, and it's worth keeping them apart since
only one of them was built in the "Chimes/Climate build-modularity" cleanup:

**1. This cleanup's own contribution — `chime.c` / `climate.c` /
`climate_history.c` gated on `BOARD_HAL_HAS_SPEAKER` /
`BOARD_HAL_HAS_CLIMATE_SENSOR`:**

- waveshare: 483 + 471 + 1158 = 2112 B
- xiao_ee02: 12 + 37 + 34 = 83 B
- **Saved: 2029 B (~2.0 KB)**

These three files are always compiled on every board (they're called
unconditionally from many other files), but on a board lacking the hardware
they now shrink to trivial `#if !BOARD_HAL_HAS_X` stub implementations instead
of carrying their full logic (cron matching, history file parsing/backup/JSON
building, room-profile classification tables, etc.).

**2. Pre-existing hardware-driver gating (unrelated to this cleanup, already
in place beforehand):**

- `audio_chime.c` (ES8311 codec driver): already gated on
  `CONFIG_BOARD_DRIVER_WAVESHARE_PHOTOPAINTER_73` at the board_hal level.
  3120 → 15 B = **3105 B**.
- `shtc3_sensor.c`: already excluded from the build entirely on boards that
  don't select `CONFIG_SENSOR_DRIVER_SHTC3`/`SHT40`. 1267 → 0 B =
  **1267 B**.
- Subtotal: **4372 B (~4.3 KB)**.

## Percentage of total firmware

Total firmware image size (`idf.py size`, "Total image size"), fresh
`--fullclean` build for each board so the reported `sdkconfig` is guaranteed
to actually match the target board (an incremental build across a board
switch silently keeps the old `sdkconfig` and under-reports the difference):

| Board | Total image size |
|---|---:|
| `waveshare_photopainter_73` (speaker + climate sensor) | 2,238,673 B (~2.14 MB) |
| `seeedstudio_xiao_ee02` (neither) | 2,154,461 B (~2.11 MB) |

Using the full-featured build's total as the reference base:

| Quantity | Bytes | % of total firmware |
|---|---:|---:|
| Full Chimes+Climate footprint | 6499 B | **0.29 %** |
| — of which this cleanup's own saving | 2029 B | 0.09 % |
| — of which pre-existing driver gating | 4372 B | 0.20 % |
| Residual stub cost (xiao_ee02) | 98 B | 0.004 % |

Note: the two boards' whole-image totals differ by 84,212 B, far more than
the 6,401 B isolated above — other per-board differences (different drivers,
partition/flash config, etc.) also change the total, so the whole-image
totals are given only for context/percentage scaling. The per-object-file
comparison in the table above remains the reliable, isolated measurement of
the Chimes/Climate cost itself.

## Summary

- Full feature footprint (both capabilities present): **~6.3 KB** flash,
  **~0.29 %** of the total firmware image.
- This session's Kconfig-gating refactor specifically saves **~2.0 KB**
  (~0.09 % of total) of that for boards without the hardware.
- The remaining **~4.3 KB** (~0.20 % of total) was already saved by the
  pre-existing board/sensor driver selection mechanism, independent of this
  work.
- Residual stub cost on a board with neither capability: **98 B total**
  (~0.004 % of total) — the price of the "always compiled, internally
  `#ifdef`-stubbed" pattern versus true file exclusion from
  `CMakeLists.txt`. Negligible.
