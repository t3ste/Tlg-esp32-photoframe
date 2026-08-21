# Scale Mode: Cover vs. Fit

Controls how an image whose aspect ratio doesn't match the display's is resized: cropped to fill
the screen (default), or shown in full with letterbox bars.

- **Cover (crop to fill)** — default. Scales the image up to cover the whole display, then
  center-crops whatever doesn't fit. No bars, but content near the long edges may be cut off.
- **Fit (letterbox)** — scales the image down to fit entirely within the display, no cropping.
  The uncovered area (top/bottom or left/right, depending on the mismatch) is filled with a solid
  background color instead.

There is no "blur background" variant (a scaled/blurred copy of the image itself filling the
letterbox bars, as some photo-frame apps do) — only a solid color. This was considered and
deliberately not built: the display's heavily reduced color palette (dithered to a handful of
colors/grays) tends to turn a smooth blur into visible banding/blotches rather than a clean
gradient, so it wouldn't reliably look better than a solid bar.

## How to change it

**Web UI → Settings → Processing tab** (or the processing controls shown next to the preview in
upload's "wide edit" mode) → **Scale Mode** dropdown. When **Fit** is selected, a **Background
Color** dropdown appears (White or Black).

This is a **global default**: it applies to every image the device processes going forward —
Storage/SD album uploads, Telegram-received photos, and URL-rotation-mode images alike, regardless
of rotation mode. It's saved via `POST /api/settings/processing` (`scaleMode`: `"cover"` or
`"fit"`, `backgroundColor`: `"white"` or `"black"`) — see `docs/API.md`'s Processing Settings
section.

### Per-image override at upload time

Independent of the global default above, the **image upload preview** (Gallery → Upload, or
Landing Page) has a third option, **Custom**, alongside Cover and Fit: drag to pan and scroll to
zoom, letting you manually frame one specific photo differently from the global default. This only
affects that single upload — it's baked into the processed image at upload time, not a persistent
setting.

## Interaction with orientation pairing

Cover/Fit only come into play for an image shown **alone**. If **Telegram pairing**
(`/pairing`) or **auto-rotate orientation pairing** (`/rotation_pairing`) is enabled and a
mismatched-orientation image arrives, the device instead combines it with another
complementary-orientation image into one composed picture — no crop or letterbox bars needed at
all for that pair. See [Multi-image orientation pairing](TELEGRAM.md#multi-image-orientation-pairing)
and [Auto-rotate orientation pairing](TELEGRAM.md#auto-rotate-orientation-pairing) in the Telegram
docs. Scale Mode still applies to any image that isn't paired (no partner available, or pairing
disabled).
