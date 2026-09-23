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

// How the Calendar column of Agenda mode displays a multi-day event - see
// NVS_AGENDA_CAL_COMPACT_KEY below and agenda_renderer.c's
// event_total_days()/event_day_index(). Values are stored as-is in NVS, so
// the numbering must stay stable across firmware versions.
typedef enum {
    AGENDA_MULTIDAY_REPEAT = 0,   // repeated under every day it spans, no prefix
    AGENDA_MULTIDAY_COMPACT = 1,  // shown once, on its first visible day, "N/M: " prefix
    AGENDA_MULTIDAY_REPEAT_NUMBERED =
        2,  // repeated under every day, each with its own "N/M: " prefix
} agenda_multiday_mode_t;

// How a timed Calendar event's time is displayed - see
// NVS_AGENDA_CAL_SHOW_DURATION_KEY below and build_event_line() in
// agenda_renderer.c. Values are stored as-is in NVS, so the numbering must
// stay stable across firmware versions. All-day events are never affected
// by any of these modes - they already show no time at all.
typedef enum {
    AGENDA_TIME_DISPLAY_OFF = 0,       // just the start time, e.g. "08:15 Kaffee trinken"
    AGENDA_TIME_DISPLAY_DURATION = 1,  // "08:15 [45m] Kaffee trinken" - compact, but longer
                                       // than a range once the event runs over an hour
                                       // ("08:00 [1h30m]" vs. "08:00-09:30")
    AGENDA_TIME_DISPLAY_RANGE = 2,     // "08:15-09:00 Kaffee trinken"
} agenda_time_display_mode_t;

// Calendar-only-fullscreen layout (main/agenda_renderer.c). GRID_A/GRID_B
// only actually take effect when the Calendar column is shown alone (no
// ToDo) - agenda_renderer_render() falls back to LIST otherwise. Values are
// stored as-is in NVS, so the numbering must stay stable across firmware
// versions.
typedef enum {
    AGENDA_CAL_LAYOUT_LIST = 0,    // existing single-column day list (1-3 days)
    AGENDA_CAL_LAYOUT_GRID_A = 1,  // 7-day grid, today = full-width row
    AGENDA_CAL_LAYOUT_GRID_B = 2,  // 7-day grid, today = double-height half-width cell
} agenda_cal_layout_mode_t;

// Optional 2-group rotation/"shift" coloring for the 7-day grid layouts
// (e.g. a 2-2-3 custody-style schedule) - see agenda_shift_group_for_day()
// in agenda_renderer.c. Segment lengths always sum to one 7-day "half";
// which group holds the first segment flips every other half, giving a
// real alternating-fortnightly pattern (the classic "2-2-3" convention),
// anchored at NVS_AGENDA_SHIFT_START_KEY. Values are stored as-is in NVS.
typedef enum {
    AGENDA_SHIFT_MODEL_NONE = 0,
    AGENDA_SHIFT_MODEL_2_2_3 = 1,      // 2/2/3-day segments
    AGENDA_SHIFT_MODEL_WEEK_WEEK = 2,  // 7-day segment (whole week alternates)
    AGENDA_SHIFT_MODEL_3_4 = 3,        // 3/4-day segments
} agenda_shift_model_t;

// Master mode for the Chimes speaker feature (see board_hal_has_speaker() /
// board_hal_play_beep_pattern() and main/chime.c). Values are stored as-is
// in NVS, so the numbering must stay stable across firmware versions.
typedef enum {
    CHIME_SPEAKER_OFF = 0,                // never play, regardless of per-event flags
    CHIME_SPEAKER_BATTERY_AND_MAINS = 1,  // play regardless of power source
    CHIME_SPEAKER_MAINS_ONLY = 2,         // play only while USB/mains powered
} chime_speaker_mode_t;

// One entry per Chimes event hook - see chime_play_if_enabled() in
// main/chime.c for the full list of call sites. Order is NVS-key-agnostic
// (each has its own bool key, see NVS_CHIME_EVENT_*_KEY below), so this
// enum's numbering can change freely.
typedef enum {
    CHIME_EVENT_ROTATION = 0,
    CHIME_EVENT_TELEGRAM_PHOTO,
    CHIME_EVENT_LOW_BATTERY,
    CHIME_EVENT_WIFI_REPROVISION,
    CHIME_EVENT_AGENDA_DUE,
    CHIME_EVENT_OTA_SUCCESS,
    CHIME_EVENT_CRITICAL_ERROR,
    CHIME_EVENT_COUNT,  // not a real event - array size for chime.c's fire-count cap
} chime_event_t;

// Preset room profile the SHTC3 climate sensor is classified against - see
// climate_classify_temperature()/climate_classify_humidity() in climate.c.
// Values are stored as-is in NVS, so the numbering must stay stable.
typedef enum {
    CLIMATE_ROOM_LIVING_ROOM = 0,
    CLIMATE_ROOM_BEDROOM = 1,
    CLIMATE_ROOM_BATHROOM = 2,
    CLIMATE_ROOM_KITCHEN = 3,
    CLIMATE_ROOM_BASEMENT = 4,
} climate_room_type_t;

// Display-only unit for the climate feature - classification thresholds are
// always defined in Celsius (climate.c); this only affects how a reading is
// formatted for the overlay badge, Agenda header, and Web UI. Values are
// stored as-is in NVS.
typedef enum {
    CLIMATE_UNIT_CELSIUS = 0,
    CLIMATE_UNIT_FAHRENHEIT = 1,
} climate_temp_unit_t;

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
// Same idea, used when the source being overlaid is EPDGZ instead of PNG
// (see NVS_OVERLAY_EPDGZ_ENABLED_KEY below) - a separate constant so the
// scratch file's extension always matches its actual content.
#define CURRENT_OVERLAY_EPDGZ_PATH FS_MOUNT_POINT "/.overlay.epdgz"

