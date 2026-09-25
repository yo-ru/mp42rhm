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
.\build\Release\mp42rhm.exe video.mp4 output --mode bw
.\build\Release\mp42rhm.exe video.mp4 output --mode grayscale
.\build\Release\mp42rhm.exe video.mp4 output --mode color --colors 32
.\build\Release\mp42rhm.exe --help
```

Writes `output.sspm` and `output-colorset.txt`. Use `--format rhm` for RHM.

Defaults: black and white, 160x90, 60 fps, 5 million notes. Adjust with `--width`, `--height`, `--fps`, and `--max-notes`.

## Playback

Apply the printed Note Scale, AR, SD, background RGB, and colorset. Use solid square notes, 1x speed, and Visualize (Auto).
