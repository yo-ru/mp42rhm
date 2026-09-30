#include "converter.h"
#include "brush_optimizer.h"
#include "archive.h"
#include "process.h"
#include "note_order.h"
#include "note_order_fixtures.h"
#include "temporary_directory.h"
#include "miniz.h"
#include <rhmParse/rhmParse.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

static void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

template<class Function>
static void must_fail(Function action) {
  bool failed = false;

  try {
    action();
  } catch (const std::exception&) {
    failed = true;
  }
  require(failed, "Expected an error");
}

static void test_temporary_cleanup(const fs::path& directory) {
  for (bool unwind : {false, true}) {
    fs::path path;
    std::future<void> unlock;

    try {
      mp42rhm::TemporaryDirectory temporary(directory);

      path = temporary.path;
      const auto staged = path / L"notes.bin";
      const HANDLE file = CreateFileW(staged.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

      require(file != INVALID_HANDLE_VALUE, "Cannot create locked staging file");
      unlock = std::async(std::launch::async, [file] {
        Sleep(200);
        CloseHandle(file);
      });
      if (unwind)
        throw std::runtime_error("Simulated export failure");
    } catch (const std::runtime_error&) {
      if (!unwind || !unlock.valid())
        throw;
    }
    unlock.get();
    require(!fs::exists(path), "Temporary cleanup failed after a transient file lock");
  }
}

static rhm::Rhm read_map(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  rhm::Rhm result;

  require(rhm::Parse(bytes, result), "Export does not parse with rhmParse");
  return result;
}

static rhm::Rhm read_sspm_v2(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  rhm::Rhm result;
  uint64_t definition_offset = 0, marker_offset = 0, marker_length = 0;
  std::array<uint8_t, 20> digest{};

  require(bytes.size() >= 128 && bytes[4] == 2 && bytes[5] == 0, "SSPM v2 header");
  require(rhm::FromSspm(bytes, result), "SSPM v2 does not parse with rhmParse");
  std::memcpy(&definition_offset, bytes.data() + 96, 8);
  std::memcpy(&marker_offset, bytes.data() + 112, 8);
  std::memcpy(&marker_length, bytes.data() + 120, 8);
  require(marker_offset + marker_length == bytes.size(), "SSPM v2 marker bounds/EOF");
  require(BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, bytes.data() + definition_offset,
    static_cast<ULONG>(bytes.size() - definition_offset), digest.data(), static_cast<ULONG>(digest.size())) >= 0,
    "Cannot hash SSPM fixture");
  require(std::memcmp(digest.data(), bytes.data() + 10, digest.size()) == 0, "SSPM v2 SHA1 mismatch");
  return result;
}

static void check_sspm(const fs::path& path, const rhm::Rhm& expected) {
  const auto actual = read_sspm_v2(path);

  require(actual.map.title == expected.map.title && actual.map.mappers == expected.map.mappers &&
    actual.map.customDifficultyName == expected.map.customDifficultyName, "SSPM metadata");
  require(actual.audio == expected.audio, "SSPM audio contents");
  require(actual.map.notes.size() == expected.map.notes.size(), "SSPM note count");
  for (size_t i = 0; i < actual.map.notes.size(); ++i)
    require(actual.map.notes[i].time == expected.map.notes[i].time &&
      actual.map.notes[i].x == expected.map.notes[i].x && actual.map.notes[i].y == expected.map.notes[i].y,
      "SSPM note contents/order");
}

static void test_large_archive(const fs::path& directory) {
  const auto path = directory / L"large.rhm";
  const std::vector<char> padding(1024 * 1024, ' ');
  mp42rhm::Archive archive(path, true);

  archive.map() << "{\"Notes\":[]";
  for (size_t i = 0; i < 4096; ++i)
    archive.map().write(padding.data(), static_cast<std::streamsize>(padding.size()));
  archive.map() << '}';
  archive.finish_map();
  archive.finish();

  FILE* input = nullptr;
  mz_zip_archive zip{};
  mz_zip_archive_file_stat metadata{};

  require(_wfopen_s(&input, path.c_str(), L"rb") == 0, "Cannot open ZIP64 test archive");
  require(mz_zip_reader_init_cfile(&zip, input, 0, 0) != 0, "Cannot read ZIP64 central directory");
  require(mz_zip_reader_file_stat(&zip, 0, &metadata) != 0, "Cannot read ZIP64 file metadata");
  require(metadata.m_uncomp_size == uint64_t(4096) * padding.size() + 12, "ZIP64 size truncated");
  require(mz_zip_validate_archive(&zip, 0) != 0, "ZIP64 decompression/CRC failed");
  mz_zip_reader_end(&zip);
  fclose(input);
}

static void test_brush_reordering() {
  std::mt19937 random(20260927);
  size_t saved = 0;

  for (uint32_t brush = 1; brush <= 64; ++brush)
    for (int trial = 0; trial < 12; ++trial) {
      mp42rhm::Options options;

      options.width = trial % 2 ? 83 : 17;
      options.height = trial % 2 ? 73 : 13;
      options.brush_size = brush;
      options.background = trial % 3 ? 0 : 0x345678;
      const int margin = brush * 2, width = options.width + margin * 2;
      const int height = options.height + margin * 2;
      std::vector<uint32_t> expected(size_t(width) * height, options.background);
      std::vector<uint32_t> actual(expected);
      std::vector<mp42rhm::BrushStroke> strokes;
      const auto paint = [&](auto& canvas, const auto& stroke) {
        for (int y = stroke.y; y < stroke.y + static_cast<int>(brush); ++y)
          std::fill_n(canvas.begin() + size_t(y + margin) * width + stroke.x + margin, brush, stroke.color);
      };
      const auto add = [&](int x, int y, uint32_t color) {
        strokes.push_back({x, y, color});
        paint(expected, strokes.back());
      };

      for (int i = 0; i < trial * 12; ++i)
        add(static_cast<int>(random() % (options.width + brush - 1)) - static_cast<int>(brush) + 1,
          static_cast<int>(random() % (options.height + brush - 1)) - static_cast<int>(brush) + 1,
          trial % 2 ? random() & 0xffffff : random() % 2 * 0xffffff);
      for (int y = -static_cast<int>(brush); y < static_cast<int>(options.height + brush); y += brush) {
        add(-static_cast<int>(brush), y, options.background);
        add(options.width, y, options.background);
      }
      for (int x = 0; x < static_cast<int>(options.width); x += brush) {
        add(x, -static_cast<int>(brush), options.background);
        add(x, options.height, options.background);
      }
      std::vector<uint8_t> pixels(size_t(options.width) * options.height * 3);

      for (uint32_t y = 0; y < options.height; ++y)
        for (uint32_t x = 0; x < options.width; ++x) {
          const auto color = expected[size_t(y + margin) * width + x + margin];
          const size_t p = (size_t(y) * options.width + x) * 3;

          pixels[p] = static_cast<uint8_t>(color >> 16);
          pixels[p + 1] = static_cast<uint8_t>(color >> 8);
          pixels[p + 2] = static_cast<uint8_t>(color);
        }
      const auto single = mp42rhm::reorder_brush_strokes(strokes, pixels, options);
      const auto result = mp42rhm::reorder_brush_strokes(strokes, pixels, options, single);
      const auto repeat = mp42rhm::reorder_brush_strokes(strokes, pixels, options, single);

      for (const auto& stroke : single)
        paint(actual, stroke);
      require(actual == expected, "Single-candidate reordering changed pixels");
      std::fill(actual.begin(), actual.end(), options.background);

      require(!result.empty() && result.size() <= strokes.size(), "Reordering must retain timing without adding notes");
      require(result.size() == repeat.size(), "Reordering must be deterministic");
      for (size_t i = 0; i < result.size(); ++i) {
        require(result[i].x == repeat[i].x && result[i].y == repeat[i].y && result[i].color == repeat[i].color,
          "Reordered stroke mismatch");
        paint(actual, result[i]);
      }
      require(actual == expected, "Reordering changed pixels or left brush spill");
      saved += strokes.size() - result.size();
    }
  require(saved > 0, "Reordering did not remove any strokes");
}

