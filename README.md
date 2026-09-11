# ESP32 PhotoFrame - with optional native Telegram integration

A modern, feature-rich firmware for ESP32-based e-paper photo frames (currently supporting **Waveshare PhotoPainter**, **Seeed Studio XIAO EE02/EE03/EE04**, and **Seeed Studio reTerminal E1002/E1003/E1004**). This firmware replaces stock firmware with a powerful RESTful API, web interface, and **significantly better image quality**.

> **This is an independently maintained fork** of [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe) (based on its `v2.18.0` release), maintained here as its own repository going forward rather than as a pull request back upstream. See [Changes from Upstream](#changes-from-upstream) below for the full list of what's different, and [Roadmap](#roadmap) for what's planned next. All companion-project links below (server, app, Home Assistant integration) point at the original upstream project's ecosystem, not this fork.

![PhotoFrame](.img/photo1.jpg)

## Key Features

### Upstream & Fork

- 🎨 **Superior Image Quality**: Measured color palette with automatic calibration produces significantly better results than stock firmware
- 🔋 **Smart Power Management**: Deep sleep mode for weeks of battery life, or always-on for Home Assistant
- 📁 **Flexible Image Sources**: SD card rotation, URL-based fetching (weather, news, random images from image server)
- 🌐 **Modern Web Interface**: Drag-and-drop uploads, gallery view, real-time battery status
- 📱 **Mobile App**: [Companion app](https://github.com/aitjcize/esp32-photoframe-app) for WiFi provisioning, image processing, and AI generation
- 🖼️ **Image Server**: [Companion server](https://github.com/aitjcize/esp32-photoframe-server) with many photo sources — Google Photos, Immich, Synology Photos, Unsplash, Pexels, Telegram bot, URL proxy, and AI generation — plus date/time and weather overlays
- 🏠 **Home Assistant Ready**: [Companion integration](https://github.com/aitjcize/ha-esp32-photoframe) available
- 🔌 **RESTful API**: Full programmatic control ([API docs](docs/API.md))

### Fork only

- 🖼️ **Crop or Letterbox**: Choose whether mismatched-aspect-ratio images are cropped to fill the screen or shown in full with letterbox bars ([docs](docs/SCALE_MODE.md))
- 🙂 **Face-Aware Crop Metadata**: Optional offline face detection in `process-cli` recommends a crop that keeps faces fully visible instead of a plain center-crop, saved as a per-image JSON sidecar; the firmware can optionally pick between pre-rendered Cover/Fit variants per its own Scale Mode setting, avoiding on-device rendering entirely ([docs](docs/FACE_CROP.md))
- 🤖 **Telegram Bot**: Send photos straight to the frame via a Telegram bot, no extra server required, plus an extensive set of chat commands for remote control and status monitoring ([docs](docs/TELEGRAM.md))
- 🧩 **Orientation Pairing**: Two portrait photos are automatically combined side by side into one image instead of letterboxing either one — works for both Telegram receives and normal auto-rotation
- ☀️ **Weather + Headline Overlays**: Optional on-device weather line and news headlines drawn on the display, no companion server required ([docs](docs/OVERLAYS.md))
- 🪫 **Low-Battery Warnings**: On-display corner badge and a Telegram alert below a configurable threshold, plus a Battery History tab with a days-remaining-until-20% estimate
- ⚠️ **On-Display Error Banner**: Shows a short error message right on the frame after repeated WiFi/internet failures, not just in the logs
- 🔁 **No-Repeat Display History**: Random rotation remembers what's already been shown so every image appears once before any repeats
- 🗓️ **Agenda Mode**: Optional full-screen ToDo (todo.txt format) + Calendar view (up to two ICS/iCal URLs, merged) on its own independent wake schedule — skips normal photo rotation entirely, with color-coded priorities/tags/due dates, per-calendar-source colors, and a configurable background ([docs](docs/AGENDA_COLORS.html))

## Screenshots

<table>
<tr>
<td align="center"><b>Gallery & Albums</b></td>
<td align="center"><b>Battery History</b></td>
</tr>
<tr>
<td><img src=".img/webui_gallery.png" width="420"/></td>
<td><img src=".img/webui_battery_history.png" width="420"/></td>
</tr>
</table>

**Weather, headline, and error overlays** are drawn directly on the device — no companion server needed:

![Overlay options: none, weather, headlines, weather+headlines combined, error banner](.img/overlay_showcase.png)

**Agenda Mode** renders ToDo and Calendar content full-screen on its own independent wake schedule, with color-coded priorities/tags/due dates and per-calendar-source colors ([color scheme docs](docs/AGENDA_COLORS.html)):

<table>
<tr>
<td align="center"><b>Agenda → ToDo</b></td>
<td align="center"><b>Agenda → Calendar</b></td>
</tr>
<tr>
<td><img src=".img/agenda_todo.png" width="380"/></td>
<td><img src=".img/agenda_calendar.png" width="380"/></td>
</tr>
</table>

<table>
<tr>
<td align="center"><b>Settings → Auto Rotate</b></td>
<td align="center"><b>Settings → Power</b></td>
</tr>
<tr>
<td><img src=".img/settings_auto_rotate_full.png" width="280"/></td>
<td><img src=".img/settings_power_full.png" width="280"/></td>
</tr>
</table>

## Ecosystem

This project has companion tools for different use cases:

| Project | Description |
|---------|-------------|
| [**ha-esp32-photoframe**](https://github.com/aitjcize/ha-esp32-photoframe) | Home Assistant integration for control, monitoring, and automation |
| [**esp32-photoframe-server**](https://github.com/aitjcize/esp32-photoframe-server) | Image server aggregating many photo sources — Gallery uploads, Google Photos, Immich, Synology Photos, Unsplash, Pexels, Telegram bot, URL proxy, and AI generation (OpenAI/Gemini) — with date/time & weather overlays and smart collage. Can be run as a Home Assistant add-on. |
| [**esp32-photoframe-app**](https://github.com/aitjcize/esp32-photoframe-app) | Mobile companion app for WiFi provisioning and device control. iOS: [App Store](https://apps.apple.com/tw/app/esp-frame/id6762510995?l=en-GB) (USD 2.99, to offset Apple's USD 99/yr developer fee). Android: [Google Play](https://play.google.com/store/apps/details?id=com.aitjcize.espframe) (free); join the [beta testers Google Group](https://groups.google.com/g/esp32-photoframe-app-testers) for early access to new features. |
| [**epaper-image-convert**](https://github.com/aitjcize/epaper-image-convert) | CLI tool & npm library for e-paper image conversion with advanced dithering |

## Third Party Integrations

| Project | Description |
|---------|-------------|
| [**puppet**](https://github.com/balloob/home-assistant-addons/tree/main/puppet) | HA Puppet add-on. Generate image URL from Puppet dashboard and use it as the Auto Rotate URL in PhotoFrame settings. |

## Image Quality Comparison

**🎨 [Try the Interactive Demo](https://aitjcize.github.io/esp32-photoframe/)** - Drag the slider to compare algorithms in real-time with your own images!

<table>
<tr>
<td align="center"><b>Original Image</b></td>
<td align="center"><b>Stock Algorithm<br/>(on computer)</b></td>
<td align="center"><b>Stock Algorithm<br/>(on device)</b></td>
<td align="center"><b>Our Algorithm<br/>(on device)</b></td>
</tr>
<tr>
<td><a href="https://github.com/aitjcize/esp32-photoframe/raw/refs/heads/main/.img/sample.jpg"><img src=".img/sample.jpg" width="200"/></a></td>
<td><a href="https://github.com/aitjcize/esp32-photoframe/raw/refs/heads/main/.img/stock_algorithm_on_computer.bmp"><img src=".img/stock_algorithm_on_computer.bmp" width="200"/></a></td>
<td><a href="https://github.com/aitjcize/esp32-photoframe/raw/refs/heads/main/.img/stock_algorithm.bmp"><img src=".img/stock_algorithm.bmp" width="200"/></a></td>
<td><a href="https://github.com/aitjcize/esp32-photoframe/raw/refs/heads/main/.img/our_algorithm.png"><img src=".img/our_algorithm.png" width="200"/></a></td>
</tr>
<tr>
<td align="center">Source JPEG</td>
<td align="center">Theoretical palette<br/>(looks OK on screen)</td>
<td align="center">Theoretical palette<br/>(washed out on device)</td>
<td align="center">Measured palette<br/>(accurate colors)</td>
</tr>
</table>

**Why Our Algorithm is Better:**

- ✅ **Accurate Color Matching**: Uses actual measured e-paper colors
- ✅ **Automatic Calibration**: Built-in palette calibration tool adapts to your specific display
- ✅ **Better Dithering**: Floyd-Steinberg algorithm with measured palette produces more natural color transitions
- ✅ **No Over-Saturation**: Avoids the washed-out appearance of theoretical palette matching

The measured palette accounts for the fact that e-paper displays show darker, more muted colors than pure RGB values. By dithering with these actual colors, the firmware makes better decisions about which palette color to use for each pixel, resulting in images that look significantly better on the physical display. The automatic calibration feature allows you to measure and optimize the palette for your specific device.

📖 **[Read the technical deep-dive on measured color palettes →](docs/MEASURED_PALETTE.md)**

## Power Management

**Deep Sleep Enabled (Default)**:
- Battery life: months
- Wake via BOOT/KEY button or auto-rotate timer
- Web interface accessible only when awake
- Power: ~10μA in sleep

**Deep Sleep Disabled (Always-On)**:
- Best for Home Assistant integration
- Web interface always accessible
- Power: ~40-80mA with auto light sleep
- Battery life: days to weeks depending on usage

**Auto-Rotation**: SD card (default) or URL-based (fetch from web)

Configure via web interface **Settings** section.

### Real-World Battery Life

Upstream issue [#121](https://github.com/aitjcize/esp32-photoframe/issues/121) reports battery drain far short of "months" (10-15%/day) on several boards. This traced to two deep-sleep-wake crashes that only reproduce on battery power — a dynamic-frequency-scaling/WiFi-interrupt race, and a main-task stack overflow (see [Stability fixes](#changes-from-upstream) below), both confirmed via on-device coredump. A crash mid-sleep-entry forces a reboot instead of a real deep sleep, which is a plausible full explanation for the reported drain.

A ~9.9-day untethered soak test on a `waveshare_photopainter_73` running this fork's fixes measured **~1.17%/day** — roughly 10x better than the reported rate — with a Telegram bot, weather overlay, and WiFi power-save mode all active throughout:

![Real-world battery drain over 9.9 days, ~1.17%/day measured](.img/battery_drain_real_world.png)

So far, the fix is confirmed and shipped on `main` only for `waveshare_photopainter_73`, the one board available for direct testing here. `reterminal_e1002` shares the same SD-card/e-paper SPI-bus wiring implicated in part of the crash and is a strong candidate for the same fix; `xiao_ee02`/`xiao_ee04` would be by analogy only. Neither has shipped yet — tracked against issue #121, pending a build for owners of that hardware to confirm before merging.

## AI Image Generation 🤖

The web interface supports client-side AI image generation using OpenAI (GPT Image, DALL-E) or Google Gemini.

- **Generate on Demand**: Create custom artwork directly from the web interface using text prompts
- **Multiple Providers**: OpenAI and Google Gemini supported
- **Client-Side Processing**: AI generation runs in your browser, then uploads to the device

Configure your API keys in **Settings > AI Generation**.


## Supported Hardware

| Board | Display | Storage | Board Name |
|-------|---------|---------|------------|
| [Waveshare PhotoPainter](https://www.waveshare.com/wiki/ESP32-S3-PhotoPainter) | 7.3" **6-color** | SD card (SDIO) | `waveshare_photopainter_73` |
| [Seeed Studio XIAO EE02](https://www.seeedstudio.com/XIAO-ePaper-DIY-Kit-EE02-for-13-3-Spectratm-6-E-Ink.html) | 13.3" 6-color | Internal flash | `seeedstudio_xiao_ee02` |
| [Seeed Studio XIAO EE03](https://wiki.seeedstudio.com/getting_started_with_ee03/) | 10.3" 16-level grayscale | Internal flash | `seeedstudio_xiao_ee03` |
| [Seeed Studio XIAO EE04](https://www.seeedstudio.com/XIAO-ePaper-EE04-DIY-Bundle-Kit.html) | 7.3" 6-color | Internal flash | `seeedstudio_xiao_ee04` |
| [Seeed Studio reTerminal E1002](https://www.seeedstudio.com/reTerminal-E1002-p-6533.html) | 7.3" 6-color | SD card (SPI) + Internal flash | `seeedstudio_reterminal_e1002` |
| [Seeed Studio reTerminal E1003](https://www.seeedstudio.com/reTerminal-E1003-p-6731.html) | 10.3" 16-level grayscale | SD card (SPI) + Internal flash | `seeedstudio_reterminal_e1003` |
| [Seeed Studio reTerminal E1004](https://www.seeedstudio.com/reTerminal-E1004-p-6692.html) | 13.3" 6-color | SD card (SPI) + Internal flash | `seeedstudio_reterminal_e1004` |

The reTerminal E1002, E1003, and E1004 also include a SHT40 temperature/humidity sensor, PCF8563 RTC, and battery monitoring. The XIAO EE03 has a SHT40 sensor and battery monitoring as well (but no RTC).

### Button Functions

Buttons behave differently depending on whether the device is awake (web UI accessible) or in deep sleep.

**When in deep sleep:**

| Button | Waveshare PhotoPainter | XIAO EE02 / EE03 / EE04 | reTerminal E1002 | reTerminal E1003 | reTerminal E1004 |
|--------|----------------------|-------------------|------------------|------------------|------------------|
| **Wake** | BOOT button | Button 3 | Green button | Refresh button | Refresh button |
| **Rotate** | KEY button | Button 1 | Left button | Left button | Right button |
| **Clear** | N/A | Button 2 | Right button | Right button | Left button |

- **Wake**: Wakes the device and starts the web UI / HTTP server (stays awake)
- **Rotate**: Wakes the device, rotates to the next image, then goes back to sleep
- **Clear**: Wakes the device, clears the display to white, then goes back to sleep

**When awake:**

| Button | Function |
|--------|----------|
| **Rotate** | Rotates to the next image |
| **Clear** | Clears the display to white |

### 💾 Internal Flash Storage
Boards with larger flash chips (XIAO EE02/EE03/EE04, reTerminal E1002/E1004) use internal flash as persistent storage via LittleFS. On the reTerminal, the SD card takes priority when inserted; internal flash serves as a fallback. The Waveshare board does not have internal flash storage due to its 16MB flash being fully allocated to OTA partitions.

### Known Issues 🚧

- **PhotoPainter Restarts**: All existing Waveshare PhotoPainter boards on the market use the AXP2101 power management IC, which causes unexplained restarts when connected to both Type-C and a lithium battery simultaneously. **Workaround:** use either USB power only or battery only. Using both at the same time may cause frequent firmware restarts due to unstable power supply. Waveshare has confirmed this issue and future boards will ship with TG28 as a replacement, which will not have this problem. See [waveshareteam/ESP32-S3-PhotoPainter#5](https://github.com/waveshareteam/ESP32-S3-PhotoPainter/issues/5#issuecomment-3876269519) for details.
- **Seeed Studio Deep Sleep & USB Power**: The XIAO EE02, EE03, and EE04 can only detect USB connections from a **PC** (via USB-Serial-JTAG SOF packets); chargers and power banks will **not** keep them awake (these boards do not route USB VBUS to an ESP32 GPIO). The same applies to **reTerminal E1002 hardware revisions earlier than V1.2**, which use the non-I2C **ETA6003** charger. The reTerminal **E1002 V1.2+** (which switched to the **SY6974B** charger) **and the E1004** read the SY6974B's power-good status over I2C, so they detect *any* USB/charger/power-bank input and stay awake on external power automatically — no workaround needed. The Waveshare PhotoPainter likewise detects USB power via its AXP2101 PMIC. **Workaround (XIAO EE02/EE03/EE04, and E1002 boards older than V1.2):** if you want the device always accessible while powered by a charger or power bank, disable deep sleep in **Settings > General**.

## Installation

### Web Flasher (Easiest) ⚡

**[🌐 Flash from Browser](https://t3ste.github.io/Tlg-esp32-photoframe/#flash)** - Chrome/Edge/Opera required

### Manual Flash

Download from [Releases](https://github.com/t3ste/Tlg-esp32-photoframe/releases):

```bash
esptool.py --chip esp32s3 --port /dev/ttyUSB0 --baud 921600 write_flash 0x0 photoframe-firmware-<board>-merged.bin
```

**Device not detected?** Hold BOOT button + press PWR to enter download mode.

**Build from source:**

We provide a `build.py` helper script to simplify building for different boards.

```bash
# Build for Waveshare PhotoPainter (default)
./build.py --board waveshare_photopainter_73

# Build for Seeed Studio XIAO EE02
./build.py --board seeedstudio_xiao_ee02

# Build for Seeed Studio XIAO EE03 (10.3" 16-level grayscale e-paper)
./build.py --board seeedstudio_xiao_ee03

# Build for Seeed Studio XIAO EE04
./build.py --board seeedstudio_xiao_ee04

# Build for Seeed Studio reTerminal E1002
./build.py --board seeedstudio_reterminal_e1002

# Build for Seeed Studio reTerminal E1003 (10.3" 16-level grayscale e-paper)
./build.py --board seeedstudio_reterminal_e1003

# Build for Seeed Studio reTerminal E1004 (13.3" 6-color e-paper)
./build.py --board seeedstudio_reterminal_e1004

# Flash the firmware
idf.py -p /dev/ttyUSB0 flash monitor
```

For more details, see [DEV.md](docs/DEV.md)

### WiFi Provisioning

The device supports two methods for WiFi provisioning:

#### Option 1: SD Card Provisioning (boards with SD card only)

1. Create a file named `wifi.txt` on your SD card with:
   ```
   YourWiFiSSID
   YourWiFiPassword
   MyPhotoFrame
   ```
   - Line 1: WiFi SSID (network name)
   - Line 2: WiFi password
   - Line 3: Device name (optional, defaults to "PhotoFrame")
   - Use plain text, no quotes or extra formatting
   - The file can be placed at the root or in a `config/` folder

2. Insert SD card and power on the device
3. Device automatically reads credentials, saves to memory, and connects
4. The `wifi.txt` file is automatically deleted after reading (to prevent issues with invalid credentials)

**Note**: If credentials are invalid, the device will clear them and fall back to captive portal mode.

#### Option 2: Captive Portal

1. Device creates a unique AP on first boot (e.g. `PhotoFrame - A1B2C3`, where `A1B2C3` is derived from the device's MAC address)
2. Connect to the AP and open `http://192.168.4.1` (or use captive portal)
3. Enter WiFi credentials (2.4GHz only)
4. Device tests connection and saves if successful

#### Option 3: Companion App

1. Install the ESP Frame companion app:
   - **iOS**: [App Store](https://apps.apple.com/tw/app/esp-frame/id6762510995?l=en-GB)
   - **Android**: install from [Google Play](https://play.google.com/store/apps/details?id=com.aitjcize.espframe) (free); for early access to new features, join the [beta testers Google Group](https://groups.google.com/g/esp32-photoframe-app-testers)
2. Tap the "+" button on the home screen
3. The app scans for PhotoFrame setup hotspots, connects automatically, and guides you through WiFi configuration

**Re-provision:** Delete credentials with `idf.py erase-flash` or place new `wifi.txt` on SD card after clearing stored credentials

## Usage

**Web Interface:** `http://photoframe.local` or device IP address
- Gallery view with drag-and-drop uploads
- Settings, battery status, display control

**API:** Full documentation in [API.md](docs/API.md)

## Troubleshooting

- **WiFi issues**: Ensure 2.4GHz network, check serial monitor for IP
- **SD card not detected**: Format as FAT32, try different card
- **Upload fails**: Check file is valid JPEG, monitor serial output
- **Device not detected for flash**: Hold BOOT + press PWR for download mode

## Offline Image Processing

Node.js CLI tool for batch processing and image serving:

### Batch Processing
```bash
cd process-cli && npm install
# Process to disk
node cli.js input.jpg --device-parameters -o /path/to/sdcard/images/

# Or upload directly to device
node cli.js ~/Photos/Albums --upload --device-parameters --host photoframe.local
```

### Image Server Mode
Serve pre-processed images directly to your ESP32 over HTTP:

```bash
node cli.js --serve ~/Photos --serve-port 9000 --device-parameters --host photoframe.local
```

The ESP32 can fetch images from your computer instead of storing them on SD card. Supports EPDGZ, BMP, PNG, and JPG formats with automatic thumbnail generation.

See [process-cli/README.md](process-cli/README.md) for details.

**Building your own image server?** The firmware's URL rotation fetch protocol — request method, custom `X-*` headers, `Authorization` / custom-header handling, and the `ETag` / `304 Not Modified` caching flow — is documented in [docs/API.md → URL Rotation Fetch](docs/API.md#url-rotation-fetch).

## Changes from Upstream

This fork is ahead of [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe)'s `main` by 80+ commits. Everything below is new in this fork, not present upstream:

**Telegram Bot integration** ([docs](docs/TELEGRAM.md)):
- feat: native Telegram Bot rotation mode — send photos straight to the frame, alongside (not replacing) SD-card/URL rotation, with progressive-JPEG fallback through Telegram's alternate photo resolutions
- feat: Telegram image caption drawn as a text overlay on the photo
- feat: extensive Telegram command set for remote control/configuration (`/status`, `/clear`, `/restart`, `/pairing`, `/rotate_cron`, `/deep_sleep`, `/auto_rotate`, `/wake_notify`, `/error_overlay`, `/wifi_perf`, `/help`, `/list_albums`, `/active_albums`, `/enable_album`, `/clear_history`, `/rotation_notify`, `/rotation_pairing`, `/keep_originals`, `/exif_date`), plus an emergency `/telegram_reset` that clears the queue and sleeps immediately, bypassing normal processing
- feat: `/start` treated as an alias for `/help` (Telegram clients send it automatically on first open)
- feat: orientation pairing — combine two complementary portrait/landscape photos into one composed image instead of letterboxing either one (Telegram-only at first, later extended to normal auto-rotation too)
- feat: album fallback rotation for Telegram mode — falls back to normal rotation instead of leaving the previous image up indefinitely when a poll doesn't yield a new image, with an optional Telegram notification (photo re-upload) so the chat still reflects what's on the frame; itself toggleable, so the frame can instead be pinned to show only actual Telegram photos and never change on a wake with nothing new — with a second, related toggle deciding separately whether a Telegram connection failure (vs. a poll that simply found nothing new) still counts as an exception that falls back
- feat: Telegram "keep originals" toggle — archives each received photo's raw bytes (always the largest available size) to `Telegram/Originals`
- feat: streaming JPEG decoder for large Telegram document uploads that don't fit in one contiguous PSRAM buffer
- feat: experimental EXIF capture-date fallback caption when a received photo has no caption of its own
- feat: low-battery Telegram warning (below 20%, debounced per discharge cycle)
- feat: reports the actually-used weather source in `/status` (the configured provider and the one that last actually succeeded can differ)
- feat: opt-in duplicate detection via Telegram's own content-based `file_unique_id` — a re-sent/forwarded photo or file is skipped before downloading anything, no local hashing needed
- feat: EPDGZ output for composed orientation pairs, following the same on-device image format setting used for a single photo
- fix: Telegram thumbnail generation could silently overwrite/corrupt the just-downloaded original photo before it was ever converted or archived
- fix: don't reject large Telegram JPEGs before download now that the streaming decoder can actually handle them
- fix: a document upload's "saved" confirmation tried to echo back its thumbnail file as a photo, which Telegram categorically rejects — falls back to a text-only confirmation now
- feat: opt-in Telegram power-save mode — minimizes wake duration/WiFi-on time on an automatic timer wake (fewer WiFi/Telegram retries, no post-rotation config-sync window, no per-photo "saved" reply, no wake status ping, no fallback-rotation photo notification — the latter two settings are grayed out, not cleared, and resume if this is turned back off), never affecting a manual button wake so the web UI always stays reachable; a nested "latest only" sub-option processes just the newest update in a batch and permanently discards the rest (other photos, captions, and "/" commands); orientation pairing and the low-battery warning are deliberately left untouched since neither costs any extra network time
- fix: `wifi_manager_connect()` could hang indefinitely if WiFi association succeeded but DHCP then stalled (no bounded wait existed); it and its callers now always return within a definite timeout — also fixes a pre-existing bug where a *known* total connection failure still wasted up to a full 60s busy-polling a result that could never change
- fix: the Web UI's "auto-rotate orientation pairing" toggle (`/rotation_pairing`, album picks during rotation) was only shown while `rotation_mode` was set to Storage, even though it — and the rest of its settings card — apply regardless: Telegram- and URL-mode's own fallback-to-storage path calls the exact same rotation code as Storage mode's primary rotation. Now shown whenever Auto-Rotate is on, any mode; also clarified its wording (and the separate, similarly-named `/pairing` for incoming Telegram photos) in the Web UI, bot replies, and docs, since the two were easy to confuse

**Face-Aware Crop** ([docs](docs/FACE_CROP.md)):
- feat: offline face detection in `process-cli` (BlazeFace, fully offline-capable) recommends a crop that keeps faces fully visible, saved as a versioned `<name>.facecrop.json` sidecar (39 unit tests, pure-logic coverage)
- feat: `--crop-output cropped|uncropped|both` — render the face-aware Cover crop, the uncropped Fit letterbox, or both side by side, so an album can be pre-rendered for either firmware Scale Mode setting without re-processing
- feat: `--crop-preview` — writes the full, unmodified source image with detected face boxes (blue) and the recommended crop rectangle (red) overlaid, for sanity-checking face detection/the crop heuristic before a real batch render
- fix: switched process-cli's canvas library from `canvas` to `@napi-rs/canvas` (severe Windows-only native memory leak that crashed large `--detect-faces --crop-output both` batches), which surfaced a latent Canvas-vs-ImageData detection bug in the shared rendering pipeline — both fixed
- fix: the face-crop heuristic's internal "does this face still fit" check rounded to whole pixels too early, which could reject a face that genuinely fit and needlessly shrink the recommended crop (observed cutting a 3-person photo down to 2) — now stays sub-pixel-precise internally and rounds only at the final output
- fix: the recommended crop grew only as much as the minimum needed to reach the target aspect ratio around the accepted faces, discarding image content on the sides/top/bottom even when nothing required it — now grows to the *largest* aspect-ratio box that still fits the image and contains the faces (e.g. a wide photo now only loses height, not width+height, to reach 5:3)
- fix: the face-crop heuristic applied each face's safety margin (`--face-margin`) *before* checking whether it still fit, so the margin buffer itself — not the actual faces — could push the union just over the available space and wrongly drop an otherwise-fitting face (observed excluding a 3rd person whose bare face box fit easily); margin is now applied once, after face selection, to the final accepted set
- feat: `--face-detect-tiles <n>` — additionally runs face detection on an NxN grid of overlapping tiles, to catch small/distant faces that whole-image detection misses (BlazeFace's frozen 128x128 input shrinks the whole photo regardless of resolution, so small faces can vanish before `--face-min-score` ever gets a candidate to threshold); confirmed recovering 2 faces (one sunglasses-occluded, one just small/distant) that whole-image detection missed on a real 5-person photo
- feat: firmware-side, opt-in on-demand Cover/Fit rendering — the device recognizes pre-rendered `<name>.cover.<ext>` / `<name>.fit.<ext>` variants and picks whichever matches its own Scale Mode setting, falling back to a one-time on-device render (cached afterwards) only for a genuinely still-undecoded source, verified by content, not filename
- feat: Web UI "Organize Crop Folders" maintenance action, and Cover/Fit variant pairs are deduplicated to one entry in the gallery listing and rotation loops

**Image formats & processing**:
- feat: new on-device EPDGZ encoder — Telegram-ingested photos can now be converted straight to EPDGZ (palette-indexed, gzip-compressed) instead of always PNG, matching what Web UI uploads already produced client-side; configurable per ingestion path (Telegram: device-side setting; Web UI: browser-side setting), with automatic fallback to PNG if EPDGZ can't get the memory it needs
- feat: streaming JPEG decode fallback (`tjpgd`'s low-level API driven directly) for oversized documents that don't fit the all-in-one-buffer decode path
- feat: `process-cli --split-by-orientation` — routes each album's output into `landscape`/`portrait`/`square` subfolders based on each photo's own orientation, instead of one flat folder; a photo that can't be decoded at all is copied unmodified into an `unknown/` subfolder rather than just being skipped with a console error. Off by default, folder mode only

**Weather & headline overlays** ([docs](docs/OVERLAYS.md)):
- feat: native 3-day forecast weather overlay + RSS/Atom news headline overlay drawn on the display — no companion server or API keys needed
- feat: selectable weather data source (Open-Meteo, wttr.in, or yr.no/MET Norway)
- feat: overlay colors (black bar/white text, or inverted) and English/German condition wording, shared with Telegram caption styling
- feat: opt-in overlay support for already-rendered EPDGZ Storage/Auto-Rotate album images (previously PNG-only) — decodes, draws, and re-encodes the one file being shown, off by default since it's an extra step per display
- fix: the Web UI's "Display Image" gallery action never applied weather/headline overlays at all (any format, any settings) — it bypassed overlay compositing entirely, unlike the Auto-Rotate loops
- feat: opt-in low-battery corner badge — a small (~17% of panel width, not a full-width bar) "BATT NN%" indicator drawn on every display update once the battery drops below a configurable threshold (default 16%, clears 4 points above it, state remembered across deep sleep), independent of Telegram/Web UI reachability; red on color-capable panels, black on grayscale-only ones
- fix: clarified that "Also overlay pre-rendered EPDGZ images" (Settings → Weather + Headline Overlays) gates the weather/headline/battery overlays for Telegram-received photos too, not just Storage-mode albums — easy to miss since EPDGZ is the recommended default output format for both
- fix: a genuinely new Telegram photo saved as EPDGZ (the recommended default format) never got weather/headline/battery overlays at all, *even with the toggle above enabled* — its display path took a shortcut straight to the panel that bypassed overlay compositing entirely, unconditionally, regardless of any setting; only the Telegram-mode fallback-to-album-rotation path (a different code path) applied overlays correctly. Now routed through the same overlay pipeline Storage-mode's own EPDGZ album images already use
- fix: "Show capture date as caption when a photo has none" was effectively dead code for Telegram photos in normal operation — it checked EXIF at *display* time, by which point the photo had already been converted to PNG/EPDGZ (neither carries EXIF); now read from the original JPEG immediately after download, before conversion
- feat: the same capture-date caption now also applies to Storage/Auto-Rotate album images (including Telegram-mode's own fallback picture) — `process-cli` writes a new `<name>.capture.json` sidecar whenever a source photo has an EXIF capture date (the original is otherwise never kept on-device for this ingestion path, so this is the only point it can be captured at all), which the firmware reads back and draws as a caption, gated on the same setting. Not yet supported for Web UI album uploads (converted entirely client-side, no EXIF extraction there)

**Web UI**:
- feat: display history with a reset button (Settings → Auto Rotate) — random rotation cycles through every image once before repeating, persisted across reboots
- feat: toggleable thumbnail loading in the gallery (was unconditionally slowing down the HTTP server)
- feat: battery history tab (hand-rolled SVG chart) with an estimated days-remaining-until-20% figure, plus a reset button
- feat: real top-level tabs (Gallery / Settings / Battery History / Updates) instead of one long stacked page
- feat: on-display error banner, testable on demand via a Web UI button
- feat: WiFi TX-power cap for Waveshare PhotoPainter battery-brownout mitigation, now user-toggleable (default on)
- feat: opt-in checkbox to include credentials (Telegram bot token, AI API keys, access token, custom auth header) in an exported config backup — off by default since an export is a plaintext JSON file; WiFi password and Calendar/ToDo URLs can never be included since the device never returns them at all
- fix: three Settings-panel actions (save settings, save palette, factory reset) referenced the wrong variable name in their error handler, throwing an unhandled `ReferenceError` on any real failure instead of surfacing it

**Stability fixes**:
- fix: resolve battery-wake stability issues on Waveshare PhotoPainter — a dynamic-frequency-scaling/WiFi-interrupt race and a main-task stack overflow, both confirmed via on-device coredump
- fix: WiFi TX-power cap was gated on USB being disconnected, backwards from the actual Waveshare PMIC brownout condition (USB **and** battery connected together) — now gated on battery presence instead
- fix: several task stack-overflow crashes in the Telegram/rotation pipeline (button task, deep-sleep wake, HTTP `/api/rotate`), each confirmed via live coredump and moved off the shared main-task stack where possible
- fix: `album_manager_delete_album()` failed to delete an album containing a subdirectory
- fix: `build.py` couldn't find `idf.py` on a standard Windows ESP-IDF PowerShell install
- fix: the WiFi captive-portal setup page's network scan could return zero SSIDs — the AP→APSTA mode switch hadn't actually settled before the scan started; added a short settle delay plus an automatic retry on the setup page
- fix: unsynchronized concurrent access to shared in-memory state in the display-history, per-album-enabled, and OTA periodic-check managers, each now mutex-protected (or, for the album-list read path, snapshotted under a lock before parsing) against a genuine race between an HTTP request and a background task
- security: `agenda_todo_url` was logged/persisted in a way that could leak it in plaintext; config-exporting a device now also strips the Telegram bot token, AI API keys, access token, and custom auth header in addition to the fields already excluded (WiFi password, Calendar/ToDo URLs), unless the new opt-in checkbox above is used
- fix (process-cli): the `/image` endpoint accepted client-supplied dimension headers without an upper bound, unlike the already-clamped `/thumbnail` endpoint — now clamped identically to prevent an oversized native canvas allocation
- fix (process-cli): the face-crop engine's final pixel-rounding step could push a crop rectangle up to 1px past the source image's edge after independently rounding x/y/w/h — now re-clamped against the image bounds after rounding

**Agenda Mode** ([colors doc](docs/AGENDA_COLORS.html)):
- feat: full-screen ToDo (todo.txt-format URL) + Calendar (up to two ICS/iCal URLs, merged and sorted together) mode on its own independent cron schedule, rendering directly to the panel and skipping the normal photo pipeline entirely for that wake
- feat: per-element ToDo coloring (priority, `+project`/`@context` tags, due-date urgency, each colored independently) and per-calendar-source coloring (Calendar A/B get their own color); day-grouped Calendar view with a divider per day, correctly showing multi-day events under every day they span
- feat: configurable shared background (white/black/any hardware-supported color) with automatic fallback if a text color would otherwise match it; landscape layout choice between stacked and side-by-side ToDo/Calendar columns
- feat: adjustable Calendar lookahead window (1-3 days, Web UI: Agenda settings)
- feat: full per-role Web UI color picker for every ToDo/Calendar color (Spectra6/color boards)
- feat: opt-in weather forecast annotation on each Calendar day divider (e.g. "Fri 11. [18/25 cloudy]"), reusing the same location/provider settings as the existing photo weather overlay, independent toggle
- fix: a Calendar event's `VALARM` reminder block could overwrite the real event's title if the alarm itself carried its own `SUMMARY`
- fix: TLS fetch failure against calendars whose certificate chain terminates at a cross-signed root (affects Google Calendar's current chain) — enabled cross-signed root verification in the mbedTLS certificate bundle
- fix: a batched Agenda-settings save could race with a concurrent NVS write and drop part of the update; saved as a single atomic batch now

**Reliability & infrastructure**:
- feat: DNS backup/fallback servers (Cloudflare `1.1.1.1`, Google `8.8.8.8`) now populate lwIP's built-in multi-server fallback slots, previously left empty — a single flaky or unreachable DNS server (typically the router's own, handed out via DHCP) could otherwise fail to resolve *any* hostname (weather, Telegram, headlines alike) for a whole wake cycle with no automatic recovery
- feat: "no internet" error banner — tracks consecutive wake cycles where WiFi connected fine but every internet-dependent request (weather, headlines, Telegram, Home Assistant, URL-mode fetch) still failed anyway (e.g. a transient DNS outage); shares the same threshold/toggle/counter as the existing WiFi-connect-failure banner, since both are just flavors of "no usable internet this wake"
- fix: debug-log flush ran *after* the SD card was unmounted during deep-sleep entry, racing the log writer task against the unmount and crashing on the next boot's flush attempt — reordered to flush first
- fix: OTA update-check parsed GitHub's release API response assuming a fixed `Content-Length`, which fails against GitHub's actual chunked-transfer responses (`"Invalid content length: 0"`) — switched to the existing streaming HTTP client already used elsewhere
- fix: OTA/update-check pointed at upstream's GitHub repo instead of this fork's — an automatic update would have silently replaced this fork's firmware with vanilla upstream
- feat: own CI/release pipeline — 7-board build matrix, automatic draft releases with per-board flashable binaries, GitHub Pages web flasher kept in sync with the latest published release
- feat: `v218.x.y` versioning — the major version permanently encodes the upstream base release (`v2.18.0`) this fork tracks; minor/patch are this fork's own release counter

## Roadmap

Planned for upcoming work on this fork:

- ~~Update config backup/restore to cover the new settings and parameters added so far~~ — done, config export/import now also covers per-album enable/disable state
- ~~Publish GitHub Releases for this fork (currently only buildable from source)~~ — done, own CI/release pipeline with per-board flashable binaries
- ~~Adapt the OTA update mechanism, which still points at the upstream project's release feed~~ — done, OTA now points at this fork's own repo and correctly handles GitHub's chunked API responses
- ~~Display delta updates instead of a full refresh~~ — investigated, not possible on the required `waveshare_photopainter_73` board (or any other color board this project targets): 6-color e-paper panels have no partial-refresh mode at the protocol or physical level. See [docs/OVERLAYS.md → Why there's no partial-refresh ("delta update") mode](docs/OVERLAYS.md#why-theres-no-partial-refresh-delta-update-mode) for the full explanation, including why the grayscale boards' IT8951 controller (which does support it) isn't pursued as a partial fix either.
- Performance/resource-usage optimization — album scanning/management via lightweight index files (txt/JSON) instead of repeated directory walks, and a configurable wake-cycle time budget (e.g. "spend at most 20 seconds on network activity, then go back to sleep"), including capping how much of a wake cycle a burst of new Telegram messages can consume
- Document the recommended course of action when a device's orientation is changed between landscape and portrait after the fact, since `process-cli`'s rendered Cover/Fit variants and face-crop metadata are generated for one specific target orientation and don't automatically adapt to a later change
- Agenda mode: multi-line ToDo/Calendar entries with word-wrap and a leading bullet character, as a selectable display option (currently every row is truncated to one physical line)
- Agenda mode: incremental/streaming ICS parsing, so the Calendar column's response-size cap doesn't need to keep being raised as a linked calendar's exported file grows over time (currently a flat 2MB buffer)

## License

This project is based on the ESP32-S3-PhotoPainter sample code. Please refer to the original project for licensing information.

## Credits

- Upstream project: [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe)
- Original PhotoPainter sample: Waveshare ESP32-S3-PhotoPainter
- E-paper drivers: Waveshare
- ESP-IDF: Espressif Systems