// Agenda (ToDo + Calendar) full-screen render scratch file - always PNG,
// no EPDGZ variant needed (this is a from-scratch canvas, never decoded
// back, so there's no "matches the source format" concern like the
// overlay paths above).
#define AGENDA_OUTPUT_PATH FS_MOUNT_POINT "/.agenda.png"

// Raw-body caches for the ToDo/Calendar sources' conditional GET (see
// NVS_AGENDA_TODO_ETAG_KEY etc. below) - todo.c/calendar_ics.c fall back to
// re-parsing whichever of these is relevant when the server replies 304 Not
// Modified, since the day-relative rendering (due-today coloring, which
// calendar days fall in the lookahead window) still needs to be redone every
// agenda wake even when the source content itself hasn't changed.
#define AGENDA_TODO_CACHE_PATH FS_MOUNT_POINT "/.agenda_todo_cache.txt"
#define AGENDA_CAL_CACHE_PATH FS_MOUNT_POINT "/.agenda_cal_cache.ics"
#define AGENDA_CAL_CACHE_PATH2 FS_MOUNT_POINT "/.agenda_cal_cache2.ics"

// Three extra, user-supplied ICS sources (e.g. holidays/school-holidays/
// special-days feeds) - unlike the two caches above, these are NOT
// refreshed on every agenda wake (see NVS_AGENDA_CAL_C_URL_KEY etc.): the
// file here is only (re)written when the user sets/changes the source URL,
// clicks "refresh now," or uploads a replacement directly. Every agenda
// wake just re-parses whatever's already on disk, no network involved.
#define AGENDA_CAL_CACHE_PATH_C FS_MOUNT_POINT "/.agenda_cal_cache_c.ics"
#define AGENDA_CAL_CACHE_PATH_D FS_MOUNT_POINT "/.agenda_cal_cache_d.ics"
#define AGENDA_CAL_CACHE_PATH_E FS_MOUNT_POINT "/.agenda_cal_cache_e.ics"

// Flat, already-expanded caches for the same three sources (see
// calendar_ics_write_expanded_cache()/_read_expanded_cache()) - lets
// agenda_manager.c avoid re-parsing a potentially large raw .ics file on
// every agenda wake. The raw file above is only re-parsed/re-expanded when
// this cache runs out of upcoming entries (see AGENDA_EXTRA_ICS_EXPAND_DAYS
// below), which for a real feed only happens roughly once a month.
#define AGENDA_CAL_CACHE_PATH_C_FLAT FS_MOUNT_POINT "/.agenda_cal_cache_c_flat.txt"
#define AGENDA_CAL_CACHE_PATH_D_FLAT FS_MOUNT_POINT "/.agenda_cal_cache_d_flat.txt"
#define AGENDA_CAL_CACHE_PATH_E_FLAT FS_MOUNT_POINT "/.agenda_cal_cache_e_flat.txt"
// How far ahead each (re-)expansion looks, in days - wide enough that a
// large source (e.g. a year of holidays/school-holidays) only needs
// re-parsing roughly this often, not on every wake.
#define AGENDA_EXTRA_ICS_EXPAND_DAYS 30

// On-demand thumbnail scratch file for telegram_bot_notify_fallback_image() -
// generated only when the image being reported has no pre-existing ".jpg"
// sidecar (true for any plain Storage/Auto-Rotate album image, since that
// sidecar is otherwise only ever created for images that went through the
// Telegram ingestion pipeline). Named ".jpg" to match what Telegram's
// sendPhoto expects, even though image_processor_make_thumbnail() actually
// writes PNG content - the existing Telegram-ingestion thumbnails already
// do the same, and Telegram's API sniffs actual content, not the extension.
// Deliberately outside every album directory (a stray file inside one would
// otherwise show up as a "new" photo to album_manager/gallery/rotation).
#define TELEGRAM_NOTIFY_THUMB_PATH FS_MOUNT_POINT "/.tg_notify_thumb.jpg"

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

// Climate (SHTC3 temperature/humidity) history log (one
// "<unix_ts>,<temp_c>,<humidity>" line per recorded reading). See
// climate_history.[ch]. No natural "reset" event like a battery recharge, so
// this only ever clears on CLIMATE_HISTORY_MAX_AGE_DAYS or a user-requested
// reset.
#define CLIMATE_HISTORY_PATH FS_MOUNT_POINT "/.climate_history"
#define CLIMATE_HISTORY_MAX_AGE_DAYS 180
// A reading is logged on every wake (main.c) and, while the device stays
// awake continuously, every CLIMATE_ACTIVE_LOG_INTERVAL_SEC (power_manager.c)
// - CLIMATE_LOG_MIN_INTERVAL_SEC is a shared debounce inside
// climate_history_record() itself (persisted in NVS, since deep sleep wipes
// RAM) so neither trigger can log more often than this, even if both fire
// close together.
#define CLIMATE_LOG_MIN_INTERVAL_SEC (5 * 60)
#define CLIMATE_ACTIVE_LOG_INTERVAL_SEC (6 * 60)

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
// Duplicate detection: skip re-downloading/re-displaying a Telegram photo or
// document whose content the device has already received - compared via
// Telegram's own "file_unique_id" (stable for identical file content across
// re-sends/forwards, unlike "file_id" which can change), so no local hashing
// of downloaded bytes is needed. Opt-in, off by default; only the last
// TELEGRAM_DEDUP_MAX_ENTRIES ids are remembered (oldest evicted first),
// persisted across deep sleep the same way the pending-pair list is.
#define NVS_TELEGRAM_DEDUP_ENABLED_KEY "tg_dedup_en"
#define NVS_TELEGRAM_SEEN_IDS_KEY "tg_seen_ids"
#define TELEGRAM_DEDUP_MAX_ENTRIES 30
#define TELEGRAM_UNIQUE_ID_MAX_LEN 40
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