static std::vector<uint32_t> render_brush_map(const mp42rhm::Options& options, size_t frames) {
  const auto map = options.format == mp42rhm::Format::Sspm ? read_sspm_v2(options.output) : read_map(options.output);
  std::ifstream file(options.colorset);
  std::vector<uint32_t> colors;
  std::vector<uint64_t> order(map.map.notes.size());
  std::string line;
  const int margin = options.brush_size * 2, width = options.width + margin * 2;
  const int height = options.height + margin * 2;
  const double pitch = mp42rhm::note_scale(options) / options.brush_size;
  std::vector<uint32_t> canvas(size_t(width) * height * frames, options.background);

  while (std::getline(file, line)) {
    require(line.size() == 6, "Colorset entries must be bare RGB hex");
    colors.push_back(mp42rhm::parse_color(line));
  }
  require(!colors.empty(), "Empty brush colorset");
  for (size_t i = 0; i < order.size(); ++i)
    order[i] = uint64_t(map.map.notes[i].time) << 32 | i;
  mp42rhm::sort_note_indices(order);
  for (size_t rank = order.size(); rank-- > 0;) {
    const auto& note = map.map.notes[static_cast<uint32_t>(order[rank])];
    size_t frame = 0;

    while (frame < frames && mp42rhm::frame_time(frame + 1, options.fps) != note.time)
      ++frame;
    require(frame < frames, "Map contains a non-frame timestamp");
    if (note.x == 100 && note.y == 100)
      continue;
    const int x = static_cast<int>(std::lround((note.x - 1) / pitch + options.width / 2.0 - options.brush_size / 2.0)) + margin;
    const int y = static_cast<int>(std::lround((note.y - 1) / pitch + options.height / 2.0 - options.brush_size / 2.0)) + margin;

    require(x >= 0 && y >= 0 && x + int(options.brush_size) <= width && y + int(options.brush_size) <= height,
      "Brush escaped verification canvas");
    for (int row = y; row < y + int(options.brush_size); ++row)
      std::fill_n(canvas.begin() + (frame * height + row) * width + x, options.brush_size, colors[rank % colors.size()]);
  }
  return canvas;
}

static void test_cuda_brush_sizes(const fs::path& directory) {
  const auto raw = directory / L"cuda-wide.rgb", video = directory / L"cuda-wide.mkv";
  const auto palette = directory / L"cuda-wide-palette.txt";
  const uint32_t colors[] = {0x112233, 0xff0000, 0x00ff00, 0x0000ff, 0xffffff};
  std::vector<uint8_t> pixels;

  std::ofstream(palette) << "#ff0000\n#00ff00\n#0000ff\n#ffffff\n";
  for (int frame = 0; frame < 36; ++frame)
    for (int y = 0; y < 97; ++y)
      for (int x = 0; x < 133; ++x) {
        const bool detail = (x * 7 + y * 11) % 137 < 3;
        const auto color = colors[frame % 3 == 2 ? 0 : detail ? (x + y + frame) % 5 : (x / 39 + y / 29 + frame) % 5];

        pixels.push_back(static_cast<uint8_t>(color >> 16));
        pixels.push_back(static_cast<uint8_t>(color >> 8));
        pixels.push_back(static_cast<uint8_t>(color));
      }
  std::ofstream(raw, std::ios::binary).write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
  mp42rhm::Process fixture({L"ffmpeg.exe", L"-v", L"error", L"-f", L"rawvideo", L"-pixel_format", L"rgb24",
    L"-video_size", L"133x97", L"-framerate", L"24", L"-i", raw.wstring(),
    L"-c:v", L"ffv1", L"-pix_fmt", L"bgr0", video.wstring()});

  fixture.finish();
  for (const uint32_t brush : {2, 3, 8, 16, 31, 32, 33, 63, 64}) {
    mp42rhm::Options options;
    const auto name = "cuda-wide-" + std::to_string(brush);
    const int frames = brush == 32 ? 36 : 3;

    options.input = video;
    options.output = directory / (name + "-cpu.sspm");
    options.colorset = directory / (name + "-cpu.txt");
    options.palette_cycle = palette;
    options.mode = mp42rhm::ColorMode::Color;
    options.background = colors[0];
    options.width = 133;
    options.height = 97;
    options.fps = 24;
    options.seconds = frames / 24.0;
    options.brush_size = brush;
    options.audio = false;
    const auto cpu_stats = mp42rhm::convert(options);
    const auto cpu_map = read_sspm_v2(options.output);
    std::ifstream cpu_file(options.colorset, std::ios::binary);
    const std::vector<char> cpu_colors((std::istreambuf_iterator<char>(cpu_file)), {});

    options.output = directory / (name + "-gpu.sspm");
    options.colorset = directory / (name + "-gpu.txt");
    options.experimental_cuda = true;
    const auto gpu_stats = mp42rhm::convert(options);
    std::ifstream gpu_file(options.colorset, std::ios::binary);
    const std::vector<char> gpu_colors((std::istreambuf_iterator<char>(gpu_file)), {});

    require(cpu_stats.notes == gpu_stats.notes && cpu_stats.peak_frame_notes == gpu_stats.peak_frame_notes,
      "Wide CUDA brush note counts differ from CPU");
    require(cpu_stats.frames == frames && gpu_stats.frames == frames, "Wide CUDA brush frame counts");
    check_sspm(options.output, cpu_map);
    require(cpu_colors == gpu_colors, "Wide CUDA brush colors differ from CPU");
    const auto actual = render_brush_map(options, frames);
    const int margin = brush * 2, width = 133 + margin * 2, height = 97 + margin * 2;

    for (int frame = 0; frame < frames; ++frame)
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
          uint32_t expected = options.background;

          if (x >= margin && x < margin + 133 && y >= margin && y < margin + 97) {
            const size_t p = ((frame * 97 + y - margin) * 133 + x - margin) * 3;

            expected = uint32_t(pixels[p]) << 16 | uint32_t(pixels[p + 1]) << 8 | pixels[p + 2];
          }
          require(actual[(frame * height + y) * width + x] == expected,
            "Wide CUDA brush changed pixels or left edge spill");
        }
    options.brush_size = 1;
    must_fail([&] { mp42rhm::validate(options); });
  }
}

