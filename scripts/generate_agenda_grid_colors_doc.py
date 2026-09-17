#!/usr/bin/env python3
"""Mirrors agenda_renderer.c's contrast helpers exactly (agenda_is_light,
agenda_safe_text_color, agenda_avoid_bg_collision, agenda_ensure_contrast)
to generate a verifiably-accurate HTML color matrix for the 7-day grid's
shift-model interaction with Calendar event text colors - see the
"Contrast problems" feedback that prompted this doc.

Run manually and paste the output into docs/AGENDA_COLORS.html (Section 8)
whenever agenda_renderer.c's contrast logic changes - not part of the
routine build.py pipeline, same category as the weather icon color doc
generator.
"""

import sys

HUES = {
    "Red": (255, 0, 0),
    "Yellow": (255, 255, 0),
    "Green": (0, 255, 0),
    "Blue": (0, 0, 255),
}
PAGE_BG = {
    "White": (255, 255, 255),
    "Black": (0, 0, 0),
}


def is_light(rgb):
    r, g, b = rgb
    return (r * 299 + g * 587 + b * 114) > 127500


def safe_text_color(bg):
    return (0, 0, 0) if is_light(bg) else (255, 255, 255)


def avoid_collision(bg, fg):
    return safe_text_color(bg) if fg == bg else fg


def hexc(rgb):
    return "#%02x%02x%02x" % rgb


def calendar_text_color(hue_name, page_bg):
    # resolve_plain_color(): role hue, then agenda_avoid_bg_collision()
    # against the page background - this is line->fr/fg/fb, precomputed
    # once at build_event_line() time.
    return avoid_collision(page_bg, HUES[hue_name])


def final_event_color(hue_name, page_bg, shift_bg):
    base = calendar_text_color(hue_name, page_bg)
    if shift_bg is None:
        return base, False
    # agenda_ensure_contrast() - only reached for whole-cell scope, only
    # for a plain (no has_bg) event line, i.e. every Calendar event today.
    fixed = avoid_collision(shift_bg, base)
    return fixed, fixed != base


def chip_html(bg, fg, label, note=""):
    return (
        f'<span class="chip" style="background:{hexc(bg)};color:{hexc(fg)};">{label}</span>'
        + (f" {note}" if note else "")
    )


def build_whole_cell_table(page_bg_name):
    page_bg = PAGE_BG[page_bg_name]
    cols = ["None (page bg)"] + list(HUES.keys())
    rows = []
    rows.append(
        "<tr><th>Calendar text color \\ Shift day background</th>"
        + "".join(f"<th>{c}</th>" for c in cols)
        + "</tr>"
    )
    for hue_name in HUES:
        cells = [f"<td><code>{hue_name}</code></td>"]
        for col in cols:
            shift_bg = None if col == "None (page bg)" else HUES[col]
            bg_for_swatch = page_bg if shift_bg is None else shift_bg
            fg, was_fixed = final_event_color(hue_name, page_bg, shift_bg)
            note = "&larr; auto-corrected" if was_fixed else ""
            cells.append(
                f'<td class="swatch-cell">{chip_html(bg_for_swatch, fg, "16:30 Termin", note)}</td>'
            )
        rows.append("<tr>" + "".join(cells) + "</tr>")
    return (
        f"<h3>Whole-cell scope, {page_bg_name} agenda background</h3>\n"
        f"<table><caption>Event text color actually drawn, by Calendar source color (rows) and the "
        f"day's shift-model background (columns) - '&larr; auto-corrected' marks a cell where "
        f"the source's own color exactly matched the shift background and "
        f"<code>agenda_ensure_contrast()</code> substituted a safe fallback instead.</caption>\n"
        + "\n".join(rows)
        + "\n</table>\n"
    )


def build_header_only_table(page_bg_name):
    page_bg = PAGE_BG[page_bg_name]
    cols = ["None (page bg)"] + list(HUES.keys())
    cells = []
    for col in cols:
        bg = page_bg if col == "None (page bg)" else HUES[col]
        fg = safe_text_color(bg)
        cells.append(f'<td class="swatch-cell">{chip_html(bg, fg, "Fr 18.")}</td>')
    header = "".join(f"<th>{c}</th>" for c in cols)
    return (
        f"<h3>Header-only scope, {page_bg_name} agenda background</h3>\n"
        f"<table><caption>The day-header label/weather-chip text color is derived directly from "
        f"its own background at draw time (<code>agenda_safe_text_color()</code>), never a stale "
        f"precomputed value - always safe by construction, shown here for completeness only.</caption>\n"
        f"<tr>{header}</tr>\n<tr>{''.join(cells)}</tr>\n</table>\n"
    )


def main():
    parts = []
    parts.append("<h2>8. 7-Day Grid &mdash; shift-model background interaction</h2>")
    parts.append(
        "<p>The 7-day grid's optional rotation/&ldquo;shift&rdquo; coloring "
        "(<code>agenda_shift_model_t</code>) paints a day's header bar (and, in "
        "&ldquo;whole cell&rdquo; scope, its event rows too) with one of 2 configurable "
        "group colors. Calendar event text color is resolved once, at "
        "<code>build_event_line()</code> time, against the page's own background - "
        "<strong>a day whose shift color happens to match that event's own Calendar "
        "source color would draw invisible text without a fix</strong> (reported live: "
        "green Calendar text on a green shift-colored day). "
        "<code>draw_calendar_grid_column()</code> now re-checks a plain event's text "
        "color against whatever background the row actually ends up on "
        "(<code>agenda_ensure_contrast()</code>) before drawing it - the tables below "
        "are generated directly from that same logic (see "
        "<code>scripts/generate_agenda_grid_colors_doc.py</code>), so they reflect real "
        "behavior, not a hand-drawn approximation.</p>"
    )
    for page_bg_name in PAGE_BG:
        parts.append(build_whole_cell_table(page_bg_name))
    for page_bg_name in PAGE_BG:
        parts.append(build_header_only_table(page_bg_name))
    parts.append(
        "<p><strong>Grayscale-only boards are unaffected</strong> - "
        "<code>resolve_plain_color()</code> always renders every Calendar source as "
        "plain black there (no spare hue to assign), and the shift-model colors "
        "resolve to plain black/white the same way "
        "(<code>resolve_plain_color()</code>'s own grayscale branch), so no collision "
        "is possible in the first place.</p>"
    )
    sys.stdout.reconfigure(encoding="utf-8")
    print("\n".join(parts))


if __name__ == "__main__":
    main()