// Extended cold-boot WiFi retry: when enabled, a cold-boot connect failure
// that is NOT a confirmed credential rejection (see
// wifi_manager_last_failure_is_credential_reject()) no longer wipes the
// saved SSID/password after just WIFI_COLD_BOOT_CONNECT_MAX_ATTEMPTS (3,
// main.c) - it instead persists a running attempt count here and reboots to
// try again after a backoff, up to WIFI_COLD_BOOT_EXTENDED_MAX_TOTAL_ATTEMPTS
// (main.c) total attempts across those reboots, so a brief AP-side outage or
// a momentary weak-signal blip doesn't force a full reprovisioning. A
// genuine credential rejection is never affected by this toggle - that still
// wipes after a single attempt either way. Off by default: live-verified
// worst case is up to ~6x the energy use of the default behavior (the frame
// stays fully awake through every retry/reboot instead of reprovisioning
// quickly), so this is opt-in for mains/USB-powered frames rather than a
// new default for every device. Real incident (2026-09-13) that prompted
// this: a cold boot got stuck retrying WIFI_REASON_AUTH_EXPIRE/
// WIFI_REASON_CONNECTION_FAIL (never a real reject reason) right after the
// AP's signal had degraded to -70dBm, and the then-unconditional wipe forced
// an unnecessary reprovisioning even though the password was fine.
#define NVS_WIFI_EXT_RETRY_ENABLED_KEY "wifi_ext_retry"
// Internal only - the running cross-reboot attempt count above. Never
// surfaced via the HTTP API (nothing for a user to usefully do with it).
#define NVS_WIFI_COLDBOOT_FAIL_COUNT_KEY "wifi_cb_fail"

// Whether a cold-boot connect exhaustion (WIFI_COLD_BOOT_CONNECT_MAX_ATTEMPTS,
// or the extended-retry cap above if that's also on) is allowed to wipe the
// saved SSID/password and reboot into provisioning at all. On (default):
// unchanged existing behavior. Off: a genuine credential rejection still
// wipes immediately either way (a wrong password can't fix itself), but a
// non-rejection exhaustion instead keeps the credentials and, if deep sleep
// is enabled, goes to sleep until the next scheduled wake (which retries the
// whole connection sequence fresh) - or, if deep sleep is disabled (USB/
// always-on/Home-Assistant-polled use), just continues the rest of the
// normal boot flow without WiFi this cycle rather than blocking here, since
// every network-touching step past this point either already checks
// wifi_manager_is_connected() first or has its own bounded timeout. Real
// incident (2026-09-19): a device a few meters from a repeater kept hitting
// this exact exhaustion path on WIFI_REASON_AUTH_EXPIRE/CONNECTION_FAIL
// (never a real reject reason) and cycled through repeated wipe ->
// reprovision -> exhaust -> wipe again, needing a fresh manual reprovision
// every time despite the saved credentials being correct the whole time.
#define NVS_WIFI_REPROV_ON_FAIL_KEY "wifi_reprov_en"

// Set during first-time setup (github.com/aitjcize/esp32-photoframe#90) when
// the user picks "use offline, no WiFi network" instead of entering real
// credentials. OR'd into wifi_provisioning_is_provisioned()'s gate so the
// device boots normally instead of looping back into the OOBE AP forever,
// and skips the cold-boot WiFi connect attempts entirely (main.c) since
// there's deliberately nothing to connect to. Does not affect the separate
// on-demand hotspot (wifi_manager_start_ap_hotspot()), which any configured
// device - offline or not - can enter any time via a long BOOT hold.
#define NVS_OFFLINE_MODE_KEY "offline_mode"

// Opt-in second HTTPS listener alongside the always-on plain HTTP one
// (github.com/aitjcize/esp32-photoframe#130) - off by default since it uses
// a per-device self-signed certificate (main/https_cert.c), which every
// browser flags with a click-through warning (no CA can vouch for a device
// with no public hostname). Protects against passive LAN sniffing of the
// session, not an active on-path attacker who ignores that warning. Takes
// effect on the next http_server_init() (boot/reconnect), not live.
#define NVS_HTTPS_ENABLED_KEY "https_enabled"

// Orientation-pairing during normal (non-Telegram) auto-rotation: when the
// randomly-picked next image doesn't match the panel's orientation, look for
// another mismatched image in the active album(s) and combine them instead
// of showing one letterboxed. Opt-in, off by default. Random rotation mode
// only - sequential mode's deterministic index cursor is left untouched.
#define NVS_ROTATION_PAIRING_ENABLED_KEY "rot_pairing_en"

// Cover/Fit pre-rendered variant selection during Storage/SD rotation (see
// docs/FACE_CROP.md): recognizes process-cli's --crop-output both output
// ("<name>.fit.<ext>" next to the original, "<name>.cover.<ext>" in a
// "crop" subdirectory, plus an optional "<name>.facecrop.json" sidecar) and
// picks whichever variant matches the device's own current Cover/Fit
// scale_mode setting, rendering the missing one on-device (once, then
// cached) only when the anchor is a genuine still-undecoded original.
// Opt-in, off by default - purely additive over existing albums either way.
#define NVS_VARIANT_SELECTION_ENABLED_KEY "variant_sel_en"