static void test_new_modes(const fs::path& directory, const fs::path& executable, bool cuda) {
  const auto raw_path = directory / L"adaptive.rgb", video = directory / L"adaptive.mkv";
  std::vector<uint8_t> pixels;

  for (int frame = 0; frame < 3; ++frame)
    for (int y = 0; y < 16; ++y)
      for (int x = 0; x < 32; ++x) {
        pixels.push_back(frame == 2 ? 0 : static_cast<uint8_t>(x * 8));
        pixels.push_back(frame == 2 ? 0 : static_cast<uint8_t>(y * 16));
        pixels.push_back(frame == 2 ? 0 : static_cast<uint8_t>((x + y + frame * 11) * 4));
      }
  std::ofstream(raw_path, std::ios::binary).write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
  mp42rhm::Process fixture({L"ffmpeg.exe", L"-v", L"error", L"-f", L"rawvideo", L"-pixel_format", L"rgb24",
    L"-video_size", L"32x16", L"-framerate", L"24000/1001", L"-i", raw_path.wstring(),
    L"-c:v", L"ffv1", L"-pix_fmt", L"bgr0", video.wstring()});

  fixture.finish();
  mp42rhm::Options options;

  options.input = video;
  options.output = directory / L"adaptive-cli.sspm";
  options.colorset = directory / L"adaptive-cli-colorset.txt";
  options.mode = mp42rhm::ColorMode::Color;
  options.adaptive_palette = true;
  options.color_count = 1024;
  options.width = 32;
  options.height = 16;
  options.fps = {24000, 1001};
  options.brush_size = cuda ? 32 : 8;
  options.audio = false;
  std::vector<std::wstring> command{executable.wstring(), video.wstring(), options.output.wstring(),
    L"--adaptive-palette", L"--colors", L"1024", L"--width", L"32", L"--height", L"16",
    L"--brush-size", std::to_wstring(options.brush_size), L"--fps", L"native", L"--no-audio"};

  if (cuda)
    command.push_back(L"--experimental-cuda");
  mp42rhm::Process adaptive(command);

  require(adaptive.finish().find("Exported 3 frames") != std::string::npos, "Native FPS changed frame count");
  const auto actual = render_brush_map(options, 3);
  const size_t margin = options.brush_size * 2, width = 32 + margin * 2, height = 16 + margin * 2;

  for (size_t f = 0; f < 3; ++f)
    for (size_t y = 0; y < height; ++y)
      for (size_t x = 0; x < width; ++x) {
        uint32_t expected = 0;

        if (x >= margin && x < 32 + margin && y >= margin && y < 16 + margin) {
          const size_t p = (f * 512 + (y - margin) * 32 + x - margin) * 3;

          expected = uint32_t(pixels[p]) << 16 | uint32_t(pixels[p + 1]) << 8 | pixels[p + 2];
        }
        require(actual[(f * height + y) * width + x] == expected, "Adaptive palette changed an already-small palette");
      }
  for (uint32_t count : {0, 2, 32, 256, 257}) {
    options.color_count = count;
    options.output = directory / (L"adaptive-quantized-" + std::to_wstring(count) + L".sspm");
    options.colorset = directory / (L"adaptive-quantized-" + std::to_wstring(count) + L".txt");
    options.experimental_cuda = cuda;
    mp42rhm::convert(options);
    const auto quantized = render_brush_map(options, 3);
    mp42rhm::Process reference_decoder({L"ffmpeg.exe", L"-v", L"error", L"-i", video.wstring(),
      L"-vf", L"setpts=PTS-STARTPTS,fps=24000/1001:start_time=0,format=rgb24,elbg=codebook_length=" +
        std::to_wstring((count ? count : 64) - 1) + L":nb_steps=1:seed=1",
      L"-pix_fmt", L"rgb24", L"-f", L"rawvideo", L"pipe:1"});
    const auto reference_pixels = reference_decoder.finish();

    require(reference_pixels.size() == pixels.size(), "Adaptive reference frame count");
    for (size_t f = 0; f < 3; ++f)
      for (size_t y = 0; y < height; ++y)
        for (size_t x = 0; x < width; ++x) {
          uint32_t expected = 0;

          if (x >= margin && x < 32 + margin && y >= margin && y < 16 + margin) {
            const size_t p = (f * 512 + (y - margin) * 32 + x - margin) * 3;

            expected = uint32_t(static_cast<uint8_t>(reference_pixels[p])) << 16 |
              uint32_t(static_cast<uint8_t>(reference_pixels[p + 1])) << 8 | static_cast<uint8_t>(reference_pixels[p + 2]);
          }
          require(quantized[(f * height + y) * width + x] == expected, "Adaptive export changed quantized pixels");
        }
  }
  for (auto mode : {mp42rhm::ColorMode::Bw, mp42rhm::ColorMode::Grayscale}) {
    options.mode = mode;
    options.adaptive_palette = false;
    options.compact_colorset = true;
    options.color_count = mode == mp42rhm::ColorMode::Bw ? 0 : 8;
    options.experimental_cuda = cuda;
    const auto name = mode == mp42rhm::ColorMode::Bw ? L"compact-bw" : L"compact-gray";

    options.output = directory / (std::wstring(name) + L".sspm");
    options.colorset = directory / (std::wstring(name) + L".txt");
    const auto stats = mp42rhm::convert(options);
    const auto compact = render_brush_map(options, 3);
    if (mode == mp42rhm::ColorMode::Grayscale) {
      auto control = options;

      control.compact_colorset = false;
      control.output = directory / L"gray-control.sspm";
      control.colorset = directory / L"gray-control.txt";
      mp42rhm::convert(control);
      require(compact == render_brush_map(control, 3), "Compact colorset changed grayscale pixels");
      control.compact_colorset = true;
      control.format = mp42rhm::Format::Rhm;
      control.output = directory / L"compact-gray.rhm";
      control.colorset = directory / L"compact-gray-rhm.txt";
      mp42rhm::convert(control);
      require(compact == render_brush_map(control, 3), "Compact RHM and SSPM differ");
    } else {
      mp42rhm::Process grayscale({L"ffmpeg.exe", L"-v", L"error", L"-i", video.wstring(),
        L"-vf", L"fps=24000/1001,format=gray", L"-pix_fmt", L"gray", L"-f", L"rawvideo", L"pipe:1"});
      const auto reference = grayscale.finish();

      require(reference.size() == 3 * 512, "BW reference length");
      for (size_t f = 0; f < 3; ++f)
        for (size_t y = 0; y < height; ++y)
          for (size_t x = 0; x < width; ++x) {
            uint32_t expected = 0;

            if (x >= margin && x < 32 + margin && y >= margin && y < 16 + margin &&
              static_cast<uint8_t>(reference[f * 512 + (y - margin) * 32 + x - margin]) >= options.threshold)
              expected = 0xffffff;
            require(compact[(f * height + y) * width + x] == expected, "Compact colorset changed BW pixels");
          }
    }
    require(stats.frames == 3 && fs::file_size(options.colorset) < 512, "Compact colorset or frame count");
    require(stats.filler_notes > 0 && stats.notes % (fs::file_size(options.colorset) / 7) == 0,
      "Compact colorset phase or filler accounting");
    const auto exact_budget = stats.notes;

    options.output = directory / (std::wstring(name) + L"-budget.sspm");
    options.colorset = directory / (std::wstring(name) + L"-budget.txt");
    options.max_notes = exact_budget;
    require(mp42rhm::convert(options).notes == exact_budget, "Exact compact note budget failed");
    options.output = directory / (std::wstring(name) + L"-overflow.sspm");
    options.colorset = directory / (std::wstring(name) + L"-overflow.txt");
    options.max_notes = exact_budget - 1;
    must_fail([&] { mp42rhm::convert(options); });
    require(!fs::exists(options.output) && !fs::exists(options.colorset), "Published partial compact map");
    options.max_notes = 100000000;
  }

  const auto subtitle_directory = directory / L"subtitle ' [test], &";

  fs::create_directory(subtitle_directory);
  const auto english = subtitle_directory / L"english.srt", other = subtitle_directory / L"other.srt";
  const auto subtitled_video = subtitle_directory / L"episode.mkv";

  std::ofstream(english) << "1\n00:00:01,100 --> 00:00:01,600\nEnglish track test\n";
  std::ofstream(other) << "1\n00:00:00,000 --> 00:00:02,000\nOther track\n";
  mp42rhm::Process subtitle_fixture({L"ffmpeg.exe", L"-v", L"error", L"-f", L"lavfi", L"-i",
    L"color=black:s=320x180:r=24:d=2", L"-i", english.wstring(), L"-i", other.wstring(),
    L"-map", L"0:v", L"-map", L"1:s", L"-map", L"2:s", L"-c:v", L"ffv1", L"-c:s", L"ass", subtitled_video.wstring()});

  subtitle_fixture.finish();
  options = {};
  options.input = subtitled_video;
  options.output = subtitle_directory / L"subtitles.sspm";
  options.colorset = subtitle_directory / L"subtitles-colorset.txt";
  options.width = 320;
  options.height = 180;
  options.fps = 24;
  options.brush_size = 8;
  mp42rhm::Process subtitles({executable.wstring(), subtitled_video.wstring(), options.output.wstring(),
    L"--mode", L"grayscale", L"--colors", L"8", L"--width", L"320", L"--height", L"180", L"--fps", L"native",
    L"--subtitles", L"1", L"--start", L"1", L"--seconds", L"1", L"--no-audio"});

  require(subtitles.finish().find("Exported 24 frames") != std::string::npos, "Subtitle export frame count");
  const auto subtitles_canvas = render_brush_map(options, 24);
  const size_t subtitle_width = 352, subtitle_height = 212;
  std::array<size_t, 24> lit{};

  for (size_t f = 0; f < 24; ++f)
    for (size_t y = 0; y < subtitle_height; ++y)
      for (size_t x = 0; x < subtitle_width; ++x) {
        const auto color = subtitles_canvas[(f * subtitle_height + y) * subtitle_width + x];

        if (color) {
          require(y >= 16 + 144 && y < 16 + 180 && x >= 16 && x < 16 + 320, "Subtitle escaped its band");
          ++lit[f];
        }
      }
  require(lit[0] == 0 && lit[6] > 30 && lit[12] > 30 && lit[20] == 0, "Subtitle track or seek timing is wrong");
  options.output = directory / L"invalid-new-mode.sspm";
  options.colorset = directory / L"invalid-new-mode.txt";
  options.subtitle_track = 0;
  options.width = 3840;
  options.height = 2160;
  options.mode = mp42rhm::ColorMode::Color;
  options.adaptive_palette = true;
  options.color_count = 65536;
  mp42rhm::validate(options);
  options.width = 3841;
  must_fail([&] { mp42rhm::validate(options); });
  options.width = 3840;
  options.adaptive_palette = false;
  must_fail([&] { mp42rhm::validate(options); });
  options.adaptive_palette = true;
  options.compact_colorset = true;
  must_fail([&] { mp42rhm::validate(options); });
}

