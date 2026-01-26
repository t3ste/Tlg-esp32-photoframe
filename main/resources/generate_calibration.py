#!/usr/bin/env python3
"""
Generate a calibration PNG with 6 color boxes for E6 display calibration.
The PNG uses the theoretical palette values that the firmware expects.
"""

import sys

import png

# Display dimensions
WIDTH = 800
HEIGHT = 480

# Theoretical E6 palette (matches firmware)
PALETTE = [
    (0, 0, 0),  # 0: Black
    (255, 255, 255),  # 1: White
    (255, 255, 0),  # 2: Yellow
    (255, 0, 0),  # 3: Red
    (0, 0, 0),  # 4: Reserved (unused)
    (0, 0, 255),  # 5: Blue
    (0, 255, 0),  # 6: Green
]

# Color indices to use (skip reserved color 4)
COLOR_INDICES = [0, 1, 2, 3, 5, 6]  # Black, White, Yellow, Red, Blue, Green


def create_calibration_png(output_path):
    """Create a 24-bit RGB PNG with 6 color boxes arranged in 2 rows x 3 columns."""

    # Calculate box dimensions - no padding, boxes fill entire display
    box_width = WIDTH // 3
    box_height = HEIGHT // 2

    # Create image data as list of rows
    # Each row is a list of RGB values (flattened: R, G, B, R, G, B, ...)
    rows = []

    for y in range(HEIGHT):
        row = []
        # Determine which box this row belongs to
        box_row = y // box_height

        for x in range(WIDTH):
            # Determine which box this pixel belongs to
            box_col = x // box_width

            # Calculate box index (0-5)
            box_idx = box_row * 3 + box_col
            if box_idx < len(COLOR_INDICES):
                color_idx = COLOR_INDICES[box_idx]
                r, g, b = PALETTE[color_idx]
            else:
                # Should not happen with proper dimensions
                r, g, b = 0, 0, 0

            row.extend([r, g, b])

        rows.append(row)

    # Write PNG file
    with open(output_path, "wb") as f:
        writer = png.Writer(width=WIDTH, height=HEIGHT, greyscale=False)
        writer.write(f, rows)

    print(f"Calibration PNG created: {output_path}")
    print(f"Image size: {WIDTH}x{HEIGHT}")
    print(f"Box arrangement: 2 rows x 3 columns")
    print(f"Colors: Black, White, Yellow, Red, Blue, Green")


if __name__ == "__main__":
    output_file = sys.argv[1] if len(sys.argv) > 1 else "calibration.png"
    create_calibration_png(output_file)
