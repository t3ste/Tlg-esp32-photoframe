# PhotoFrame CLI Image Processor

Node.js CLI tool that shares the exact same image processing code (`image-processor.js`) with the webapp for consistent results.

## Features

- Shared processing code with webapp for consistent results
- S-curve tone mapping, saturation adjustment, Floyd-Steinberg dithering
- Measured palette for accurate e-paper color matching
- Batch folder processing with automatic album organization
- Device parameter sync and direct upload
- Image server mode for HTTP-based serving

## Installation

```bash
cd process-cli
npm install
```

**System Requirements:**
- Node.js 14+ up to Node.js 20 (dependencies are no longer supported for higher versions)
- macOS/Linux: Install Cairo dependencies (see below)

**macOS:**
```bash
brew install pkg-config cairo pango libpng jpeg giflib librsvg pixman
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install build-essential libcairo2-dev libpango1.0-dev libjpeg-dev libgif-dev librsvg2-dev
```

## Usage

### Single File Processing

```bash
# Basic usage
node cli.js input.jpg

# With output directory
node cli.js input.jpg -o /path/to/output

# Custom parameters
node cli.js input.jpg --scurve-strength 0.8 --saturation 1.5
```

### Folder Processing

```bash
# Process entire album directory
node cli.js ~/Photos/Albums -o output/
```

Automatically processes subdirectories as albums, preserving folder structure.

### Device Parameters

```bash
# Fetch settings and palette from device
node cli.js input.jpg --device-parameters
node cli.js ~/Photos/Albums --device-parameters -o output/
node cli.js input.jpg --device-parameters --host 192.168.1.100
```

### Direct Upload

```bash
# Upload to device (no disk output)
node cli.js input.jpg --upload --host photoframe.local
node cli.js ~/Photos/Albums --upload --device-parameters --host photoframe.local
```

Processes in temp directory, uploads via HTTP API, auto-cleans up.

### Image Server Mode

```bash
# Serve images over HTTP (no SD card needed)
node cli.js --serve ~/Photos/Albums --serve-port 9000 --device-parameters --host photoframe.local

# Different formats: png (smaller), bmp (faster), jpg (smallest)
node cli.js --serve ~/Photos --serve-port 9000 --serve-format png
```

Serves random images on each request. Configure ESP32: **Rotation Mode** → URL, **Image URL** → `http://your-ip:9000/image`

## Options

```
Usage: photoframe-process [options] <input>

Arguments:
  input                          Input image file or directory with album subdirectories

Options:
  -V, --version               output the version number
  -o, --output-dir <dir>      Output directory (default: ".")
  --suffix <suffix>           Suffix to add to output filename (single file mode only) (default: "")
  --upload                    Upload converted PNG and thumbnail to device (requires --host)
  --direct                    Display image directly on device without saving (requires --host, single file only)
  --serve                     Start HTTP server to serve images from album directory structure
  --serve-port <port>         Port for HTTP server in --serve mode (default: "8080")
  --serve-format <format>     Image format to serve: png, jpg, or bmp (default: "png")
  --host <host>               Device hostname or IP address (default: "photoframe.local")
  --device-parameters         Fetch processing parameters from device
  --exposure <value>          Exposure multiplier (0.5-2.0, 1.0=normal) (default: 1)
  --saturation <value>        Saturation multiplier (0.5-2.0, 1.0=normal) (default: 1.3)
  --tone-mode <mode>          Tone mapping mode: scurve or contrast (default: "scurve")
  --contrast <value>          Contrast multiplier for simple mode (0.5-2.0, 1.0=normal) (default: 1)
  --scurve-strength <value>   S-curve overall strength (0.0-1.0) (default: 0.9)
  --scurve-shadow <value>     S-curve shadow boost (0.0-1.0) (default: 0)
  --scurve-highlight <value>  S-curve highlight compress (0.5-5.0) (default: 1.5)
  --scurve-midpoint <value>   S-curve midpoint (0.3-0.7) (default: 0.5)
  --color-method <method>     Color matching: rgb or lab (default: "rgb")
  --render-measured           Render BMP with measured palette colors (darker output for preview)
  --processing-mode <mode>    Processing algorithm: enhanced (with tone mapping) or stock (Waveshare original) (default: "enhanced")
  -h, --help                  display help for command
```

## Output Files

- `photo.bmp` - 800x480 dithered BMP (theoretical palette for device)
- `photo.jpg` - 400x240 thumbnail
- `--render-measured` - Use measured palette (darker, preview mode)

## Processing Pipeline

1. Load image → 2. EXIF orientation → 3. Rotate if portrait → 4. Resize to 800x480 (cover mode) → 5. Tone mapping (S-curve/contrast) → 6. Saturation adjustment → 7. Floyd-Steinberg dithering → 8. Output BMP + thumbnail

## Examples

```bash
# Basic processing
node cli.js photo.jpg -o output/

# With device parameters (recommended)
node cli.js photo.jpg --device-parameters -o output/

# Batch folder upload
node cli.js ~/Photos/Albums --upload --device-parameters --host photoframe.local

# Custom tone mapping
node cli.js photo.jpg --scurve-strength 1.0 --saturation 1.5 -o output/

# Preview mode (darker, matches e-paper)
node cli.js photo.jpg --render-measured -o preview/
```
