#!/usr/bin/env python3
"""Generate docs/WEATHER_ICON_COLORS.html - a visual reference of every
weather icon (both selectable sets) rendered in its assigned traffic-light
severity color, on BOTH a light and a dark background, for reviewing/
adjusting the color choices and explaining their meaning to users.
Companion to docs/AGENDA_COLORS.html (same self-contained, light/dark-aware
HTML doc style).

Rendering on both backgrounds is deliberate, not decorative: the firmware
draws this icon in two contexts whose background can be either black or
white (the photo overlay bar's own bg/fg, or Agenda mode's day-divider chip,
which flips polarity to match the chosen page background) - a color choice
that looks fine on white but goes invisible on black (or vice versa) is
exactly the kind of bug this page exists to catch before it ships. Confirmed
live (2026-09-16): the "neutral" categories (partly cloudy/overcast/snow
grains) were a literal black RGB constant, invisible against a dark Agenda
background - fixed in image_processor.c to fall through to the caller's own
already-contrast-safe text color instead of a fixed black. That's why the
neutral column here shows black-on-white and white-on-black rather than a
single swatch: it deliberately has no fixed color of its own.

Reuses generate_weather_icons.py's own ICON_CATEGORIES/ICON_SETS/
load_and_threshold() so this can never drift out of sync with the actual
generated bitmaps - one source of truth for which source PNG maps to which
icon id.

The color-by-icon-id table below is a plain copy of
image_processor.c's weather_icon_color_for_id() - keep the two in sync by
hand if either changes (small, rarely-touched table).
"""

import base64
import io
import os
import sys

from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, SCRIPT_DIR)
from generate_weather_icons import (  # isort:skip
    ICON_CATEGORIES,
    ICON_HEIGHT,
    ICON_SETS,
    ICON_WIDTH,
    load_and_threshold,
)

OUTPUT_HTML = os.path.join(REPO_ROOT, "docs", "WEATHER_ICON_COLORS.html")

# Mirrors image_processor.c's weather_icon_color_for_id() exactly - name,
# hex (matching the firmware's theoretical palette[] values), and a short
# rationale. None = "neutral" (no fixed color - see module docstring).
# Keep in sync by hand if the C table changes.
COLORS_BY_ICON_ID = [
    ("Green", "#00ff00", "Good/light - no particular concern"),
    ("Green", "#00ff00", "Good/light - no particular concern"),
    (None, None, "Neutral - no warning implied, so no fixed color of its own"),
    (None, None, "Neutral - no warning implied, so no fixed color of its own"),
    ("Yellow", "#ffff00", "Caution - reduced visibility"),
    ("Red", "#ff0000", "Hazard - icy + reduced visibility"),
    (
        "Blue",
        "#0000ff",
        "Light - harmless, distinguished from clear/sunny green by shape not color",
    ),
    ("Yellow", "#ffff00", "Moderate - umbrella recommended"),
    ("Red", "#ff0000", "Heavy - storm risk"),
    ("Yellow", "#ffff00", "Light icing forming - drive carefully"),
    ("Red", "#ff0000", "Severe icing - acute hazard"),
    ("Blue", "#0000ff", "Classic snow color - minor impact"),
    ("Yellow", "#ffff00", "Increased snow - slip risk"),
    ("Red", "#ff0000", "Snowstorm - major impact"),
    (
        None,
        None,
        "Distinct from soft snow-blue, but still just neutral - no fixed color",
    ),
    ("Yellow", "#ffff00", "Lightning risk - increased attention"),
    ("Red", "#ff0000", "Hail damage potential"),
]

WHITE = (255, 255, 255)
BLACK = (0, 0, 0)
UPSCALE = 4  # 24px source -> 96px in the doc, crisp nearest-neighbor


def hex_to_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i : i + 2], 16) for i in range(0, 6, 2))


def render_icon_png_b64(bitmap, ink_rgb, bg_rgb):
    """1bpp bitmap -> upscaled PNG (ink_rgb on bg_rgb), base64 data URI."""
    img = Image.new("RGB", (ICON_WIDTH, ICON_HEIGHT), bg_rgb)
    for y in range(ICON_HEIGHT):
        for x in range(ICON_WIDTH):
            if bitmap[y][x]:
                img.putpixel((x, y), ink_rgb)
    img = img.resize((ICON_WIDTH * UPSCALE, ICON_HEIGHT * UPSCALE), Image.NEAREST)
    buf = io.BytesIO()
    img.save(buf, format="PNG")
    return base64.b64encode(buf.getvalue()).decode("ascii")


SET_LABELS = {
    "flaticon": "Flaticon (InkyPi)",
    "metno": "MET Norway / yr.no",
}


