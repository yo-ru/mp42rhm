#pragma once

#include "converter.h"

namespace mp42rhm {
  uint32_t subtitle_band_height(const Options& options);
  std::wstring prepare_subtitles(const Options& options, const std::filesystem::path& directory);
}
