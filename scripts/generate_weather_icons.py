#!/usr/bin/env python3
"""Generate the weather-condition icon bitmap header(s) from source PNGs.

One-off/manual tool (like the vendored Font24 table, not run by build.py):
regenerate only if an icon set or the target size changes.

Two selectable icon sets, both mapped onto the same WMO-code-derived icon
index (see ICON_CATEGORIES) so the firmware's marker-byte encoding and
rendering code don't need to know which set is active, only a bitmap table
pointer:

- "flaticon": InkyPi project's weather plugin icons (Flaticon-licensed free
  tier - see the attribution note in README.md's Credits section), source
  PNGs in _icons/src_png/.
- "metno": MET Norway / yr.no's official weathericons (MIT licensed,
  https://github.com/metno/weathericons) - this project already uses yr.no
  as a weather data source. Source PNGs in _icons/metno_src/.

Both source sets are 512x/200x-ish flat-color or gradient-fill PNGs with a
transparent background - thresholding on color LUMINANCE was tried first
and failed badly (bright fill colors like a yellow sun read as
"background-bright" and got dropped, leaving only stray dark gradient
edges). Thresholding on the ALPHA channel instead - "is this pixel part of
the icon's silhouette at all" - correctly recovers the full shape
regardless of fill color.

One size only (24px, matching Font24's text height exactly) - used both by
the photo overlay bar and by Agenda mode's Calendar day-divider weather
chip, which shares a fixed row height with the rest of that grid and can't
grow it. An earlier version generated a second, larger 28px variant for the
overlay bar only; simplified back down to one size since 24px already reads
fine and one size is simpler firmware.

Output: main/weather_icons_data.h with two independent bitmap tables
(weather_icon_table_flaticon / weather_icon_table_metno), plus a preview
grid per set under _icons/preview_<set>.png for visual inspection.
"""

import os

from PIL import Image

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
OUTPUT_HEADER = os.path.join(REPO_ROOT, "main", "weather_icons_data.h")

ICON_WIDTH = 24
ICON_HEIGHT = 24
ALPHA_THRESHOLD = 128

# icon id (0..N-1) -> (C identifier, meaning). This ordering is the byte
# value (0x01 + index) embedded in weather overlay lines - both source
# filename lists below MUST be in this exact same order, and
# main/weather.c's weather_code_to_icon_id() must return indices into it.
# "thunderstorm" and "thunderstorm_hail" deliberately share the same source
# icon in both sets (neither has a distinct hail glyph) but get separate
# ids anyway, so image_processor.c's per-icon-id severity color table (see
# weather_icon_color_for_id()) can tell the two apart even though they look
# identical - see docs/DIFF.md's weather-icon-colors entry for why.
ICON_CATEGORIES = [
    ("clear", "Clear sky (WMO 0)"),
    ("mostly_clear", "Mainly clear (WMO 1)"),
    ("partly_cloudy", "Partly cloudy (WMO 2)"),
    ("overcast", "Overcast (WMO 3)"),
    ("fog", "Fog (WMO 45)"),
    ("icy_fog", "Icy fog (WMO 48)"),
    ("rain_light", "Drizzle/rain/showers - light (WMO 51,61,80)"),
    ("rain_moderate", "Drizzle/rain/showers - moderate (WMO 53,63,81)"),
    ("rain_heavy", "Drizzle/rain/showers - heavy (WMO 55,65,82)"),
    ("freezing_drizzle_light", "Freezing drizzle - light (WMO 56,66)"),
    ("freezing_drizzle", "Freezing drizzle (WMO 57,67)"),
    ("snow_light", "Snow - light (WMO 71,85)"),
    ("snow_moderate", "Snow - moderate (WMO 73)"),
    ("snow_heavy", "Snow - heavy (WMO 75,86)"),
    ("snow_grains", "Snow grains (WMO 77)"),
    ("thunderstorm", "Thunderstorm (WMO 95)"),
    ("thunderstorm_hail", "Thunderstorm with hail (WMO 96,99)"),
]

# One source PNG basename per category, per set (see ICON_CATEGORIES order).
ICON_SETS = {
    "flaticon": {
        "src_dir": os.path.join(REPO_ROOT, "_icons", "src_png"),
        "files": [
            "01d", "022d", "02d", "04d", "50d", "48d", "51d", "53d", "09d",
            "56d", "57d", "71d", "73d", "13d", "77d", "11d", "11d",
        ],
    },
    "metno": {
        "src_dir": os.path.join(REPO_ROOT, "_icons", "metno_src"),
        # No distinct "icy fog"/"snow grains" icon in this set - reuse the
        # closest match (fog / snow), same fallback approach as flaticon's
        # 48d/77d choices.
        "files": [
            "clearsky_day", "fair_day", "partlycloudy_day", "cloudy", "fog", "fog",
            "lightrain", "rain", "heavyrain", "lightsleet", "sleet",
            "lightsnow", "snow", "heavysnow", "snow", "rainandthunder", "rainandthunder",
        ],
    },
}


