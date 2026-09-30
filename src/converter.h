#pragma once

#include "frame_rate.h"
#include <cstdint>
#include <filesystem>
#include <string>

namespace mp42rhm {
  enum class Format { Sspm, Rhm };
  enum class ColorMode { Bw, Grayscale, Color };

  struct Options {
    Format format = Format::Sspm;
    ColorMode mode = ColorMode::Bw;
    uint32_t color_count = 0;
    std::filesystem::path input;
    std::filesystem::path output;
    std::filesystem::path colorset;
    std::filesystem::path palette_cycle;
    uint32_t background = 0;
    std::string title;
    std::string difficulty_name;
    std::wstring ffmpeg = L"ffmpeg.exe";
    std::wstring ffprobe = L"ffprobe.exe";
    uint32_t width = 160;
    uint32_t height = 90;
    FrameRate fps;
    uint32_t brush_size = 1;
    uint32_t threshold = 128;
    uint64_t max_notes = 100000000;
    double start = 0;
    double seconds = 0;
    double span = 0;
    bool invert = false;
    bool audio = true;
    bool experimental_cuda = false;
    bool adaptive_palette = false;
    bool compact_colorset = false;
    uint32_t subtitle_track = 0;
  };

  struct Statistics {
    uint64_t frames = 0;
    uint64_t notes = 0;
    uint64_t filler_notes = 0;
    uint64_t peak_frame_notes = 0;
    int32_t duration_ms = 0;
  };

  uint32_t parse_color(const std::string& text);
  double note_scale(const Options& options);
  void validate(const Options& options);
  Statistics convert(const Options& options);
}