int main(int argc, char** argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--normalize") {
      mp42rhm::normalize_zip64(argv[2]);
      std::cout << "ZIP64 headers normalized\n";
      return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--verify") {
      mz_zip_error error = MZ_ZIP_NO_ERROR;

      if (!mz_zip_validate_file_archive(argv[2], 0, &error))
        throw std::runtime_error(mz_zip_get_error_string(error));
      std::cout << "All archive entries passed decompression and CRC checks\n";
      return 0;
    }
    test_brush_reordering();
    require(mp42rhm::frame_time(1, 60) == 17, "First frame end");
    require(mp42rhm::frame_time(2, 60) == 33, "Second frame end");
    require(mp42rhm::frame_time(3, 60) == 50, "Third frame end");
    require(mp42rhm::frame_time(60 * 3600, 60) == 3600000, "Long-term timing drift");
    must_fail([] { mp42rhm::frame_time(UINT64_MAX, 60); });
    require(mp42rhm::frame_time(24000, {24000, 1001}) == 1001000, "Fractional FPS drift");
    require(mp42rhm::frame_time(24000ULL * 600, {24000, 1001}) == 600600000,
      "Fractional FPS long-run timing");
    const auto fractional = mp42rhm::parse_frame_rate("23.976");

    require(fractional.numerator == 2997 && fractional.denominator == 125, "Decimal FPS parsing");
    for (const auto text : {"0", "24000/0", "0/0", "61", "1/2", "12junk", "23.1234567", "nan"})
      must_fail([&] { mp42rhm::parse_frame_rate(text); });
    for (const auto& fixture : SORT_FIXTURES) {
      std::vector<uint64_t> order(fixture.count);

      for (uint32_t i = 0; i < fixture.count; ++i) {
        const uint32_t time = fixture.pattern == 0 ? 42 : fixture.pattern == 1 ? i / 7 :
          fixture.pattern == 2 ? i * 37 / 101 : i * 7919 % 101;

        order[i] = (uint64_t(time) << 32) | i;
      }
      mp42rhm::sort_note_indices(order);
      uint64_t hash = 14695981039346656037ULL;

      for (uint64_t key : order)
        for (unsigned shift = 0; shift < 64; shift += 8)
          hash = (hash ^ static_cast<uint8_t>(key >> shift)) * 1099511628211ULL;
      require(hash == fixture.hash, "C++ sort differs from the independent .NET permutation");
    }

    const auto directory = fs::current_path() / (L"test-fixtures-" + std::to_wstring(GetTickCount64()));

    fs::create_directory(directory);
    test_temporary_cleanup(directory);
    if (argc == 2 && std::string(argv[1]) == "--large") {
      test_large_archive(directory);
      fs::remove_all(directory);
      std::cout << "ZIP64 payload above 4 GiB passed size, decompression, and CRC checks\n";
      return 0;
    }
    test_new_modes(directory, fs::absolute(argv[0]).parent_path() / L"mp42rhm.exe",
      argc == 2 && std::string(argv[1]) == "--cuda");
    if (argc == 2 && std::string(argv[1]) == "--cuda")
      test_cuda_brush_sizes(directory);
    const auto raw_path = directory / L"pixels.rgb";
    const auto video_path = directory / L"test & sample.mp4";
    const uint8_t pixels[] = {
      255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255,
      0, 0, 0, 255, 255, 255, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };

    std::ofstream(raw_path, std::ios::binary).write(reinterpret_cast<const char*>(pixels), sizeof(pixels));
    mp42rhm::Process fixture({L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"error", L"-nostdin",
      L"-f", L"rawvideo", L"-pixel_format", L"rgb24", L"-video_size", L"2x2", L"-framerate", L"60",
      L"-i", raw_path.wstring(), L"-f", L"lavfi", L"-i", L"sine=frequency=440:duration=0.05",
      L"-c:v", L"libx264rgb", L"-crf", L"0", L"-preset", L"ultrafast", L"-c:a", L"aac",
      L"-shortest", video_path.wstring()});

    fixture.finish();

    mp42rhm::Options options;

    require(options.fps.numerator == 12 && options.fps.denominator == 1 && options.max_notes == 100000000,
      "Full-video defaults changed");
    const auto executable = fs::absolute(argv[0]).parent_path() / L"mp42rhm.exe";
    mp42rhm::Process version({executable.wstring(), L"--version"});

    require(version.finish().find("mp42rhm ") == 0, "Missing CLI version");
    const auto cli_video = directory / L"cli.mp4";
    mp42rhm::Process cli_fixture({L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"error", L"-nostdin",
      L"-f", L"lavfi", L"-i", L"color=c=white:s=16x16:r=24:d=0.25",
      L"-c:v", L"libx264rgb", L"-crf", L"0", cli_video.wstring()});

    cli_fixture.finish();
    for (const auto mode : {L"bw", L"grayscale", L"color"}) {
      const auto base = directory / (std::wstring(L"cli-") + mode);
      std::vector<std::wstring> arguments{executable.wstring(), cli_video.wstring(), base.wstring()};

      if (std::wstring(mode) != L"bw")
        arguments.insert(arguments.end(), {L"--mode", mode});
      mp42rhm::Process command(arguments);
      auto output = command.finish();

      output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
      const auto generated = read_sspm_v2(base.wstring() + L".sspm");
      const auto scale = std::wstring(mode) == L"bw" ? "Note Scale: 0.01" : "Note Scale: 0.08";

      require(output.find("Exported 3 frames") != std::string::npos && output.find(scale) != std::string::npos,
        "CLI frame rate or automatic brush default changed");
      require(!generated.map.notes.empty() && fs::exists(base.wstring() + L"-colorset.txt"), "Missing default CLI outputs");
      std::ifstream settings(base.wstring() + L"-settings.txt", std::ios::binary);
      const std::string saved((std::istreambuf_iterator<char>(settings)), {});

      require(saved == output && saved.find("Map: ") != std::string::npos &&
        saved.find("B)") != std::string::npos && saved.find("Time: ") != std::string::npos &&
        saved.find("FOV: 30") != std::string::npos, "Playback settings or export summary missing");
      for (const auto& note : generated.map.notes)
        require(note.time == 83 || note.time == 167 || note.time == 250, "Default CLI timing");
    }
    for (const auto format : {L"sspm", L"rhm"})
      for (const uint32_t brush : {1U, 8U})
        for (const auto span : {L"0.33", L"0.001"}) {
          const auto base = directory / (std::wstring(L"scale-") + format + L"-" +
            std::to_wstring(brush) + L"-" + span);
          const auto output_path = base.wstring() + L"." + format;
          mp42rhm::Process command({executable.wstring(), cli_video.wstring(), output_path,
            L"--format", format, L"--mode", L"grayscale", L"--width", L"16", L"--height", L"16",
            L"--brush-size", std::to_wstring(brush), L"--span", span});
          auto output = command.finish();

          output.erase(std::remove(output.begin(), output.end(), '\r'), output.end());
          const double scale = std::wstring(span) == L"0.001" ? .01 : brush == 1 ? .02 : .17;
          const auto map = std::wstring(format) == L"sspm" ? read_sspm_v2(output_path) : read_map(output_path);

          require(output.find("Note Scale: " + std::to_string(scale).substr(0, 4) + "\n") != std::string::npos,
            "CLI note scale is not playable");
          require(!map.map.notes.empty(), "Empty scale regression map");
          for (const auto& note : map.map.notes) {
            if (note.x == 100 && note.y == 100)
              continue;
            const double x = (note.x - 1) * brush / scale + 8 - brush / 2.0;
            const double y = (note.y - 1) * brush / scale + 8 - brush / 2.0;

            require(std::abs(x - std::round(x)) < .001 && std::abs(y - std::round(y)) < .001,
              "Coordinates disagree with the playable note scale");
          }
        }
    const auto blocked = directory / L"cli-settings-exist";
    const auto blocked_settings = blocked.wstring() + L"-settings.txt";

    std::ofstream(blocked_settings) << "keep";
    mp42rhm::Process blocked_command({executable.wstring(), cli_video.wstring(), blocked.wstring()});

    must_fail([&] { blocked_command.finish(); });
    std::ifstream original(blocked_settings);
    std::string contents;

    original >> contents;
    require(contents == "keep" && !fs::exists(blocked.wstring() + L".sspm"), "Existing settings overwritten");
    original.close();
    const auto collision = directory / L"cli-settings-collision";
    mp42rhm::Process collision_command({executable.wstring(), cli_video.wstring(), collision.wstring(),
      L"--colorset", collision.wstring() + L"-SETTINGS.TXT"});

    must_fail([&] { collision_command.finish(); });
    require(!fs::exists(collision.wstring() + L".sspm") &&
      !fs::exists(collision.wstring() + L"-SETTINGS.TXT"), "Conflicting settings output published");
    const auto overridden = directory / L"cli-overridden.rhm";
    mp42rhm::Process override_command({executable.wstring(), cli_video.wstring(), overridden.wstring(),
      L"--mode", L"color", L"--brush-size", L"1", L"--colors", L"4", L"--fps", L"24",
      L"--width", L"32", L"--height", L"18", L"--format", L"rhm"});
    const auto override_output = override_command.finish();

    require(override_output.find("Exported 6 frames") != std::string::npos &&
      override_output.find("Note Scale: 0.01") != std::string::npos, "CLI defaults overrode explicit options");
    require(!read_map(overridden).map.notes.empty(), "Explicit RHM output failed");
    options.fps = 60;
    options.input = video_path;
    options.span = 2;
    options.format = mp42rhm::Format::Rhm;
    options.output = directory / L"mono.rhm";
    options.width = 2;
    options.height = 2;
    const auto mono_stats = mp42rhm::convert(options);
    const auto mono = read_map(options.output);

    require(mono_stats.frames == 3 && mono.map.duration == 50, "Trailing black frames lost");
    require(mono.map.notes.size() == 3, "Incorrect luminance threshold");
    require(mono.map.notes[0].x == 1.5f && mono.map.notes[0].y == 0.5f, "Coordinate orientation");
    require(mono.map.notes[0].time == 17 && mono.map.notes.back().time == 33, "Incorrect note timing");
    require(!mono.audio.empty(), "Missing embedded audio");
    must_fail([&] { mp42rhm::convert(options); });
    require(read_map(options.output).map.notes.size() == 3, "Overwrote existing output");

    auto aspect_options = options;

    aspect_options.format = mp42rhm::Format::Rhm;
    aspect_options.output = directory / L"aspect.rhm";
    aspect_options.width = 4;
    aspect_options.span = 4;
    mp42rhm::convert(aspect_options);
    const auto aspect = read_map(aspect_options.output);

    require(aspect.map.notes.size() == mono.map.notes.size(), "Aspect-preserving padding changed notes");
    for (size_t i = 0; i < mono.map.notes.size(); ++i)
      require(aspect.map.notes[i].x == mono.map.notes[i].x && aspect.map.notes[i].y == mono.map.notes[i].y,
        "Source was stretched to output aspect ratio");

    auto metadata_options = options;

    metadata_options.title = "Bad Apple!!";
    metadata_options.difficulty_name = "144p60 Video";
    metadata_options.format = mp42rhm::Format::Rhm;
    metadata_options.output = directory / L"metadata.rhm";
    mp42rhm::convert(metadata_options);
    const auto metadata_rhm = read_map(metadata_options.output);

    require(metadata_rhm.map.title == "Bad Apple!!" && metadata_rhm.map.songName == "Bad Apple!!" &&
      metadata_rhm.map.customDifficultyName == "144p60 Video", "RHM custom metadata");
    metadata_options.format = mp42rhm::Format::Sspm;
    metadata_options.output = directory / L"metadata.sspm";
    mp42rhm::convert(metadata_options);
    const auto metadata_sspm = read_sspm_v2(metadata_options.output);

    require(metadata_sspm.map.title == "Bad Apple!!" && metadata_sspm.map.songName == "Bad Apple!!" &&
      metadata_sspm.map.customDifficultyName == "144p60 Video", "SSPM custom metadata");
    require(metadata_sspm.audio == mono.audio, "SSPM v2 embedded audio");
    require(metadata_sspm.map.notes.size() == mono.map.notes.size(), "SSPM v2 note count");
    for (size_t i = 0; i < mono.map.notes.size(); ++i)
      require(metadata_sspm.map.notes[i].x == mono.map.notes[i].x && metadata_sspm.map.notes[i].y == mono.map.notes[i].y &&
        metadata_sspm.map.notes[i].time == mono.map.notes[i].time, "SSPM v2 note contents/order");
    metadata_options.format = mp42rhm::Format::Sspm;
    metadata_options.output = directory / L"metadata-silent.sspm";
    metadata_options.audio = false;
    mp42rhm::convert(metadata_options);
    require(read_sspm_v2(metadata_options.output).audio.empty(), "SSPM v2 no-audio layout");
    metadata_options.format = mp42rhm::Format::Sspm;
    metadata_options.output = directory / L"metadata-invalid.sspm";
    metadata_options.difficulty_name.assign(65536, 'x');
    must_fail([&] { mp42rhm::convert(metadata_options); });
    require(!fs::exists(metadata_options.output), "Published invalid SSPM metadata");

    auto cycle_options = options;
    const auto cycle_path = directory / L"palette-cycle.txt";
    const std::string cycle_text = "#ff0000\n#ffffff\n#ffffff\n#00ff00\n#0000ff\n";

    std::ofstream(cycle_path, std::ios::binary) << cycle_text;
    cycle_options.mode = mp42rhm::ColorMode::Color;
    cycle_options.palette_cycle = cycle_path;
    cycle_options.colorset = directory / L"cycle-colors.txt";
    cycle_options.format = mp42rhm::Format::Sspm;
    cycle_options.output = directory / L"cycle.sspm";
    cycle_options.difficulty_name = "Color Video";
    const auto cycle_stats = mp42rhm::convert(cycle_options);
    const auto cycle_map = read_sspm_v2(cycle_options.output);
    std::ifstream cycle_colors(cycle_options.colorset, std::ios::binary);
    const std::string exported_cycle((std::istreambuf_iterator<char>(cycle_colors)), {});

    cycle_colors.close();
    require(exported_cycle == "ff0000\nffffff\nffffff\n00ff00\n0000ff\n", "Palette cycle changed or expanded per note");
    require(cycle_stats.frames == 3 && cycle_stats.duration_ms == 50 && cycle_stats.notes == 10 &&
      cycle_stats.filler_notes == 5 && cycle_map.map.notes.size() == 10, "Palette padding/frame counts");

    const uint32_t cycle_rgb[] = {0xff0000, 0xffffff, 0xffffff, 0x00ff00, 0x0000ff};
    std::vector<uint64_t> cycle_order(cycle_map.map.notes.size());

    for (size_t i = 0; i < cycle_order.size(); ++i)
      cycle_order[i] = (uint64_t(cycle_map.map.notes[i].time) << 32) | i;
    mp42rhm::sort_note_indices(cycle_order);
    std::array<uint8_t, sizeof(pixels)> reconstructed{};
    std::array<bool, sizeof(pixels) / 3> seen{};

    for (size_t i = 0; i < cycle_map.map.notes.size(); ++i) {
      const auto& note = cycle_map.map.notes[static_cast<uint32_t>(cycle_order[i])];

      require(note.time == (i < 5 ? 17 : 33), "Palette colors are staggered");
      if (note.x == 100 && note.y == 100)
        continue;
      require((note.x == 0.5f || note.x == 1.5f) && (note.y == 0.5f || note.y == 1.5f),
        "Palette note moved off the pixel grid");
      const size_t pixel = (i < 5 ? 0 : 4) + static_cast<size_t>(note.y) * 2 + static_cast<size_t>(note.x);
      const uint32_t rgb = cycle_rgb[i % 5];

      require(!seen[pixel], "Palette scheduler emitted a duplicate visible pixel");
      seen[pixel] = true;
      reconstructed[pixel * 3] = static_cast<uint8_t>(rgb >> 16);
      reconstructed[pixel * 3 + 1] = static_cast<uint8_t>(rgb >> 8);
      reconstructed[pixel * 3 + 2] = static_cast<uint8_t>(rgb);
    }
    require(std::memcmp(reconstructed.data(), pixels, sizeof(pixels)) == 0,
      "Equal-time reordering broke reconstructed pixel colors");
    auto crowded_options = cycle_options;

    crowded_options.format = mp42rhm::Format::Sspm;
    crowded_options.output = directory / L"cycle-crowded.sspm";
    crowded_options.colorset = directory / L"cycle-crowded.txt";
    crowded_options.width = 8;
    crowded_options.height = 8;
    require(mp42rhm::convert(crowded_options).notes > 16, "Dense palette conversion failed");
    auto compensated_options = crowded_options;
    const auto expanded_raw = directory / L"expanded.rgb";
    std::vector<uint8_t> expanded_pixels;

    for (size_t frame = 0; frame < 3; ++frame)
      for (size_t y = 0; y < 8; ++y)
        for (size_t x = 0; x < 8; ++x) {
          const size_t reference = (frame * 4 + (y / 4) * 2 + x / 4) * 3;

          expanded_pixels.insert(expanded_pixels.end(), pixels + reference, pixels + reference + 3);
        }
    std::ofstream(expanded_raw, std::ios::binary).write(reinterpret_cast<const char*>(expanded_pixels.data()),
      static_cast<std::streamsize>(expanded_pixels.size()));
    compensated_options.input = directory / L"expanded.mp4";
    mp42rhm::Process expanded_fixture({L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"error", L"-nostdin",
      L"-f", L"rawvideo", L"-pixel_format", L"rgb24", L"-video_size", L"8x8", L"-framerate", L"60",
      L"-i", expanded_raw.wstring(), L"-c:v", L"libx264rgb", L"-crf", L"0", L"-preset", L"ultrafast",
      compensated_options.input.wstring()});

    expanded_fixture.finish();

    compensated_options.audio = false;
    compensated_options.format = mp42rhm::Format::Sspm;
    compensated_options.output = directory / L"compensated.sspm";
    compensated_options.colorset = directory / L"compensated.txt";
    const auto compensated_stats = mp42rhm::convert(compensated_options);
    const auto compensated_map = read_sspm_v2(compensated_options.output);
    std::vector<uint64_t> imported_order(compensated_map.map.notes.size());
    std::array<uint32_t, 3 * 64> imported_pixels{};
    std::array<bool, 3 * 64> imported_seen{};

    require(compensated_stats.frames == 3 && compensated_stats.duration_ms == 50 &&
      compensated_stats.notes == 120 && compensated_stats.filler_notes == 40,
      "Simultaneous export changed frame counts or padding");
    require(compensated_map.audio.empty(), "Compensated no-audio export has audio");
    for (size_t i = 0; i < imported_order.size(); ++i)
      imported_order[i] = (uint64_t(compensated_map.map.notes[i].time) << 32) | i;
    mp42rhm::sort_note_indices(imported_order);
    for (size_t rank = 0; rank < imported_order.size(); ++rank) {
      const auto& note = compensated_map.map.notes[static_cast<uint32_t>(imported_order[rank])];

      require(note.time == 17 || note.time == 33, "Compensated colors are staggered");
      if (note.x == 100 && note.y == 100)
        continue;
      const size_t x = static_cast<size_t>(note.x * 4);
      const size_t y = static_cast<size_t>(note.y * 4);
      const size_t pixel = (note.time == 17 ? 0 : 64) + y * 8 + x;

      require(x < 8 && y < 8 && !imported_seen[pixel], "Compensation lost or duplicated a pixel");
      imported_seen[pixel] = true;
      imported_pixels[pixel] = cycle_rgb[rank % 5];
    }
    for (size_t frame = 0; frame < 3; ++frame)
      for (size_t y = 0; y < 8; ++y)
        for (size_t x = 0; x < 8; ++x) {
          const size_t reference = (frame * 4 + (y / 4) * 2 + x / 4) * 3;
          const uint32_t rgb = uint32_t(pixels[reference]) << 16 |
            uint32_t(pixels[reference + 1]) << 8 | pixels[reference + 2];

          require(imported_pixels[frame * 64 + y * 8 + x] == rgb,
            "Rhythia sort broke the compensated changing-image fixture");
        }
    for (const auto format : {mp42rhm::Format::Sspm, mp42rhm::Format::Rhm}) {
      for (const auto mode : {mp42rhm::ColorMode::Bw, mp42rhm::ColorMode::Grayscale, mp42rhm::ColorMode::Color}) {
        auto automatic = compensated_options;
        const auto name = "auto-" + std::to_string(static_cast<int>(format)) + "-" +
          std::to_string(static_cast<int>(mode));

        automatic.format = format;
        automatic.mode = mode;
        automatic.palette_cycle.clear();
        automatic.output = directory / (name + (format == mp42rhm::Format::Sspm ? ".sspm" : ".rhm"));
        automatic.colorset = directory / (name + ".txt");
        const auto stats = mp42rhm::convert(automatic);
        const auto generated = format == mp42rhm::Format::Sspm ? read_sspm_v2(automatic.output) : read_map(automatic.output);
        std::ifstream colors(automatic.colorset);
        std::vector<uint32_t> rgb;
        std::string line;

        while (std::getline(colors, line)) {
          require(line.size() == 6, "Colorset entries must be bare RGB hex");
          rgb.push_back(mp42rhm::parse_color(line));
        }
        require(!rgb.empty() && rgb.size() <= 31, "Generated colorset is not compact");
        if (mode == mp42rhm::ColorMode::Bw)
          require(rgb == std::vector<uint32_t>{0xffffff}, "BW colorset is not white");
        if (mode == mp42rhm::ColorMode::Grayscale)
          require(rgb == std::vector<uint32_t>{0x555555, 0xaaaaaa, 0xffffff}, "Grayscale levels");
        std::vector<uint64_t> order(generated.map.notes.size());
        std::array<uint32_t, 3 * 64> image{};
        std::array<bool, 3 * 64> seen{};

        for (size_t i = 0; i < order.size(); ++i)
          order[i] = (uint64_t(generated.map.notes[i].time) << 32) | i;
        mp42rhm::sort_note_indices(order);
        for (size_t rank = 0; rank < order.size(); ++rank) {
          const auto& note = generated.map.notes[static_cast<uint32_t>(order[rank])];

          if (note.x == 100 && note.y == 100)
            continue;
          const size_t frame = (note.time * 60 + 500) / 1000 - 1;
          const size_t x = static_cast<size_t>(note.x * 4);
          const size_t y = static_cast<size_t>(note.y * 4);
          const size_t pixel = frame * 64 + y * 8 + x;

          require(frame < 3 && x < 8 && y < 8 && !seen[pixel], "Generated map misplaced a pixel");
          seen[pixel] = true;
          image[pixel] = rgb[rank % rgb.size()];
        }
        if (mode == mp42rhm::ColorMode::Color) {
          for (size_t i = 0; i < image.size(); ++i)
            require(image[i] == (uint32_t(expanded_pixels[i * 3]) << 16 |
              uint32_t(expanded_pixels[i * 3 + 1]) << 8 | expanded_pixels[i * 3 + 2]),
              "Automatic palette colors scrambled after sorting");
        } else if (mode == mp42rhm::ColorMode::Grayscale) {
          require(std::find(image.begin(), image.end(), 0x555555) != image.end() ||
            std::find(image.begin(), image.end(), 0xaaaaaa) != image.end(), "Grayscale lost intermediate tones");
        }
        require(stats.frames == 3, "Automatic mode lost trailing black frames");
        if (format == mp42rhm::Format::Sspm && mode == mp42rhm::ColorMode::Color) {
          automatic.color_count = 2;
          automatic.output = directory / "two-color.sspm";
          automatic.colorset = directory / "two-color.txt";
          mp42rhm::convert(automatic);
          std::ifstream two_colors(automatic.colorset);

          require(std::getline(two_colors, line) && mp42rhm::parse_color(line) != 0 &&
            !std::getline(two_colors, line), "Two-color palette must contain one foreground color");
          require(!read_sspm_v2(automatic.output).map.notes.empty(), "Two-color map is empty");
        }
      }
    }

    const auto brush_raw = directory / L"brush.rgb";
    const auto brush_video = directory / L"brush.mp4";
    const auto brush_palette = directory / L"brush-palette.txt";
    std::ofstream brush_colors(brush_palette);
    std::vector<uint8_t> brush_pixels;
    const uint32_t brush_rgb[] = {0, 0xff0000, 0xffffff, 0x00ff00, 0x0000ff};

    brush_colors << "#ff0000\n#ffffff\n#00ff00\n";
    for (uint32_t color = 1; color <= 251; ++color)
      brush_colors << '#' << std::hex << std::setw(6) << std::setfill('0') << color << '\n';
    brush_colors << "#0000ff\n";
    brush_colors.close();
    for (size_t frame = 0; frame < 4; ++frame)
      for (size_t y = 0; y < 9; ++y)
        for (size_t x = 0; x < 13; ++x) {
          const size_t source_frame = frame < 2 ? 0 : frame - 1;
          const auto color = brush_rgb[source_frame == 2 ? 0 : (x / 2 + y / 3 * 4 + x * y / (source_frame + 1)) % 5];

          brush_pixels.push_back(static_cast<uint8_t>(color >> 16));
          brush_pixels.push_back(static_cast<uint8_t>(color >> 8));
          brush_pixels.push_back(static_cast<uint8_t>(color));
        }
    std::ofstream(brush_raw, std::ios::binary).write(reinterpret_cast<const char*>(brush_pixels.data()),
      static_cast<std::streamsize>(brush_pixels.size()));
    mp42rhm::Process brush_fixture({L"ffmpeg.exe", L"-hide_banner", L"-loglevel", L"error", L"-nostdin",
      L"-f", L"rawvideo", L"-pixel_format", L"rgb24", L"-video_size", L"13x9", L"-framerate", L"60",
      L"-i", brush_raw.wstring(), L"-c:v", L"libx264rgb", L"-crf", L"0", L"-preset", L"ultrafast", brush_video.wstring()});

    brush_fixture.finish();
    const std::array<uint32_t, 9> brush_sizes{2, 3, 8, 16, 31, 32, 33, 63, 64};

    for (unsigned variant = 0; variant < brush_sizes.size() * 2; ++variant) {
      const auto format = variant % 2 ? mp42rhm::Format::Rhm : mp42rhm::Format::Sspm;
      const uint32_t brush = brush_sizes[variant / 2];
      const int margin = brush * 2, canvas_width = 13 + margin * 2, canvas_height = 9 + margin * 2;
      const size_t canvas_size = size_t(canvas_width) * canvas_height;
      auto brush_options = compensated_options;
      const auto name = "brush-" + std::to_string(variant);

      brush_options.format = format;
      brush_options.input = brush_video;
      brush_options.palette_cycle = brush_palette;
      brush_options.width = 13;
      brush_options.height = 9;
      brush_options.span = 3.25;
      brush_options.brush_size = brush;
      brush_options.output = directory / (name + (format == mp42rhm::Format::Sspm ? ".sspm" : ".rhm"));
      brush_options.colorset = directory / (name + ".txt");
      const auto stats = mp42rhm::convert(brush_options);
      const auto generated = format == mp42rhm::Format::Sspm ? read_sspm_v2(brush_options.output) : read_map(brush_options.output);
      std::ifstream colors(brush_options.colorset);
      std::vector<uint32_t> rgb;
      std::string line;
      std::vector<uint64_t> order(generated.map.notes.size());
      std::vector<uint32_t> canvas(canvas_size * 4);
      bool reflected = false;

      while (std::getline(colors, line))
        rgb.push_back(mp42rhm::parse_color(line));
      colors.close();
      require(rgb.size() == stats.notes && stats.notes == order.size(), "Brush colors must match note count");
      require(stats.frames == 4 && stats.duration_ms == 67 && stats.filler_notes == 0, "Brush frame counts");
      require(std::count_if(generated.map.notes.begin(), generated.map.notes.end(),
        [](const auto& note) { return note.time == 67; }) == 1, "Background frame must retain one timestamp");
      if (brush == 3)
        require(stats.notes < 288, "Brush compaction did not reduce the scan-only fixture");
      for (size_t i = 0; i < order.size(); ++i)
        order[i] = (uint64_t(generated.map.notes[i].time) << 32) | i;
      mp42rhm::sort_note_indices(order);
      for (size_t rank = order.size(); rank-- > 0;) {
        const auto& note = generated.map.notes[static_cast<uint32_t>(order[rank])];
        const size_t frame = (note.time * 60 + 500) / 1000 - 1;
        const int x = static_cast<int>(std::lround((note.x - 1) * 4 + 6.5 - brush / 2.0)) + margin;
        const int y = static_cast<int>(std::lround((note.y - 1) * 4 + 4.5 - brush / 2.0)) + margin;

        reflected |= x < margin || y < margin;
        require(frame < 4 && x >= 0 && y >= 0 && x + brush <= canvas_width && y + brush <= canvas_height, "Brush position outside canvas");
        require(note.time == mp42rhm::frame_time(frame + 1, 60), "Brush colors are staggered");
        for (int row = y; row < y + static_cast<int>(brush); ++row)
          for (int column = x; column < x + static_cast<int>(brush); ++column)
            canvas[frame * canvas_size + row * canvas_width + column] = rgb[rank];
      }
      require(reflected, "Brush fixture did not exercise reflected edge masks");
      for (size_t frame = 0; frame < 4; ++frame)
        for (size_t y = 0; y < size_t(canvas_height); ++y)
          for (size_t x = 0; x < size_t(canvas_width); ++x) {
            uint32_t expected = 0;

            if (x >= size_t(margin) && x < size_t(margin + 13) && y >= size_t(margin) && y < size_t(margin + 9)) {
              const size_t pixel = (frame * 117 + (y - margin) * 13 + x - margin) * 3;

              expected = uint32_t(brush_pixels[pixel]) << 16 |
                uint32_t(brush_pixels[pixel + 1]) << 8 | brush_pixels[pixel + 2];
            }
            require(canvas[frame * canvas_size + y * canvas_width + x] == expected, "Brush repaint changed pixels or left edge spill");
          }
      if (argc == 2 && std::string(argv[1]) == "--cuda") {
        auto gpu_options = brush_options;

        gpu_options.experimental_cuda = true;
        gpu_options.output = directory / (name + "-cuda" + (format == mp42rhm::Format::Sspm ? ".sspm" : ".rhm"));
        gpu_options.colorset = directory / (name + "-cuda.txt");
        const auto gpu_stats = mp42rhm::convert(gpu_options);
        const auto gpu_map = format == mp42rhm::Format::Sspm ? read_sspm_v2(gpu_options.output) : read_map(gpu_options.output);

        require(gpu_stats.frames == stats.frames && gpu_stats.notes == stats.notes &&
          gpu_stats.peak_frame_notes == stats.peak_frame_notes, "CUDA frame/note counts differ from CPU");
        require(gpu_map.map.notes.size() == generated.map.notes.size(), "CUDA map note count differs from CPU");
        for (size_t i = 0; i < generated.map.notes.size(); ++i) {
          const auto& expected = generated.map.notes[i];
          const auto& actual = gpu_map.map.notes[i];

          require(actual.x == expected.x && actual.y == expected.y && actual.time == expected.time,
            "CUDA note placement or ordering differs from CPU");
        }
        std::ifstream gpu_colors(gpu_options.colorset);

        for (const auto color : rgb) {
          require(bool(std::getline(gpu_colors, line)), "CUDA colorset is truncated");
          require(mp42rhm::parse_color(line) == color, "CUDA colorset differs from CPU");
        }
        require(!std::getline(gpu_colors, line), "CUDA colorset has extra entries");
        gpu_colors.close();
        gpu_options.output = directory / (name + "-cuda-budget" + (format == mp42rhm::Format::Sspm ? ".sspm" : ".rhm"));
        gpu_options.colorset = directory / (name + "-cuda-budget.txt");
        gpu_options.max_notes = stats.notes - 1;
        must_fail([&] { mp42rhm::convert(gpu_options); });
        require(!fs::exists(gpu_options.output) && !fs::exists(gpu_options.colorset), "Published partial CUDA export");
      }
      brush_options.output = directory / (name + "-budget" + (format == mp42rhm::Format::Sspm ? ".sspm" : ".rhm"));
      brush_options.colorset = directory / (name + "-budget.txt");
      brush_options.max_notes = stats.notes - 1;
      must_fail([&] { mp42rhm::convert(brush_options); });
      require(!fs::exists(brush_options.output) && !fs::exists(brush_options.colorset), "Published partial brush export");
      brush_options.brush_size = 0;
      must_fail([&] { mp42rhm::validate(brush_options); });
      brush_options.brush_size = 65;
      must_fail([&] { mp42rhm::validate(brush_options); });
      brush_options.brush_size = brush;
      brush_options.mode = mp42rhm::ColorMode::Bw;
      brush_options.palette_cycle.clear();
      must_fail([&] { mp42rhm::validate(brush_options); });
    }

    auto invalid_compensation = compensated_options;

    invalid_compensation.format = mp42rhm::Format::Rhm;
    invalid_compensation.output = directory / L"invalid-compensation.rhm";
    invalid_compensation.colorset = directory / L"invalid-compensation.txt";
    mp42rhm::validate(invalid_compensation);
    invalid_compensation.format = mp42rhm::Format::Sspm;
    invalid_compensation.output = directory / L"invalid-compensation.sspm";
    invalid_compensation.difficulty_name.clear();
    mp42rhm::validate(invalid_compensation);
    invalid_compensation.palette_cycle.clear();
    invalid_compensation.mode = mp42rhm::ColorMode::Bw;
    invalid_compensation.color_count = 8;
    must_fail([&] { mp42rhm::validate(invalid_compensation); });
    invalid_compensation = compensated_options;
    invalid_compensation.format = mp42rhm::Format::Sspm;
    invalid_compensation.output = directory / L"compensation-budget.sspm";
    invalid_compensation.colorset = directory / L"compensation-budget.txt";
    invalid_compensation.max_notes = 119;
    must_fail([&] { mp42rhm::convert(invalid_compensation); });
    require(!fs::exists(invalid_compensation.output) && !fs::exists(invalid_compensation.colorset),
      "Published a partial compensated map");
    cycle_options.format = mp42rhm::Format::Sspm;
    cycle_options.output = directory / L"cycle-budget.sspm";
    cycle_options.colorset = directory / L"cycle-budget.txt";
    cycle_options.max_notes = 9;
    must_fail([&] { mp42rhm::convert(cycle_options); });
    require(!fs::exists(cycle_options.output) && !fs::exists(cycle_options.colorset), "Published partial palette map");
    cycle_options.background = 0xffffff;
    must_fail([&] { mp42rhm::convert(cycle_options); });
    require(mp42rhm::parse_color("#d8D4cf") == 0xd8d4cf, "RGB hex parsing");
    must_fail([] { mp42rhm::parse_color("#zzzzzz"); });

    options.format = mp42rhm::Options{}.format;
    options.output = directory / L"mono.sspm";
    mp42rhm::convert(options);
    check_sspm(options.output, mono);
    must_fail([&] { mp42rhm::convert(options); });
    check_sspm(options.output, mono);

    options.format = mp42rhm::Format::Rhm;
    options.output = directory / L"zip64-audio.rhm";
    mp42rhm::convert(options);
    require(mz_zip_validate_file_archive(options.output.string().c_str(), 0, nullptr) != 0,
      "ZIP64 archive with audio failed header/decompression/CRC checks");

    options.format = mp42rhm::Format::Rhm;
    options.output = directory / L"color.rhm";
    options.mode = mp42rhm::ColorMode::Color;
    options.colorset = directory / L"color.txt";
    options.audio = false;
    const auto color_stats = mp42rhm::convert(options);
    const auto color = read_map(options.output);
    std::ifstream colors(options.colorset, std::ios::binary);
    std::string palette((std::istreambuf_iterator<char>(colors)), {});

    colors.close();
    require(color_stats.notes - color_stats.filler_notes == 5 && color.map.notes.size() == color_stats.notes,
      "Automatic color mode omitted visible pixels");
    require(color.audio.empty(), "no-audio ignored");
    require(palette.size() == 4 * 7 && palette.find("ffffff\n") != std::string::npos &&
      palette.find("ff0000\n") != std::string::npos && palette.find("00ff00\n") != std::string::npos &&
      palette.find("0000ff\n") != std::string::npos, "Generated palette lost source colors");

    options.format = mp42rhm::Format::Sspm;
    options.output = directory / L"color.sspm";
    options.colorset = directory / L"color-sspm.txt";
    mp42rhm::convert(options);
    check_sspm(options.output, color);

    options.format = mp42rhm::Format::Rhm;
    options.output = directory / L"budget.rhm";
    options.colorset = directory / L"budget.txt";
    options.max_notes = 1;
    must_fail([&] { mp42rhm::convert(options); });
    require(!fs::exists(options.output) && !fs::exists(options.colorset), "Published partial output");
    options.format = mp42rhm::Format::Sspm;
    options.output = directory / L"budget.sspm";
    must_fail([&] { mp42rhm::convert(options); });
    require(!fs::exists(options.output) && !fs::exists(options.colorset), "Published partial SSPM");

    options.format = mp42rhm::Format::Rhm;
    options.output = directory / L"clipped.rhm";
    options.colorset.clear();
    options.mode = mp42rhm::ColorMode::Bw;
    options.max_notes = 100;
    options.start = 1.0 / 60;
    options.seconds = 1.0 / 60;
    const auto clip = mp42rhm::convert(options);

    require(clip.frames == 1 && clip.notes == 1 && clip.duration_ms == 17, "Clip timing did not reset");
    for (const auto& entry : fs::directory_iterator(directory))
      if (entry.path().filename().wstring().find(L".mp42rhm-") == 0)
        throw std::runtime_error("Leaked temporary files: " + entry.path().u8string());
    fs::remove_all(directory);
    std::cout << "All converter tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