// When a Telegram-mode wake falls back to normal album rotation (no new
// Telegram image this cycle), send a thumbnail of whatever got displayed to
// the chat, so it stays visible what the frame is showing even without a
// push. Opt-in, off by default.
#define NVS_TELEGRAM_ROTATION_NOTIFY_KEY "tg_rot_notify"
// Whether a Telegram-mode wake with no new image falls back to normal album
// rotation at all (existing behavior, now toggleable). On (default,
// preserves existing behavior): the display still changes every wake, same
// as the non-Telegram rotation modes. Off: the display only ever changes on
// a wake that actually receives a new Telegram image - every other wake
// (timer/button) leaves the current image up unchanged. Independent of
// NVS_TELEGRAM_ROTATION_NOTIFY_KEY above, which only controls whether a
// fallback display change (when this is on) also gets announced to the chat.
#define NVS_TELEGRAM_FALLBACK_ROTATION_ENABLED_KEY "tg_fallback_rot"
// Only consulted while NVS_TELEGRAM_FALLBACK_ROTATION_ENABLED_KEY above is
// off (the restrictive "only ever change display on a genuine new Telegram
// photo" policy) - decides whether a poll that fails outright (Telegram
// unreachable, or the bot not configured at all) is still treated as an
// exception that falls back to normal album rotation (on, default -
// preserves the original pre-toggle behavior, where any poll failure always
// fell back), or is folded into that same "no display change" policy (off).
// Has no effect while the main toggle above is on - that path already always
// falls back on error, unconditionally, as it always has.
#define NVS_TELEGRAM_FALLBACK_ON_ERROR_ENABLED_KEY "tg_fallback_err"

// Keep a copy of each Telegram photo exactly as received (pre-processing) in
// TELEGRAM_ORIGINALS_DIRECTORY. Opt-in, off by default.
#define NVS_TELEGRAM_KEEP_ORIGINALS_KEY "tg_keep_orig"

// Minimizes wake duration and WiFi-on time on an automatic (timer-triggered)
// Telegram-mode wake: fewer WiFi/Telegram HTTP retries before giving up, and
// skips the post-rotation "hold window" that otherwise keeps the device
// awake a bit longer for web UI/HA config sync. Never applies to a manual
// button-triggered wake, which always keeps its full retry budget and hold
// window - a deliberate, permanent escape hatch to reach the web UI even
// while this is on. Opt-in, off by default.
#define NVS_TELEGRAM_POWER_SAVE_ENABLED_KEY "tg_power_save"
// Only consulted while the toggle above is also on. Processes only the
// single newest update in a poll batch (photo or document) and discards
// every other update, message, and "/" command in that batch - permanently,
// since Telegram's getUpdates offset acknowledgment is one-way (nothing
// dropped this way is ever redelivered). Also disables orientation-pairing
// for the surviving image (it always displays alone, never composed with a
// pending partner) and skips the per-photo "saved" confirmation reply. A
// command-only update is never itself discarded by this - see telegram_bot.c.
// Opt-in, off by default.
#define NVS_TELEGRAM_POWER_SAVE_LATEST_ONLY_KEY "tg_ps_latest"
// WiFi connect timeout used on an automatic Telegram-mode wake while power
// save is on, in place of the normal 60s budget - a manual button wake is
// never affected (see NVS_TELEGRAM_POWER_SAVE_ENABLED_KEY above).
#define TELEGRAM_POWER_SAVE_WIFI_TIMEOUT_SEC 15
// Reconnect-attempt budget used in place of the normal 5 (see
// wifi_manager_set_max_retries()) under the same conditions.
#define TELEGRAM_POWER_SAVE_WIFI_MAX_RETRIES 1

// On-device output format for Telegram-ingested photos. EPDGZ is the
// recommended default: it stores the already-resolved 4-bit palette index,
// gzip-compressed, so every future display is a plain gzip-inflate + nibble
// read - no per-pixel RGB->palette re-matching the way reading a "processed"
// PNG back still requires (see GUI_PNGfile.c's read_png_mapped()). PNG
// remains selectable for compatibility/inspection.
#define NVS_TELEGRAM_IMAGE_FORMAT_KEY "tg_img_fmt"
#define TELEGRAM_IMAGE_FORMAT_MAX_LEN 8
#define TELEGRAM_IMAGE_FORMAT_PNG "png"
#define TELEGRAM_IMAGE_FORMAT_EPDGZ "epdgz"
#define TELEGRAM_IMAGE_FORMAT_DEFAULT TELEGRAM_IMAGE_FORMAT_EPDGZ

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

// The provider that actually produced the currently-displayed weather data
// (updated on every successful weather_fetch_forecast(), regardless of
// which of the three providers above is configured) - persisted so
// /status and the wake notification can report it even when queried in a
// later wake than the one that last actually refreshed the overlay. Empty
// until the first successful fetch.
#define NVS_WEATHER_LAST_SOURCE_KEY "wthr_last_src"

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
// Extends the weather/headline overlay to already-rendered EPDGZ album
// images too (Storage/Auto-Rotate's own on-disk display files, typically the
// majority of what's actually on an SD card - see docs/FACE_CROP.md's
// discussion of the same convention). Off by default: applying it means an
// extra decode/redraw/re-encode round-trip per display, not needed for
// anyone who doesn't use weather/headline overlays with Storage mode's own
// pre-rendered EPDGZ files. PNG support needs no toggle (always on, as
// before) - see docs/OVERLAYS.md. BMP is not supported either way - the
// firmware has no BMP decoder (only a one-way PNG->BMP writer for boards
// whose native display format is BMP), so there's no RGB buffer to draw an
// overlay onto.
#define NVS_OVERLAY_EPDGZ_ENABLED_KEY "ovl_epdgz_en"
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

// Render the weather condition as a small icon instead of the spelled-out
// word (e.g. a cloud glyph instead of "cloudy"/"bedeckt") - saves horizontal
// space, the original motivation for NVS_WEATHER_MULTILINE_KEY above too.
// String enum rather than a bool since there are two selectable icon sets,
// not just on/off - "none" (default, unchanged text behavior), "flaticon"
// (InkyPi project's weather icons, Flaticon-licensed - see README.md
// Credits), or "metno" (MET Norway/yr.no's official weathericons, MIT
// licensed - this project already uses yr.no as a weather data source).
// Both sets are pre-baked into main/weather_icons_data.h (generated by
// scripts/generate_weather_icons.py) at the same 16-category index, keyed
// by weather_code_to_icon_id() in weather.c - an unrecognized/invalid value
// here falls back to "none".
#define NVS_WEATHER_ICON_SET_KEY "wthr_iconset"
#define WEATHER_ICON_SET_MAX_LEN 9  // "flaticon\0" is the longest value
#define WEATHER_ICON_SET_DEFAULT "none"

