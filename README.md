# mp42rhm

C++17 video-to-map converter for Steam Rhythia. Windows only.

## Build

Requires Visual Studio 2022 C++ tools, CMake 3.24+, Git, and FFmpeg/ffprobe on PATH.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

## Usage

```powershell
.\build\Release\mp42rhm.exe video.mp4 bw
.\build\Release\mp42rhm.exe video.mp4 gray --mode grayscale
.\build\Release\mp42rhm.exe video.mp4 color --mode color
```

Writes `<output>.sspm` and `<output>-colorset.txt`, with audio embedded in the map. Use `--format rhm` for RHM.

Add `--seconds 5` for a preview. Defaults: black and white, 160x90, 12 fps, 100 million notes. Color uses up to 64 colors; grayscale uses 4 levels. Both use 8-pixel brushes.

`--help` lists common controls. `--help-all` lists every option.

Resolution supports up to 3840x2160. `--fps` accepts decimals, fractions such as `24000/1001`, or `native` for the source's average frame rate.

| Option | Behavior |
|---|---|
| `--adaptive-palette` | Per-frame color palettes; `--colors` supports 2..65536 including the background. Requires color brushes. |
| `--compact-colorset` | Tiny repeating colorsets for BW/grayscale brushes. Adds an analysis pass, temporary disk use, and offscreen filler notes. |
| `--subtitles N` | Burns text subtitle track N (1-based) into a black band inside the output dimensions, using plain white text. |

```powershell
.\build\Release\mp42rhm.exe video.mp4 adaptive --adaptive-palette --colors 2048 --fps native
.\build\Release\mp42rhm.exe episode.mkv episode --mode grayscale --compact-colorset --width 640 --height 480 --subtitles 1
```

`--experimental-cuda` enables NVIDIA acceleration for color/grayscale and compact BW. Brush sizes 2..64 are supported, defaulting to 32. Requires an NVIDIA driver and CUDA 12/13 NVRTC with its matching builtins DLL beside the executable or on PATH. Stroke buffers are reused in place; frame batching adjusts to available VRAM. CPU encoding remains the default.

## Encoding

1. FFmpeg decodes frames at the requested FPS and fits them to the output resolution, preserving aspect ratio with padding.
2. Black and white uses a brightness threshold. Grayscale uses fixed levels; color uses a global or adaptive per-frame palette, without dithering.
3. Frames become simultaneous square notes. Brush mode compares 16 painting orders, repositions squares, then combines and reorders strokes from the best two results to remove more notes. Pixels remain exact after scaling and palette conversion; the extra search takes longer to encode.
4. Note order is compensated for Steam's sorting and reverse draw order. Notes and MP3 audio are packaged as SSPM v2 or RHM.

Colorsets use bare `RRGGBB` lines (7 bytes per entry). Brush colorsets normally contain one line per note. Compact mode repeats a short sequence and schedules strokes around it without changing pixels. `--colors` controls palette size, not colorset length.

Raster scans track only the active brush edge, and stroke analysis uses SIMD. Adaptive palettes up to 256 colors use indexed FFmpeg frames to reduce pipe traffic. These optimizations are automatic and preserve the generated notes and colors.

## Playback

Apply the printed Note Scale, AR, SD, background RGB, and colorset. Use solid square notes, 1x speed, and Visualize (Auto).

Brush maps require Note Opacity 100% and Fade Length 0.
