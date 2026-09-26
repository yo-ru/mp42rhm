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

`--experimental-cuda` enables NVIDIA acceleration for color/grayscale and defaults to 32-pixel brushes. Other brush sizes are unsupported. Requires an NVIDIA driver and CUDA 12/13 NVRTC with its matching builtins DLL beside the executable or on PATH. Frame batching adjusts to available VRAM; CPU encoding remains the default.

## Encoding

1. FFmpeg decodes frames at the requested FPS and fits them to the output resolution, preserving aspect ratio with padding.
2. Black and white uses a brightness threshold. Grayscale uses fixed levels; color uses a generated or supplied palette, without dithering.
3. Frames become simultaneous square notes. Color/grayscale brush mode compares 16 painting orders, repositions overlapping squares, and removes redundant strokes. It preserves the pixels after scaling and palette conversion; deeper optimization takes longer to encode.
4. Note order is compensated for Steam's sorting and reverse draw order. Notes and MP3 audio are packaged as SSPM v2 or RHM.

Brush colorsets contain one line per note. `--colors` controls unique palette colors, not the number of lines.

## Playback

Apply the printed Note Scale, AR, SD, background RGB, and colorset. Use solid square notes, 1x speed, and Visualize (Auto).

Brush maps require Note Opacity 100% and Fade Length 0.
