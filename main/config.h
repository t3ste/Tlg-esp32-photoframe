#ifndef CONFIG_H
#define CONFIG_H

#include <driver/gpio.h>

// Uncomment to debug deep sleep wake
// #define DEBUG_DEEP_SLEEP_WAKE

typedef enum {
    ROTATION_MODE_STORAGE = 0,
    ROTATION_MODE_URL = 1,
    ROTATION_MODE_TELEGRAM = 2
} rotation_mode_t;

typedef enum { SD_ROTATION_RANDOM = 0, SD_ROTATION_SEQUENTIAL = 1 } sd_rotation_mode_t;

typedef enum {
    DISPLAY_ORIENTATION_LANDSCAPE = 0,
    DISPLAY_ORIENTATION_PORTRAIT = 1
} display_orientation_t;

// IP configuration mode (#43): DHCP (default) or a static address. The DNS
// override is independent — it applies in both modes (empty = automatic).
typedef enum { IP_MODE_DHCP = 0, IP_MODE_STATIC = 1 } ip_mode_t;

#define IP_ADDR_STR_MAX_LEN 16  // dotted IPv4 + NUL

#define DEVICE_NAME_MAX_LEN 64
#define WIFI_SSID_MAX_LEN 32
#define WIFI_PASS_MAX_LEN 64
#define IMAGE_URL_MAX_LEN 256
#define HA_URL_MAX_LEN 256
#define ROTATION_MODE_MAX_LEN 16
#define TIMEZONE_MAX_LEN 64
#define NTP_SERVER_MAX_LEN 128
#define ACCESS_TOKEN_MAX_LEN 512
#define HTTP_HEADER_KEY_MAX_LEN 64
#define HTTP_HEADER_VALUE_MAX_LEN 512
#define CA_CERT_MAX_LEN 4096
#define HTTP_ETAG_MAX_LEN 128
#define TELEGRAM_BOT_TOKEN_MAX_LEN 128
#define TELEGRAM_CHAT_ID_MAX_LEN 32

#define DEFAULT_DEVICE_NAME "PhotoFrame"
#define DEFAULT_WIFI_SSID "PhotoFrame"
#define DEFAULT_WIFI_PASSWORD "photoframe123"
#define DEFAULT_IMAGE_URL "https://loremflickr.com/800/480"
#define DEFAULT_HA_URL ""
#define DEFAULT_TELEGRAM_BOT_TOKEN ""
#define DEFAULT_TELEGRAM_CHAT_ID ""
#define DEFAULT_TIMEZONE "UTC0"
#define DEFAULT_NTP_SERVER "pool.ntp.org"

#define DEFAULT_ALBUM_NAME "Default"

#include "board_hal.h"

// Overridable so host tests can point the .current.* scheme at a local dir
#ifndef FS_MOUNT_POINT
#define FS_MOUNT_POINT "/storage"
#endif

#define IMAGE_DIRECTORY FS_MOUNT_POINT "/images"
#define DOWNLOAD_DIRECTORY IMAGE_DIRECTORY "/Downloads"
#define TELEGRAM_DOWNLOAD_DIRECTORY IMAGE_DIRECTORY "/Telegram"
// Optional archive of Telegram photos exactly as received, before e-paper
// processing (dithering/palette quantization) overwrites them. Not a
// subdirectory album manager/rotation ever look inside (they only enumerate
// top-level album dirs and DT_REG files directly within them), so it never
// shows up in the gallery or rotation. Opt-in, off by default.
#define TELEGRAM_ORIGINALS_DIRECTORY TELEGRAM_DOWNLOAD_DIRECTORY "/Originals"

#define CURRENT_UPLOAD_PATH FS_MOUNT_POINT "/.current.tmp"
#define CURRENT_JPG_PATH FS_MOUNT_POINT "/.current.jpg"
#define CURRENT_BMP_PATH FS_MOUNT_POINT "/.current.bmp"
#define CURRENT_PNG_PATH FS_MOUNT_POINT "/.current.png"
#define CURRENT_EPD_PATH FS_MOUNT_POINT "/.current.epdgz"
#define CURRENT_IMAGE_LINK FS_MOUNT_POINT "/.current.lnk"
#define TELEGRAM_THUMBNAIL_MAX_DIMENSION 300
#define CURRENT_CALIBRATION_PATH FS_MOUNT_POINT "/.calibration.png"
// Scratch copy used to composite the weather/headline overlay bar onto the
// image a rotation is about to show, without ever mutating the saved album
// file (its content is only valid for the current wake). Deliberately not
// CURRENT_PNG_PATH - that's already a shared scratch target written by the
// Telegram display path and the error overlay; reusing it here risks a
// same-cycle collision when the rotation's own source is CURRENT_PNG_PATH.
#define CURRENT_OVERLAY_PNG_PATH FS_MOUNT_POINT "/.overlay.png"

