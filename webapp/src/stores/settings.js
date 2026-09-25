import { defineStore } from "pinia";
import { ref, watch } from "vue";
import {
  getPreset,
  getPresetOptions,
  SPECTRA6,
  getDefaultParams,
} from "@aitjcize/epaper-image-convert";
import { validateTimezone } from "../utils/timezone";

export const useSettingsStore = defineStore("settings", () => {
  const API_BASE = "";

  // UI state
  const activeSettingsTab = ref("general");

  // Processing parameters
  const params = ref(getDefaultParams());

  // Web UI upload format - client-side only (this browser's own preference,
  // never sent to /api/config or the device): which format ImageUpload.vue's
  // client-side conversion produces before uploading. EPDGZ (default) is
  // already the recommended format - already-resolved palette index,
  // gzip-compressed, no per-pixel re-matching needed at display time. PNG
  // remains selectable for compatibility/inspection.
  const uploadImageFormat = ref(localStorage.getItem("uploadImageFormat") || "epdgz");
  watch(uploadImageFormat, (value) => {
    try {
      localStorage.setItem("uploadImageFormat", value);
    } catch (_error) {
      // Storage unavailable (private browsing, quota) - preference just
      // won't survive a reload, nothing else depends on it persisting
    }
  });

  // Device settings (UI representation)
  const deviceSettings = ref({
    // General
    deviceName: "PhotoFrame",
    // The POSIX TZ rule exactly as the device applies it (tzset). Kept
    // verbatim: a DST rule such as CET-1CEST,M3.5.0,M10.5.0/3 has no
    // numeric form, and reducing it to an offset would clobber it on save.
    timezone: "UTC0",
    ntpServer: "pool.ntp.org",
    // Network: static IP / DNS override (#43)
    ipMode: "dhcp",
    staticIp: "",
    staticNetmask: "255.255.255.0",
    staticGateway: "",
    dnsServer: "",
    displayOrientation: "landscape",
    displayRotationDeg: 180,
    wifiSsid: "",
    wifiPassword: "",
    // Auto Rotate
    autoRotate: true,
    rotateCron: ["0 */12 *"],
    rotationMode: "storage",
    // Auto Rotate - SDCARD
    sdRotationMode: "random",
    // Auto Rotate - URL
    imageUrl: "https://loremflickr.com/800/480",
    caCertSet: false,
    lastFetchError: "",
    accessToken: "",
    // Write-only: the device reports only whether a password is set, never the
    // value. Blank means "leave whatever is stored alone".
    httpPassword: "",
    httpAuthEnabled: false,
    httpAuthWasEnabled: false,
    httpHeaderKey: "",
    httpHeaderValue: "",
    saveDownloadedImages: true,
    // Auto Rotate - Telegram
    telegramBotToken: "",
    telegramChatId: "",
    telegramConfigured: false,
    telegramPairingEnabled: true,
    telegramWakeNotifyEnabled: false,
    // Home Assistant
    haUrl: "",
    haEnabled: false,
    // Power
    deepSleepEnabled: true,
    otaCheckEnabled: true,
    wifiPerformanceModeEnabled: true,
    wifiTxPowerCapEnabled: true,
    wifiExtendedRetryEnabled: false,
    wifiReprovisionOnFailEnabled: true,
    // Read-only status (set during first-time setup / by the hotspot itself,
    // not a settable field here - see the offline-hotspot section below).
    offlineModeEnabled: false,
    apHotspotActive: false,
    httpsEnabled: false,
    rotationPairingEnabled: false,
    variantSelectionEnabled: false,
    telegramRotationNotifyEnabled: false,
    telegramFallbackRotationEnabled: true,
    telegramFallbackOnErrorEnabled: true,
    telegramPowerSaveEnabled: false,
    telegramPowerSaveLatestOnly: false,
    telegramKeepOriginalsEnabled: false,
    telegramImageFormat: "epdgz",
    telegramDedupEnabled: false,
    // Weather + headline overlays (on-device, no companion server needed)
    weatherOverlayEnabled: false,
    weatherLocationName: "",
    weatherLat: "",
    weatherLon: "",
    weatherProvider: "open-meteo",
    headlinesOverlayEnabled: false,
    headlinesRssUrl: "",
    headlinesCount: 3,
    headlinesWrapLines: 1,
    overlayInvertColors: false,
    overlayEpdgzEnabled: false,
    overlayLanguage: "en",
    captionInvertColorsEnabled: false,
    weatherMultilineEnabled: false,
    weatherIconSet: "none",
    weatherIconColored: false,
    showExifDatetimeEnabled: false,
    lowBatteryOverlayEnabled: false,
    lowBatteryOverlayThreshold: 16,
    // Chimes (speaker feedback, waveshare_photopainter_73 only) -
    // chimeSpeakerAvailable is a read-only hardware capability flag, never
    // sent in a PATCH. Off by default across the board, per-event flags
    // mirror the firmware's own conservative defaults (see config.h).
    chimeSpeakerAvailable: false,
    // Read-only capability flag too (never sent in a PATCH): the Alarm tab's voice
    // tools (microphone level meter, stop word) exist in this firmware.
    voiceAvailable: false,
    chimeSpeakerMode: "off",
    chimeVolume: 80,
    chimeQuietEnabled: false,
    chimeQuietStart: "22:00",
    chimeQuietEnd: "07:00",
    chimeEventRotationEnabled: false,
    chimeEventTelegramPhotoEnabled: false,
    chimeEventLowBatteryEnabled: true,
    chimeEventWifiReprovisionEnabled: true,
    chimeEventAgendaDueEnabled: false,
    chimeEventOtaSuccessEnabled: true,
    chimeEventCriticalErrorEnabled: true,
    // Climate (SHTC3 temperature/humidity) - generic feature (works on any
    // board whose sensor actually answers, not just
    // waveshare_photopainter_73, unlike Chimes above).
    // climateSensorAvailable is a read-only, live hardware probe (not a
    // fixed capability flag - the same board can succeed or fail at
    // runtime), never sent in a PATCH.
    climateSensorAvailable: false,
    climateRoomType: "living_room",
    climateTempUnit: "celsius",
    climateLoggingEnabled: true,
    climateOverlayEnabled: false,
    climateAgendaHeaderEnabled: false,
    // Auto-backup to SD before each history log's 180-day-age reset -
    // climate defaults on (mirrors climateLoggingEnabled's default),
    // battery defaults off (a full charge cycle resets far more often than
    // 180 days, so backups would pile up unless opted into). Surfaced as a
    // switch directly on each chart card (BatteryHistory.vue/
    // ClimateHistory.vue), not in this settings tab.
    batteryHistoryBackupEnabled: false,
    climateHistoryBackupEnabled: true,
    // Calibration offsets, always in Celsius/percentage-points regardless of
    // climateTempUnit - SettingsPanel.vue converts to/from the display unit
    // for its own input fields.
    climateTempOffset: 0,
    climateHumOffset: 0,
    // Alarm Clock - only present in a firmware build compiled with
    // CONFIG_ALARM_CLOCK_ENABLED (build.py --alarmclock). alarmClockAvailable
    // is a read-only capability flag (like chimeSpeakerAvailable above),
    // never sent in a PATCH - the Settings page uses it to hide the whole
    // tab on a build without the feature. No separate "enabled" toggle: the
    // alarm is armed purely by having at least one alarmCron rule.
    alarmClockAvailable: false,
    alarmCron: [],
    alarmRingDurationSec: 60,
    // Agenda (ToDo + Calendar) - a full-screen display mode, not a photo
    // overlay. agendaTodoUrl/agendaCalUrl are write-only (never returned by
    // GET /api/config, same treatment as wifiPassword above) - both start
    // empty even when a URL is actually configured on the device, since
    // either can carry a credential embedded as a query param (not just
    // the Calendar URL - a security review found the original assumption
    // that a ToDo feed is always a public, non-secret gist doesn't hold in
    // general).
    agendaTodoEnabled: false,
    agendaCalEnabled: false,
    agendaTodoUrl: "",
    agendaCalUrl: "",
    // Read-only, server-reported "is a URL actually saved?" flag - since the
    // write-only field above always starts empty, this is the only way the
    // UI can show a persistent "configured" confirmation across a reload.
    agendaCalUrlConfigured: false,
    // Optional second calendar (e.g. work vs. personal) - merged with the
    // first at render time, colored per its own origin. Same write-only
    // treatment as agendaCalUrl above.
    agendaCalUrl2: "",
    agendaCalUrl2Configured: false,
    // Three extra ICS sources (e.g. holidays/school-holidays/other
    // special-days feeds) shown in the same Calendar column as A/B, each
    // independently enabled/named/colored. Unlike A/B above, these are
    // NEVER refreshed automatically - only when the URL is set/changed, the
    // user clicks "refresh now" (agendaCalCRefetch etc., a one-shot flag,
    // not a persisted setting), or a file is uploaded directly via
    // /api/agenda/extra-ics. Same write-only URL treatment as agendaCalUrl.
    agendaCalCEnabled: false,
    agendaCalDEnabled: false,
    agendaCalEEnabled: false,
    agendaCalCUrl: "",
    agendaCalDUrl: "",
    agendaCalEUrl: "",
    // Read-only, server-reported "is a raw .ics file on disk right now?"
    // flag - true whether it got there via URL fetch or direct upload; same
    // "otherwise no persistent confirmation" reasoning as agendaCalUrlConfigured.
    agendaCalCConfigured: false,
    agendaCalDConfigured: false,
    agendaCalEConfigured: false,
    agendaCalCName: "",
    agendaCalDName: "",
    agendaCalEName: "",
    // Optional display names shown in the Calendar header instead of the
    // generic "Calendar A"/"Calendar B" fallback - not secrets, always
    // returned/saved plainly (unlike the URL fields above).
    agendaCalName: "",
    agendaCalName2: "",
    agendaCalDays: 2,
    // Annotates each Calendar day divider with that day's forecast (reuses
    // the same weather settings/provider as the photo weather overlay -
    // see weatherLocationName/weatherProvider etc. below). Off by default.
    agendaCalWeatherEnabled: false,
    // Placement only - centered (default) or right-aligned; doesn't change
    // how much forecast text can fit (see agenda_renderer.c).
    agendaCalWeatherRightAligned: false,
    // How a multi-day event is displayed - "repeat" (default, shown under
    // every day it spans, no prefix), "compact" (shown once, on its first
    // visible day, with an "N/M: " position prefix), or "repeat_numbered"
    // (repeated under every day like "repeat", but each occurrence also
    // gets its own "N/M: " prefix).
    agendaCalMultidayMode: "repeat",
    // "off" (default): timed events show just "HH:MM Summary". "duration":
    // appends a compact "[Xm]"/"[Xh]" suffix. "range": shows the full
    // "HH:MM-HH:MM" span instead. Never affects all-day events.
    agendaCalTimeDisplayMode: "off",
    // Calendar-only-fullscreen layout - "list" (default, today's 1-3 day
    // list) or a 7-day grid template ("grid_a"/"grid_b"). Only takes
    // effect when the ToDo column is off - see agenda_renderer.c.
    agendaCalLayoutMode: "list",
    // Optional 2-group rotation/"shift" coloring for the 7-day grid - see
    // agenda_shift_model_t (config.h). "none" (default) = no coloring.
    agendaShiftModel: "none",
    // "YYYY-MM-DD", empty = unset (no coloring even if a model is chosen).
    agendaShiftStart: "",
    agendaCron: ["0 6-18 *"],
    // true = ToDo above Calendar (default), false = side by side. Portrait
    // boards always stack regardless of this setting - see agenda_renderer.c.
    agendaStackLayout: true,
    // Which imported Calendar-view color-profile slot is active (0 = none,
    // built-in plain default; 1..3 = a stored slot) - see
    // agenda_color_profile.h. The profiles themselves (name/slot list) are
    // fetched separately via agendaColorProfileSlots, not saved as part of
    // this settings blob.
    agendaColorProfileActive: 0,
    // Per-role color pickers for the ToDo column only (Spectra6/color
    // boards only - grayscale has no spare hue to choose between). Each is
    // one of "red"/"yellow"/"blue"/"green"; defaults match this feature's
    // original hardcoded colors. The Calendar column's own colors come
    // from the imported profile above instead.
    agendaPriAColor: "red",
    agendaPriBColor: "yellow",
    agendaPriCColor: "green",
    agendaPriDColor: "blue",
    agendaDueOverdueColor: "red",
    agendaDueTodayColor: "yellow",
    agendaDueLaterColor: "blue",
    agendaProjectColor: "blue",
    agendaContextColor: "green",
    // Debugging
    debugLogEnabled: false,
    errorOverlayEnabled: false,
    // AI API Keys (for client-side AI generation)
    aiCredentials: {
      openaiApiKey: "",
      googleApiKey: "",
    },
  });

  // ... (existing code)

  // Original config from server (for change detection)
  let originalConfig = {};

  // The time zone rule as the device last reported or accepted it, so the UI
  // can tell a rule the user edited from one it merely loaded.
  const savedTimezone = ref("UTC0");

  // Orientation as currently saved/applied on the device. The image preview uses
  // this (not the live dropdown) so it only re-lays-out when the user saves.
  const appliedOrientation = ref("landscape");

  let originalParams = {};

  // Palette - use defaults from epaper-image-convert library
  const palette = ref({ ...SPECTRA6.perceived });

  // Preset
  const preset = ref("balanced");

  // Get preset names from library (excluding "custom")
  const presetNames = getPresetOptions().map((p) => p.value);

  // Keys to compare for preset matching
  const presetKeys = [
    "exposure",
    "saturation",
    "toneMode",
    "contrast",
    "strength",
    "shadowBoost",
    "highlightCompress",
    "midpoint",
    "colorMethod",
    "ditherAlgorithm",
    "compressDynamicRange",
  ];

  function matchesPreset(presetName) {
    const target = getPreset(presetName);
    if (!target) return false;

    const current = params.value;
    for (const key of presetKeys) {
      if (!(key in target)) continue;
      const a = current[key];
      const b = target[key];
      // Compare numbers with a tolerance: the device persists floats as 32-bit,
      // so e.g. 1.4 round-trips as 1.39999998 and exact matching would never
      // detect the preset (it would fall back to "custom").
      if (typeof a === "number" && typeof b === "number") {
        if (Math.abs(a - b) > 1e-3) return false;
      } else if (a !== b) {
        return false;
      }
    }
    return true;
  }

  function derivePresetFromParams() {
    for (const name of presetNames) {
      if (matchesPreset(name)) return name;
    }
    return "custom";
  }

  // Actions
  function applyPreset(presetName) {
    const presetParams = getPreset(presetName);
    if (presetParams) {
      preset.value = presetName;
      // Copy only processing params (exclude name, title, description)
      // eslint-disable-next-line no-unused-vars
      const { name, title, description, ...processingParams } = presetParams;
      Object.assign(params.value, processingParams);
    }
  }

  // For a fresh grayscale frame (processing still at library defaults), default
  // to the grayscale preset, which is tuned for monochrome (LAB + s-curve).
  // A device that has already customized its settings is left untouched.
  function applyGrayscaleDefaultIfUntouched() {
    const def = getDefaultParams();
    const untouched = presetKeys.every((k) => !(k in def) || params.value[k] === def[k]);
    if (untouched) applyPreset("grayscale");
  }

  watch(
    params,
    () => {
      preset.value = derivePresetFromParams();
    },
    { deep: true }
  );

  async function loadSettings() {
    try {
      const response = await fetch(`${API_BASE}/api/settings/processing`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        return;
      }
      const data = await response.json();
      Object.assign(params.value, data);
      // Store original params for change detection
      originalParams = JSON.parse(JSON.stringify(data));
    } catch (_error) {
      console.log("Settings API not available (standalone mode)");
    }
  }

  async function loadDeviceSettings() {
    try {
      const response = await fetch(`${API_BASE}/api/config`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        return;
      }
      const data = await response.json();

      // Store original config for change detection
      originalConfig = JSON.parse(JSON.stringify(data));

      // Parse config into UI-friendly format
      deviceSettings.value.autoRotate = data.auto_rotate || false;

      // Rotation schedule: array of cron rules (fall back to the default).
      deviceSettings.value.rotateCron =
        Array.isArray(data.rotate_cron) && data.rotate_cron.length
          ? data.rotate_cron
          : ["0 */12 *"];

      deviceSettings.value.displayRotationDeg = data.display_rotation_deg ?? 180;
      deviceSettings.value.imageUrl = data.image_url || "https://loremflickr.com/800/480";
      deviceSettings.value.caCertSet = data.ca_cert_set || false;
      deviceSettings.value.lastFetchError = data.last_fetch_error || "";
      deviceSettings.value.deepSleepEnabled = data.deep_sleep_enabled !== false;
      deviceSettings.value.otaCheckEnabled = data.ota_check_enabled !== false;
      deviceSettings.value.wifiPerformanceModeEnabled =
        data.wifi_performance_mode_enabled !== false;
      deviceSettings.value.wifiTxPowerCapEnabled = data.wifi_tx_power_cap_enabled !== false;
      deviceSettings.value.wifiExtendedRetryEnabled = data.wifi_extended_retry_enabled === true;
      deviceSettings.value.wifiReprovisionOnFailEnabled =
        data.wifi_reprovision_on_fail_enabled !== false;
      deviceSettings.value.offlineModeEnabled = data.offline_mode_enabled === true;
      deviceSettings.value.apHotspotActive = data.ap_hotspot_active === true;
      deviceSettings.value.httpsEnabled = data.https_enabled === true;
      deviceSettings.value.rotationPairingEnabled = data.rotation_pairing_enabled === true;
      deviceSettings.value.variantSelectionEnabled = data.variant_selection_enabled === true;
      deviceSettings.value.telegramRotationNotifyEnabled =
        data.telegram_rotation_notify_enabled === true;
      deviceSettings.value.telegramFallbackRotationEnabled =
        data.telegram_fallback_rotation_enabled !== false;
      deviceSettings.value.telegramFallbackOnErrorEnabled =
        data.telegram_fallback_on_error_enabled !== false;
      deviceSettings.value.telegramPowerSaveEnabled = data.telegram_power_save_enabled === true;
      deviceSettings.value.telegramPowerSaveLatestOnly =
        data.telegram_power_save_latest_only === true;
      deviceSettings.value.telegramKeepOriginalsEnabled =
        data.telegram_keep_originals_enabled === true;
      deviceSettings.value.telegramImageFormat = data.telegram_image_format || "epdgz";
      deviceSettings.value.telegramDedupEnabled = data.telegram_dedup_enabled === true;
      deviceSettings.value.weatherOverlayEnabled = data.weather_overlay_enabled === true;
      deviceSettings.value.weatherLocationName = data.weather_location_name || "";
      deviceSettings.value.weatherLat = data.weather_lat || "";
      deviceSettings.value.weatherLon = data.weather_lon || "";
      deviceSettings.value.weatherProvider = data.weather_provider || "open-meteo";
      deviceSettings.value.headlinesOverlayEnabled = data.headlines_overlay_enabled === true;
      deviceSettings.value.headlinesRssUrl = data.headlines_rss_url || "";
      deviceSettings.value.headlinesCount = data.headlines_count ?? 3;
      deviceSettings.value.headlinesWrapLines = data.headlines_wrap_lines ?? 1;
      deviceSettings.value.overlayInvertColors = data.overlay_invert_colors === true;
      deviceSettings.value.overlayEpdgzEnabled = data.overlay_epdgz_enabled === true;
      deviceSettings.value.overlayLanguage = data.overlay_language || "en";
      deviceSettings.value.captionInvertColorsEnabled = data.caption_invert_colors_enabled === true;
      deviceSettings.value.weatherMultilineEnabled = data.weather_multiline_enabled === true;
      deviceSettings.value.weatherIconSet = data.weather_icon_set || "none";
      deviceSettings.value.weatherIconColored = data.weather_icon_colored === true;
      deviceSettings.value.showExifDatetimeEnabled = data.show_exif_datetime_enabled === true;
      deviceSettings.value.lowBatteryOverlayEnabled = data.low_battery_overlay_enabled === true;
      deviceSettings.value.lowBatteryOverlayThreshold = data.low_battery_overlay_threshold ?? 16;
      deviceSettings.value.batteryHistoryBackupEnabled =
        data.battery_history_backup_enabled === true;
      deviceSettings.value.chimeSpeakerAvailable = data.chime_speaker_available === true;
      deviceSettings.value.voiceAvailable = data.voice_available === true;
      deviceSettings.value.chimeSpeakerMode = data.chime_speaker_mode || "off";
      deviceSettings.value.chimeVolume = data.chime_volume ?? 80;
      deviceSettings.value.chimeQuietEnabled = data.chime_quiet_enabled === true;
      deviceSettings.value.chimeQuietStart = data.chime_quiet_start || "22:00";
      deviceSettings.value.chimeQuietEnd = data.chime_quiet_end || "07:00";
      deviceSettings.value.chimeEventRotationEnabled = data.chime_event_rotation_enabled === true;
      deviceSettings.value.chimeEventTelegramPhotoEnabled =
        data.chime_event_telegram_photo_enabled === true;
      deviceSettings.value.chimeEventLowBatteryEnabled =
        data.chime_event_low_battery_enabled === true;
      deviceSettings.value.chimeEventWifiReprovisionEnabled =
        data.chime_event_wifi_reprovision_enabled === true;
      deviceSettings.value.chimeEventAgendaDueEnabled =
        data.chime_event_agenda_due_enabled === true;
      deviceSettings.value.chimeEventOtaSuccessEnabled =
        data.chime_event_ota_success_enabled === true;
      deviceSettings.value.chimeEventCriticalErrorEnabled =
        data.chime_event_critical_error_enabled === true;
      deviceSettings.value.climateSensorAvailable = data.climate_sensor_available === true;
      deviceSettings.value.climateRoomType = data.climate_room_type || "living_room";
      deviceSettings.value.climateTempUnit = data.climate_temp_unit || "celsius";
      deviceSettings.value.climateLoggingEnabled = data.climate_logging_enabled === true;
      deviceSettings.value.climateHistoryBackupEnabled =
        data.climate_history_backup_enabled === true;
      deviceSettings.value.climateOverlayEnabled = data.climate_overlay_enabled === true;
      deviceSettings.value.climateAgendaHeaderEnabled = data.climate_agenda_header_enabled === true;
      deviceSettings.value.climateTempOffset = data.climate_temp_offset ?? 0;
      deviceSettings.value.climateHumOffset = data.climate_hum_offset ?? 0;
      deviceSettings.value.alarmClockAvailable = data.alarm_clock_available === true;
      deviceSettings.value.alarmCron = Array.isArray(data.alarm_cron) ? data.alarm_cron : [];
      deviceSettings.value.alarmRingDurationSec = data.alarm_ring_duration_sec ?? 60;
      deviceSettings.value.agendaTodoEnabled = data.agenda_todo_enabled === true;
      deviceSettings.value.agendaCalEnabled = data.agenda_cal_enabled === true;
      // agenda_todo_url/agenda_cal_url are intentionally never present in
      // this response (see http_server.c) - stay empty even when a URL is actually
      // configured, same write-only treatment as wifiPassword above.
      deviceSettings.value.agendaCalCEnabled = data.agenda_cal_c_enabled === true;
      deviceSettings.value.agendaCalDEnabled = data.agenda_cal_d_enabled === true;
      deviceSettings.value.agendaCalEEnabled = data.agenda_cal_e_enabled === true;
      deviceSettings.value.agendaCalCName = data.agenda_cal_c_name || "";
      deviceSettings.value.agendaCalDName = data.agenda_cal_d_name || "";
      deviceSettings.value.agendaCalEName = data.agenda_cal_e_name || "";
      deviceSettings.value.agendaCalName = data.agenda_cal_name || "";
      deviceSettings.value.agendaCalName2 = data.agenda_cal_name2 || "";
      deviceSettings.value.agendaCalDays = data.agenda_cal_days ?? 2;
      deviceSettings.value.agendaCalWeatherEnabled = data.agenda_cal_weather_enabled === true;
      deviceSettings.value.agendaCalWeatherRightAligned =
        data.agenda_cal_weather_right_aligned === true;
      deviceSettings.value.agendaCalMultidayMode = data.agenda_cal_multiday_mode || "repeat";
      deviceSettings.value.agendaCalTimeDisplayMode = data.agenda_cal_time_display_mode || "off";
      deviceSettings.value.agendaCalLayoutMode = data.agenda_cal_layout_mode || "list";
      deviceSettings.value.agendaShiftModel = data.agenda_shift_model || "none";
      deviceSettings.value.agendaShiftStart = data.agenda_shift_start || "";
      deviceSettings.value.agendaCalUrlConfigured = data.agenda_cal_url_configured === true;
      deviceSettings.value.agendaCalUrl2Configured = data.agenda_cal_url2_configured === true;
      deviceSettings.value.agendaCalCConfigured = data.agenda_cal_c_configured === true;
      deviceSettings.value.agendaCalDConfigured = data.agenda_cal_d_configured === true;
      deviceSettings.value.agendaCalEConfigured = data.agenda_cal_e_configured === true;
      deviceSettings.value.agendaCron =
        Array.isArray(data.agenda_cron) && data.agenda_cron.length
          ? data.agenda_cron
          : ["0 6-18 *"];
      deviceSettings.value.agendaStackLayout = data.agenda_stack_layout !== false;
      deviceSettings.value.agendaColorProfileActive = data.agenda_color_profile_active ?? 0;
      deviceSettings.value.agendaPriAColor = data.agenda_pri_a_color || "red";
      deviceSettings.value.agendaPriBColor = data.agenda_pri_b_color || "yellow";
      deviceSettings.value.agendaPriCColor = data.agenda_pri_c_color || "green";
      deviceSettings.value.agendaPriDColor = data.agenda_pri_d_color || "blue";
      deviceSettings.value.agendaDueOverdueColor = data.agenda_due_overdue_color || "red";
      deviceSettings.value.agendaDueTodayColor = data.agenda_due_today_color || "yellow";
      deviceSettings.value.agendaDueLaterColor = data.agenda_due_later_color || "blue";
      deviceSettings.value.agendaProjectColor = data.agenda_project_color || "blue";
      deviceSettings.value.agendaContextColor = data.agenda_context_color || "green";
      deviceSettings.value.debugLogEnabled = data.debug_log_enabled === true;
      deviceSettings.value.errorOverlayEnabled = data.error_overlay_enabled === true;
      deviceSettings.value.haUrl = data.ha_url || "";
      deviceSettings.value.haEnabled = data.ha_enabled === true;
      deviceSettings.value.saveDownloadedImages = data.save_downloaded_images !== false;
      deviceSettings.value.accessToken = data.access_token || "";
      // The HTTP API password is never echoed back; keep the input blank and
      // track only whether one is configured.
      deviceSettings.value.httpPassword = "";
      deviceSettings.value.httpAuthEnabled = data.http_auth_enabled === true;
      deviceSettings.value.httpAuthWasEnabled = data.http_auth_enabled === true;
      deviceSettings.value.httpHeaderKey = data.http_header_key || "";
      deviceSettings.value.httpHeaderValue = data.http_header_value || "";
      deviceSettings.value.displayOrientation = data.display_orientation || "landscape";
      appliedOrientation.value = deviceSettings.value.displayOrientation;
      deviceSettings.value.rotationMode = data.rotation_mode || "storage";
      deviceSettings.value.sdRotationMode = data.sd_rotation_mode || "random";
      deviceSettings.value.telegramBotToken = data.telegram_bot_token || "";
      deviceSettings.value.telegramChatId = data.telegram_chat_id || "";
      deviceSettings.value.telegramConfigured = data.telegram_configured === true;
      deviceSettings.value.telegramWakeNotifyEnabled = data.telegram_wake_notify_enabled === true;
      deviceSettings.value.telegramPairingEnabled = data.telegram_pairing_enabled !== false;
      deviceSettings.value.deviceName = data.device_name || "PhotoFrame";
      deviceSettings.value.ntpServer = data.ntp_server || "pool.ntp.org";
      deviceSettings.value.ipMode = data.ip_mode || "dhcp";
      deviceSettings.value.staticIp = data.static_ip || "";
      deviceSettings.value.staticNetmask = data.static_netmask || "255.255.255.0";
      deviceSettings.value.staticGateway = data.static_gateway || "";
      deviceSettings.value.dnsServer = data.dns_server || "";
      deviceSettings.value.wifiSsid = data.wifi_ssid || "";
      // Don't load password from server for security
      deviceSettings.value.wifiPassword = "";

      // AI API Keys (for client-side AI generation)
      deviceSettings.value.aiCredentials.openaiApiKey = data.openai_api_key || "";
      deviceSettings.value.aiCredentials.googleApiKey = data.google_api_key || "";

      // Kept as the raw POSIX string - see the `timezone` field's own
      // comment above for why this is never parsed into a numeric offset.
      deviceSettings.value.timezone = data.timezone || "UTC0";
      savedTimezone.value = deviceSettings.value.timezone;
    } catch (_error) {
      console.log("Device settings API not available (standalone mode)");
    }
  }

  // The device stores at most this many bytes (HTTP_PASSWORD_MAX_LEN - 1).
  const HTTP_PASSWORD_MAX_BYTES = 63;

  // The password is write-only, so it never appears in originalConfig and
  // can't go through the changed-fields diff. Decide here whether this save
  // touches it: undefined leaves the stored password alone, "" clears it, and
  // a non-empty string sets it.
  function httpPasswordChange() {
    const { httpAuthEnabled, httpAuthWasEnabled, httpPassword } = deviceSettings.value;
    if (!httpAuthEnabled) {
      return { value: httpAuthWasEnabled ? "" : undefined };
    }
    if (!httpPassword) {
      // Blank keeps an existing password; there is nothing to keep when auth
      // is being switched on, and saving would silently leave it off.
      return httpAuthWasEnabled
        ? { value: undefined }
        : { error: "Enter a password to require one for this device" };
    }
    if (new TextEncoder().encode(httpPassword).length > HTTP_PASSWORD_MAX_BYTES) {
      return { error: `Device password is too long (max ${HTTP_PASSWORD_MAX_BYTES} bytes)` };
    }
    return { value: httpPassword };
  }

  async function saveDeviceSettings() {
    const currentConfig = {
      auto_rotate: deviceSettings.value.autoRotate,
      rotate_cron: deviceSettings.value.rotateCron,
      display_rotation_deg: deviceSettings.value.displayRotationDeg,
      rotation_mode: deviceSettings.value.rotationMode,
      sd_rotation_mode: deviceSettings.value.sdRotationMode,
      image_url: deviceSettings.value.imageUrl,
      telegram_bot_token: deviceSettings.value.telegramBotToken,
      telegram_chat_id: deviceSettings.value.telegramChatId,
      telegram_pairing_enabled: deviceSettings.value.telegramPairingEnabled,
      telegram_wake_notify_enabled: deviceSettings.value.telegramWakeNotifyEnabled,
      ha_url: deviceSettings.value.haUrl,
      ha_enabled: deviceSettings.value.haEnabled,
      deep_sleep_enabled: deviceSettings.value.deepSleepEnabled,
      ota_check_enabled: deviceSettings.value.otaCheckEnabled,
      wifi_performance_mode_enabled: deviceSettings.value.wifiPerformanceModeEnabled,
      wifi_tx_power_cap_enabled: deviceSettings.value.wifiTxPowerCapEnabled,
      wifi_extended_retry_enabled: deviceSettings.value.wifiExtendedRetryEnabled,
      wifi_reprovision_on_fail_enabled: deviceSettings.value.wifiReprovisionOnFailEnabled,
      https_enabled: deviceSettings.value.httpsEnabled,
      rotation_pairing_enabled: deviceSettings.value.rotationPairingEnabled,
      variant_selection_enabled: deviceSettings.value.variantSelectionEnabled,
      telegram_rotation_notify_enabled: deviceSettings.value.telegramRotationNotifyEnabled,
      telegram_fallback_rotation_enabled: deviceSettings.value.telegramFallbackRotationEnabled,
      telegram_fallback_on_error_enabled: deviceSettings.value.telegramFallbackOnErrorEnabled,
      telegram_power_save_enabled: deviceSettings.value.telegramPowerSaveEnabled,
      telegram_power_save_latest_only: deviceSettings.value.telegramPowerSaveLatestOnly,
      telegram_keep_originals_enabled: deviceSettings.value.telegramKeepOriginalsEnabled,
      telegram_image_format: deviceSettings.value.telegramImageFormat,
      telegram_dedup_enabled: deviceSettings.value.telegramDedupEnabled,
      weather_overlay_enabled: deviceSettings.value.weatherOverlayEnabled,
      weather_location_name: deviceSettings.value.weatherLocationName,
      weather_lat: deviceSettings.value.weatherLat,
      weather_lon: deviceSettings.value.weatherLon,
      weather_provider: deviceSettings.value.weatherProvider,
      headlines_overlay_enabled: deviceSettings.value.headlinesOverlayEnabled,
      headlines_rss_url: deviceSettings.value.headlinesRssUrl,
      headlines_count: deviceSettings.value.headlinesCount,
      headlines_wrap_lines: deviceSettings.value.headlinesWrapLines,
      overlay_invert_colors: deviceSettings.value.overlayInvertColors,
      overlay_epdgz_enabled: deviceSettings.value.overlayEpdgzEnabled,
      overlay_language: deviceSettings.value.overlayLanguage,
      caption_invert_colors_enabled: deviceSettings.value.captionInvertColorsEnabled,
      weather_multiline_enabled: deviceSettings.value.weatherMultilineEnabled,
      weather_icon_set: deviceSettings.value.weatherIconSet,
      weather_icon_colored: deviceSettings.value.weatherIconColored,
      show_exif_datetime_enabled: deviceSettings.value.showExifDatetimeEnabled,
      low_battery_overlay_enabled: deviceSettings.value.lowBatteryOverlayEnabled,
      low_battery_overlay_threshold: deviceSettings.value.lowBatteryOverlayThreshold,
      battery_history_backup_enabled: deviceSettings.value.batteryHistoryBackupEnabled,
      chime_speaker_mode: deviceSettings.value.chimeSpeakerMode,
      chime_volume: deviceSettings.value.chimeVolume,
      chime_quiet_enabled: deviceSettings.value.chimeQuietEnabled,
      chime_quiet_start: deviceSettings.value.chimeQuietStart,
      chime_quiet_end: deviceSettings.value.chimeQuietEnd,
      chime_event_rotation_enabled: deviceSettings.value.chimeEventRotationEnabled,
      chime_event_telegram_photo_enabled: deviceSettings.value.chimeEventTelegramPhotoEnabled,
      chime_event_low_battery_enabled: deviceSettings.value.chimeEventLowBatteryEnabled,
      chime_event_wifi_reprovision_enabled: deviceSettings.value.chimeEventWifiReprovisionEnabled,
      chime_event_agenda_due_enabled: deviceSettings.value.chimeEventAgendaDueEnabled,
      chime_event_ota_success_enabled: deviceSettings.value.chimeEventOtaSuccessEnabled,
      chime_event_critical_error_enabled: deviceSettings.value.chimeEventCriticalErrorEnabled,
      climate_room_type: deviceSettings.value.climateRoomType,
      climate_temp_unit: deviceSettings.value.climateTempUnit,
      climate_logging_enabled: deviceSettings.value.climateLoggingEnabled,
      climate_history_backup_enabled: deviceSettings.value.climateHistoryBackupEnabled,
      climate_overlay_enabled: deviceSettings.value.climateOverlayEnabled,
      climate_agenda_header_enabled: deviceSettings.value.climateAgendaHeaderEnabled,
      climate_temp_offset: deviceSettings.value.climateTempOffset,
      climate_hum_offset: deviceSettings.value.climateHumOffset,
      alarm_cron: deviceSettings.value.alarmCron,
      alarm_ring_duration_sec: deviceSettings.value.alarmRingDurationSec,
      agenda_todo_enabled: deviceSettings.value.agendaTodoEnabled,
      agenda_cal_enabled: deviceSettings.value.agendaCalEnabled,
      agenda_cal_c_enabled: deviceSettings.value.agendaCalCEnabled,
      agenda_cal_d_enabled: deviceSettings.value.agendaCalDEnabled,
      agenda_cal_e_enabled: deviceSettings.value.agendaCalEEnabled,
      agenda_cal_c_name: deviceSettings.value.agendaCalCName,
      agenda_cal_d_name: deviceSettings.value.agendaCalDName,
      agenda_cal_e_name: deviceSettings.value.agendaCalEName,
      agenda_cal_name: deviceSettings.value.agendaCalName,
      agenda_cal_name2: deviceSettings.value.agendaCalName2,
      agenda_cal_days: deviceSettings.value.agendaCalDays,
      agenda_cal_weather_enabled: deviceSettings.value.agendaCalWeatherEnabled,
      agenda_cal_weather_right_aligned: deviceSettings.value.agendaCalWeatherRightAligned,
      agenda_cal_multiday_mode: deviceSettings.value.agendaCalMultidayMode,
      agenda_cal_time_display_mode: deviceSettings.value.agendaCalTimeDisplayMode,
      agenda_cal_layout_mode: deviceSettings.value.agendaCalLayoutMode,
      agenda_shift_model: deviceSettings.value.agendaShiftModel,
      agenda_shift_start: deviceSettings.value.agendaShiftStart,
      agenda_cron: deviceSettings.value.agendaCron,
      agenda_stack_layout: deviceSettings.value.agendaStackLayout,
      agenda_color_profile_active: deviceSettings.value.agendaColorProfileActive,
      agenda_pri_a_color: deviceSettings.value.agendaPriAColor,
      agenda_pri_b_color: deviceSettings.value.agendaPriBColor,
      agenda_pri_c_color: deviceSettings.value.agendaPriCColor,
      agenda_pri_d_color: deviceSettings.value.agendaPriDColor,
      agenda_due_overdue_color: deviceSettings.value.agendaDueOverdueColor,
      agenda_due_today_color: deviceSettings.value.agendaDueTodayColor,
      agenda_due_later_color: deviceSettings.value.agendaDueLaterColor,
      agenda_project_color: deviceSettings.value.agendaProjectColor,
      agenda_context_color: deviceSettings.value.agendaContextColor,
      debug_log_enabled: deviceSettings.value.debugLogEnabled,
      error_overlay_enabled: deviceSettings.value.errorOverlayEnabled,
      save_downloaded_images: deviceSettings.value.saveDownloadedImages,
      display_orientation: deviceSettings.value.displayOrientation,
      device_name: deviceSettings.value.deviceName,
      ntp_server: deviceSettings.value.ntpServer,
      ip_mode: deviceSettings.value.ipMode,
      static_ip: deviceSettings.value.staticIp,
      static_netmask: deviceSettings.value.staticNetmask,
      static_gateway: deviceSettings.value.staticGateway,
      dns_server: deviceSettings.value.dnsServer,
      timezone: deviceSettings.value.timezone,
      access_token: deviceSettings.value.accessToken,
      http_header_key: deviceSettings.value.httpHeaderKey,
      http_header_value: deviceSettings.value.httpHeaderValue,
      wifi_ssid: deviceSettings.value.wifiSsid,
      openai_api_key: deviceSettings.value.aiCredentials.openaiApiKey,
      google_api_key: deviceSettings.value.aiCredentials.googleApiKey,
    };

    // Only include password if it's been changed (not empty)
    if (deviceSettings.value.wifiPassword && deviceSettings.value.wifiPassword.length > 0) {
      currentConfig.wifi_password = deviceSettings.value.wifiPassword;
    }

    // Same write-only treatment for the ToDo/Calendar feed URLs (any of
    // them can carry a credential embedded as a query param, not just the
    // Calendar URL's Google-documented "secret address" case) - never
    // round-tripped from GET /api/config, so only send one when the user
    // actually typed a new value.
    if (deviceSettings.value.agendaTodoUrl && deviceSettings.value.agendaTodoUrl.length > 0) {
      currentConfig.agenda_todo_url = deviceSettings.value.agendaTodoUrl;
    }
    if (deviceSettings.value.agendaCalUrl && deviceSettings.value.agendaCalUrl.length > 0) {
      currentConfig.agenda_cal_url = deviceSettings.value.agendaCalUrl;
    }
    if (deviceSettings.value.agendaCalUrl2 && deviceSettings.value.agendaCalUrl2.length > 0) {
      currentConfig.agenda_cal_url2 = deviceSettings.value.agendaCalUrl2;
    }
    if (deviceSettings.value.agendaCalCUrl && deviceSettings.value.agendaCalCUrl.length > 0) {
      currentConfig.agenda_cal_c_url = deviceSettings.value.agendaCalCUrl;
    }
    if (deviceSettings.value.agendaCalDUrl && deviceSettings.value.agendaCalDUrl.length > 0) {
      currentConfig.agenda_cal_d_url = deviceSettings.value.agendaCalDUrl;
    }
    if (deviceSettings.value.agendaCalEUrl && deviceSettings.value.agendaCalEUrl.length > 0) {
      currentConfig.agenda_cal_e_url = deviceSettings.value.agendaCalEUrl;
    }

    // Compare with original config and only send changed fields.
    // Arrays (rotate_cron) need a value comparison, not reference equality.
    const changedFields = {};
    for (const key in currentConfig) {
      const cur = currentConfig[key];
      const orig = originalConfig[key];
      const differs =
        Array.isArray(cur) || Array.isArray(orig)
          ? JSON.stringify(cur) !== JSON.stringify(orig)
          : cur !== orig;
      if (differs) {
        changedFields[key] = cur;
      }
    }

    // Only a rule that is about to be sent is checked, so an odd value that
    // an older firmware let through can't block unrelated saves.
    if (changedFields.timezone !== undefined) {
      const tzError = validateTimezone(changedFields.timezone);
      if (tzError) {
        return { success: false, message: tzError };
      }
    }

    const passwordChange = httpPasswordChange();
    if (passwordChange.error) {
      return { success: false, message: passwordChange.error };
    }
    if (passwordChange.value !== undefined) {
      changedFields.http_password = passwordChange.value;
    }

    // If nothing changed, return success
    if (Object.keys(changedFields).length === 0) {
      return { success: true, message: "No changes to save" };
    }

    // Check if WiFi credentials are being changed
    const wifiChanging =
      changedFields.wifi_ssid !== undefined || changedFields.wifi_password !== undefined;

    // The WiFi flow below polls /api/config to confirm the reconnect. If the
    // same save switched on the password, those polls would carry no
    // credentials, get a 401 and report a failed reconnect.
    if (wifiChanging && changedFields.http_password) {
      return {
        success: false,
        message: "Change WiFi and the device password in separate saves",
      };
    }

    // If WiFi is changing, expect connection reset and handle specially
    if (wifiChanging) {
      const targetSsid = changedFields.wifi_ssid || deviceSettings.value.wifiSsid;

      try {
        // Send the PATCH request (will likely fail with connection reset)
        await fetch(`${API_BASE}/api/config`, {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(changedFields),
        });
      } catch (error) {
        // Expected - connection will reset when WiFi switches
        console.log("Connection reset during WiFi change (expected):", error.message);
      }

      // Wait for device to switch networks
      await new Promise((resolve) => setTimeout(resolve, 2000));

      // Retry logic to check if device is back online
      const maxRetries = 10;
      const retryDelay = 2000; // 2 seconds

      for (let i = 0; i < maxRetries; i++) {
        try {
          const statusResponse = await fetch(`${API_BASE}/api/config`, {
            method: "GET",
            signal: AbortSignal.timeout(3000), // 3 second timeout
          });

          console.log(`Retry ${i + 1}: status=${statusResponse.status}, ok=${statusResponse.ok}`);

          if (statusResponse.ok) {
            const data = await statusResponse.json();
            console.log(`Retry ${i + 1}: wifi_ssid="${data.wifi_ssid}", target="${targetSsid}"`);

            // Check if WiFi SSID actually changed
            if (data.wifi_ssid === targetSsid) {
              // Success! WiFi changed to new network
              console.log("WiFi change successful!");
              await loadDeviceSettings();
              return { success: true, message: "WiFi settings updated successfully" };
            } else {
              // Device came back but on old network (connection failed)
              console.log("WiFi change failed - device reverted to old network");
              await loadDeviceSettings();
              return {
                success: false,
                message:
                  "Failed to connect to new WiFi network. Device reverted to previous network.",
              };
            }
          }
        } catch (retryError) {
          // Connection failed, retry
          console.log(`Retry ${i + 1}/${maxRetries} failed:`, retryError.message);
          await new Promise((resolve) => setTimeout(resolve, retryDelay));
        }
      }

      // If we get here, device didn't come back online
      return {
        success: false,
        message: "WiFi changed but device did not reconnect. Please check your network settings.",
      };
    }

    // Normal save flow for non-WiFi changes
    try {
      const response = await fetch(`${API_BASE}/api/config`, {
        method: "PATCH",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(changedFields),
      });

      const data = await response.json();

      if (data.status === "success") {
        // Update original config with new values. The write-only password
        // is not part of it; record only whether one is now set.
        const { http_password: savedPassword, ...savedFields } = changedFields;
        Object.assign(originalConfig, savedFields);
        if (savedFields.timezone !== undefined) {
          savedTimezone.value = savedFields.timezone;
        }
        if (savedPassword !== undefined) {
          deviceSettings.value.httpAuthWasEnabled = savedPassword !== "";
          deviceSettings.value.httpPassword = "";
        }
        appliedOrientation.value = deviceSettings.value.displayOrientation;
        return { success: true, message: "Settings saved successfully" };
      } else {
        return { success: false, message: data.message || "Failed to save settings" };
      }
    } catch (error) {
      console.error("Error saving config:", error);
      return { success: false, message: "Error saving settings" };
    }
  }

  async function loadPalette() {
    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`);
      if (!response.ok || response.headers.get("content-type")?.includes("text/html")) {
        return;
      }
      const data = await response.json();
      palette.value = data;
    } catch (_error) {
      console.log("Palette API not available (standalone mode)");
    }
  }

  // Everything persisted to the device: the preset params plus the layout
  // fields, which are deliberately not part of preset identity
  const persistedKeys = [...presetKeys, "scaleMode", "backgroundColor"];

  function hasProcessingSettingsChanged() {
    const current = params.value;
    for (const key of persistedKeys) {
      if (current[key] !== originalParams[key]) {
        return true;
      }
    }
    return false;
  }

  async function saveSettings() {
    // Skip save if nothing changed
    if (!hasProcessingSettingsChanged()) {
      return true;
    }

    try {
      const response = await fetch(`${API_BASE}/api/settings/processing`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(params.value),
      });
      if (response.ok) {
        // Update original params after successful save
        originalParams = JSON.parse(JSON.stringify(params.value));
      }
      return response.ok;
    } catch (error) {
      console.error("Failed to save settings:", error);
      return false;
    }
  }

  async function savePalette() {
    try {
      const response = await fetch(`${API_BASE}/api/settings/palette`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(palette.value),
      });
      return response.ok;
    } catch (error) {
      console.error("Failed to save palette:", error);
      return false;
    }
  }

  async function factoryReset() {
    try {
      const response = await fetch(`${API_BASE}/api/factory-reset`, {
        method: "POST",
      });

      if (response.ok) {
        return {
          success: true,
          message:
            "Factory reset successful. Device is restarting... Connect to the 'PhotoFrame' WiFi network to reconfigure.",
        };
      } else {
        return { success: false, message: "Failed to perform factory reset" };
      }
    } catch (error) {
      console.error("Error performing factory reset:", error);
      return { success: false, message: "Error performing factory reset" };
    }
  }

  // On-demand offline hotspot (github.com/aitjcize/esp32-photoframe#90) -
  // mirrors the long-BOOT-hold trigger. Switching WiFi mode drops the very
  // connection this request travels over, so the response may never arrive
  // even on success - the server sends it before actually switching, but
  // that race is best-effort by nature; the caller should tell the user to
  // reconnect via the returned SSID regardless of whether this resolves.
  async function startApHotspot() {
    try {
      const response = await fetch(`${API_BASE}/api/wifi/hotspot/start`, { method: "POST" });
      const data = await response.json().catch(() => ({}));
      deviceSettings.value.apHotspotActive = true;
      return { success: true, ssid: data.ssid || "", url: data.url || "http://192.168.4.1" };
    } catch {
      // Expected on success too (see comment above) - still report it as
      // started so the UI shows the "reconnect to the hotspot" guidance.
      deviceSettings.value.apHotspotActive = true;
      return { success: true, ssid: "", url: "http://192.168.4.1" };
    }
  }

  async function stopApHotspot() {
    try {
      await fetch(`${API_BASE}/api/wifi/hotspot/stop`, { method: "POST" });
    } catch {
      // Same best-effort caveat as startApHotspot() above.
    }
    deviceSettings.value.apHotspotActive = false;
  }

  return {
    activeSettingsTab,
    params,
    uploadImageFormat,
    deviceSettings,
    savedTimezone,
    appliedOrientation,
    palette,
    preset,
    presetNames,
    startApHotspot,
    stopApHotspot,
    applyPreset,
    applyGrayscaleDefaultIfUntouched,
    loadSettings,
    loadDeviceSettings,
    saveDeviceSettings,

    loadPalette,
    saveSettings,
    savePalette,
    factoryReset,
  };
});
