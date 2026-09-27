#include "frame_rate.h"
#include <numeric>
#include <stdexcept>

namespace mp42rhm {
  std::wstring FrameRate::text() const {
    return std::to_wstring(numerator) + L"/" + std::to_wstring(denominator);
  }

  FrameRate parse_frame_rate(const std::string& text) {
    const auto end = text.find_last_not_of("\r\n ");
    const auto value = text.substr(0, end == std::string::npos ? 0 : end + 1);
    const auto slash = value.find('/'), point = value.find('.');
    const auto integer = [](const std::string& part) {
      if (part.empty() || part.size() > 9 || part.find_first_not_of("0123456789") != std::string::npos)
        throw std::runtime_error("FPS must be a number or fraction, such as 23.976 or 24000/1001");
      return static_cast<uint32_t>(std::stoul(part));
    };
    FrameRate result;

    if (slash != std::string::npos)
      result = {integer(value.substr(0, slash)), integer(value.substr(slash + 1))};
    else if (point != std::string::npos) {
      const auto digits = value.size() - point - 1;

      if (!digits || digits > 6)
        throw std::runtime_error("FPS accepts up to six decimal places");
      uint32_t denominator = 1;

      for (size_t i = 0; i < digits; ++i)
        denominator *= 10;
      result = {integer(value.substr(0, point) + value.substr(point + 1)), denominator};
    } else
      result = integer(value);
    if (!result.numerator || !result.denominator || result.value() < 1 || result.value() > 60)
      throw std::runtime_error("FPS must be between 1 and 60");
    const auto divisor = std::gcd(result.numerator, result.denominator);

    result.numerator /= divisor;
    result.denominator /= divisor;
    return result;
  }

  int32_t frame_time(uint64_t frame, FrameRate fps) {
    const uint64_t interval = uint64_t(fps.denominator) * 1000;

    if (!fps.numerator || !fps.denominator || fps.value() > 1000 ||
      frame > uint64_t(INT32_MAX) * fps.numerator / interval)
      throw std::runtime_error("Frame time exceeds RHM's signed 32-bit millisecond range");
    return static_cast<int32_t>((frame * interval + fps.numerator / 2) / fps.numerator);
  }
}
