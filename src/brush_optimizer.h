#pragma once

#include "cuda_brush.h"

namespace mp42rhm {
  std::vector<BrushStroke> reorder_brush_strokes(std::vector<BrushStroke> strokes,
    const std::vector<uint8_t>& pixels, const Options& options,
    const std::vector<BrushStroke>& alternative = {});
  std::vector<BrushStroke> cycle_brush_strokes(std::vector<BrushStroke> strokes,
    const Options& options, const std::vector<uint32_t>& cycle);
}