// Display-history file (one shown image's full path per line) - lets random
// rotation and the Telegram fallback rotation cycle through every image once
// before repeating. See history_manager.[ch].
#define DISPLAY_HISTORY_PATH FS_MOUNT_POINT "/.display_history"

// Battery history log (one "<unix_ts>,<percent>,<charging 0|1>" line per
// recorded reading, appended once per successfully displayed image). See
// battery_history.[ch]. Cleared automatically on a fresh full charge or
// after BATTERY_HISTORY_MAX_AGE_DAYS, whichever comes first.
#define BATTERY_HISTORY_PATH FS_MOUNT_POINT "/.battery_history"
#define BATTERY_HISTORY_RESET_PERCENT 95
#define BATTERY_HISTORY_MAX_AGE_DAYS 180
#define BATTERY_HISTORY_TARGET_PERCENT 20

#ifdef DEBUG_DEEP_SLEEP_WAKE
#define AUTO_SLEEP_TIMEOUT_SEC 60
#else
#define AUTO_SLEEP_TIMEOUT_SEC 120
#endif

// Longer auto-sleep window during out-of-box setup (captive-portal
// provisioning), so the user has time to scan the QR code and configure WiFi
// via the app before the device sleeps.
#define OOBE_AUTO_SLEEP_TIMEOUT_SEC 600

// If a timer wake turns out to be early (RTC drift revealed by external RTC
// restore or NTP sync), rotate anyway when the scheduled time is at most this
// close; otherwise go back to sleep until the scheduled time. Must stay
// shorter than the time a rotation takes end-to-end: a rotation that starts
// within tolerance then finishes past its scheduled minute, so the next
// wake-up computation lands on the following scheduled time instead of
// re-firing the one that was just serviced.
#define EARLY_WAKE_TOLERANCE_SEC 5

// Upper bound on how long a server may ask us to stay awake after rotating (via
// the X-Post-Rotate-Wait-Sec image-response header) so it can pull our config.
// Caps a misbehaving/hostile server from keeping the frame awake and draining
// the battery.
#define POST_ROTATE_WAIT_MAX_SEC 30

// How long HA-configured frames keep the HTTP server up after rotating so a
// late config push can land. A server-requested post-rotate wait can extend it.
#define HA_CONFIG_WINDOW_SEC 10

// Default rotation schedule for fresh / factory-reset devices: every 12 hours.
// Simplified 3-field cron: "minute hour day-of-week".
#define DEFAULT_ROTATE_CRON "0 */12 *"
#define MAX_CRON_RULES 7
#define CRON_RULE_MAX_LEN 64

// WiFi
#define NVS_WIFI_SSID_KEY "wifi_ssid"
#define NVS_WIFI_PASS_KEY "wifi_pass"

// General
#define NVS_NAMESPACE "photoframe"
#define NVS_SETUP_COMPLETE_KEY "setup_complete"
#define NVS_DEVICE_NAME_KEY "device_name"
#define NVS_TIMEZONE_KEY "timezone"
#define NVS_DISPLAY_ORIENTATION_KEY "disp_orient"
#define NVS_DISPLAY_ROTATION_DEG_KEY "disp_rot_deg"

// Advanced network settings (collapsed section in the UI): custom NTP server,
// static IP instead of DHCP, and DNS override (#43)
#define NVS_NTP_SERVER_KEY "ntp_server"
#define NVS_IP_MODE_KEY "ip_mode"
#define NVS_STATIC_IP_KEY "static_ip"
#define NVS_STATIC_NETMASK_KEY "static_mask"
#define NVS_STATIC_GATEWAY_KEY "static_gw"
#define NVS_DNS_SERVER_KEY "dns_server"

