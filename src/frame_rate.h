#pragma once

#include <cstdint>
#include <string>

namespace mp42rhm {
  struct FrameRate {
    uint32_t numerator;
    uint32_t denominator;

    FrameRate(uint32_t numerator = 12, uint32_t denominator = 1)
      : numerator(numerator), denominator(denominator) {}

    double value() const { return double(numerator) / denominator; }
    std::wstring text() const;
  };

  FrameRate parse_frame_rate(const std::string& text);
  int32_t frame_time(uint64_t frame, FrameRate fps);
}