// Traffic-light severity coloring for the icon above (e.g. red for heavy
// rain, green for light rain, blue for snow) instead of the overlay bar's
// plain single foreground color - off by default (monochrome, matching
// every other overlay-bar element). Meaningless with NVS_WEATHER_ICON_SET_KEY
// = "none" (no icon to color) and automatically ignored on grayscale-only
// boards (see board_is_grayscale() in image_processor.c) the same way the
// Climate feature's badge colors already are.
#define NVS_WEATHER_ICON_COLORED_KEY "wthr_colored"

// Small always-on-render corner badge (not a full-width bar, unlike the
// overlays above) shown whenever the battery is below a configurable
// threshold - independent of Telegram/Web UI reachability, so the user
// notices the device needs charging just by looking at the display. Opt-in,
// off by default.
#define NVS_LOW_BATTERY_OVERLAY_ENABLED_KEY "lowbatt_ov_en"
// Percent, e.g. 16 - the badge appears once the battery drops below this.
#define NVS_LOW_BATTERY_OVERLAY_THRESHOLD_KEY "lowbatt_ov_pct"
#define LOW_BATTERY_OVERLAY_THRESHOLD_DEFAULT 16
#define LOW_BATTERY_OVERLAY_THRESHOLD_MIN 1
#define LOW_BATTERY_OVERLAY_THRESHOLD_MAX 50
// Fixed hysteresis gap (not user-configurable, to keep the Web UI to one
// number) - the badge only clears once the battery recovers past
// threshold + this margin, mirroring the existing Telegram low-battery
// warning's own 20/25 (5-point) gap. Default threshold 16 + this margin (4)
// matches the original 16%-show/20%-clear example exactly.
#define LOW_BATTERY_OVERLAY_CLEAR_MARGIN 4
// Internal hysteresis state - NOT a user setting, never exposed to the Web
// UI (same as NVS_TELEGRAM_LOW_BATT_WARNED_KEY). Must be NVS-persisted, not
// just held in memory, since deep sleep reboots the device every wake.
#define NVS_LOW_BATTERY_OVERLAY_ACTIVE_KEY "lowbatt_ov_act"