// Auto Rotate
#define NVS_AUTO_ROTATE_KEY "auto_rotate"
#define NVS_ROTATE_CRON_KEY "rotate_cron"
#define NVS_ROTATE_INTERVAL_KEY "rotate_int"  // legacy: read once to migrate to cron
#define NVS_ROTATION_MODE_KEY "rotation_mode"
#define NVS_SLEEP_SCHEDULE_ENABLED_KEY "sleep_sched_en"
#define NVS_SLEEP_SCHEDULE_START_KEY "sleep_start"
#define NVS_SLEEP_SCHEDULE_END_KEY "sleep_end"

// Auto Rotate - SDCard
#define NVS_SD_ROTATION_MODE_KEY "sd_rot_mode"
#define NVS_LAST_INDEX_KEY "last_idx"
#define NVS_ENABLED_ALBUMS_KEY "enabled_albums"

// Auto Rotate - URL
#define NVS_IMAGE_URL_KEY "image_url"
#define NVS_CA_CERT_KEY "ca_cert"
#define NVS_ACCESS_TOKEN_KEY "access_token"
#define NVS_HTTP_HEADER_KEY_KEY "http_hdr_key"
#define NVS_HTTP_HEADER_VALUE_KEY "http_hdr_val"
#define NVS_SAVE_DOWNLOADED_KEY "save_dl"
#define NVS_IMAGE_ETAG_KEY "image_etag"
#define NVS_LAST_FETCH_ERROR_KEY "last_fetch_err"

// Power
#define NVS_DEEP_SLEEP_KEY "deep_sleep"

// Debugging
#define NVS_DEBUG_LOG_KEY "debug_log"

// Home Assistant
#define NVS_HA_URL_KEY "ha_url"

// Telegram Bot
#define NVS_TELEGRAM_BOT_TOKEN_KEY "tg_bot_token"
#define NVS_TELEGRAM_CHAT_ID_KEY "tg_chat_id"
#define NVS_TELEGRAM_LAST_UPDATE_ID_KEY "tg_last_upd_id"
// Orientation-pairing: combine two mismatched-orientation Telegram photos
// (e.g. two portrait shots on a landscape frame) into one image instead of
// ever showing one alone. Every mismatched image that arrives is queued here
// (not just one) so nothing is lost if several arrive before a partner shows
// up; persisted across deep sleep (RAM/files don't survive on MemFS-only
// boards, NVS does).
#define NVS_TELEGRAM_PAIRING_KEY "tg_pairing"
#define NVS_TELEGRAM_PENDING_LIST_KEY "tg_pend_list"
#define TELEGRAM_MAX_PENDING_IMAGES 6
#define NVS_TELEGRAM_LOW_BATT_WARNED_KEY "tg_low_batt"
// Wake-up status ping (SSID/IP/battery/wake reason/rotation schedule) sent to
// Telegram every poll, even with no new updates - opt-in, off by default.
#define NVS_TELEGRAM_WAKE_NOTIFY_KEY "tg_wake_notify"

// Home Assistant
#define NVS_HA_ENABLED_KEY "ha_enabled"

// On-display error overlay for persistent failures (e.g. repeated WiFi
// connect failure on a scheduled wake) - opt-in, off by default.
#define NVS_ERROR_OVERLAY_ENABLED_KEY "err_overlay_en"
#define NVS_WIFI_FAIL_COUNT_KEY "wifi_fail_cnt"
#define WIFI_FAIL_OVERLAY_THRESHOLD 3

// WiFi performance mode: when enabled (default), the existing tiered policy
// (power_manager's sleep_timer_task) grants full-RX/low-latency WiFi during
// interactive wakes or USB power. When disabled, WiFi power-save always stays
// on regardless of that policy, trading web UI responsiveness for lower draw.
#define NVS_WIFI_PERF_MODE_ENABLED_KEY "wifi_perf_mode"

// Orientation-pairing during normal (non-Telegram) auto-rotation: when the
// randomly-picked next image doesn't match the panel's orientation, look for
// another mismatched image in the active album(s) and combine them instead
// of showing one letterboxed. Opt-in, off by default. Random rotation mode
// only - sequential mode's deterministic index cursor is left untouched.
#define NVS_ROTATION_PAIRING_ENABLED_KEY "rot_pairing_en"

