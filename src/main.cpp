#include "converter.h"

#include <Windows.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

static void usage(bool all) {
  std::cout <<
    "Video maps for Steam Rhythia\n"
    "Usage: mp42rhm input.mp4 output [options]\n\n"
    "  --mode MODE             bw, grayscale, color (bw)\n"
    "  --width N --height N    Resolution (160x90)\n"
    "  --fps N                 Frame rate (12)\n"
    "  --colors N              Palette size (gray: 4, color: 64)\n"
    "  --seconds N             Clip duration (full video)\n"
    "  --max-notes N           Note budget (100000000)\n"
    "  --title TEXT            Map title\n";

  if (!all) {
    std::cout << "\nAll options: mp42rhm --help-all\n";
    return;
  }
  std::cout <<
    "\nAdvanced:\n"
    "  --format sspm|rhm       Map format (SSPM v2)\n"
    "  --difficulty-name TEXT  Difficulty label\n"
    "  --brush-size N          Brush pixels (bw: 1, gray/color: 8)\n"
    "  --experimental-cuda     NVIDIA encoder (32-pixel brushes)\n"
    "  --span N                Image width (default: width * 0.01)\n"
    "  --palette-cycle PATH    Custom color palette\n"
    "  --threshold N           Black/white threshold (128)\n"
    "  --invert                Invert brightness\n"
    "  --background HEX        Background color (000000)\n"
    "  --start SECONDS         Clip start (0)\n"
    "  --no-audio              Exclude audio\n"
    "  --colorset PATH         Colorset output (<output>-colorset.txt)\n"
    "  --ffmpeg PATH           FFmpeg executable\n"
    "  --ffprobe PATH          ffprobe executable\n";
}

static uint64_t integer(const std::wstring& value, uint64_t maximum) {
  if (value.empty() || value.find_first_not_of(L"0123456789") != std::wstring::npos)
    throw std::runtime_error("Expected a nonnegative integer option value");

  size_t consumed = 0;
  const uint64_t parsed = std::stoull(value, &consumed);

  if (consumed != value.size() || parsed > maximum)
    throw std::runtime_error("Integer option is out of range");
  return parsed;
}

static double decimal(const std::wstring& value) {
  size_t consumed = 0;
  const double parsed = std::stod(value, &consumed);

  if (consumed != value.size() || !std::isfinite(parsed))
    throw std::runtime_error("Expected a finite decimal option value");
  return parsed;
}