// Agenda mode (ToDo + Calendar) - a full-screen display mode, NOT a photo
// overlay: whenever a wake matches its own independent schedule below, the
// device renders ToDo/Calendar content instead of a photo for that wake,
// then goes back to sleep. Normal photo auto-rotation is unaffected and
// keeps running on its own separate schedule. See agenda_manager.h.
#define NVS_AGENDA_TODO_ENABLED_KEY "agenda_todo_en"
#define NVS_AGENDA_CAL_ENABLED_KEY "agenda_cal_en"
// A plain todo.txt file, re-validated on every agenda wake via a
// conditional GET (see NVS_AGENDA_TODO_ETAG_KEY below) - see todo.h for the
// parsed grammar.
#define NVS_AGENDA_TODO_URL_KEY "agenda_todo_url"
#define AGENDA_TODO_URL_MAX_LEN 256
// An iCalendar/ICS feed - e.g. a Google Calendar "secret address in iCal
// format" (plain HTTPS GET, no OAuth). Treated like a credential: never
// surfaced via GET /api/config, same write-only treatment as
// NVS_WIFI_PASS_KEY (see config_manager.c).
#define NVS_AGENDA_CAL_URL_KEY "agenda_cal_url"
#define AGENDA_CAL_URL_MAX_LEN 256
// Optional second calendar (e.g. work vs. personal) - merged with the first
// at render time, each colored per its own origin (agenda_renderer.c's
// calendar_source_color()). Same write-only credential treatment as the
// first URL. Only this second feed is disabled if left empty; the first
// remains the only one required to enable Calendar at all.
#define NVS_AGENDA_CAL_URL2_KEY "agenda_cal_url2"
#define AGENDA_CAL_URL2_MAX_LEN 256
// Cached ETag validators for the conditional GET above - same purpose as
// NVS_IMAGE_ETAG_KEY for the rotation image fetch, one per source URL. A
// 304 reply skips the download but not the re-parse: see
// AGENDA_TODO_CACHE_PATH etc. above. Internal only - not a credential (an
// ETag is an opaque cache-validation token, not secret), never surfaced via
// the HTTP API either way, and irrelevant to config export/import (a
// stale/missing value after an import just means the next fetch is
// unconditional).
#define NVS_AGENDA_TODO_ETAG_KEY "agenda_todo_et"
#define NVS_AGENDA_CAL_ETAG_KEY "agenda_cal_et"
#define NVS_AGENDA_CAL_ETAG2_KEY "agenda_cal_et2"
// How many upcoming days (including today) of calendar events to show.
#define NVS_AGENDA_CAL_DAYS_KEY "agenda_cal_days"
#define AGENDA_CAL_DAYS_DEFAULT 2
#define AGENDA_CAL_DAYS_MIN 1
#define AGENDA_CAL_DAYS_MAX 3
// Calendar-only-fullscreen layout - see agenda_cal_layout_mode_t above.
// Only takes effect when the Calendar column is shown alone (no ToDo).
#define NVS_AGENDA_CAL_LAYOUT_KEY "agenda_cal_lay"
// 2-group rotation/"shift" coloring for the 7-day grid layouts (event rows
// only - the day header itself is never shift-colored, see draw_day_cell()
// in agenda_renderer.c for why) - see agenda_shift_model_t above.
#define NVS_AGENDA_SHIFT_MODEL_KEY "agenda_shft_md"
// "YYYY-MM-DD" anchor date - which day the first segment of the first
// (non-flipped) half starts on. Empty = unset, treated as "no coloring"
// even if a model above is selected (fail-soft: an unanchored pattern
// can't be resolved to an actual group).
#define NVS_AGENDA_SHIFT_START_KEY "agenda_shft_dt"
#define AGENDA_SHIFT_START_MAX_LEN 11
// The rotation's single marker color (which of the 2 groups is "marked" on
// any given day still comes from agenda_shift_group_for_day() above - only
// the *color* used to paint that group's days is chosen here) now comes
// from the active color profile's "mark" field instead of a device setting
// - see agenda_color_profile.h. The unmarked group simply keeps the
// profile's plain text/textBg colors, per the user's explicit "a switch
// model only needs one marker color, other days keep their normal
// background" requirement.
// Opt-in: annotates each Calendar day divider with that day's forecast
// (min/max temp + short condition, e.g. "Fr 11. [18/25 cloudy]"), reusing
// the same weather_fetch_forecast() / location / provider settings as the
// existing photo weather overlay - a separate toggle since Agenda mode is
// an independent display path from the photo overlay pipeline, not because
// the underlying weather data or config differs. WEATHER_FORECAST_DAYS is
// 3, so a 4th calendar day (agenda_cal_days can reach into a 4th day late
// in the evening - see calendar_ics.c) simply shows no forecast, same as
// any other day the forecast doesn't happen to cover.
#define NVS_AGENDA_CAL_WEATHER_KEY "agenda_cal_wthr"
// Opt-in: right-align the forecast chip instead of centering it (day label
// stays left-aligned either way). Purely a placement preference - the
// available space reserved for the forecast (and therefore how much of it
// can fit before being clipped) is identical either way, see
// draw_day_divider()'s max_weather_chars computation, so this can't lose
// any information a centered layout would have kept, in either the
// stacked or side-by-side column layout.
#define NVS_AGENDA_CAL_WTHR_ALIGN_KEY "agenda_cal_wal"
// Multi-day event display mode - see agenda_multiday_mode_t above. NVS key
// name/values predate the third mode (0/1 used to be a plain bool, "compact
// multi-day" on/off) - kept as-is so existing devices' saved choice still
// means the same thing after an upgrade.
#define NVS_AGENDA_CAL_COMPACT_KEY "agenda_cal_cpt"
// How a timed event's time is shown - see agenda_time_display_mode_t above.
// Off by default (unchanged, existing behavior: just the bare start time).
#define NVS_AGENDA_CAL_SHOW_DURATION_KEY "agenda_cal_dur"
// Optional display name shown in the Calendar column header instead of the
// generic "Calendar A"/"Calendar B" fallback (agenda_renderer.c) - e.g.
// "Private"/"Work". Not a credential, unlike the URL fields above - shown
// as-is in GET /api/config.
#define NVS_AGENDA_CAL_NAME_KEY "agenda_cal_nm"
#define NVS_AGENDA_CAL_NAME2_KEY "agenda_cal_nm2"
#define AGENDA_CAL_NAME_MAX_LEN 24
// Independent schedule - same simplified 3-field cron grammar/limits as
// DEFAULT_ROTATE_CRON/MAX_CRON_RULES/CRON_RULE_MAX_LEN above (reused
// as-is, just a second rule set under its own NVS key). E.g. "0 6-18 *"
// for hourly, 6am-6pm, every day.
#define NVS_AGENDA_CRON_KEY "agenda_cron"
#define DEFAULT_AGENDA_CRON "0 6-18 *"
// Landscape layout only (portrait always stacks top/bottom - too narrow
// otherwise): true stacks ToDo above Calendar, false shows them side by
// side. Default stacked, per user preference - side-by-side was the
// original default and is kept as an option.
#define NVS_AGENDA_STACK_KEY "agenda_stack"
#define AGENDA_STACK_DEFAULT true
// ToDo column background - plain, fixed black-on-white (no user setting):
// the Calendar column's appearance is fully controlled by the imported
// color-profile system below instead (see agenda_color_profile.h), and the
// old shared "agenda_bg_color" setting was removed along with it rather than
// kept as a separate ToDo-only knob.

