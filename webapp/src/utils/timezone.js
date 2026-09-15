// POSIX TZ helpers for Settings. The device stores/accepts the raw string
// (setenv("TZ", ...) in main/config_manager.c - a plain "UTC±H[:MM]" or a
// full DST rule such as "CET-1CEST,M3.5.0/2,M10.5.0/3"). The old UI instead
// round-tripped the value through a numeric UTC-offset field: parsing a
// DST-aware string against a "UTC±H" regex silently left the offset at 0,
// and saving with that 0 rewrote the device's real timezone to "UTC0" - not
// just on a deliberate edit, but on ANY Settings save. Keeping the raw
// string as the only source of truth (a v-combobox in SettingsPanel.vue,
// not a number field) removes the lossy round-trip entirely.

export const TIMEZONE_PRESETS = [
  { title: "UTC", value: "UTC0" },
  { title: "Amsterdam/Berlin (CET/CEST, DST-aware)", value: "CET-1CEST,M3.5.0/2,M10.5.0/3" },
  { title: "London (GMT/BST, DST-aware)", value: "GMT0BST,M3.5.0/1,M10.5.0" },
  { title: "US Eastern (EST/EDT, DST-aware)", value: "EST5EDT,M3.2.0/2,M11.1.0/2" },
  { title: "US Pacific (PST/PDT, DST-aware)", value: "PST8PDT,M3.2.0/2,M11.1.0/2" },
  { title: "UTC+1 (fixed, no DST)", value: "UTC-1" },
  { title: "UTC+2 (fixed, no DST)", value: "UTC-2" },
  { title: "UTC-5 (fixed, no DST)", value: "UTC5" },
  { title: "UTC-8 (fixed, no DST)", value: "UTC8" },
  { title: "UTC+8 (China)", value: "UTC-8" },
  { title: "UTC+5:30 (India)", value: "UTC-5:30" },
];

// Parses `/api/time`'s "time" field - the device's own already-localized
// wall-clock string ("YYYY-MM-DD HH:MM:SS", from localtime_r() against
// whatever TZ is actually set, DST included). Treated as a NAIVE local
// Date (the numbers as-is, no further timezone conversion) purely so the
// Settings page can tick it forward client-side between fetches - never
// used to derive an offset. Returns null if the string doesn't match.
export function parseDeviceWallClock(timeStr) {
  if (!timeStr) return null;
  const match = String(timeStr).match(/^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})$/);
  if (!match) return null;
  const [, year, month, day, hour, minute, second] = match.map(Number);
  return new Date(year, month - 1, day, hour, minute, second);
}

// Inverse of parseDeviceWallClock() - formats a naive local Date back into
// the same "YYYY-MM-DD HH:MM:SS" shape for display.
export function formatDeviceWallClock(date) {
  const pad = (n) => String(n).padStart(2, "0");
  return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())} ${pad(date.getHours())}:${pad(date.getMinutes())}:${pad(date.getSeconds())}`;
}
