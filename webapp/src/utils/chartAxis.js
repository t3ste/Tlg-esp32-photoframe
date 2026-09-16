// Shared x-axis tick label formatting for the history mini-charts
// (BatteryHistory.vue, ClimateHistory.vue) - a short data span (e.g. only a
// few hours/days of history) makes a date-only label useless, since all
// ticks then share the same day and only the hour actually varies.
export function formatTimeAxisTick(epochSeconds, spanSeconds) {
  const date = new Date(epochSeconds * 1000);
  const spanHours = spanSeconds / 3600;

  if (spanHours < 36) {
    return date.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
  }
  if (spanHours < 96) {
    const day = date.toLocaleDateString(undefined, { day: "numeric" });
    const time = date.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
    return `${day}., ${time}`;
  }
  return date.toLocaleDateString(undefined, { month: "short", day: "numeric" });
}