// When a Telegram-mode wake falls back to normal album rotation (no new
// Telegram image this cycle), send a thumbnail of whatever got displayed to
// the chat, so it stays visible what the frame is showing even without a
// push. Opt-in, off by default.
#define NVS_TELEGRAM_ROTATION_NOTIFY_KEY "tg_rot_notify"

// Keep a copy of each Telegram photo exactly as received (pre-processing) in
// TELEGRAM_ORIGINALS_DIRECTORY. Opt-in, off by default.
#define NVS_TELEGRAM_KEEP_ORIGINALS_KEY "tg_keep_orig"

// Weather + headline overlays: composited as a text bar across the TOP of
// whatever image a rotation is about to show (see CURRENT_OVERLAY_PNG_PATH) -
// on-device alternative to esp32-photoframe-server's weather overlay, no
// companion server required. Both opt-in, off by default, independently
// togglable. Refreshed opportunistically on whatever wake/rotation cadence
// the user's own cron schedule already produces - no separate wake timer.
#define NVS_WEATHER_OVERLAY_ENABLED_KEY "wthr_overlay_en"
// Free-text location name (e.g. "Berlin"), geocoded once via Open-Meteo's
// free geocoding API; the resolved coordinates are cached in
// NVS_WEATHER_LAT_KEY/NVS_WEATHER_LON_KEY (re-geocoded only when this name
// changes, tracked via NVS_WEATHER_GEOCODED_NAME_KEY) to avoid repeating that
// round trip every wake. Leave lat/lon set manually instead to skip
// geocoding entirely.
#define NVS_WEATHER_LOCATION_NAME_KEY "wthr_loc_name"
#define NVS_WEATHER_LAT_KEY "wthr_lat"
#define NVS_WEATHER_LON_KEY "wthr_lon"
#define NVS_WEATHER_GEOCODED_NAME_KEY "wthr_geo_name"
#define WEATHER_LOCATION_NAME_MAX_LEN 64
#define WEATHER_LATLON_MAX_LEN 16

// Which weather data source to use. All three are free/keyless; wttr.in and
// yr.no exist as user-selectable alternatives to fall back to manually if
// Open-Meteo doesn't work reliably for a given network/region - there is no
// automatic runtime failover between them (predictable behavior over silent
// retries against a different provider). Same resolved lat/lon (see
// resolve_lat_lon() in weather.c) is used regardless of provider.
#define NVS_WEATHER_PROVIDER_KEY "wthr_provider"
#define WEATHER_PROVIDER_MAX_LEN 16
#define WEATHER_PROVIDER_OPEN_METEO "open-meteo"
#define WEATHER_PROVIDER_WTTR_IN "wttr.in"
#define WEATHER_PROVIDER_YR_NO "yr.no"
#define WEATHER_PROVIDER_DEFAULT WEATHER_PROVIDER_OPEN_METEO

#define NVS_HEADLINES_OVERLAY_ENABLED_KEY "hdln_overlay_en"
// Any RSS/Atom feed URL (Tagesschau, Spiegel, BBC, ...) - no API key, no
// rate limit, works with essentially any news outlet.
#define NVS_HEADLINES_RSS_URL_KEY "hdln_rss_url"
#define NVS_HEADLINES_COUNT_KEY "hdln_count"  // 1-3, default 3
#define HEADLINES_RSS_URL_MAX_LEN 256
#define HEADLINES_COUNT_DEFAULT 3
#define HEADLINES_COUNT_MIN 1
#define HEADLINES_COUNT_MAX 3
// When headlines_count == 1, optionally word-wrap that single headline
// across this many overlay lines instead of hard-truncating it to one line.
// 1 (default) = unchanged single-line-with-ellipsis behavior.
#define NVS_HEADLINES_WRAP_LINES_KEY "hdln_wrap_ln"
#define HEADLINES_WRAP_LINES_DEFAULT 1
#define HEADLINES_WRAP_LINES_MIN 1
#define HEADLINES_WRAP_LINES_MAX 3

