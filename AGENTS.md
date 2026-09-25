# Using mp42rhm

Use the CLI to export video maps for unmodified Steam Rhythia. Export requests do not require source changes.

## Setup

- Use `build/Release/mp42rhm.exe`. If missing, follow the build commands in README.md.
- FFmpeg and ffprobe must be on PATH, or supplied through `--ffmpeg` and `--ffprobe`.
- Check `--help` for the current options.

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
- `--mode color`: automatic compact palette, up to 32 colors by default.
- `--colors 2..256`: grayscale/color palette size, including the background.
- `--palette-cycle PATH`: reuse a custom palette and its repeated color entries. Exclude the background; do not combine with `--colors`.
- `--background HEX`: color-mode background. Black and white/grayscale require black.

Colorsets and Steam color ordering are automatic. Default pixel spacing is 0.01; `--span` overrides image width in grid units.

Set `--max-notes` for larger exports; the default is five million. Both total notes and peak notes per frame affect Steam playback. Do not assume a universal safe limit or claim an export was tested in-game without testing it.

## Handoff

Confirm the export succeeded and both the map and colorset exist. Return their paths, total/peak notes, and the printed Note Scale, AR, SD, background RGB, speed, and playback settings. Keep the response concise.
