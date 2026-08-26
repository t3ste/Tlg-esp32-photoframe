# Face-aware crop metadata (process-cli)

An opt-in `process-cli` feature: detect faces in a source photo, compute a crop rectangle that
keeps large faces fully visible for a given display's aspect ratio, and save that as a small
versioned JSON file next to the processed image. Off by default - existing `photoframe-process`
invocations without the new flags behave exactly as before.

Face detection runs entirely on the machine running `process-cli` (your PC), never on the ESP32
itself - the firmware only ever sees the finished crop, either baked into the image `process-cli`
already renders, or - opt-in, see [Firmware usage](#firmware-usage-implemented-opt-in) - read from
the metadata file for original photos that haven't been rendered yet.

This is about *where* faces are and *how big* they are - nothing about *who* they are. No face
recognition/identification, no embeddings, no name/identity tracking of any kind.

A complete flowchart of `process-cli`'s entire CLI - every option, not just the face-crop ones - is
in `docs/diagrams/` as three PlantUML files:
[process-cli-options.puml](diagrams/process-cli-options.puml) (option legend),
[process-cli-main-flow.puml](diagrams/process-cli-main-flow.puml) (main control flow), and
[process-cli-image-flow.puml](diagrams/process-cli-image-flow.puml) (the per-image
`processImageFile()` detail).

## Quick start

```bash
# Metadata only - no rendered image written, just <name>.facecrop.json
photoframe-process photo.jpg --detect-faces --metadata-only --board waveshare_photopainter_73

# Metadata + a face-aware-cropped rendered image (the normal case)
photoframe-process photo.jpg --detect-faces --board waveshare_photopainter_73 -o output/

# Metadata + the full image rendered WITHOUT cropping (letterboxed, "as before")
photoframe-process photo.jpg --detect-faces --crop-output uncropped --board waveshare_photopainter_73 -o output/

# Metadata + BOTH renders (<name>.cover.<ext> and <name>.fit.<ext>) - avoids
# needing to decide Cover vs Fit (or re-render) later, see "Cover vs. Fit" below
photoframe-process photo.jpg --detect-faces --crop-output both --board waveshare_photopainter_73 -o output/

# Visually check face detection + the recommended crop before committing to a
# batch render - writes photo_test.cover.jpg/photo_test.fit.jpg (annotated,
# uncropped copies) instead of a real render - see "Crop preview" below
photoframe-process photo.jpg --detect-faces --crop-preview --board waveshare_photopainter_73 -o output/

# Batch process a whole album folder - one .facecrop.json per photo
photoframe-process ~/Photos/Albums --detect-faces --crop-output both --board waveshare_photopainter_73 -o output/

# Windows (PowerShell) - identical flags, just a different path style
photoframe-process C:\Photos\Albums --detect-faces --crop-output both --board waveshare_photopainter_73 -o C:\output
```

Without `--detect-faces`, nothing changes: no model is loaded, no metadata is written, and the
rendered image uses the exact same center-crop it always has.

## CLI options

| Option | Effect |
|---|---|
| `--detect-faces` | Enables face detection. Writes `<name>.facecrop.json` next to the output, and steers the rendered image's "cover" crop toward the recommended crop instead of a plain center-crop. |
| `--metadata-only` | Combined with `--detect-faces`: write only the metadata file, skip rendering any image entirely (fast - skips dithering/encoding). Errors if used without `--detect-faces`. Takes priority over `--crop-output` (which is then ignored, with a warning). |
| `--crop-output <mode>` | Combined with `--detect-faces`: which rendered image(s) to produce - `cropped` (default), `uncropped`, or `both`. See [Cover vs. Fit: rendering one, the other, or both](#cover-vs-fit-rendering-one-the-other-or-both) below. Errors if used without `--detect-faces`. |
| `--crop-preview` | Combined with `--detect-faces`: writes annotated, uncropped debug images instead of a real render - see [Crop preview](#crop-preview-visualizing-face-detection--the-recommended-crop) below. Errors if used without `--detect-faces`; conflicts with `--metadata-only` and `--upload`/`--direct`; overrides `--crop-output` (with a warning). |
| `--board <id>` | Target board id (see [Target geometry](#target-geometry-board--resolution--display-size-mm--orientation) below). |
| `--resolution <WxH>` | Target resolution in pixels, e.g. `800x480`. Alias of the existing `--dimension`/`--display-width`+`--display-height` - all four ultimately set the same thing. |
| `--display-size-mm <WxH>` | Physical panel size in mm, e.g. `160x96`. Only used to help auto-derive orientation - it cannot by itself supply a pixel resolution. |
| `--face-margin <percent>` | Safety margin added around each face, as a fraction of its own size (default `0.12`). |
| `--face-min-score <value>` | Minimum detection confidence to keep a face, 0-1 (default `0.75`). |
| `--face-model-dir <dir>` | Local directory with a previously-downloaded model, for fully offline use - see [Engine and offline use](#engine-and-offline-use). |

## Target geometry: `--board` / `--resolution` / `--display-size-mm` / `--orientation`

The crop engine always works against a single normalized target: `{ width, height, aspectRatio,
orientation }`. The same aspect ratio can be landscape or portrait, and the crop for each is
genuinely different (a portrait crop keeps a subject centered top-to-bottom differently than a
landscape crop of the same photo) - this is why orientation is tracked as its own value, not just
implied by whichever width/height numbers happen to be given.

**Pixel size precedence** (highest wins): `--resolution` (or `--dimension`/`--display-width`+
`--display-height`) > `--board`'s own resolution > the CLI's existing 800x480 default.

**Orientation precedence** (highest wins): explicit `--orientation landscape|portrait` > derived
from `--resolution`'s own width-vs-height > derived from `--display-size-mm`'s own width-vs-height >
`--board`'s own native orientation > `landscape`.

`--orientation` itself is the CLI's pre-existing flag (unchanged default of `landscape`, used
by the core rendering pipeline for rotation decisions); face-crop only treats it as an *explicit*
override when you actually type `--orientation` on the command line, so leaving it out entirely
means "auto-derive" for face-crop purposes without changing that flag's own default elsewhere.

Conflicting hints (e.g. `--board` and `--resolution` disagree, or `--resolution` and
`--display-size-mm` would derive different orientations) produce a `Warning:` line naming which
source won - behavior stays fully deterministic, just documented.

Board ids and their pixel resolutions come from the firmware's own `boards/boards.json` when
running from a full source checkout, so they always stay in sync with the actual board list; when
`process-cli` is installed standalone (it's also published to npm - see the main README's
"Publishing to npm" section), `face-crop/boards.json` ships an embedded copy instead. `displaySizeMm`
values (used only for the docs/UI, not required for cropping) are derived from each panel's published
diagonal size and pixel resolution, not independently measured - treat them as an approximation.

### Examples

```bash
photoframe-process photo.jpg --detect-faces --board waveshare_photopainter_73
photoframe-process photo.jpg --detect-faces --board waveshare_photopainter_73 --orientation portrait
photoframe-process photo.jpg --detect-faces --resolution 800x480
photoframe-process photo.jpg --detect-faces --resolution 800x480 --orientation portrait
photoframe-process photo.jpg --detect-faces --display-size-mm 160x96
photoframe-process photo.jpg --detect-faces --display-size-mm 160x96 --orientation portrait
```

## Cover vs. Fit: rendering one, the other, or both

The firmware has two display modes for a mismatched-aspect-ratio photo (see
[docs/SCALE_MODE.md](SCALE_MODE.md)): **Cover** (crop to fill) and **Fit** (letterbox, full image,
no crop). `--crop-output` controls which of these `--detect-faces` renders:

| `--crop-output` | Renders | Filename(s) |
|---|---|---|
| `cropped` (default) | One image, Cover-style, using the face-aware recommended crop | `<name>.<ext>` |
| `uncropped` | One image, Fit-style, the full photo letterboxed - no crop applied at all, faces or not | `<name>.<ext>` |
| `both` | Both of the above | `<name>.cover.<ext>` and `<name>.fit.<ext>` |

Metadata is written in all three cases (unless `--metadata-only` is also given, which then skips
every rendered image regardless of `--crop-output`) - `uncropped` is exactly the "process like
before this feature existed, but still tell me where the faces are" mode: useful when you want to
decide the crop later (by hand, or by re-running with `--crop-output cropped` once you've reviewed
the metadata), while keeping a full, uncropped fallback image on hand in the meantime.

`--crop-output both` exists so a whole album can be pre-rendered for *either* firmware display
setting without re-processing later or rendering anything on the ESP32 itself: drop both files plus
the metadata onto the SD card, and the frame picks whichever file matches its own Cover/Fit setting
per source photo - see below.

### Firmware: picking Cover vs. Fit automatically (implemented, opt-in)

**Enable via Web UI: Settings → Auto Rotate → "Use pre-rendered Cover/Fit variants"** (default
**off** - purely additive over existing albums either way; toggling it off restores byte-for-byte
identical behavior to before this feature existed).

**Directory layout** - `--crop-output both`'s two files are *not* both placed in the album's root.
Only `<name>.fit.<ext>` sits there, next to the original; `<name>.cover.<ext>` goes in a `crop`
subdirectory instead:

```
Albums/MyAlbum/
  photo1.jpg                 <- OPTIONAL reference thumbnail (small, always a real JPEG -
                                 see "About photo1.jpg" below) - NOT a render source
  photo1.fit.<ext>            <- rendered, no crop - same directory as the thumbnail
  photo1.facecrop.json        <- metadata sidecar - same directory as the thumbnail
  crop/
    photo1.cover.<ext>         <- rendered, cropped to fill - in the "crop" subdirectory
```

(`photo1.fit.<ext>` may equally be a bare `photo1.<ext>` instead - see the `cropped`/`uncropped`
table above; `<ext>` here is never `.jpg`, since the device can't display raw JPEG.)

Putting the Cover variant in a subdirectory (rather than the album root) means the firmware's
existing directory-listing loops don't need any pairing logic for it at all - they already only
ever look at regular files (`crop/` is invisible to them as a subdirectory) - only the Fit variant,
which does sit in the album root, needs same-directory pairing awareness.

Use **Settings → Maintenance → "Organize Crop Folders"** to retrofit this layout onto existing
`--crop-output both` output: it creates each album's `crop/` subdirectory (if missing) and moves any
`<name>.cover.<ext>` files sitting loose in an album root into it.

#### About `photo1.jpg`

Users are instructed to only ever place already-rendered display files on the SD card - the frame
never receives a full-resolution camera original at all. The only thing named `<name>.jpg` that
legitimately shows up in an album is the small **reference thumbnail**: both `process-cli`
(`renderVariant()` in `process-cli/cli.js`, written unconditionally whenever thumbnail generation is
on) and the firmware's own Telegram/Web-UI ingestion path always write it as a real JPEG, downscaled
to a preview size, under exactly `<name>.jpg` - it is never a candidate for on-device rendering, only
ever a small preview image for the Web UI gallery.

Because of this, the firmware's fallback lookup (used only when a `.fit.<ext>` anchor exists but its
`crop/.cover.<ext>` counterpart is still missing) deliberately does **not** treat a `<name>.jpg`
sibling as something to render Cover from, even though `.jpg` is a format the on-device decoder
otherwise accepts. It only looks for a bare `<name>.png` sibling - and even that is admitted as a
render source only if it fails the same "already display-ready" content check described next, so a
same-named already-rendered PNG (the `cropped`/`uncropped`-without-`both` case) is correctly excluded
too.

**Important**: none of `<name>.<ext>`, `<name>.cover.<ext>`, or `<name>.fit.<ext>` are ever assumed
to be a valid on-device render source just from their filename or position - the firmware decides
purely by inspecting the file's actual content, the identical check `finalize_telegram_image()` in
`main/telegram_bot.c` already uses for incoming Telegram photos, reused verbatim here as
`is_decodable_original()`:

- `image_processor_detect_format()` reads the file's first bytes (PNG signature / `BM` / JPEG SOI
  marker / gzip magic) - a `.bmp`/`.epdgz` file, or anything whose *content* isn't actually a JPEG or
  PNG, is immediately excluded, regardless of what it's named.
- A PNG additionally has to pass `image_processor_is_processed()`, which opens it and requires its
  *actual* `IHDR` width/height to exactly match this board's display resolution and 3 RGB channels -
  an already-rendered PNG always satisfies this and is excluded too.

Only a file whose content is genuinely still a JPEG, or a PNG that fails that exact-resolution
check, is treated as "not yet rendered." Under the stated SD-card policy above, every legitimately
placed rendered file will always be BMP/EPDGZ/exact-resolution-PNG, so this check will always exclude
it and the on-device render path (step 3 below) simply never fires in normal operation - see
[When does the firmware actually render a PNG on-device?](#when-does-the-firmware-actually-render-a-png-on-device)
for the concrete scenarios where it does.

Selection algorithm, per photo, each time it's about to be displayed:

1. **Cover** active: use `<name>.cover.<ext>` from `crop/` if it already exists.
2. **Fit** active: use `<name>.fit.<ext>` from the album root if it already exists.
3. If the wanted variant is missing **and** a genuine, still-undecoded source is found (per the check
   above - for Cover from a `.fit.` anchor, only a bare `<name>.png` sibling is ever considered, never
   `<name>.jpg`): render it on-device - Cover reads `<name>.facecrop.json`'s `recommended_crop` if
   present (plain center-crop otherwise), Fit never crops - and cache the result under its final name
   (written to a temp file and atomically renamed into place only once complete, so a crash or power
   loss mid-render can never leave a half-written file trusted as "already there" next time).
4. Otherwise: display `<name>.<ext>` exactly as today - no change at all.

### When does the firmware actually render a PNG on-device?

Given the SD-card policy above (only already-rendered files get placed there), this on-device path
is a dormant safety net in normal use, not something that fires routinely. It only actually triggers
when a PNG genuinely fails the exact-resolution/3-channel check, which happens in scenarios like:

- **A photo was copied over from a different board's album** (or the display resolution/orientation
  was changed after the photo was rendered) - the PNG's real `IHDR` dimensions no longer match this
  board's current display resolution, so `image_processor_is_processed()` returns false and the
  firmware (correctly) treats it as not-yet-rendered-for-this-board and re-renders it at the right
  size.
- **A user manually drops a real, unprocessed source PNG onto the SD card** (against the stated
  policy, e.g. a screenshot or a PNG export from photo-editing software) next to a `.fit.<ext>` anchor
  whose `crop/.cover.<ext>` variant is missing - the bare `<name>.png` sibling check picks it up as a
  genuine original and renders the missing Cover variant from it.

Note this can also happen in **Fit** mode without a `.cover.`/`.fit.` split at all: if a PNG anchor in
an album root simply isn't a display-ready render for *this* board yet (either of the two cases
above, applied to the anchor file itself rather than a sibling), `resolve_display_variant()`'s
`SCALE_MODE_FIT` branch renders and caches a `.fit.<ext>` from it the same way.

None of these are expected under the documented workflow - they're intentionally left as a graceful
fallback (a correct, if slower, on-device render) rather than a hard error, so a misplaced or
stale/board-mismatched file never breaks display of that photo.

The on-device render (`image_processor_render_variant()` in `main/image_processor.c`) reuses the
exact same decode/tone-map/dither pipeline every other on-device conversion already goes through,
just with the target scale mode forced (rather than read from the device's own current setting) and,
for Cover, an optional pre-crop applied right after decode - the same idea as this CLI's own
`cropRect` handling in `process-cli/utils.js`, just in C. Output format follows the same
[on-device image format](TELEGRAM.md#on-device-image-format) setting Telegram ingestion uses -
EPDGZ by default, falling back to PNG if EPDGZ can't get the memory it needs at that moment.

**Known limitation**: the Web UI gallery correctly shows one entry per photo now (a `<name>.<ext>` /
`<name>.fit.<ext>` pair is deduplicated the same way the rotation loops are), but its delete action
still only removes the one listed file - deleting a paired photo's `.fit.` entry currently leaves its
`crop/.cover.<ext>` sibling (and any `.facecrop.json`/original) behind as orphaned files. Proper
multi-file delete semantics for paired photos is left for a future pass.

## Crop preview: visualizing face detection + the recommended crop

`--crop-preview` (combined with `--detect-faces`) is a debugging aid for sanity-checking face
detection and the crop heuristic below *before* committing to a real batch render - it never
crops/resizes/dithers the source image at all, and never writes a real `.cover.`/`.fit.` display
file. Instead, for `photo.jpg` it writes:

- `photo_test.cover.jpg` - a full, unmodified copy of the source image with every detected face
  boxed in **blue** and the recommended cover-mode crop rectangle boxed in **red**.
- `photo_test.fit.jpg` - the same, but with only the face boxes (no crop rectangle, since fit
  mode never crops anything).
- `photo.facecrop.json` - the normal metadata sidecar, written exactly as it would be without
  `--crop-preview`.

Both preview images are always JPEG, regardless of `--board`/`--resolution`/output-format
settings - they're for a human to look at, not for the device. Box line width scales with the
image's own resolution so it stays visible on both small and very large photos.

```bash
photoframe-process photo.jpg --detect-faces --crop-preview --board waveshare_photopainter_73 -o output/
```

Useful for spotting cases worth a closer look - e.g. a photo with several people where the
heuristic's "grow toward the largest faces first, skip one that would push an already-included
larger face out of frame" rule (see [Crop heuristic](#crop-heuristic) below) ends up excluding
someone from the recommended crop, visible directly as a face box sitting entirely outside the
red rectangle.

Requires `--detect-faces` (errors otherwise); conflicts with `--metadata-only` (one skips
rendering, the other requires it) and with `--upload`/`--direct` (these debug images are never
meant for the device); overrides `--crop-output` if both are given (with a warning), since the
two output shapes are mutually exclusive.

## Crop heuristic

1. If no faces are found (or none pass `--face-min-score`), fall back to the exact same
   aspect-ratio-matched center-crop the pipeline has always used for "cover" mode - a no-op change
   for photos without people in them.
2. Otherwise, start from the largest detected face plus a safety margin (`--face-margin`), then walk
   the remaining faces largest-to-smallest, growing the crop to include each one - but only if doing
   so (after re-fitting the target aspect ratio and clamping to the image bounds) doesn't push any
   already-included, larger face out of frame. A face that would only fit by displacing a bigger one
   is left out; the biggest faces always win.
3. The result is grown to the target aspect ratio - using as much of the source image as fits, not
   just the minimum needed to reach that ratio around the accepted faces - then clamped to the
   image bounds (uniform scale-down + translate only - never distorted).

**Retaining as much of the photo as possible, not just the minimum around the faces**: step 3
prefers the *largest* aspect-ratio-matching box that both fits within the image and still contains
the accepted faces, centered on them - not the smallest one that merely reaches the target aspect
ratio. Concretely: if the source image is wider than the target ratio, only its height gets cropped
down to reach 5:3 (no left/right loss at all, so long as the accepted faces stay in frame within
that band); only when the image is genuinely too narrow/short relative to what the faces need does
the crop shrink below the image's own full extent. Confirmed on real output: a 2592x1944 (4:3)
photo of 3 people spread across nearly the full width, targeting 5:3, needs only ~389px trimmed off
its height (2592/(5/3) ≈ 1555px tall) to fit - an earlier version instead additionally trimmed
~600px off the left/right sides too, even though nothing about the accepted faces required it.

Step 2's "does this face still fit" containment check applies the same "use as much of the image as
possible" rule and works in full sub-pixel precision internally, only rounding to whole pixels at
the very end (an earlier version rounded the intermediate check's box too, which could shave up to
~1px total off its right/bottom edge and occasionally reject a face that genuinely fit). The crop is
only ever smaller than the full image when the accepted faces truly need more room than the image's
own aspect-fit ceiling provides - never smaller than that just to look "tighter."

## JSON schema (`<name>.facecrop.json`)

One file per image, saved next to the rendered output (not a single global index) - stays correct
if files are moved/copied independently onto an SD card, trivial for anything to associate with its
source image by filename, no database to keep in sync.

```json
{
  "schema": 1,
  "source": "urlaub01.jpg",
  "image": { "width": 4032, "height": 3024 },
  "target": {
    "board": "waveshare_photopainter_73",
    "width": 800,
    "height": 480,
    "aspect_ratio": 1.6667,
    "orientation": "landscape"
  },
  "faces": [
    { "x": 820, "y": 640, "w": 760, "h": 760, "score": 0.94 },
    { "x": 2500, "y": 700, "w": 420, "h": 420, "score": 0.81 }
  ],
  "recommended_crop": { "x": 410, "y": 560, "w": 2520, "h": 1512 },
  "strategy": { "mode": "largest-face-priority", "margin_percent": 0.12, "engine": "blazeface" },
  "timestamp": "2026-08-23T10:53:12.613Z",
  "generator_version": "esp32-photoframe-cli@1.0.0"
}
```

`image`, `faces`, and `recommended_crop` are all expressed in the **same coordinate space**: the
upright image after EXIF-orientation correction and (if `--auto-orient` was used) the
landscape/portrait auto-rotation - i.e. exactly the pixels a viewer would see the photo in, before
any resizing/cropping for the display. `faces` with zero entries means no face was detected (or none
passed `--face-min-score`) - `recommended_crop` in that case is the plain fallback center-crop, and
`strategy.mode` reads `"center-crop-fallback"` instead of `"largest-face-priority"`.

## Engine and offline use

Face detection uses [BlazeFace](https://github.com/tensorflow/tfjs-models/tree/master/blazeface) via
`@tensorflow/tfjs-core` + `tfjs-converter` + `tfjs-backend-cpu` - not the full `@tensorflow/tfjs`
umbrella package (which bundles a browser-only WebGL backend and other unused pieces) and not
`@tensorflow/tfjs-node` (a native addon that would need a matching prebuilt binary per platform/Node
version). The pure-JS CPU backend is slower per image but has no native code to compile and runs
unmodified on Windows and Linux - the right trade for a batch tool where correctness and "just
installs" matter more than raw throughput. See `process-cli/face-crop/detector.js` for the small
adapter interface this is built behind - a different engine (e.g. a `tiny-face-detector`-based
adapter) can be swapped in later without touching the crop engine or the CLI.

By default, the model (~400 KB) is fetched once per `process-cli` run from
`https://tfhub.dev/tensorflow/tfjs-model/blazeface/1/default/1` (Node's built-in `fetch`, no extra
polyfill needed on Node 18+) and reused for every photo in that run - so a whole album batch only
pays the download once, but each fresh CLI invocation re-fetches it.

For fully offline use (no network at all, including for the model), download the model once into a
local folder and pass `--face-model-dir`:

```bash
mkdir -p ~/.cache/photoframe-blazeface
curl -L -o ~/.cache/photoframe-blazeface/model.json \
  https://tfhub.dev/tensorflow/tfjs-model/blazeface/1/default/1/model.json?tfjs-format=file
curl -L -o ~/.cache/photoframe-blazeface/group1-shard1of1.bin \
  https://tfhub.dev/tensorflow/tfjs-model/blazeface/1/default/1/group1-shard1of1.bin?tfjs-format=file

photoframe-process photo.jpg --detect-faces --board waveshare_photopainter_73 \
  --face-model-dir ~/.cache/photoframe-blazeface
```

(Check the actual shard filename(s) listed in the downloaded `model.json`'s `weightsManifest` -
BlazeFace currently ships as a single shard, but this isn't guaranteed to stay that way forever.)

## Face detection accuracy

BlazeFace is a small, fast, general-purpose detector - it can miss faces that are small, angled far
from front-facing, partly occluded, or in poor lighting, and (rarely) false-positive on
face-like patterns in non-face objects. There's no correctness guarantee here, only a heuristic
that's usually a meaningful improvement over a naive center-crop for photos with people in them.
Nothing breaks if it's wrong - worst case is a crop no better (or occasionally worse) than the
existing center-crop fallback, never a corrupted image or crash.

## App integration (not implemented - no companion app in this workspace)

This workspace doesn't currently contain the Flutter (or other) companion/mobile app mentioned in
the broader project ecosystem, so no app-side code was written here. For whoever picks up
app-side integration later, the shape to build toward:

1. **Photo picker** - user selects one or more originals from their device library, same as any
   existing upload flow the app already has.
2. **Face detection in-app** - run an on-device face detector (e.g. Google's ML Kit Face Detection
   on Android/iOS, which is fast, offline, and purpose-built for mobile - a more natural fit there
   than shipping a TensorFlow.js/BlazeFace stack into a mobile app) to get bounding boxes in the
   same `{x, y, w, h, score}` shape this doc's JSON schema uses.
3. **Crop preview** - reuse this doc's crop heuristic (`face-crop/crop-engine.js`'s
   `computeRecommendedCrop` is pure, dependency-free JS/TS-portable logic - port it directly rather
   than re-deriving the heuristic) to show the user the recommended crop overlaid on the photo
   before upload, exactly as `process-cli` would compute it for the same target geometry.
4. **Manual correction** - let the user drag/resize the suggested crop rectangle; the corrected
   rectangle simply replaces `recommended_crop` in the metadata that gets sent (still schema
   version 1 - a manual override doesn't need a new field, just a different `strategy.mode`, e.g.
   `"manual"`).
5. **Upload/export** - send both the original photo and its `<name>.facecrop.json` (this doc's
   exact schema) to the device/server, so the firmware-side flow below can use it verbatim.

## Firmware usage (implemented, opt-in)

The firmware itself does **not** run face detection (out of scope for the ESP32, and explicitly not
wanted for this feature) - it only ever *reads* a `.facecrop.json` that `process-cli` already
produced. See [Firmware: picking Cover vs. Fit automatically](#firmware-picking-cover-vs-fit-automatically-implemented-opt-in)
above for the full mechanism: `main/facecrop_metadata.c`'s `facecrop_read_recommended_crop()` reads
the sidecar, and `main/display_manager.c`'s `resolve_display_variant()` applies it (Cover mode only -
Fit never crops) via `image_processor_render_variant()`, caching the rendered result on SD exactly
once - the metadata file is only consulted on that first render, never on subsequent displays of the
same cached file. This is the schema version 1 originally defined below, unchanged.

This keeps the firmware's job simple (read one small JSON, apply one crop rectangle) and never
requires it to run any ML inference itself.