// Overlay bar appearance/language, shared by both the weather and headlines
// overlay content. Colors default to the existing look (black bar, white
// text); swapped when enabled. Language selects both the weather condition
// text and weekday abbreviations ("en": Mon..Sun: default; "de": Mo..So).
#define NVS_OVERLAY_INVERT_COLORS_KEY "ovl_invert_col"
#define NVS_OVERLAY_LANGUAGE_KEY "ovl_lang"
#define OVERLAY_LANGUAGE_MAX_LEN 4
#define OVERLAY_LANGUAGE_DEFAULT "en"
// Also apply the invert-colors setting above to Telegram photo captions
// (both a plain caption and an orientation-paired composite's) - a separate
// opt-in so turning on overlay color inversion doesn't silently change the
// look of every Telegram caption too. Off by default (fixed black
// bar/white text, as before this setting existed).
#define NVS_CAPTION_INVERT_COLORS_KEY "cap_invert_col"

// Falls back to the photo's own EXIF "DateTimeOriginal" (capture date) as a
// caption, for a Telegram photo received with no caption text. Off by
// default. Only meaningful for Telegram - the original JPEG (with EXIF
// intact) never reaches the device for Storage/album uploads, which are
// processed client-side in the browser before upload (see exif_reader.h).
#define NVS_SHOW_EXIF_DATETIME_KEY "exif_dt_en"

// 46 characters/line is comfortably below what any single condition+temps
// segment needs (see docs/OVERLAYS.md), but three of them on ONE line can
// still overflow for longer condition words even after abbreviation - this
// gives each day its own line instead. Only takes effect while the
// headlines overlay is disabled (there isn't room for 3 weather lines AND
// headline lines together); weather always renders as one combined line
// whenever headlines are also enabled, regardless of this setting.
#define NVS_WEATHER_MULTILINE_KEY "wthr_multiline"

// WiFi association draws a brief high-current TX burst; whenever a battery
// is in the loop (battery-only, or USB+battery together - see
// wifi_manager.c), capping TX power lowers that peak (at some cost to
// range). Value is in units of 0.25 dBm (esp_wifi_set_max_tx_power()
// convention) - 60 = 15 dBm, versus the factory default of up to ~20 dBm (80).
#define WIFI_BATTERY_MAX_TX_POWER_QUARTER_DBM 60
// User-facing on/off switch for the cap above. Defaults to enabled (the
// PhotoPainter's original AXP2101 PMIC is the board this mitigates); boards
// without a marginal battery rail, or users who'd rather trade the small
// brownout-risk reduction back for full WiFi range, can turn it off.
#define NVS_WIFI_TX_POWER_CAP_ENABLED_KEY "tx_pwr_cap_en"

// AI API Keys (for webapp client use)
#define AI_API_KEY_MAX_LEN 256
#define NVS_OPENAI_API_KEY_KEY "openai_key"
#define NVS_GOOGLE_API_KEY_KEY "google_key"

// OTA Configuration
#define GITHUB_API_URL "https://api.github.com/repos/aitjcize/esp32-photoframe/releases/latest"
#define OTA_CHECK_INTERVAL_MS (24 * 60 * 60 * 1000)  // 24 hours
#define NVS_OTA_CHECK_ENABLED_KEY "ota_check_en"

// Telegram Bot API
// Note: the real Telegram Bot API base is "https://api.telegram.org/bot<TOKEN>/<METHOD>"
// (not "https://telegram.org<TOKEN>/..."). TELEGRAM_API_HOST + TELEGRAM_API_BASE_FMT
// build that URL at runtime once the token is known.
#define TELEGRAM_API_HOST "api.telegram.org"
#define TELEGRAM_API_BASE_FMT "https://api.telegram.org/bot%s/%s"
#define TELEGRAM_POLL_TIMEOUT_SEC 10    // long-poll timeout passed to getUpdates
#define TELEGRAM_HTTP_TIMEOUT_MS 15000  // per-request HTTP timeout
#define TELEGRAM_MAX_UPDATES_PER_POLL 50
#define TELEGRAM_RESET_COMMAND "/telegram_reset"
#define TELEGRAM_MAX_PENDING_COMMANDS 8
#define TELEGRAM_COMMAND_MAX_LEN 128
#define TELEGRAM_CAPTION_MAX_LEN 128
#define TELEGRAM_FILE_ID_MAX_LEN 128

#endif