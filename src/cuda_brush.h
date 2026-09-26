#pragma once

#include "converter.h"
#include <memory>
#include <vector>

namespace mp42rhm {
  struct BrushStroke {
    int32_t x, y;
    uint32_t color;
  };

  class CudaBrushEncoder {
  public:
    explicit CudaBrushEncoder(const Options& options);
    ~CudaBrushEncoder();
    size_t parallel_frames() const;
    std::vector<BrushStroke> compact(std::vector<std::vector<BrushStroke>>& candidates,
      const std::vector<uint8_t>& pixels);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
  };
}
