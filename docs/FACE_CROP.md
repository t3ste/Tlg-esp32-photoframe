# Face-aware crop metadata (process-cli)

An opt-in `process-cli` feature: detect faces in a source photo, compute a crop rectangle that
keeps large faces fully visible for a given display's aspect ratio, and save that as a small
versioned JSON file next to the processed image. Off by default - existing `photoframe-process`
invocations without the new flags behave exactly as before.

Face detection runs entirely on the machine running `process-cli` (your PC), never on the ESP32
itself - the firmware only ever sees the finished crop, either baked into the image `process-cli`
already renders, or (later - see [Firmware usage (planned)](#firmware-usage-planned---not-implemented)) read from the
metadata file for original photos that haven't been rendered yet.

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
the metadata onto the SD card, and (once the planned firmware enhancement below ships) the frame
picks whichever file matches its own Cover/Fit setting per source photo.

> **Not yet plug-and-play with a normal rotation album.** Today's firmware has no concept of
> `<name>.cover.<ext>` / `<name>.fit.<ext>` being two renders of the *same* photo - every album
> listing loop just treats each matching file extension as its own independent picture. Dropping
> `--crop-output both`'s output straight into a Storage-rotation album **today** would show the same
> photo twice per cycle, once per variant - not the intended use. Use `both` mode to stage files for
> the planned firmware pairing below, or to inspect/pick manually, not (yet) as a normal album.

### Firmware: picking Cover vs. Fit automatically (planned - not implemented)

Concept for whoever implements this later - also noted as a source comment right above
`rotate_sequential()` in `main/display_manager.c`:

1. Before/while listing an album directory's image files, recognize `<name>.cover.<ext>` /
   `<name>.fit.<ext>` pairs (same base name, same folder) and treat each pair as **one** logical
   photo entry instead of two - every `*_sequential()`/`*_random()` listing loop in
   `main/display_manager.c` (`strcasecmp(ext, ...)` scans) would need this.
2. When about to display a paired entry, pick the file matching the device's own
   `processing_settings_get_scale_mode()` (`SCALE_MODE_FIT` → the `.fit` file, otherwise → the
   `.cover` file) and display it directly - already fully rendered, so no on-device
   decode/crop/dither work at all for these photos.
3. A file with no pairing partner (an ordinary single-render photo, or one from `--crop-output
   cropped`/`uncropped`) displays exactly as it does today - this is purely additive.

This is a different (simpler) mechanism than the [original-photo firmware
flow](#firmware-usage-planned---not-implemented) described below: here, both candidate renders
already exist on disk - the firmware only ever picks between two pre-rendered files, never renders
anything itself.

## Crop heuristic

1. If no faces are found (or none pass `--face-min-score`), fall back to the exact same
   aspect-ratio-matched center-crop the pipeline has always used for "cover" mode - a no-op change
   for photos without people in them.
2. Otherwise, start from the largest detected face plus a safety margin (`--face-margin`), then walk
   the remaining faces largest-to-smallest, growing the crop to include each one - but only if doing
   so (after re-fitting the target aspect ratio and clamping to the image bounds) doesn't push any
   already-included, larger face out of frame. A face that would only fit by displacing a bigger one
   is left out; the biggest faces always win.
3. The result is grown to the target aspect ratio and clamped to the image bounds (uniform
   scale-down + translate only - never distorted).

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

## Firmware usage (planned - not implemented)

The firmware itself does **not** run face detection (out of scope for the ESP32, and explicitly not
wanted for this feature) and does not read `.facecrop.json` files today - `process-cli` currently
only *produces* them for external tooling/curation and the future app flow above. The intended
later firmware flow, for the case where only an original photo (not yet a rendered display file)
ends up on the SD card:

1. When about to display an album entry, check whether a already-processed display file exists
   for it (as today).
2. If not, check whether a `<name>.facecrop.json` sits next to the original.
3. If present (and its `schema` field is a version the firmware understands), read
   `recommended_crop` and apply it before the existing resize/dither pipeline, instead of that
   pipeline's own default center-crop.
4. Render the display file once, exactly as for any other new photo.
5. Cache the rendered result on SD, same as the existing pipeline already does - the metadata file
   is only consulted on this first render, never on subsequent displays of the same cached image.
6. Display normally from there on, unchanged.

This keeps the firmware's job simple (read one small JSON, apply one crop rectangle) and never
requires it to run any ML inference itself.
