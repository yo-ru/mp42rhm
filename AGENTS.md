# Using mp42rhm

Use the CLI to export video maps for unmodified Rhythia. Export requests do not require source changes.

## Setup

- Use `mp42rhm.exe` from a release ZIP or `build/Release/mp42rhm.exe` from a source build. If missing, follow README.md.
- FFmpeg and ffprobe must be on PATH, or supplied through `--ffmpeg` and `--ffprobe`.
- Use `--help` for common options and `--help-all` for the complete CLI.

## Workflow

1. Use the supplied local video. For URLs, download into `media/` with yt-dlp if available; otherwise request a local file.
2. Inspect resolution, frame rate, and duration with ffprobe. Preserve the source aspect ratio when choosing dimensions.
3. Choose the requested color mode, resolution, FPS, and note budget. State any assumptions briefly.
4. Create the output directory. Use separate names for previews and full exports; preserve existing files.
5. Export a short representative clip before a large conversion. Then run the full export with the selected settings.

```powershell
.\build\Release\mp42rhm.exe media/video.mp4 output/video-preview --mode bw --seconds 5
```

Omit `--seconds` for the full video. Use `--start` to select a clip, `--title` for the map title, and `--difficulty-name` for its label.

## Export choices

- Default format: SSPM v2. Use `--format rhm` when requested.
- `--mode bw`: thresholded black and white.
- `--mode grayscale`: 4 levels by default.
- `--mode color`: automatic compact palette, up to 64 colors by default.
- `--colors 2..256`: grayscale/global color palette size, including the background.
- `--adaptive-palette`: per-frame color palettes, supporting `--colors 2..65536`. Requires color brushes; remains opt-in.
- `--compact-colorset`: repeating colorsets for BW/grayscale brushes. Uses a disk-backed analysis pass and adds offscreen filler notes. Remains opt-in.
- `--subtitles N`: 1-based text subtitle track, rendered as plain white text in a black band inside the output dimensions. Requires at least 120 pixels of output height.
- `--fps`: integer, decimal, fraction such as `24000/1001`, or `native` for the source's average frame rate.
- Resolution supports up to 3840x2160. Larger canvases can require substantial RAM and VRAM even when note counts are low.
- `--palette-cycle PATH`: reuse a custom palette and its repeated color entries. Exclude the background; do not combine with `--colors`.
- `--background HEX`: color-mode background. Black and white/grayscale require black.
- Color/grayscale and compact BW default to `--brush-size 8`: overlapping squares, normally one colorset line per note. Requires Note Opacity 100% and Fade Length 0. Use `--brush-size 1` for pixel mode; custom palette repetitions only apply there.
- `--experimental-cuda` supports brush sizes 2..64 for color/grayscale or compact BW, defaulting to 32. CPU supports the same brush sizes, plus pixel mode (`--brush-size 1`).

Colorsets and Rhythia color ordering are automatic. Default pixel spacing is 0.01; `--span` requests image width in grid units. Note Scale is rounded to two decimals (minimum 0.01), and coordinate spacing changes with it. `--span 3.3` with FOV 30 is a starting point for a large 16:9 picture; check for clipping.

Defaults are 160x90 at 12 fps, with a 100-million-note budget. Set `--max-notes` to change the budget; reaching it aborts rather than lowering quality. Both total notes and peak notes per frame affect Rhythia playback. Do not assume a universal safe limit or claim an export was tested in-game without testing it.

## Handoff

Confirm the map, colorset, and `<output>-settings.txt` exist. Return file paths and sizes, total/peak notes, and the saved Note Scale, AR, SD, FOV, background RGB, speed, and playback settings. Keep the response concise.
