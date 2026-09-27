#include "brush_optimizer.h"
#include <algorithm>
#include <array>
#include <intrin.h>
#include <queue>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace mp42rhm {
  static std::vector<BrushStroke> schedule_strokes(std::vector<BrushStroke> strokes,
    const std::vector<uint8_t>& pixels, const Options& options,
    const std::vector<BrushStroke>& alternative, const std::vector<uint32_t>& cycle) {
    if (cycle.empty() && (options.brush_size == 1 || strokes.size() < 2))
      return strokes;

    auto candidates = strokes;

    if (!alternative.empty()) {
      const auto key = [](const BrushStroke& s) { return std::tie(s.x, s.y, s.color); };

      candidates.insert(candidates.end(), alternative.begin(), alternative.end());
      std::sort(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) { return key(a) < key(b); });
      candidates.erase(std::unique(candidates.begin(), candidates.end(), [&](const auto& a, const auto& b) {
        return key(a) == key(b);
      }), candidates.end());
    }
    const int brush = static_cast<int>(options.brush_size), margin = brush * 2;
    const int width = options.width + margin * 2, height = options.height + margin * 2;
    const int columns = (width + brush - 1) / brush, rows = (height + brush - 1) / brush;
    const int stride = (width + 63) / 64;
    const uint64_t full = UINT64_MAX >> (64 - brush);
    const size_t count = candidates.size();
    std::vector<uint32_t> target(size_t(width) * height, options.background), stamps(count);
    std::vector<uint64_t> masks(count * brush), covered(size_t(stride) * height);
    std::vector<uint16_t> bad(count);
    std::vector<std::vector<uint32_t>> cells(size_t(columns) * rows);
    std::unordered_map<uint32_t, size_t> color_indices;

    for (auto color : cycle)
      color_indices.emplace(color, color_indices.size());
    std::vector<std::priority_queue<std::pair<int, uint32_t>>> queues(cycle.empty() ? 1 : color_indices.size());
    const auto enqueue = [&](uint32_t i, int area) {
      queues[cycle.empty() ? 0 : color_indices.at(candidates[i].color)].emplace(cycle.empty() ? area : 0, i);
    };

    for (uint32_t y = 0; !pixels.empty() && y < options.height; ++y)
      for (uint32_t x = 0; x < options.width; ++x) {
        const size_t p = (size_t(y) * options.width + x) * 3;

        target[size_t(y + margin) * width + x + margin] =
          uint32_t(pixels[p]) << 16 | uint32_t(pixels[p + 1]) << 8 | pixels[p + 2];
      }
    if (pixels.empty())
      for (const auto& s : strokes)
        for (int y = s.y; y < s.y + brush; ++y)
          std::fill_n(target.begin() + size_t(y + margin) * width + s.x + margin, brush, s.color);
    for (uint32_t i = 0; i < count; ++i) {
      const auto& s = candidates[i];

      for (int y = 0; y < brush; ++y) {
        uint64_t bits = 0;

        for (int x = 0; x < brush; ++x)
          if (target[size_t(s.y + y + margin) * width + s.x + x + margin] != s.color)
            bits |= uint64_t(1) << x;
        masks[size_t(i) * brush + y] = bits;
        bad[i] += static_cast<uint16_t>(__popcnt64(bits));
      }
      if (!bad[i])
        enqueue(i, brush * brush);
      for (int y = (s.y + margin) / brush; y <= (s.y + margin + brush - 1) / brush; ++y)
        for (int x = (s.x + margin) / brush; x <= (s.x + margin + brush - 1) / brush; ++x)
          cells[size_t(y) * columns + x].push_back(i);
    }

    std::vector<BrushStroke> result;
    std::array<uint64_t, 64> exposed{};
    uint32_t serial = 0;
    size_t remaining = count;

    result.reserve(strokes.size());
    // Work back to front, matching every pixel not covered by a later stroke
    while (remaining) {
      auto& ready = queues[cycle.empty() ? 0 : color_indices.at(cycle[result.size() % cycle.size()])];

      if (ready.empty()) {
        if (cycle.empty())
          break;
        if (result.size() >= options.max_notes)
          throw std::runtime_error("Compact colorset exceeds the note budget");
        result.push_back({INT32_MIN, INT32_MIN, cycle[result.size() % cycle.size()]});
        continue;
      }
      const auto [previous_area, i] = ready.top();
      const auto& s = candidates[i];
      const int shift = (s.x + margin) & 63, word_x = (s.x + margin) / 64;
      int visible = 0, top = brush, bottom = 0, left = brush, right = 0;

      ready.pop();
      for (int y = 0; y < brush; ++y) {
        const size_t word = size_t(s.y + y + margin) * stride + word_x;
        uint64_t bits = covered[word] >> shift;

        if (shift + brush > 64)
          bits |= covered[word + 1] << (64 - shift);
        exposed[y] = ~bits & full;
        visible += static_cast<int>(__popcnt64(exposed[y]));
      }
      if (!visible) {
        --remaining;
        continue;
      }
      if (cycle.empty() && visible != previous_area) {
        ready.emplace(visible, i);
        continue;
      }
      --remaining;
      if (!cycle.empty() && result.size() >= options.max_notes)
        throw std::runtime_error("Compact colorset exceeds the note budget");
      result.push_back(s);
      if (cycle.empty() && result.size() >= strokes.size())
        return strokes;
      for (int y = 0; y < brush; ++y) {
        if (!exposed[y])
          continue;
        const size_t word = size_t(s.y + y + margin) * stride + word_x;
        unsigned long first, last;

        _BitScanForward64(&first, exposed[y]);
        _BitScanReverse64(&last, exposed[y]);
        left = std::min(left, static_cast<int>(first));
        right = std::max(right, static_cast<int>(last) + 1);
        top = std::min(top, y);
        bottom = y + 1;
        covered[word] |= full << shift;
        if (shift + brush > 64)
          covered[word + 1] |= full >> (64 - shift);
      }
      ++serial;
      // Only newly covered pixels can unlock another stroke
      for (int cy = (s.y + top + margin) / brush; cy <= (s.y + bottom - 1 + margin) / brush; ++cy)
        for (int cx = (s.x + left + margin) / brush; cx <= (s.x + right - 1 + margin) / brush; ++cx)
          for (uint32_t j : cells[size_t(cy) * columns + cx]) {
            if (!bad[j] || stamps[j] == serial)
              continue;
            stamps[j] = serial;
            const auto& other = candidates[j];
            const int dy = s.y - other.y, dx = s.x - other.x;

            if (dx + right <= 0 || dx + left >= brush)
              continue;
            for (int y = std::max(top, -dy); y < std::min(bottom, brush - dy); ++y) {
              const uint64_t erase = dx >= 0 ? exposed[y] << dx : exposed[y] >> -dx;
              auto& bits = masks[size_t(j) * brush + y + dy];

              bad[j] -= static_cast<uint16_t>(__popcnt64(bits & erase));
              bits &= ~erase;
            }
            if (!bad[j])
              enqueue(j, brush * brush);
          }
    }
    while (!cycle.empty() && result.size() % cycle.size()) {
      if (result.size() >= options.max_notes)
        throw std::runtime_error("Compact colorset exceeds the note budget");
      result.push_back({INT32_MIN, INT32_MIN, cycle[result.size() % cycle.size()]});
    }
    if (result.empty())
      return strokes;
    std::reverse(result.begin(), result.end());
    return result;
  }

  std::vector<BrushStroke> reorder_brush_strokes(std::vector<BrushStroke> strokes,
    const std::vector<uint8_t>& pixels, const Options& options,
    const std::vector<BrushStroke>& alternative) {
    return schedule_strokes(std::move(strokes), pixels, options, alternative, {});
  }

  std::vector<BrushStroke> cycle_brush_strokes(std::vector<BrushStroke> strokes,
    const Options& options, const std::vector<uint32_t>& cycle) {
    return schedule_strokes(std::move(strokes), {}, options, {}, cycle);
  }
}
