#include "note_order.h"
#include "converter.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <numeric>
#include <stdexcept>

namespace mp42rhm {
  // .NET 9 ArraySortHelper adaptation; see licenses/dotnet-runtime.txt
  static bool less_time(uint64_t left, uint64_t right) {
    return (left >> 32) < (right >> 32);
  }

  static void swap_if_greater(uint64_t* keys, size_t left, size_t right) {
    if (less_time(keys[right], keys[left]))
      std::swap(keys[left], keys[right]);
  }

  static void down_heap(uint64_t* keys, size_t index, size_t count) {
    const auto value = keys[index - 1];

    while (index <= count / 2) {
      size_t child = 2 * index;

      if (child < count && less_time(keys[child - 1], keys[child]))
        ++child;
      if (!less_time(value, keys[child - 1]))
        break;
      keys[index - 1] = keys[child - 1];
      index = child;
    }
    keys[index - 1] = value;
  }

  static void intro_sort(uint64_t* keys, size_t count, unsigned depth) {
    while (count > 1) {
      if (count <= 16) {
        if (count == 2) {
          swap_if_greater(keys, 0, 1);
          return;
        }
        if (count == 3) {
          swap_if_greater(keys, 0, 1);
          swap_if_greater(keys, 0, 2);
          swap_if_greater(keys, 1, 2);
          return;
        }
        for (size_t i = 1; i < count; ++i) {
          const auto value = keys[i];
          size_t j = i;

          while (j > 0 && less_time(value, keys[j - 1])) {
            keys[j] = keys[j - 1];
            --j;
          }
          keys[j] = value;
        }
        return;
      }
      if (depth == 0) {
        for (size_t i = count / 2; i > 0; --i)
          down_heap(keys, i, count);
        for (size_t i = count; i > 1; --i) {
          std::swap(keys[0], keys[i - 1]);
          down_heap(keys, 1, i - 1);
        }
        return;
      }
      --depth;

      const size_t hi = count - 1;
      const size_t middle = hi / 2;

      swap_if_greater(keys, 0, middle);
      swap_if_greater(keys, 0, hi);
      swap_if_greater(keys, middle, hi);
      const auto pivot = keys[middle];

      std::swap(keys[middle], keys[hi - 1]);
      size_t left = 0, right = hi - 1;

      while (left < right) {
        while (less_time(keys[++left], pivot)) {}
        while (less_time(pivot, keys[--right])) {}
        if (left >= right)
          break;
        std::swap(keys[left], keys[right]);
      }
      if (left != hi - 1)
        std::swap(keys[left], keys[hi - 1]);
      intro_sort(keys + left + 1, count - left - 1, depth);
      count = left;
    }
  }

  void sort_note_indices(std::vector<uint64_t>& order) {
    unsigned bits = 0;

    for (size_t n = order.size(); n > 0; n >>= 1)
      ++bits;
    intro_sort(order.data(), order.size(), 2 * bits);
  }

  void compensate_note_order(const std::filesystem::path& input_path, const std::filesystem::path& output_path,
    const std::vector<uint32_t>& frame_counts, uint32_t fps) {
    const uint64_t count = std::accumulate(frame_counts.begin(), frame_counts.end(), uint64_t{0});

    if (count > INT32_MAX)
      throw std::runtime_error("Steam color compensation exceeds the signed array-index range");
    if (std::filesystem::file_size(input_path) != count * 14)
      throw std::runtime_error("Unexpected SSPM v2 note staging size");

    std::vector<uint64_t> order(static_cast<size_t>(count));
    size_t begin = 0;

    for (size_t frame = 0; frame < frame_counts.size(); ++frame) {
      const uint64_t time = static_cast<uint32_t>(frame_time(frame + 1, fps));

      for (size_t i = 0; i < frame_counts[frame]; ++i)
        order[begin + i] = (time << 32) | (begin + i);
      begin += frame_counts[frame];
    }
    sort_note_indices(order);

    std::ifstream input(input_path, std::ios::binary);
    std::ofstream output(output_path, std::ios::binary);
    std::vector<char> original, compensated;

    input.exceptions(std::ios::failbit | std::ios::badbit);
    output.exceptions(std::ios::failbit | std::ios::badbit);
    begin = 0;
    for (uint32_t frame_count : frame_counts) {
      if (frame_count == 0)
        continue;
      original.resize(size_t(frame_count) * 14);
      compensated.resize(original.size());
      input.read(original.data(), static_cast<std::streamsize>(original.size()));
      for (size_t i = 0; i < frame_count; ++i) {
        const size_t destination = static_cast<uint32_t>(order[begin + i]);

        if (destination < begin || destination >= begin + frame_count)
          throw std::runtime_error("Color compensation crossed a frame boundary");
        std::memcpy(compensated.data() + (destination - begin) * 14, original.data() + i * 14, 14);
      }
      output.write(compensated.data(), static_cast<std::streamsize>(compensated.size()));
      begin += frame_count;
    }
    output.close();
  }
}