// Per-role color customization for the ToDo column (Spectra6/color boards
// only - grayscale has no spare hue to pick between, see agenda_renderer.c's
// role_hue()). Every value is one of "red"/"yellow"/"blue"/"green" (the 4
// chromatic Spectra6 hues) - never a free RGB value, since anything off this
// exact palette dithers into visual noise on real hardware (see
// agenda_renderer.c's priority_color() comment for the full story). Each
// role falls back to its original hardcoded default if unset/unrecognized. A
// role whose chosen hue exactly matches the ToDo column's fixed plain
// background automatically falls back to the same black/white polarity the
// day divider and column headers use, rather than silently disappearing
// into the page background. (The Calendar column's own colors are no longer
// part of this scheme - see agenda_color_profile.h.)
#define AGENDA_ROLE_COLOR_MAX_LEN 8
#define NVS_AGENDA_PRI_A_KEY "agenda_pri_a"
#define AGENDA_PRI_A_DEFAULT "red"
#define NVS_AGENDA_PRI_B_KEY "agenda_pri_b"
#define AGENDA_PRI_B_DEFAULT "yellow"
#define NVS_AGENDA_PRI_C_KEY "agenda_pri_c"
#define AGENDA_PRI_C_DEFAULT "green"
#define NVS_AGENDA_PRI_D_KEY "agenda_pri_d"
#define AGENDA_PRI_D_DEFAULT "blue"
#define NVS_AGENDA_DUE_OD_KEY "agenda_due_od"
#define AGENDA_DUE_OD_DEFAULT "red"
#define NVS_AGENDA_DUE_TDY_KEY "agenda_due_tdy"
#define AGENDA_DUE_TDY_DEFAULT "yellow"
#define NVS_AGENDA_DUE_LTR_KEY "agenda_due_ltr"
#define AGENDA_DUE_LTR_DEFAULT "blue"
#define NVS_AGENDA_PROJ_C_KEY "agenda_proj_c"
#define AGENDA_PROJ_C_DEFAULT "blue"
#define NVS_AGENDA_CTX_C_KEY "agenda_ctx_c"
#define AGENDA_CTX_C_DEFAULT "green"
// Three extra, independently-named ICS sources (e.g. holidays, school
// holidays, other special-days feeds a user finds/exports as .ics) shown in
// the same Calendar column as A/B. Unlike A/B, these have NO periodic
// refresh (see AGENDA_CAL_CACHE_PATH_C etc. above) - only (re)fetched when
// the URL is set/changed, "refresh now" is clicked, or a file is uploaded
// directly. Per-source color used to live here too (agenda_cal_c/d/e_color)
// but is now controlled by the imported color-profile system instead (see
// agenda_color_profile.h) - only the source identity/URL/name settings
// remain per-role.
#define NVS_AGENDA_CAL_C_ENABLED_KEY "agenda_cal_c_en"
#define NVS_AGENDA_CAL_C_URL_KEY "agenda_cal_c_url"
#define AGENDA_CAL_C_URL_MAX_LEN 256
#define NVS_AGENDA_CAL_C_NAME_KEY "agenda_cal_c_nm"
#define NVS_AGENDA_CAL_D_ENABLED_KEY "agenda_cal_d_en"
#define NVS_AGENDA_CAL_D_URL_KEY "agenda_cal_d_url"
#define AGENDA_CAL_D_URL_MAX_LEN 256
#define NVS_AGENDA_CAL_D_NAME_KEY "agenda_cal_d_nm"
#define NVS_AGENDA_CAL_E_ENABLED_KEY "agenda_cal_e_en"
#define NVS_AGENDA_CAL_E_URL_KEY "agenda_cal_e_url"
#define AGENDA_CAL_E_URL_MAX_LEN 256
#define NVS_AGENDA_CAL_E_NAME_KEY "agenda_cal_e_nm"
#define AGENDA_CAL_CDE_NAME_MAX_LEN 24

// User-authored Calendar-view color profiles (see agenda_color_profile.h),
// imported via the Web UI as JSON exported by the companion browser tool
// "profile-editor.html". Up to AGENDA_COLOR_PROFILE_SLOTS profiles can be
// stored on the device at once; at most one is "active" at a time
// (0 = none, use the built-in plain default). Each slot is a whole JSON
// file on the SD card rather than an NVS blob - profiles are small
// (well under 1KB) but arbitrary/free-form, unlike every other Agenda
// setting here which is a single scalar value.
#define AGENDA_COLOR_PROFILE_SLOTS 3
#define NVS_AGENDA_COLOR_PROFILE_ACTIVE_KEY "agenda_clrp_a"
#define AGENDA_COLOR_PROFILE_PATH_1 FS_MOUNT_POINT "/.agenda_color_profile_1.json"
#define AGENDA_COLOR_PROFILE_PATH_2 FS_MOUNT_POINT "/.agenda_color_profile_2.json"
#define AGENDA_COLOR_PROFILE_PATH_3 FS_MOUNT_POINT "/.agenda_color_profile_3.json"
// Generous but bounded - profile-editor.html's export is a small fixed-shape
// JSON document (16 color fields + a handful of flags/strings), never
// user-supplied free text.
#define AGENDA_COLOR_PROFILE_MAX_BYTES 8192

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

// Default bound for wifi_manager_connect()'s wait - matches the ~60s budget
// callers effectively relied on before that wait was made explicitly bounded
// (see wifi_manager.c: it used to block forever via portMAX_DELAY, which
// could hang indefinitely on a DHCP stall after a successful association).
#define WIFI_CONNECT_DEFAULT_TIMEOUT_MS 60000

// AI API Keys (for webapp client use)
#define AI_API_KEY_MAX_LEN 256
#define NVS_OPENAI_API_KEY_KEY "openai_key"
#define NVS_GOOGLE_API_KEY_KEY "google_key"

// OTA Configuration
#define GITHUB_API_URL "https://api.github.com/repos/t3ste/Tlg-esp32-photoframe/releases/latest"
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