static std::string utf8(const std::wstring& value) {
  const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
    static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);

  if (length == 0)
    throw std::runtime_error("Metadata text must be nonempty valid Unicode");

  std::string result(length, '\0');

  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
    static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
  return result;
}

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 2 && (std::wstring(argv[1]) == L"--help" || std::wstring(argv[1]) == L"--help-all")) {
      usage(std::wstring(argv[1]) == L"--help-all");
      return 0;
    }
    if (argc < 3) {
      std::cerr << "Usage: mp42rhm input.mp4 output [options]\nRun mp42rhm --help for options.\n";
      return 1;
    }

    mp42rhm::Options options;
    bool mode_set = false;
    bool brush_set = false;

    options.input = argv[1];
    options.output = argv[2];
    for (int i = 3; i < argc; ++i) {
      const std::wstring key = argv[i];

      if (key == L"--invert") {
        options.invert = true;
        continue;
      }
      if (key == L"--no-audio") {
        options.audio = false;
        continue;
      }
      if (key == L"--experimental-cuda") {
        options.experimental_cuda = true;
        continue;
      }
      const auto value = [&]() {
        if (++i == argc)
          throw std::runtime_error("Missing value for " + utf8(key));
        return std::wstring(argv[i]);
      };

      if (key == L"--format") {
        const auto format = value();

        if (format == L"sspm")
          options.format = mp42rhm::Format::Sspm;
        else if (format == L"rhm")
          options.format = mp42rhm::Format::Rhm;
        else
          throw std::runtime_error("Expected --format sspm or rhm");
      } else if (key == L"--mode") {
        const auto mode = value();

        mode_set = true;
        if (mode == L"bw")
          options.mode = mp42rhm::ColorMode::Bw;
        else if (mode == L"grayscale")
          options.mode = mp42rhm::ColorMode::Grayscale;
        else if (mode == L"color")
          options.mode = mp42rhm::ColorMode::Color;
        else
          throw std::runtime_error("Expected --mode bw, grayscale, or color");
      } else if (key == L"--colors") {
        options.color_count = static_cast<uint32_t>(integer(value(), 256));
        if (options.color_count < 2)
          throw std::runtime_error("Colors must be between 2 and 256");
      } else if (key == L"--width")
        options.width = static_cast<uint32_t>(integer(value(), 1920));
      else if (key == L"--height")
        options.height = static_cast<uint32_t>(integer(value(), 1080));
      else if (key == L"--fps")
        options.fps = static_cast<uint32_t>(integer(value(), 60));
      else if (key == L"--brush-size") {
        options.brush_size = static_cast<uint32_t>(integer(value(), 64));
        brush_set = true;
      } else if (key == L"--threshold")
        options.threshold = static_cast<uint32_t>(integer(value(), 255));
      else if (key == L"--max-notes")
        options.max_notes = integer(value(), UINT64_MAX);
      else if (key == L"--start")
        options.start = decimal(value());
      else if (key == L"--seconds")
        options.seconds = decimal(value());
      else if (key == L"--span")
        options.span = decimal(value());
      else if (key == L"--ffmpeg")
        options.ffmpeg = value();
      else if (key == L"--ffprobe")
        options.ffprobe = value();
      else if (key == L"--colorset")
        options.colorset = value();
      else if (key == L"--palette-cycle")
        options.palette_cycle = value();
      else if (key == L"--background")
        options.background = mp42rhm::parse_color(utf8(value()));
      else if (key == L"--title")
        options.title = utf8(value());
      else if (key == L"--difficulty-name")
        options.difficulty_name = utf8(value());
      else
        throw std::runtime_error("Unknown option: " + utf8(key));
    }

    if (!options.output.has_extension())
      options.output += options.format == mp42rhm::Format::Sspm ? L".sspm" : L".rhm";
    if (!mode_set && !options.palette_cycle.empty())
      options.mode = mp42rhm::ColorMode::Color;
    if (!brush_set && options.mode != mp42rhm::ColorMode::Bw)
      options.brush_size = options.experimental_cuda ? 32 : 8;
    if (options.colorset.empty())
      options.colorset = options.output.parent_path() / (options.output.stem().wstring() + L"-colorset.txt");
    mp42rhm::validate(options);

    std::cerr << "Converting " << options.width << 'x' << options.height << " at " << options.fps << " fps...\n";

    const auto result = mp42rhm::convert(options);
    const double note_scale = (options.span == 0 ? 0.01 : options.span / options.width) * options.brush_size;
    // Round AR up to avoid overlapping quantized video frames
    const double approach_rate = std::ceil(1000.0 / (1000 / options.fps)) / 100;

    std::cout << "Exported " << result.frames << " frames, " << result.notes
      << " notes (peak " << result.peak_frame_notes << "/frame).\n\n"
      << "Steam Rhythia settings:\n"
      << "  Note Scale: " << note_scale << '\n';
    if (options.brush_size > 1)
      std::cout << "  Note Opacity: 100%\n  Fade Length: 0\n";
    std::cout << "  AR: " << approach_rate << '\n'
      << "  SD: 0.01\n"
      << "  Background (R, G, B): " << ((options.background >> 16) & 255) << ", "
      << ((options.background >> 8) & 255) << ", " << (options.background & 255) << '\n'
      << "  Speed: 1x\n"
      << "  Notes: Solid squares\n"
      << "  Playback: Visualize (Auto)\n"
      << "  Colorset: " << utf8(options.colorset.wstring()) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
