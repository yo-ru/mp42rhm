#pragma once

#include "frame_rate.h"
#include <cstdint>
#include <filesystem>
#include <vector>

namespace mp42rhm {
  // High 32 bits are time, low 32 bits are the original index
  void sort_note_indices(std::vector<uint64_t>& order);
  void compensate_note_order(const std::filesystem::path& input, const std::filesystem::path& output,
    const std::vector<uint32_t>& frame_counts, FrameRate fps);
}