def load_and_threshold(src_path):
    img = Image.open(src_path).convert("RGBA")
    resized = img.resize((ICON_WIDTH, ICON_HEIGHT), Image.LANCZOS)
    return [
        [1 if resized.getpixel((x, y))[3] >= ALPHA_THRESHOLD else 0 for x in range(ICON_WIDTH)]
        for y in range(ICON_HEIGHT)
    ]


def pack_msb_first(bitmap):
    """Pack a [row][col] 0/1 bitmap MSB-first per row, matching draw_glyph()."""
    bytes_per_row = (ICON_WIDTH + 7) // 8
    out = bytearray(bytes_per_row * ICON_HEIGHT)
    for y in range(ICON_HEIGHT):
        for x in range(ICON_WIDTH):
            if bitmap[y][x]:
                out[y * bytes_per_row + x // 8] |= 0x80 >> (x % 8)
    return out


def write_preview(set_name, bitmaps):
    UPSCALE = 8
    cols = 8
    rows = (len(ICON_CATEGORIES) + cols - 1) // cols
    cell = ICON_WIDTH * UPSCALE + 8
    grid = Image.new("L", (cols * cell, rows * (ICON_HEIGHT * UPSCALE + 8)), 255)
    for idx, bitmap in enumerate(bitmaps):
        icon_img = Image.new("L", (ICON_WIDTH, ICON_HEIGHT), 255)
        for y in range(ICON_HEIGHT):
            for x in range(ICON_WIDTH):
                if bitmap[y][x]:
                    icon_img.putpixel((x, y), 0)
        icon_img = icon_img.resize((ICON_WIDTH * UPSCALE, ICON_HEIGHT * UPSCALE), Image.NEAREST)
        gx, gy = (idx % cols) * cell + 4, (idx // cols) * (ICON_HEIGHT * UPSCALE + 8) + 4
        grid.paste(icon_img, (gx, gy))
    preview_path = os.path.join(REPO_ROOT, "_icons", f"preview_{set_name}.png")
    grid.save(preview_path)
    return preview_path


def main():
    header_lines = [
        "// Auto-generated by scripts/generate_weather_icons.py - do not edit by",
        "// hand. Re-run the script (after adjusting ICON_WIDTH/HEIGHT, ICON_SETS,",
        "// or the source PNGs) to regenerate. 1bpp, MSB-first per row, matching",
        "// image_processor.c's draw_glyph() bit order exactly.",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        f"#define WEATHER_ICON_WIDTH {ICON_WIDTH}",
        f"#define WEATHER_ICON_HEIGHT {ICON_HEIGHT}",
        f"#define WEATHER_ICON_COUNT {len(ICON_CATEGORIES)}",
        "",
    ]

    for set_name, set_info in ICON_SETS.items():
        bitmaps = [
            load_and_threshold(os.path.join(set_info["src_dir"], f"{fname}.png"))
            for fname in set_info["files"]
        ]
        packed_icons = [pack_msb_first(b) for b in bitmaps]

        for (ident, meaning), packed in zip(ICON_CATEGORIES, packed_icons):
            header_lines.append(f"// {meaning}")
            header_lines.append(f"static const uint8_t weather_icon_{set_name}_{ident}[] = {{")
            row_strs = []
            for i in range(0, len(packed), 12):
                chunk = packed[i : i + 12]
                row_strs.append("    " + ", ".join(f"0x{b:02X}" for b in chunk))
            header_lines.append(",\n".join(row_strs))
            header_lines.append("};")
            header_lines.append("")

        header_lines.append(
            f"static const uint8_t *const weather_icon_table_{set_name}[WEATHER_ICON_COUNT] = {{"
        )
        for ident, _ in ICON_CATEGORIES:
            header_lines.append(f"    weather_icon_{set_name}_{ident},")
        header_lines.append("};")
        header_lines.append("")

        preview_path = write_preview(set_name, bitmaps)
        total_bytes = sum(len(p) for p in packed_icons)
        print(f"{set_name}: {len(ICON_CATEGORIES)} icons, {total_bytes} bytes, preview at {preview_path}")

    with open(OUTPUT_HEADER, "w", encoding="utf-8") as f:
        f.write("\n".join(header_lines) + "\n")
    print(f"Wrote {OUTPUT_HEADER}")


if __name__ == "__main__":
    main()