// Chimes (speaker feedback, waveshare_photopainter_73 only - see
// board_hal_has_speaker()). Master mode - see chime_speaker_mode_t above.
// Off by default: this is new, previously-silent hardware, so an update
// shouldn't start making sound on its own.
#define NVS_CHIME_SPEAKER_MODE_KEY "chime_mode"
// DAC volume, 0-100% - linearly mapped to the ES8311's DAC_VOL register
// range in audio_chime.c. Applies to every chime alike (success/warning/
// error), by design - see chime_repeat_gate() below for how urgency is
// instead conveyed by repetition, not loudness.
#define NVS_CHIME_VOLUME_KEY "chime_vol"
#define CHIME_DEFAULT_VOLUME_PERCENT 80
// Quiet hours - a plain daily HH:MM-HH:MM window (wraps past midnight if
// end < start) during which no chime plays regardless of the master mode
// or any per-event flag. Off by default.
#define NVS_CHIME_QUIET_ENABLED_KEY "chime_quiet_on"
#define NVS_CHIME_QUIET_START_KEY "chime_quiet_st"
#define NVS_CHIME_QUIET_END_KEY "chime_quiet_ed"
#define CHIME_TIME_STR_MAX_LEN 6  // "HH:MM" + NUL
#define CHIME_DEFAULT_QUIET_START "22:00"
#define CHIME_DEFAULT_QUIET_END "07:00"
// One bool per chime_event_t - see main/chime.c's chime_play_if_enabled().
// low-battery/wifi-reprovision/ota-success/critical-error default on (the
// user asked for these specifically); the rest default off since they fire
// far more often (every rotation) or are more a "nice to have" (Telegram
// photo received, an overdue agenda item).
#define NVS_CHIME_EVENT_ROTATION_KEY "chime_ev_rotate"
#define NVS_CHIME_EVENT_TELEGRAM_KEY "chime_ev_tg"
#define NVS_CHIME_EVENT_LOWBATT_KEY "chime_ev_lowbat"
#define NVS_CHIME_EVENT_WIFIPROV_KEY "chime_ev_wifi"
#define NVS_CHIME_EVENT_AGENDA_KEY "chime_ev_agenda"
#define NVS_CHIME_EVENT_OTA_KEY "chime_ev_ota"
#define NVS_CHIME_EVENT_CRIT_KEY "chime_ev_crit"
// Persisted repeat-until-resolved counters - see chime_repeat_gate() in
// main/chime.c. An "actionable" event (needs the user to do something,
// unlike a one-shot informational event like rotation/Telegram/OTA) fires
// once per wake/render while its underlying condition stays true, up to
// CHIME_REPEAT_MAX times, then goes quiet until the condition actually
// resolves (which resets the counter back to 0, so it repeats again if the
// same problem recurs later). Only the 3 events below currently have a
// meaningful "still ongoing vs. resolved" state to repeat against -
// WIFI_REPROVISION is inherently one-shot (the device reboots into a
// different mode immediately after), and ROTATION/TELEGRAM_PHOTO/OTA_SUCCESS
// are one-off good-news events with nothing to "still be wrong" about.
#define CHIME_REPEAT_MAX 5
#define NVS_CHIME_REPEAT_LOWBATT_KEY "chime_rc_lowbat"
#define NVS_CHIME_REPEAT_CRIT_KEY "chime_rc_crit"
#define NVS_CHIME_REPEAT_AGENDA_KEY "chime_rc_agenda"

// Climate (SHTC3) settings - see climate_room_type_t/climate_temp_unit_t
// above and main/climate.[ch]. Generic feature: available on any board
// whose board_hal_get_temperature()/get_humidity() actually succeed, not
// gated to one specific board.
#define NVS_CLIMATE_ROOM_TYPE_KEY "climate_room"
#define NVS_CLIMATE_TEMP_UNIT_KEY "climate_unit"
// Logging defaults on (mirrors battery history's always-on-when-hardware-
// present behavior, no visual clutter involved); the overlay badge and
// Agenda-header readout default off since they add visible clutter to
// every image/render until the user opts in.
#define NVS_CLIMATE_LOGGING_ENABLED_KEY "climate_log_en"
#define NVS_CLIMATE_OVERLAY_ENABLED_KEY "climate_ovl_en"
// Auto-backup to persistent storage (see battery_history.c/climate_history.c)
// right before either history log's automatic 180-day-age reset would
// otherwise discard it. Climate defaults on (matches its logging-enabled
// default above); battery defaults off since a full discharge/recharge
// cycle - and therefore a fresh reset - happens far more often than for
// climate, so backups would accumulate faster unless a user opts in.
#define NVS_BATTERY_HISTORY_BACKUP_KEY "batt_hist_bkup"
#define NVS_CLIMATE_HISTORY_BACKUP_KEY "clim_hist_bkup"
#define NVS_CLIMATE_AGENDA_HEADER_ENABLED_KEY "climate_hdr_en"
// Persisted debounce anchor for CLIMATE_LOG_MIN_INTERVAL_SEC above - unix
// timestamp of the last actually-recorded reading (not every check), same
// int64 NVS pattern as "cfg_updated" (config_manager_get_config_last_updated()).
#define NVS_CLIMATE_LAST_LOG_KEY "climate_lastlog"
// User-correctable calibration offset, applied to every displayed/logged
// climate value (NOT to GET /api/sensor's raw reading, which stays
// unadjusted on purpose - useful as a reference for picking these values).
// Always stored/transmitted in Celsius/percentage-points regardless of the
// user's display-unit preference; climate_temp_offset_c is a DELTA, so
// converting it to/from Fahrenheit for display never adds the usual +32
// (see climate_celsius_to_fahrenheit() vs. a plain *9/5 delta conversion).
#define NVS_CLIMATE_TEMP_OFFSET_KEY "climate_toff"
#define NVS_CLIMATE_HUM_OFFSET_KEY "climate_hoff"
#define CLIMATE_OFFSET_MAX_LEN 16

// ----------------------------------------------------------------------------
// Alarm Clock (only present in a build compiled with CONFIG_ALARM_CLOCK_ENABLED
// - see main/Kconfig, `build.py --alarmclock`, docs/ALARMCLOCK_FEASIBILITY.md).
// Same simplified 3-field cron grammar/limits as the rotate/agenda schedules
// above (MAX_CRON_RULES/CRON_RULE_MAX_LEN, reused as-is) - an alarm is
// "armed" purely by having at least one rule, "permanently disabled" purely
// by having none, no separate enabled flag. No seeded default: unlike the
// agenda schedule, there's no "enabled but no schedule" state to unstick -
// an empty schedule just means no alarm is set, which is the correct
// starting state for a fresh device.
// ----------------------------------------------------------------------------
#define NVS_ALARM_CRON_KEY "alarm_cron"
#define NVS_ALARM_RING_SEC_KEY "alarm_ring_sec"
#define ALARM_RING_DURATION_DEFAULT_SEC 60
#define ALARM_RING_DURATION_MAX_SEC 600  // 10 minutes - generous upper bound, not a hard spec limit

#endif