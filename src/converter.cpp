#include "converter.h"
#include "brush_optimizer.h"
#include "subtitles.h"
#include "cuda_brush.h"
#include "archive.h"
#include "process.h"
#include "note_order.h"
#include "temporary_directory.h"
#include <rhmParse/rhmParse.h>
#include <bcrypt.h>
#include <intrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <chrono>
#include <cstring>
#include <fstream>
#include <future>
#include <deque>
#include <iomanip>
#include <iostream>
#include <locale>
#include <map>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

namespace mp42rhm {
  namespace fs = std::filesystem;

  uint32_t parse_color(const std::string& text) {
    const auto hex = !text.empty() && text.front() == '#' ? text.substr(1) : text;

    if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
      throw std::runtime_error("Expected a six-digit RGB hex color");
    return static_cast<uint32_t>(std::stoul(hex, nullptr, 16));
  }

  double note_scale(const Options& options) {
    const double scale = (options.span == 0 ? 0.01 : options.span / options.width) * options.brush_size;

    // Rhythia only accepts two decimal places
    return std::max(0.01, std::round(scale * 100) / 100);
  }

  static std::string color_hex(uint32_t color) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result = "000000";

    for (size_t i = 0; i < 6; ++i)
      result[5 - i] = HEX[(color >> (i * 4)) & 15];
    return result;
  }

  struct PaletteCycle {
    std::vector<uint32_t> colors;
    std::vector<size_t> slots;
    std::vector<size_t> quotas;
  };

  static PaletteCycle read_palette(const fs::path& path, uint32_t background) {
    std::ifstream input(path);
    PaletteCycle palette;
    std::string line;

    if (!input)
      throw std::runtime_error("Cannot open palette cycle");
    input.exceptions(std::ios::badbit);
    while (std::getline(input, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      const uint32_t color = parse_color(line);

      if (color == background)
        throw std::runtime_error("Palette cycle must exclude the background color");
      const auto found = std::find(palette.colors.begin(), palette.colors.end(), color);
      const size_t index = static_cast<size_t>(found - palette.colors.begin());

      if (found == palette.colors.end()) {
        if (palette.colors.size() == 255)
          throw std::runtime_error("Palette supports 255 visible colors plus the background");
        palette.colors.push_back(color);
        palette.quotas.push_back(0);
      }
      palette.slots.push_back(index);
      ++palette.quotas[index];
    }
    if (palette.slots.empty())
      throw std::runtime_error("Palette cycle is empty");
    return palette;
  }

  void validate(const Options& options) {
    if (options.width < 2 || options.width > 3840 || options.height < 2 || options.height > 2160)
      throw std::runtime_error("Sampling size must be between 2x2 and 3840x2160");
    if (!options.fps.denominator || options.fps.value() < 1 || options.fps.value() > 60)
      throw std::runtime_error("FPS must be between 1 and 60");
    if (options.brush_size < 1 || options.brush_size > 64)
      throw std::runtime_error("Brush size must be between 1 and 64");
    if (options.experimental_cuda && (options.brush_size == 1 ||
      (options.mode == ColorMode::Bw && !options.compact_colorset)))
      throw std::runtime_error("--experimental-cuda requires 2..64-pixel color, grayscale, or compact BW brushes");
    if (options.brush_size > 1 && options.mode == ColorMode::Bw && !options.compact_colorset)
      throw std::runtime_error("BW brushes require --compact-colorset");
    if (options.threshold > 255 || options.max_notes == 0)
      throw std::runtime_error("Threshold must be 0..255 and max-notes must be positive");
    if (options.color_count != 0 && (options.color_count < 2 || options.color_count > (options.adaptive_palette ? 65536U : 256U)))
      throw std::runtime_error("Colors must be 2..256, or 2..65536 with --adaptive-palette");
    if (options.adaptive_palette && (options.mode != ColorMode::Color ||
      options.brush_size == 1 || !options.palette_cycle.empty()))
      throw std::runtime_error("--adaptive-palette requires color brushes without --palette-cycle");
    if (options.compact_colorset && (options.mode == ColorMode::Color || options.brush_size == 1 || options.colorset.empty()))
      throw std::runtime_error("--compact-colorset requires BW or grayscale brushes and a colorset output");
    if (options.subtitle_track && options.height < 120)
      throw std::runtime_error("Subtitles require an output height of at least 120 pixels");
    if (options.color_count && (options.mode == ColorMode::Bw || !options.palette_cycle.empty()))
      throw std::runtime_error("--colors requires grayscale or automatic color mode");
    if (!options.palette_cycle.empty() && options.mode != ColorMode::Color)
      throw std::runtime_error("--palette-cycle requires color mode");
    if (options.mode != ColorMode::Color && options.background != 0)
      throw std::runtime_error("Black and white/grayscale modes require a black background");
    if (options.mode != ColorMode::Bw && options.colorset.empty())
      throw std::runtime_error("Color and grayscale modes require a colorset output");
    if (!options.palette_cycle.empty() && (options.colorset.empty() || !fs::is_regular_file(options.palette_cycle)))
      throw std::runtime_error("Palette-cycle mode requires a palette file and --colorset output");
    if (options.background > 0xffffff)
      throw std::runtime_error("Background must be a 24-bit RGB color");
    if (!std::isfinite(options.span) || options.span < 0 || options.span > 100)
      throw std::runtime_error("Span must be 0 (automatic) or at most 100 grid units");
    if (!std::isfinite(options.start) || options.start < 0 ||
      !std::isfinite(options.seconds) || options.seconds < 0 ||
      options.start + options.seconds > INT32_MAX / 1000.0)
      throw std::runtime_error("Invalid start/duration; expected nonnegative seconds within the RHM time range");
    if (!fs::is_regular_file(options.input))
      throw std::runtime_error("Input video does not exist");
    if (options.output.empty() || fs::exists(options.output))
      throw std::runtime_error("Output must name a new file; existing files are never overwritten");
    if (!options.colorset.empty() && (fs::exists(options.colorset) ||
      fs::absolute(options.colorset).lexically_normal() == fs::absolute(options.output).lexically_normal() ||
      !fs::is_directory(fs::absolute(options.colorset).parent_path())))
      throw std::runtime_error("Colorset must name a separate new file in an existing directory");
    if (!fs::is_directory(fs::absolute(options.output).parent_path()))
      throw std::runtime_error("Output directory does not exist");
    const auto extension = options.format == Format::Sspm ? L".sspm" : L".rhm";

    if (_wcsicmp(options.output.extension().c_str(), extension) != 0)
      throw std::runtime_error("Output extension must match --format (sspm or rhm)");
    if (options.format == Format::Sspm &&
      (options.title.size() > UINT16_MAX || options.difficulty_name.size() > UINT16_MAX))
      throw std::runtime_error("SSPM metadata strings cannot exceed 65535 UTF-8 bytes");
  }

  static std::string json_string(const std::string& text) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string result = "\"";

    for (unsigned char ch : text) {
      if (ch == '"' || ch == '\\') {
        result += '\\';
        result += static_cast<char>(ch);
      } else if (ch < 32) {
        result += "\\u00";
        result += HEX[ch >> 4];
        result += HEX[ch & 15];
      } else {
        result += static_cast<char>(ch);
      }
    }
    return result + '"';
  }

  static std::vector<std::wstring> input_arguments(const Options& options) {
    return {options.ffmpeg, L"-hide_banner", L"-loglevel", L"error", L"-nostdin",
      L"-ss", std::to_wstring(options.start), L"-i", fs::absolute(options.input).wstring()};
  }

  static PaletteCycle generate_palette(const Options& options, const std::wstring& filter) {
    PaletteCycle palette;
    const uint32_t count = options.color_count ? options.color_count :
      options.mode == ColorMode::Grayscale ? 4 : 64;

    if (options.mode == ColorMode::Bw)
      return {{0xffffff}, {0}, {1}};
    if (options.mode == ColorMode::Grayscale) {
      for (uint32_t i = 1; i < count; ++i)
        palette.colors.push_back((i * 255 / (count - 1)) * 0x010101);
    } else {
      std::cerr << "Analyzing colors...\n";
      auto arguments = input_arguments(options);
      auto palette_filter = filter;

      if (options.seconds > 0)
        arguments.insert(arguments.end() - 2, {L"-t", std::to_wstring(options.seconds + 1.0 / options.fps.value())});
      if (options.seconds > 0)
        palette_filter += L",trim=duration=" + std::to_wstring(options.seconds);
      palette_filter += L",palettegen=max_colors=" + std::to_wstring(count) +
        (count == 2 ? L":reserve_transparent=0" : L":reserve_transparent=1") + L":stats_mode=full";
      arguments.insert(arguments.end(), {L"-vf", palette_filter, L"-an", L"-sn",
        L"-pix_fmt", L"rgba", L"-f", L"rawvideo", L"pipe:1"});
      Process generator(arguments);
      const auto bytes = generator.finish();

      if (bytes.size() != 256 * 4)
        throw std::runtime_error("No palette generated; check the clip interval");
      for (size_t i = 0; i < bytes.size(); i += 4) {
        const uint32_t color = (uint32_t(static_cast<uint8_t>(bytes[i])) << 16) |
          (uint32_t(static_cast<uint8_t>(bytes[i + 1])) << 8) | static_cast<uint8_t>(bytes[i + 2]);

        if (bytes[i + 3] != 0 && color != options.background &&
          std::find(palette.colors.begin(), palette.colors.end(), color) == palette.colors.end())
          palette.colors.push_back(color);
      }
      // FFmpeg needs two opaque colors when transparency is disabled
      if (count == 2 && palette.colors.size() == 2) {
        const auto a = palette.colors[0], b = palette.colors[1];

        palette.colors = {((((a >> 16) + (b >> 16)) / 2) << 16) |
          (((((a >> 8) & 255) + ((b >> 8) & 255)) / 2) << 8) | (((a & 255) + (b & 255)) / 2)};
      }
    }
    if (palette.colors.empty())
      throw std::runtime_error("The clip contains only the background color");
    for (size_t i = 0; i < palette.colors.size(); ++i) {
      palette.slots.push_back(i);
      palette.quotas.push_back(1);
    }
    return palette;
  }

  static std::string coordinate(double value) {
    char buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::general, 9);

    if (result.ec != std::errc{})
      throw std::runtime_error("Cannot format note coordinate");
    return {buffer, result.ptr};
  }

  static std::vector<uint32_t> make_brush_cycle(const std::map<uint32_t, uint64_t>& frequency) {
    double weight = 0;
    std::vector<uint32_t> colors, quotas, cycle;
    std::vector<int64_t> credit(frequency.size());
    uint32_t length = 0;

    for (const auto& entry : frequency)
      weight += std::sqrt(double(entry.second));
    for (const auto& [color, count] : frequency) {
      const auto quota = std::max(1U, static_cast<uint32_t>(std::round(std::sqrt(double(count)) / weight * frequency.size() * 4)));

      colors.push_back(color);
      quotas.push_back(quota);
      length += quota;
    }
    for (uint32_t slot = 0; slot < length; ++slot) {
      size_t best = 0;

      for (size_t i = 0; i < colors.size(); ++i) {
        credit[i] += quotas[i];
        if (credit[i] > credit[best])
          best = i;
      }
      cycle.push_back(colors[best]);
      credit[best] -= length;
    }
    std::rotate(cycle.begin(), std::find(cycle.begin(), cycle.end(), 0U), cycle.end());
    return cycle;
  }

  static void write_sspm_v2(const fs::path& output_path, const fs::path& notes_path,
    const fs::path& audio_path, const std::string& title, const std::string& song_name,
    const std::string& difficulty_name, uint32_t last_note, uint32_t count) {
    rhm::Rhm metadata;

    metadata.map.legacyId = "mp42rhm-" + std::to_string(GetTickCount64()) + '-' + std::to_string(GetCurrentProcessId());
    metadata.map.title = title;
    metadata.map.songName = song_name;
    metadata.map.mappers = {"mp42rhm"};
    metadata.map.customDifficultyName = difficulty_name;

    auto header = rhm::ToSspm(metadata);
    const uint64_t audio_size = audio_path.empty() ? 0 : fs::file_size(audio_path);
    const uint64_t note_size = uint64_t(count) * 14;
    uint64_t definition_offset = 0;

    std::memcpy(&definition_offset, header.data() + 96, 8);
    // Insert streamed audio before definitions and markers, then update SSPM v2 pointers
    const auto patch = [&](size_t offset, auto value) {
      std::memcpy(header.data() + offset, &value, sizeof(value));
    };

    patch(30, last_note);
    patch(34, count);
    patch(38, count);
    header[45] = audio_size != 0;
    patch(64, audio_size ? definition_offset : uint64_t(0));
    patch(72, audio_size);
    patch(96, definition_offset + audio_size);
    patch(112, uint64_t(header.size()) + audio_size);
    patch(120, note_size);

    BCRYPT_HASH_HANDLE raw_hash = nullptr;

    if (BCryptCreateHash(BCRYPT_SHA1_ALG_HANDLE, &raw_hash, nullptr, 0, nullptr, 0, 0) < 0)
      throw std::runtime_error("Cannot initialize SSPM SHA1");

    std::unique_ptr<void, decltype(&BCryptDestroyHash)> hash(raw_hash, BCryptDestroyHash);
    std::ofstream output(output_path, std::ios::binary);
    const auto definition_size = static_cast<ULONG>(header.size() - definition_offset);
    std::vector<char> buffer(1024 * 1024);
    const auto copy = [&](const fs::path& path, bool hash_data) {
      std::ifstream input(path, std::ios::binary);

      input.exceptions(std::ios::badbit);
      if (!input)
        throw std::runtime_error("Cannot open SSPM payload");
      while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto size = input.gcount();

        output.write(buffer.data(), size);
        if (hash_data && BCryptHashData(hash.get(), reinterpret_cast<PUCHAR>(buffer.data()),
          static_cast<ULONG>(size), 0) < 0)
          throw std::runtime_error("Cannot hash SSPM markers");
      }
    };

    output.exceptions(std::ios::failbit | std::ios::badbit);
    output.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(definition_offset));
    if (audio_size)
      copy(audio_path, false);
    output.write(reinterpret_cast<const char*>(header.data() + definition_offset), definition_size);
    if (BCryptHashData(hash.get(), header.data() + definition_offset, definition_size, 0) < 0)
      throw std::runtime_error("Cannot hash SSPM definitions");
    copy(notes_path, true);

    std::array<uint8_t, 20> digest{};

    if (BCryptFinishHash(hash.get(), digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
      throw std::runtime_error("Cannot finish SSPM SHA1");
    output.seekp(10);
    output.write(reinterpret_cast<const char*>(digest.data()), digest.size());
    output.close();
  }

  static std::vector<BrushStroke> compact_brush_strokes(const std::vector<BrushStroke>& strokes,
    const std::vector<uint8_t>& pixels, const Options& options) {
    const int32_t brush = static_cast<int32_t>(options.brush_size), margin = brush * 2;
    const int32_t width = options.width + margin * 2, height = options.height + margin * 2;
    const int32_t words = (width + 63) / 64 + 1;
    const uint64_t mask = brush == 64 ? UINT64_MAX : (uint64_t(1) << brush) - 1;
    std::vector<uint32_t> target(size_t(width) * height, options.background);
    std::vector<uint64_t> covered(size_t(words) * height);
    std::vector<BrushStroke> result;

    for (uint32_t y = 0; y < options.height; ++y)
      for (uint32_t x = 0; x < options.width; ++x) {
        const size_t pixel = (size_t(y) * options.width + x) * 3;

        target[size_t(y + margin) * width + x + margin] = (uint32_t(pixels[pixel]) << 16) |
          (uint32_t(pixels[pixel + 1]) << 8) | pixels[pixel + 2];
      }
    const auto exposed = [&](int32_t x, int32_t y) {
      const size_t word = size_t(y) * words + x / 64;
      const unsigned shift = x % 64;

      return ((~covered[word] >> shift) | (shift ? ~covered[word + 1] << (64 - shift) : 0)) & mask;
    };

    for (auto stroke = strokes.rbegin(); stroke != strokes.rend(); ++stroke) {
      const int32_t x = stroke->x + margin, y = stroke->y + margin;
      int32_t left = x + brush, top = y + brush, right = x - 1, bottom = y - 1;
      int best_count = 0;

      for (int32_t row = y; row < y + brush; ++row) {
        const uint64_t bits = exposed(x, row);
        unsigned long low, high;

        if (!bits)
          continue;
        _BitScanForward64(&low, bits);
        _BitScanReverse64(&high, bits);
        left = std::min(left, x + static_cast<int32_t>(low));
        right = std::max(right, x + static_cast<int32_t>(high));
        top = std::min(top, row);
        bottom = row;
        best_count += static_cast<int>(__popcnt64(bits));
      }
      if (best_count == 0)
        continue;

      // Keep every exposed pixel of the original stroke
      const int32_t min_x = std::max(0, right - brush + 1), max_x = std::min(left, width - brush);
      const int32_t min_y = std::max(0, bottom - brush + 1), max_y = std::min(top, height - brush);
      int32_t best_x = x, best_y = y;

      for (int32_t cy = min_y; cy <= max_y; ++cy)
        for (int32_t cx = min_x; cx <= max_x; ++cx) {
          if (cx == x && cy == y)
            continue;
          int count = 0;
          bool valid = true;

          for (int32_t row = cy; row < cy + brush && valid; ++row) {
            uint64_t bits = exposed(cx, row);

            count += static_cast<int>(__popcnt64(bits));
            while (bits) {
              unsigned long bit;

              _BitScanForward64(&bit, bits);
              if (target[size_t(row) * width + cx + bit] != stroke->color) {
                // This pixel also blocks every placement up to its column
                cx = std::min(max_x, cx + static_cast<int32_t>(bit));
                valid = false;
                break;
              }
              bits &= bits - 1;
            }
          }
          if (valid && count > best_count) {
            best_count = count;
            best_x = cx;
            best_y = cy;
          }
        }
      for (int32_t row = best_y; row < best_y + brush; ++row) {
        const size_t word = size_t(row) * words + best_x / 64;
        const unsigned shift = best_x % 64;

        covered[word] |= mask << shift;
        if (shift)
          covered[word + 1] |= mask >> (64 - shift);
      }
      result.push_back({best_x - margin, best_y - margin, stroke->color});
    }
    std::reverse(result.begin(), result.end());
    return result;
  }

  static std::vector<BrushStroke> remove_redundant_strokes(const std::vector<BrushStroke>& strokes,
    const Options& options) {
    const int32_t brush = static_cast<int32_t>(options.brush_size), margin = brush * 2;
    const int32_t width = options.width + margin * 2, height = options.height + margin * 2;
    std::vector<uint32_t> owner(size_t(width) * height, UINT32_MAX), canvas(owner.size(), options.background);
    std::vector<BrushStroke> result;

    for (size_t i = 0; i < strokes.size(); ++i) {
      const auto& stroke = strokes[i];

      for (int32_t y = stroke.y + margin; y < stroke.y + margin + brush; ++y)
        std::fill_n(owner.begin() + size_t(y) * width + stroke.x + margin, brush, static_cast<uint32_t>(i));
    }
    for (size_t i = 0; i < strokes.size(); ++i) {
      const auto& stroke = strokes[i];
      bool needed = false;

      for (int32_t y = stroke.y + margin; y < stroke.y + margin + brush && !needed; ++y)
        for (int32_t x = stroke.x + margin; x < stroke.x + margin + brush; ++x) {
          const size_t pixel = size_t(y) * width + x;

          if (owner[pixel] == i && canvas[pixel] != stroke.color) {
            needed = true;
            break;
          }
        }
      if (!needed)
        continue;
      result.push_back(stroke);
      for (int32_t y = stroke.y + margin; y < stroke.y + margin + brush; ++y)
        std::fill_n(canvas.begin() + size_t(y) * width + stroke.x + margin, brush, stroke.color);
    }
    // Retain a timestamp for background-only frames
    if (result.empty())
      result.push_back(strokes.back());
    return result;
  }

  static std::vector<BrushStroke> paint_frame(const std::vector<uint8_t>& pixels, const Options& options,
    const PaletteCycle& palette, CudaBrushEncoder* cuda = nullptr) {
    const int32_t brush = static_cast<int32_t>(options.brush_size);
    auto colors = palette.colors;
    std::unordered_map<uint32_t, uint16_t> indices;

    if (options.adaptive_palette)
      for (size_t i = 0; i < pixels.size(); i += 3) {
        const uint32_t color = uint32_t(pixels[i]) << 16 | uint32_t(pixels[i + 1]) << 8 | pixels[i + 2];

        if (color != options.background && indices.emplace(color, uint16_t{0}).second)
          colors.push_back(color);
      }
    if (colors.size() > 65535)
      throw std::runtime_error("Frame exceeds 65535 foreground colors");
    const auto background = static_cast<uint16_t>(colors.size());
    std::vector<uint16_t> canvas(size_t(options.width) * options.height), target(canvas.size()), indexed(canvas.size());
    std::vector<BrushStroke> best, second, strokes;
    std::vector<std::vector<BrushStroke>> candidates;

    colors.push_back(options.background);
    for (size_t i = 0; i < colors.size(); ++i)
      indices[colors[i]] = static_cast<uint16_t>(i);
    for (size_t i = 0; i < indexed.size(); ++i) {
      const uint32_t color = (uint32_t(pixels[i * 3]) << 16) |
        (uint32_t(pixels[i * 3 + 1]) << 8) | pixels[i * 3 + 2];

      indexed[i] = indices.at(color);
    }
    for (int scan = 0; scan < 16; ++scan) {
      const int orientation = scan == 0 ? 0 : scan < 9 ? scan - 1 : scan - 8;
      const int32_t width = (orientation & 4) ? options.height : options.width;
      const int32_t height = (orientation & 4) ? options.width : options.height;

      const bool raster = scan == 0 || scan >= 9;
      std::vector<uint16_t> frontier(raster ? width : 0);
      std::vector<int32_t> expires(raster ? width : 0);

      if (!raster)
        std::fill(canvas.begin(), canvas.end(), background);
      strokes.clear();
      for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x) {
          const int32_t rx = (orientation & 1) ? width - 1 - x : x;
          const int32_t ry = (orientation & 2) ? height - 1 - y : y;
          const size_t pixel = (orientation & 4) ? size_t(rx) * options.width + ry : size_t(ry) * options.width + rx;

          target[size_t(y) * width + x] = indexed[pixel];
        }
      const auto paint = [&](int32_t x, int32_t y) {
        const uint16_t color = target[size_t(y) * width + x];

        if (raster) {
          if ((expires[x] > y ? frontier[x] : background) == color)
            return;
          strokes.push_back({x, y, colors[color]});
          // Later raster strokes expire no earlier than the strokes they cover
          std::fill_n(frontier.begin() + x, std::min(brush, width - x), color);
          std::fill_n(expires.begin() + x, std::min(brush, width - x), y + brush);
        } else {
          if (canvas[size_t(y) * width + x] == color)
            return;
          strokes.push_back({x, y, colors[color]});
          for (int32_t row = y; row < std::min(height, y + brush); ++row)
            std::fill_n(canvas.begin() + size_t(row) * width + x, std::min(brush, width - x), color);
        }
      };

      if (scan == 0 || scan >= 9) {
        for (int32_t y = 0; y < height; ++y)
          for (int32_t x = 0; x < width; ++x)
            paint(x, y);
      } else {
        // Morton order preserves painted pixels above and to the left
        const auto visit = [&](const auto& self, int32_t x, int32_t y, int32_t size) -> void {
          if (x >= width || y >= height)
            return;
          if (size == 1) {
            paint(x, y);
            return;
          }
          const int32_t half = size / 2;

          self(self, x, y, half);
          self(self, x + half, y, half);
          self(self, x, y + half, half);
          self(self, x + half, y + half, half);
        };
        int32_t size = 1;

        while (size < std::max(width, height))
          size *= 2;
        visit(visit, 0, 0, size);
      }
      // Erase brush spill before reflecting back to image coordinates
      for (int32_t y = 0; y < height + brush; y += brush)
        strokes.push_back({width, y, options.background});
      for (int32_t x = 0; x < width; x += brush)
        strokes.push_back({x, height, options.background});
      for (auto& stroke : strokes) {
        if (orientation & 1)
          stroke.x = width - stroke.x - brush;
        if (orientation & 2)
          stroke.y = height - stroke.y - brush;
        if (orientation & 4)
          std::swap(stroke.x, stroke.y);
      }
      candidates.push_back(std::move(strokes));
    }
    if (cuda) {
      cuda->compact(candidates, pixels);
      return reorder_brush_strokes(std::move(candidates[0]), pixels, options, candidates[1]);
    }
    const size_t workers = std::max(1U, std::min(4U, std::thread::hardware_concurrency()));

    for (size_t begin = 0; begin < candidates.size(); begin += workers) {
      std::vector<std::future<std::vector<BrushStroke>>> jobs;

      for (size_t i = begin; i < std::min(candidates.size(), begin + workers); ++i)
        jobs.push_back(std::async(std::launch::async, [&, i] {
          return remove_redundant_strokes(compact_brush_strokes(candidates[i], pixels, options), options);
        }));
      for (auto& job : jobs) {
        auto candidate = job.get();

        if (best.empty() || candidate.size() < best.size()) {
          second = std::move(best);
          best = std::move(candidate);
        } else if (second.empty() || candidate.size() < second.size())
          second = std::move(candidate);
      }
    }
    candidates.clear();
    return reorder_brush_strokes(std::move(best), pixels, options, second);
  }

  Statistics convert(const Options& options) {
    validate(options);
    std::unique_ptr<CudaBrushEncoder> cuda;

    if (options.experimental_cuda) {
      cuda = std::make_unique<CudaBrushEncoder>(options);
      std::cerr << "Experimental CUDA: " << cuda->parallel_frames() << " frames in flight.\n";
    }

    TemporaryDirectory temporary(fs::absolute(options.output).parent_path());
    const auto archive_path = temporary.path / options.output.filename();
    const bool sspm = options.format == Format::Sspm;
    const bool brush_mode = options.brush_size > 1;
    const bool cycle_mode = options.mode != ColorMode::Bw || brush_mode;
    const bool indexed_decode = options.adaptive_palette && options.color_count <= 256;
    const bool binary = sspm || cycle_mode;
    const auto song_name = options.title.empty() ? options.input.stem().u8string() : options.title;
    const auto title = options.title.empty() ? song_name + " [video]" : options.title;
    auto notes_path = temporary.path / L"notes.bin";
    std::unique_ptr<Archive> archive;
    std::ofstream binary_notes;

    if (binary)
      binary_notes.open(notes_path, std::ios::binary);
    if (!sspm)
      archive = std::make_unique<Archive>(archive_path, true);

    auto& map = binary ? static_cast<std::ostream&>(binary_notes) : archive->map();
    map.exceptions(std::ios::failbit | std::ios::badbit);
    map.imbue(std::locale::classic());
    map << std::setprecision(9);
    if (!sspm)
      archive->map() << "{\"OnlineId\":null,\"OnlineStatus\":null,\"LegacyId\":"
      << json_string("mp42rhm-" + std::to_string(GetTickCount64()) + "-" + std::to_string(GetCurrentProcessId()))
      << ",\"SongName\":" << json_string(song_name)
      << ",\"Mappers\":[\"mp42rhm\"],\"Title\":"
      << json_string(title)
      << ",\"Difficulty\":0,\"CustomDifficultyName\":"
      << json_string(options.difficulty_name.empty() ? "Video" : options.difficulty_name)
      << ",\"StarRating\":0,\"Notes\":[";

    const auto picture_height = options.height - subtitle_band_height(options);
    const auto subtitle_filter = prepare_subtitles(options, temporary.path);
    std::wstring filter = L"setpts=PTS-STARTPTS,fps=" + options.fps.text() +
      L":start_time=0,scale=" + std::to_wstring(options.width) + L":" +
      std::to_wstring(picture_height) + L":force_original_aspect_ratio=decrease:reset_sar=1:flags=" +
      (cycle_mode ? L"area" : L"lanczos") + L",format=" +
      (options.mode == ColorMode::Color ? L"bgra" : L"gray");

    if (options.invert)
      filter += L",negate";
    if (options.mode == ColorMode::Grayscale || (options.mode == ColorMode::Bw && brush_mode))
      filter += L",format=bgra";
    const auto background_hex = color_hex(options.background);

    filter += L",pad=" + std::to_wstring(options.width) + L":" + std::to_wstring(picture_height) +
      L":(ow-iw)/2:(oh-ih)/2:" + (cycle_mode ? L"0x" + std::wstring(background_hex.begin(), background_hex.end()) : L"black");
    if (options.subtitle_track)
      filter += L",pad=" + std::to_wstring(options.width) + L":" + std::to_wstring(options.height) +
        L":0:0:black" + subtitle_filter;
    if (options.mode == ColorMode::Bw && brush_mode)
      filter += L",format=gray,lut=c0='if(gte(val," + std::to_wstring(options.threshold) + L"),255,0)',format=bgra";

    const auto palette = !options.palette_cycle.empty() ? read_palette(options.palette_cycle, options.background) :
      cycle_mode && !options.adaptive_palette ? generate_palette(options, filter) : PaletteCycle{};
    std::ofstream colors;

    if (!options.colorset.empty()) {
      colors.open(temporary.path / L"colors.txt", std::ios::binary);
      colors.exceptions(std::ios::failbit | std::ios::badbit);
      if (cycle_mode && !brush_mode)
        for (size_t slot : palette.slots)
          colors << color_hex(palette.colors[slot]) << '\n';
      if (!cycle_mode)
        colors << "ffffff\n";
    }

    auto arguments = input_arguments(options);

    if (cycle_mode && !options.adaptive_palette) {
      const auto palette_path = temporary.path / L"palette.ppm";
      std::ofstream palette_image(palette_path, std::ios::binary);

      palette_image.exceptions(std::ios::failbit | std::ios::badbit);
      palette_image << "P6\n16 16\n255\n";
      for (size_t pixel = 0; pixel < 256; ++pixel) {
        const auto index = pixel * (palette.colors.size() + 1) / 256;
        const uint32_t color = index == palette.colors.size() ? options.background : palette.colors[index];

        palette_image.put(static_cast<char>(color >> 16));
        palette_image.put(static_cast<char>(color >> 8));
        palette_image.put(static_cast<char>(color));
      }
      palette_image.close();
      arguments.insert(arguments.end(), {L"-i", palette_path.wstring()});
    }
    if (options.seconds > 0)
      arguments.insert(arguments.end(), {L"-t", std::to_wstring(options.seconds)});
    if (options.adaptive_palette) {
      const auto count = options.color_count ? options.color_count : 64;

      filter += L",format=rgb24,elbg=codebook_length=" + std::to_wstring(count - 1) + L":nb_steps=1:seed=1";
      if (indexed_decode)
        filter += L":pal8=1";
      arguments.insert(arguments.end(), {L"-map", L"0:v:0", L"-vf", filter});
    } else if (cycle_mode)
      arguments.insert(arguments.end(), {L"-filter_complex", L"[0:v]" + filter + L"[video];[video][1:v]paletteuse=dither=none[out]", L"-map", L"[out]"});
    else
      arguments.insert(arguments.end(), {L"-map", L"0:v:0", L"-vf", filter});
    arguments.insert(arguments.end(), {L"-an", L"-sn",
      L"-pix_fmt", indexed_decode ? L"pal8" : cycle_mode ? L"rgb24" : L"gray", L"-f", L"rawvideo", L"pipe:1"});

    Process decoder(arguments);
    std::vector<uint8_t> pixels(size_t(options.width) * options.height * (cycle_mode ? 3 : 1));
    std::vector<uint8_t> indexed_frame(indexed_decode ? size_t(options.width) * options.height + 1024 : 0);
    std::vector<uint8_t> previous_pixels;
    std::vector<BrushStroke> previous_strokes;
    std::vector<std::string> x_coordinates(options.width);
    std::vector<std::string> positions(brush_mode ? 0 : size_t(options.width) * options.height);
    const double pitch = note_scale(options) / options.brush_size;
    Statistics statistics;
    uint32_t last_note = 0;
    std::string frame_json;
    std::vector<std::vector<size_t>> buckets(palette.colors.size());
    std::vector<uint32_t> frame_counts;
    auto last_progress = std::chrono::steady_clock::now();

    frame_json.reserve(size_t(options.width) * options.height * (binary ? 14 : 64));

    for (uint32_t x = 0; x < options.width; ++x)
      x_coordinates[x] = coordinate(1 + (x + 0.5 - options.width / 2.0) * pitch);
    for (uint32_t y = 0; !brush_mode && y < options.height; ++y) {
      const auto position_y = coordinate(1 + (y + 0.5 - options.height / 2.0) * pitch);

      for (uint32_t x = 0; x < options.width; ++x) {
        auto& position = positions[size_t(y) * options.width + x];

        if (binary) {
          const float px = std::stof(x_coordinates[x]);
          const float py = std::stof(position_y);

          position.push_back(0);
          position.push_back(1);
          position.append(reinterpret_cast<const char*>(&px), sizeof(px));
          position.append(reinterpret_cast<const char*>(&py), sizeof(py));
        } else {
          position = ",\"X\":" + x_coordinates[x] + ",\"Y\":" + position_y + '}';
        }
      }
    }

    const size_t filler_pixel = positions.size();

    if (cycle_mode) {
      std::string position = ",\"X\":100,\"Y\":100}";

      if (binary) {
        const float offscreen = 100;

        position.clear();
        position.push_back(0);
        position.push_back(1);
        position.append(reinterpret_cast<const char*>(&offscreen), sizeof(offscreen));
        position.append(reinterpret_cast<const char*>(&offscreen), sizeof(offscreen));
      }
      positions.push_back(position);
    }

    std::deque<std::shared_future<std::vector<BrushStroke>>> jobs;
    std::shared_future<std::vector<BrushStroke>> last_job;
    bool decode_finished = false;
    const auto read_frame = [&] {
      auto& frame = indexed_decode ? indexed_frame : pixels;
      size_t received = 0;

      while (received < frame.size()) {
        const size_t count = decoder.read(frame.data() + received, frame.size() - received);

        if (count == 0)
          break;
        received += count;
      }
      if (received && received != frame.size())
        throw std::runtime_error("FFmpeg returned a truncated video frame");
      if (received && indexed_decode) {
        const size_t count = size_t(options.width) * options.height;

        // Raw PAL8 carries 256 native-endian ARGB entries after the indices
        for (size_t i = 0; i < count; ++i) {
          const size_t color = count + size_t(frame[i]) * 4;

          pixels[i * 3] = frame[color + 2];
          pixels[i * 3 + 1] = frame[color + 1];
          pixels[i * 3 + 2] = frame[color];
        }
      }
      return received != 0;
    };

    const auto next_frame = [&] {
      if (cuda) {
        while (jobs.size() < cuda->parallel_frames() && !decode_finished) {
          if (!read_frame()) {
            decode_finished = true;
            break;
          }
          if (pixels != previous_pixels) {
            last_job = std::async(std::launch::async, [&, frame = pixels] {
              return paint_frame(frame, options, palette, cuda.get());
            }).share();
            previous_pixels = pixels;
          }
          jobs.push_back(last_job);
        }
        if (jobs.empty())
          return false;
        previous_strokes = jobs.front().get();
        jobs.pop_front();
      } else {
        if (!read_frame())
          return false;
        if (brush_mode && pixels != previous_pixels) {
          previous_strokes = paint_frame(pixels, options, palette);
          previous_pixels = pixels;
        }
      }
      return true;
    };
    std::vector<uint32_t> compact_cycle;
    std::ifstream staged;
    uint64_t staged_frames = 0;

    if (options.compact_colorset) {
      const auto path = temporary.path / L"brushes.bin";
      std::ofstream staging(path, std::ios::binary);
      std::map<uint32_t, uint64_t> frequency{{0, 1}};

      staging.exceptions(std::ios::badbit | std::ios::failbit);
      std::cerr << "Analyzing brush colors...\n";
      while (next_frame()) {
        const auto count = static_cast<uint32_t>(previous_strokes.size());

        staging.write(reinterpret_cast<const char*>(&count), sizeof(count));
        staging.write(reinterpret_cast<const char*>(previous_strokes.data()), size_t(count) * sizeof(BrushStroke));
        for (const auto& stroke : previous_strokes)
          ++frequency[stroke.color];
        ++staged_frames;
        const auto now = std::chrono::steady_clock::now();

        if (now - last_progress >= std::chrono::seconds(5)) {
          std::cerr << "Analyzed " << staged_frames << " frames\n";
          last_progress = now;
        }
      }
      decoder.finish();
      staging.close();
      compact_cycle = make_brush_cycle(frequency);
      for (auto color : compact_cycle)
        colors << color_hex(color) << '\n';
      staged.open(path, std::ios::binary);
      staged.exceptions(std::ios::badbit | std::ios::failbit);
    }

    while (true) {
      if (options.compact_colorset) {
        if (statistics.frames == staged_frames)
          break;
        uint32_t count;

        staged.read(reinterpret_cast<char*>(&count), sizeof(count));
        previous_strokes.resize(count);
        staged.read(reinterpret_cast<char*>(previous_strokes.data()), size_t(count) * sizeof(BrushStroke));
        previous_strokes = cycle_brush_strokes(std::move(previous_strokes), options, compact_cycle);
      } else if (!next_frame())
        break;

      // Hit times mark frame ends because notes approach before they are hit
      const int32_t time = frame_time(statistics.frames + 1, options.fps);
      auto prefix = binary ? std::string(reinterpret_cast<const char*>(&time), sizeof(time)) :
        ",{\"Time\":" + std::to_string(time);
      uint64_t frame_notes = 0;
      const auto emit = [&](const std::string& position, bool filler = false) {
        if (statistics.notes == options.max_notes)
          throw std::runtime_error("Note limit reached; reduce resolution/duration or raise --max-notes");
        if (sspm && statistics.notes == UINT32_MAX)
          throw std::runtime_error("SSPM cannot store more than 4294967295 notes");
        if (cycle_mode && statistics.notes == INT32_MAX)
          throw std::runtime_error("Rhythia color compensation exceeds the signed array-index range");
        frame_json.append(prefix, !binary && statistics.notes == 0 ? 1 : 0);
        frame_json.append(position);
        ++statistics.notes;
        ++frame_notes;
        if (filler)
          ++statistics.filler_notes;
        last_note = static_cast<uint32_t>(time);
      };

      frame_json.clear();
      if (brush_mode) {
        // Rhythia draws simultaneous notes in reverse index order
        for (auto stroke = previous_strokes.rbegin(); stroke != previous_strokes.rend(); ++stroke) {
          if (stroke->x == INT32_MIN) {
            emit(positions[filler_pixel], true);
            continue;
          }
          const float x = static_cast<float>(1 + (stroke->x + options.brush_size / 2.0 - options.width / 2.0) * pitch);
          const float y = static_cast<float>(1 + (stroke->y + options.brush_size / 2.0 - options.height / 2.0) * pitch);
          std::string position;

          position.push_back(0);
          position.push_back(1);
          position.append(reinterpret_cast<const char*>(&x), sizeof(x));
          position.append(reinterpret_cast<const char*>(&y), sizeof(y));
          emit(position);
          if (!options.compact_colorset)
            colors << color_hex(stroke->color) << '\n';
        }
      } else if (cycle_mode) {
        for (auto& bucket : buckets)
          bucket.clear();
        for (size_t pixel = 0; pixel < size_t(options.width) * options.height; ++pixel) {
          const uint32_t color = (uint32_t(pixels[pixel * 3]) << 16) |
            (uint32_t(pixels[pixel * 3 + 1]) << 8) | pixels[pixel * 3 + 2];

          if (color == options.background)
            continue;
          const auto found = std::find(palette.colors.begin(), palette.colors.end(), color);

          if (found == palette.colors.end())
            throw std::runtime_error("FFmpeg returned a color outside the requested palette");
          buckets[static_cast<size_t>(found - palette.colors.begin())].push_back(pixel);
        }

        size_t cycles = 0;
        std::vector<size_t> consumed(buckets.size(), 0);

        for (size_t color = 0; color < buckets.size(); ++color)
          cycles = std::max(cycles, (buckets[color].size() + palette.quotas[color] - 1) / palette.quotas[color]);
        for (size_t cycle = 0; cycle < cycles; ++cycle)
          for (size_t color : palette.slots) {
            const auto pixel = consumed[color] < buckets[color].size() ? buckets[color][consumed[color]++] : filler_pixel;

            emit(positions[pixel], pixel == filler_pixel);
          }
      } else {
        for (uint32_t y = 0; y < options.height; ++y) {
          for (uint32_t x = 0; x < options.width; ++x) {
            const size_t pixel = size_t(y) * options.width + x;

            if (pixels[pixel] < options.threshold)
              continue;
            emit(positions[pixel]);
          }
        }
      }
      map.write(frame_json.data(), static_cast<std::streamsize>(frame_json.size()));
      if (cycle_mode)
        frame_counts.push_back(static_cast<uint32_t>(frame_notes));
      ++statistics.frames;
      statistics.duration_ms = time;
      statistics.peak_frame_notes = std::max(statistics.peak_frame_notes, frame_notes);
      const auto now = std::chrono::steady_clock::now();

      if (now - last_progress >= std::chrono::seconds(5)) {
        std::cerr << statistics.frames << " frames, " << statistics.notes << " notes\n";
        last_progress = now;
      }
    }
    if (!options.compact_colorset)
      decoder.finish();
    if (statistics.frames == 0)
      throw std::runtime_error("The requested interval contains no video frames");
    if (statistics.notes == 0)
      throw std::runtime_error("The clip has no visible pixels");

    fs::path audio_path;

    if (options.audio) {
      Process probe({options.ffprobe, L"-v", L"error", L"-select_streams", L"a:0",
        L"-show_entries", L"stream=index", L"-of", L"csv=p=0", fs::absolute(options.input).wstring()});

      if (!probe.finish().empty()) {
        audio_path = temporary.path / L"audio.mp3";
        auto audio_arguments = input_arguments(options);

        audio_arguments.insert(audio_arguments.end(), {L"-map", L"0:a:0", L"-vn", L"-af", L"apad",
          L"-t", std::to_wstring(statistics.duration_ms / 1000.0), L"-c:a", L"libmp3lame",
          L"-b:a", L"192k", audio_path.wstring()});

        Process encoder(audio_arguments);

        encoder.finish();
      }
    }
    if (!options.colorset.empty())
      colors.close();

    if (binary)
      binary_notes.close();
    if (cycle_mode) {
      std::cerr << "Ordering colors...\n";
      const auto compensated_path = temporary.path / L"compensated.bin";

      compensate_note_order(notes_path, compensated_path, frame_counts, options.fps);
      notes_path = compensated_path;
    }
    if (sspm) {
      std::cerr << "Saving SSPM v2...\n";
      write_sspm_v2(archive_path, notes_path, audio_path, title, song_name,
        options.difficulty_name.empty() ? "Video" : options.difficulty_name,
        last_note, static_cast<uint32_t>(statistics.notes));
    } else {
      auto& json = archive->map();

      json.imbue(std::locale::classic());
      if (cycle_mode) {
        std::ifstream notes(notes_path, std::ios::binary);
        std::array<char, 14> note{};

        notes.exceptions(std::ios::failbit | std::ios::badbit);
        json << std::setprecision(9);
        for (uint64_t i = 0; i < statistics.notes; ++i) {
          uint32_t time;
          float x, y;

          notes.read(note.data(), note.size());
          std::memcpy(&time, note.data(), 4);
          std::memcpy(&x, note.data() + 6, 4);
          std::memcpy(&y, note.data() + 10, 4);
          if (i != 0)
            json << ',';
          json << "{\"Time\":" << time << ",\"X\":" << x << ",\"Y\":" << y << '}';
        }
      }
      json << "],\"Duration\":" << statistics.duration_ms << ",\"AudioFileName\":"
        << json_string(audio_path.empty() ? "" : "audio.mp3") << ",\"ImagePath\":null}";
      archive->finish_map();
      std::cerr << "Saving RHM...\n";
      if (!audio_path.empty())
        archive->add_audio(audio_path);
      archive->finish();
    }
    if (!MoveFileW(archive_path.c_str(), fs::absolute(options.output).c_str()))
      throw std::runtime_error("Cannot publish output file; destination may already exist");
    if (!options.colorset.empty() && !MoveFileW((temporary.path / L"colors.txt").c_str(), fs::absolute(options.colorset).c_str())) {
      std::error_code error;

      fs::remove(options.output, error);
      throw std::runtime_error("Cannot publish colorset; map export rolled back");
    }
    return statistics;
  }
}
