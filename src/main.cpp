#include "converter.h"
#include "process.h"
#include "version.h"

#include <Windows.h>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

static void usage(bool all) {
  std::cout <<
    "Video beatmaps for Rhythia\n"
    "Usage: mp42rhm input.mp4 output [options]\n\n"
    "  --mode MODE             bw, grayscale, color (bw)\n"
    "  --width N --height N    Resolution (160x90)\n"
    "  --fps RATE              Frame rate, fraction, or native (12)\n"
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
    "  --experimental-cuda     NVIDIA encoder (default brush size: 32)\n"
    "  --adaptive-palette      Per-frame color palette, up to 65536 colors\n"
    "  --compact-colorset      Repeating BW/grayscale brush colorset\n"
    "  --subtitles N           Text subtitle track in a black band (1-based)\n"
    "  --span N                Image width in grid units (rounded)\n"
    "  --palette-cycle PATH    Custom color palette\n"
    "  --threshold N           Black/white threshold (128)\n"
    "  --invert                Invert brightness\n"
    "  --background HEX        Background color (000000)\n"
    "  --start SECONDS         Clip start (0)\n"
    "  --no-audio              Exclude audio\n"
    "  --colorset PATH         Colorset output (<output>-colorset.txt)\n"
    "  --ffmpeg PATH           FFmpeg executable\n"
    "  --ffprobe PATH          ffprobe executable\n"
    "  --version               Print version\n";
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

static std::string file_size_label(const std::filesystem::path& path) {
  static constexpr const char* UNITS[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double size = static_cast<double>(std::filesystem::file_size(path));
  size_t unit = 0;
  std::ostringstream label;

  while (size >= 1024 && unit < 4) {
    size /= 1024;
    ++unit;
  }
  label << std::fixed << std::setprecision(unit ? 2 : 0) << size << ' ' << UNITS[unit];
  return label.str();
}

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 2 && std::wstring(argv[1]) == L"--version") {
      std::cout << "mp42rhm " << mp42rhm::VERSION << '\n';
      return 0;
    }
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
    bool native_fps = false;

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
      if (key == L"--adaptive-palette") {
        options.adaptive_palette = true;
        continue;
      }
      if (key == L"--compact-colorset") {
        options.compact_colorset = true;
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
        options.color_count = static_cast<uint32_t>(integer(value(), 65536));
        if (options.color_count < 2)
          throw std::runtime_error("Colors must be at least 2");
      } else if (key == L"--width")
        options.width = static_cast<uint32_t>(integer(value(), 3840));
      else if (key == L"--height")
        options.height = static_cast<uint32_t>(integer(value(), 2160));
      else if (key == L"--fps") {
        const auto rate = value();

        native_fps = rate == L"native";
        if (!native_fps)
          options.fps = mp42rhm::parse_frame_rate(utf8(rate));
      } else if (key == L"--subtitles") {
        options.subtitle_track = static_cast<uint32_t>(integer(value(), UINT32_MAX));
        if (!options.subtitle_track)
          throw std::runtime_error("Subtitle tracks are numbered from 1");
      } else if (key == L"--brush-size") {
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
    if (!mode_set && (!options.palette_cycle.empty() || options.adaptive_palette))
      options.mode = mp42rhm::ColorMode::Color;
    if (!brush_set && (options.mode != mp42rhm::ColorMode::Bw || options.compact_colorset))
      options.brush_size = options.experimental_cuda ? 32 : 8;
    if (options.colorset.empty())
      options.colorset = options.output.parent_path() / (options.output.stem().wstring() + L"-colorset.txt");
    if (native_fps) {
      mp42rhm::Process probe({options.ffprobe, L"-v", L"error", L"-select_streams", L"v:0",
        L"-show_entries", L"stream=avg_frame_rate", L"-of", L"default=nw=1:nk=1", options.input.wstring()});

      options.fps = mp42rhm::parse_frame_rate(probe.finish());
    }
    mp42rhm::validate(options);
    const auto settings_path = options.output.parent_path() / (options.output.stem().wstring() + L"-settings.txt");

    if (std::filesystem::exists(settings_path) || _wcsicmp(
      std::filesystem::absolute(settings_path).lexically_normal().c_str(),
      std::filesystem::absolute(options.colorset).lexically_normal().c_str()) == 0)
      throw std::runtime_error("Settings must name a separate new file: " + utf8(settings_path.wstring()));

    std::cerr << "Converting " << options.width << 'x' << options.height << " at " << options.fps.value() << " fps...\n";

    const auto started = std::chrono::steady_clock::now();
    const auto result = mp42rhm::convert(options);
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    // Round AR up to avoid overlapping quantized video frames
    const double approach_rate = std::ceil(1000.0 / std::floor(1000.0 / options.fps.value())) / 100;
    std::ostringstream summary;

    summary << "Exported " << result.frames << " frames, " << result.notes
      << " notes (peak " << result.peak_frame_notes << "/frame).\n"
      << "Time: " << std::fixed << std::setprecision(1) << elapsed << " s\n"
      << "Map: " << utf8(std::filesystem::absolute(options.output).wstring())
      << " (" << file_size_label(options.output) << ")\n"
      << "Colorset: " << utf8(std::filesystem::absolute(options.colorset).wstring())
      << " (" << file_size_label(options.colorset) << ")\n"
      << "Settings: " << utf8(std::filesystem::absolute(settings_path).wstring()) << "\n\n"
      << "Rhythia settings:\n"
      << "  Note Scale: " << std::setprecision(2) << mp42rhm::note_scale(options) << '\n';
    if (options.brush_size > 1)
      summary << "  Note Opacity: 100%\n  Fade Length: 0\n";
    summary << "  AR: " << approach_rate << '\n'
      << "  SD: 0.01\n"
      << "  FOV: 30 (starting point)\n"
      << "  Background (R, G, B): " << ((options.background >> 16) & 255) << ", "
      << ((options.background >> 8) & 255) << ", " << (options.background & 255) << '\n'
      << "  Speed: 1x\n"
      << "  Notes: Solid squares\n"
      << "  Playback: Visualize (Auto)\n"
      << "\nmp42rhm " << mp42rhm::VERSION << '\n';

    const auto text = summary.str();
    const HANDLE settings = CreateFileW(settings_path.c_str(), GENERIC_WRITE, 0, nullptr,
      CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

    std::cout << text;
    if (settings == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Map and colorset exported, but settings file could not be created");

    DWORD written = 0;
    const BOOL saved = WriteFile(settings, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);

    CloseHandle(settings);
    if (!saved || written != text.size()) {
      DeleteFileW(settings_path.c_str());
      throw std::runtime_error("Map and colorset exported, but settings file could not be saved");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Error: " << error.what() << '\n';
    return 1;
  }
}
