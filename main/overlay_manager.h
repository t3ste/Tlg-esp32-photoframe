#ifndef OVERLAY_MANAGER_H
#define OVERLAY_MANAGER_H

/**
 * @brief Composites the weather/headline overlay (if either is enabled)
 * onto `source_path` for display, WITHOUT modifying `source_path` itself.
 *
 * Returns `source_path` unchanged when: neither overlay is enabled,
 * `source_path` isn't a processed PNG (e.g. BMP/EPDGZ - no RGB buffer to
 * draw into), or every enabled data source failed to fetch this cycle (a
 * transient network/parse failure, best-effort - never blocks the normal
 * rotation display). Otherwise returns CURRENT_OVERLAY_PNG_PATH, a scratch
 * copy of `source_path` with the overlay bar drawn on top - callers whose
 * returned path differs from `source_path` should re-mark
 * history_manager_mark_shown()/current-image identity to the real
 * `source_path` afterward (the scratch copy's content is only valid for the
 * current wake), mirroring the existing precedent in
 * process_and_display_telegram_image() (telegram_bot.c).
 *
 * At most one weather line (if enabled) followed by up to
 * config_manager_get_headlines_count() headline lines (if enabled) are
 * drawn as one top-anchored bar - never a second panel refresh, and the
 * original saved album file is never touched.
 */
const char *overlay_manager_apply(const char *source_path);

#endif