def main():
    set_bitmaps = {}
    for set_name, set_info in ICON_SETS.items():
        set_bitmaps[set_name] = [
            load_and_threshold(os.path.join(set_info["src_dir"], f"{fname}.png"))
            for fname in set_info["files"]
        ]

    rows_html = []
    for idx, (ident, meaning) in enumerate(ICON_CATEGORIES):
        color_name, color_hex, rationale = COLORS_BY_ICON_ID[idx]
        is_neutral = color_hex is None
        cells = []
        for set_name in ICON_SETS:
            bitmap = set_bitmaps[set_name][idx]
            # Monochrome column: on white the ink is black, on black the ink
            # is white - exactly the same "use whatever contrasts" logic the
            # firmware's default_color already follows, shown explicitly
            # rather than assumed.
            mono_html = (
                f'<div class="pair">'
                f'<img src="data:image/png;base64,{render_icon_png_b64(bitmap, BLACK, WHITE)}" class="on-white" title="Monochrome on white background">'
                f'<img src="data:image/png;base64,{render_icon_png_b64(bitmap, (255, 255, 255), BLACK)}" class="on-black" title="Monochrome on black background">'
                f"</div>"
            )
            if is_neutral:
                # Neutral: no fixed color - same adaptive black-on-white /
                # white-on-black behavior as monochrome mode, just shown
                # again under "colored" to make explicit that turning
                # "Colored icons" on changes nothing for this category.
                colored_html = mono_html
            else:
                ink = hex_to_rgb(color_hex)
                colored_html = (
                    f'<div class="pair">'
                    f'<img src="data:image/png;base64,{render_icon_png_b64(bitmap, ink, WHITE)}" class="on-white" title="{color_name} on white background">'
                    f'<img src="data:image/png;base64,{render_icon_png_b64(bitmap, ink, BLACK)}" class="on-black" title="{color_name} on black background">'
                    f"</div>"
                )
            cells.append(f'<td class="icon-cell">{mono_html}{colored_html}</td>')

        if is_neutral:
            swatch_cell = (
                '<td class="swatch-cell"><em>neutral - no fixed color</em></td>'
            )
        else:
            swatch_cell = f'<td class="swatch-cell"><span class="swatch" style="background:{color_hex}"></span>{color_name}</td>'

        rows_html.append(
            f"<tr>"
            f"<td>{meaning}</td>"
            f'{"".join(cells)}'
            f"{swatch_cell}"
            f"<td>{rationale}</td>"
            f"</tr>"
        )

    set_headers = "".join(f"<th>{SET_LABELS[s]}</th>" for s in ICON_SETS)

    html = f"""<!doctype html>
<html lang="en">
  <head>
    <meta charset="UTF-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1.0" />
    <title>Weather Icon Color Reference</title>
    <style>
      :root {{
        color-scheme: light dark;
        --bg: #ffffff;
        --fg: #1a1a1a;
        --muted: #5a5a5a;
        --border: #d8d8d8;
        --code-bg: #f2f2f2;
        --thead-bg: #eef0f3;
        --note-bg: #fff8e6;
        --note-border: #e0c264;
        --warn-bg: #fdeaea;
        --warn-border: #d98a8a;
        --link: #2b5fb0;
      }}
      @media (prefers-color-scheme: dark) {{
        :root {{
          --bg: #14161a;
          --fg: #e6e6e6;
          --muted: #a0a4ab;
          --border: #33363c;
          --code-bg: #1e2126;
          --thead-bg: #1e2126;
          --note-bg: #2a2410;
          --note-border: #6b5a1e;
          --warn-bg: #3a2020;
          --warn-border: #8a4a4a;
          --link: #7fb0ff;
        }}
      }}
      * {{ box-sizing: border-box; }}
      body {{
        background: var(--bg);
        color: var(--fg);
        font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Oxygen, Ubuntu,
          Cantarell, sans-serif;
        line-height: 1.55;
        max-width: 1200px;
        margin: 0 auto;
        padding: 32px 24px 80px;
      }}
      h1 {{ font-size: 1.9rem; margin-bottom: 6px; }}
      h2 {{ font-size: 1.35rem; margin-top: 2.4em; border-bottom: 1px solid var(--border); padding-bottom: 6px; }}
      p.lede {{ color: var(--muted); margin-top: 0; }}
      .note {{
        background: var(--note-bg);
        border: 1px solid var(--note-border);
        border-radius: 8px;
        padding: 14px 18px;
        margin: 1.4em 0;
      }}
      .warn {{
        background: var(--warn-bg);
        border: 1px solid var(--warn-border);
        border-radius: 8px;
        padding: 14px 18px;
        margin: 1.4em 0;
      }}
      table {{
        border-collapse: collapse;
        width: 100%;
        margin: 1.2em 0;
        font-size: 0.92rem;
      }}
      th, td {{
        border: 1px solid var(--border);
        padding: 8px 10px;
        text-align: left;
        vertical-align: middle;
      }}
      thead th {{ background: var(--thead-bg); }}
      .icon-cell {{ text-align: center; }}
      .pair {{
        display: inline-flex;
        margin: 3px 6px;
        border: 1px solid var(--border);
        border-radius: 4px;
        overflow: hidden;
      }}
      .pair img {{
        width: 40px;
        height: 40px;
        image-rendering: pixelated;
        display: block;
      }}
      .pair .on-white {{ background: #ffffff; }}
      .pair .on-black {{ background: #000000; }}
      .swatch-cell {{ white-space: nowrap; }}
      .swatch {{
        display: inline-block;
        width: 16px;
        height: 16px;
        border-radius: 3px;
        border: 1px solid rgba(128, 128, 128, 0.5);
        vertical-align: middle;
        margin-right: 6px;
      }}
      code {{
        background: var(--code-bg);
        padding: 1px 5px;
        border-radius: 4px;
        font-size: 0.9em;
      }}
      a {{ color: var(--link); }}
      .legend {{ display: flex; gap: 18px; flex-wrap: wrap; margin: 1em 0; }}
      .legend-item {{ display: flex; align-items: center; gap: 6px; font-size: 0.95rem; }}
      .row-label {{ font-weight: 600; margin-top: 1.6em; }}
    </style>
  </head>
  <body>
    <h1>Weather Icon Color Reference</h1>
    <p class="lede">
      Every weather-condition icon, in both selectable icon sets, shown on <strong>both a white
      and a black background</strong> - both monochrome (the default) and in its assigned
      traffic-light severity color (opt-in "Colored icons" setting, Settings &rarr; Overlays).
      Generated by <code>scripts/generate_weather_icon_colors_doc.py</code> from the exact same
      bitmaps and color table the firmware uses - never hand-edited pixel data, so this always
      reflects what the device actually draws.
    </p>

    <div class="warn">
      <strong>Why both backgrounds matter (not just a nice-to-have):</strong> the firmware draws
      this icon in two places whose background can be either black or white - the photo overlay
      bar (black by default, white if "Invert overlay colors" is on) and Agenda mode's day-divider
      chip (flips polarity to match the chosen page background). A color that looks fine on one
      background and disappears on the other is a real bug, not a style choice. This is exactly how
      the "neutral" categories (partly cloudy/overcast/snow grains) were caught going invisible - a
      literal black icon on a black Agenda background - and fixed to use whatever the surrounding
      text color already is (already contrast-safe for its own background) instead of a fixed
      black. That's why the neutral column shows an adaptive black-on-white / white-on-black pair
      rather than a single fixed-color swatch.
    </div>

    <div class="note">
      <strong>How to use this page:</strong> review whether each color assignment still makes
      sense on <em>both</em> backgrounds, then edit the <code>weather_icon_color_for_id()</code>
      table in <code>main/image_processor.c</code> (and the mirrored <code>COLORS_BY_ICON_ID</code>
      list at the top of the generator script, so this page stays in sync) if something should
      change. Colors are shown at the exact hex values the e-paper firmware uses
      (<code>palette[]</code> in <code>image_processor.c</code>) - not a web-safe approximation -
      so what you see here is what the hardware actually renders on a color-capable panel.
      Grayscale-only panels always fall back to plain black/white, regardless of this setting.
    </div>

    <h2>Color legend</h2>
    <div class="legend">
      <div class="legend-item"><span class="swatch" style="background:#00ff00"></span> Green - good / light, no particular concern</div>
      <div class="legend-item"><span class="swatch" style="background:#ffff00"></span> Yellow - moderate / caution</div>
      <div class="legend-item"><span class="swatch" style="background:#ff0000"></span> Red - severe / hazardous</div>
      <div class="legend-item"><span class="swatch" style="background:#0000ff"></span> Blue - snow (distinct from the rain scale)</div>
      <div class="legend-item"><em>Neutral</em> - no fixed color; adapts to whatever already contrasts with its background</div>
    </div>

    <h2>All icons ({len(ICON_CATEGORIES)} categories &times; {len(ICON_SETS)} sets)</h2>
    <table>
      <thead>
        <tr>
          <th>Condition</th>
          {set_headers}
          <th>Assigned color</th>
          <th>Rationale</th>
        </tr>
      </thead>
      <tbody>
        {"".join(rows_html)}
      </tbody>
    </table>

    <p class="lede">
      Each icon cell shows two side-by-side pairs: the default monochrome glyph (top) and the same
      glyph in colored-icon mode (bottom) - each pair itself shown on white (left) and black
      (right) background, so any background-dependent contrast problem is visible directly rather
      than needing to imagine it. Icon size is 24&times;24px on the actual device (Font24
      text-height match) - upscaled {UPSCALE}&times; here for legibility, so the edge pixelation
      shown is exactly what the real, small on-device icon looks like blown up, not an artifact of
      this page.
    </p>
  </body>
</html>
"""

    with open(OUTPUT_HTML, "w", encoding="utf-8") as f:
        f.write(html)
    print(f"Wrote {OUTPUT_HTML} ({os.path.getsize(OUTPUT_HTML) / 1024:.0f} KB)")


if __name__ == "__main__":
    main()
