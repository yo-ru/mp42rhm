using U64 = unsigned long long;
struct Stroke {
  int x, y;
  unsigned color;
};

static_assert(sizeof(Stroke) == 12);
static constexpr int THREADS = 128;
static constexpr int CULLED = -2147483647 - 1;

__device__ int imin(int a, int b) { return a < b ? a : b; }
__device__ int imax(int a, int b) { return a > b ? a : b; }
__device__ U64 exposed(const U64* covered, int words, int x, int y, U64 mask) {
  int word = y * words + x / 64;
  unsigned shift = x % 64;

  return ((~covered[word] >> shift) | (shift ? ~covered[word + 1] << (64 - shift) : 0)) & mask;
}

extern "C" __global__ void compact(const Stroke* input, Stroke* output, const unsigned* offsets,
  const unsigned* counts, const unsigned* target, U64* coverage, int width, int height,
  int brush, int first, int steps, int groups_per_frame, unsigned* owner) {
  const int tid = threadIdx.x, group = blockIdx.x;
  const int words = (width + 63) / 64 + 1;
  const int margin = brush * 2;
  const U64 mask = brush == 64 ? ~U64(0) : (U64(1) << brush) - 1;
  U64* covered = coverage + group * words * height;
  target += (group / groups_per_frame) * width * height;
  owner += group * width * height;
  __shared__ int warp_score[THREADS / 32], warp_rank[THREADS / 32];
  __shared__ U64 row_exposed[64], row_bad[64];
  __shared__ int min_x, min_y, nx, ny, original_count, chosen_x, chosen_y;

  for (int step = 0; step < steps; ++step) {
    const int index = int(counts[group]) - 1 - first - step;

    if (index < 0)
      return;
    const Stroke stroke = input[offsets[group] + index];
    const int x = stroke.x + margin, y = stroke.y + margin;

    if (tid < 32) {
      int l = x + brush, r = x - 1, t = y + brush, b = y - 1, count = 0;

      for (int row = tid; row < brush; row += 32) {
        const U64 bits = exposed(covered, words, x, y + row, mask);

        if (bits) {
          l = imin(l, x + __ffsll(bits) - 1);
          r = imax(r, x + 63 - __clzll(bits));
          t = imin(t, y + row);
          b = imax(b, y + row);
          count += __popcll(bits);
        }
      }
      for (int delta = 16; delta; delta /= 2) {
        l = imin(l, __shfl_down_sync(0xffffffff, l, delta));
        r = imax(r, __shfl_down_sync(0xffffffff, r, delta));
        t = imin(t, __shfl_down_sync(0xffffffff, t, delta));
        b = imax(b, __shfl_down_sync(0xffffffff, b, delta));
        count += __shfl_down_sync(0xffffffff, count, delta);
      }
      if (tid == 0) {
        original_count = count;
        min_x = imax(0, r - brush + 1);
        min_y = imax(0, b - brush + 1);
        nx = imin(l, width - brush) - min_x + 1;
        ny = imin(t, height - brush) - min_y + 1;
        chosen_x = x;
        chosen_y = y;
      }
    }
    __syncthreads();
    if (!original_count) {
      if (tid == 0)
        output[offsets[group] + index] = {CULLED, 0, 0};
      continue;
    }
    if (nx * ny > 1) {
      int best = -1, best_rank = 2147483647;

      if (nx * ny > 8) {
        const int lane = tid % 32, warp = tid / 32;
        const int region_w = nx + brush - 1, region_h = ny + brush - 1;
        const U64 region_mask = (U64(1) << region_w) - 1;

        for (int row = warp; row < region_h; row += THREADS / 32) {
          const int py = min_y + row;
          const U64 visible = exposed(covered, words, min_x, py, region_mask);
          const bool a = lane < region_w && (visible & (U64(1) << lane)) &&
            target[py * width + min_x + lane] != stroke.color;
          const bool b = lane + 32 < region_w && (visible & (U64(1) << (lane + 32))) &&
            target[py * width + min_x + lane + 32] != stroke.color;
          const U64 bad = U64(__ballot_sync(0xffffffff, a)) |
            (U64(__ballot_sync(0xffffffff, b)) << 32);
          if (lane == 0) {
            row_exposed[row] = visible;
            row_bad[row] = bad;
          }
        }
        __syncthreads();
        const int rows_per_warp = (ny + 3) / 4;
        const int begin = warp * rows_per_warp, end = imin(ny, begin + rows_per_warp);

        if (lane < nx && begin < end) {
          const U64 window = mask << lane;
          int count = 0, bad = 0;

          for (int row = begin; row < begin + brush; ++row) {
            count += __popcll(row_exposed[row] & window);
            bad += (row_bad[row] & window) != 0;
          }
          for (int cy = begin; cy < end; ++cy) {
            const int rank = cy * nx + lane;

            if (!bad && (min_x + lane != x || min_y + cy != y) && count > best) {
              best = count;
              best_rank = rank;
            }
            if (cy + 1 < end) {
              count += __popcll(row_exposed[cy + brush] & window) - __popcll(row_exposed[cy] & window);
              bad += int((row_bad[cy + brush] & window) != 0) - int((row_bad[cy] & window) != 0);
            }
          }
        }
      } else {
        for (int rank = tid; rank < nx * ny; rank += THREADS) {
          const int cx = min_x + rank % nx, cy = min_y + rank / nx;

          if (cx == x && cy == y)
            continue;
          int value = 0;
          bool valid = true;

          for (int row = cy; row < cy + brush && valid; ++row) {
            U64 bits = exposed(covered, words, cx, row, mask);
            value += __popcll(bits);
            while (bits) {
              const int bit = __ffsll(bits) - 1;

              if (target[row * width + cx + bit] != stroke.color) {
                valid = false;
                break;
              }
              bits &= bits - 1;
            }
          }
          if (valid && value > best) {
            best = value;
            best_rank = rank;
          }
        }
      }
      for (int delta = 16; delta; delta /= 2) {
        const int other = __shfl_down_sync(0xffffffff, best, delta);
        const int rank = __shfl_down_sync(0xffffffff, best_rank, delta);

        if (other > best || (other == best && rank < best_rank)) {
          best = other;
          best_rank = rank;
        }
      }
      if (tid % 32 == 0) {
        warp_score[tid / 32] = best;
        warp_rank[tid / 32] = best_rank;
      }
      __syncthreads();
      if (tid == 0) {
        for (int i = 1; i < THREADS / 32; ++i)
          if (warp_score[i] > best || (warp_score[i] == best && warp_rank[i] < best_rank)) {
            best = warp_score[i];
            best_rank = warp_rank[i];
          }
        if (best > original_count) {
          chosen_x = min_x + best_rank % nx;
          chosen_y = min_y + best_rank / nx;
        }
      }
      __syncthreads();
    }
    if (tid < brush) {
      const int word = (chosen_y + tid) * words + chosen_x / 64;
      const unsigned shift = chosen_x % 64;
      U64 visible = exposed(covered, words, chosen_x, chosen_y + tid, mask);

      while (visible) {
        const int bit = __ffsll(visible) - 1;
        owner[(chosen_y + tid) * width + chosen_x + bit] = index + 1;
        visible &= visible - 1;
      }
      covered[word] |= mask << shift;
      if (shift)
        covered[word + 1] |= mask >> (64 - shift);
    }
    if (tid == 0)
      output[offsets[group] + index] = {chosen_x - margin, chosen_y - margin, stroke.color};
    __syncthreads();
  }
}

extern "C" __global__ void cleanup(const Stroke* input, Stroke* output,
  const unsigned* offsets, const unsigned* counts, const unsigned* owner, unsigned* canvas,
  unsigned* kept, int width, int height, int first, int steps) {
  const int tid = threadIdx.x, group = blockIdx.x;
  owner += group * width * height;
  canvas += group * width * height;
  for (int step = 0; step < steps; ++step) {
    const unsigned index = first + step;

    if (index >= counts[group])
      return;
    const Stroke stroke = input[offsets[group] + index];

    if (stroke.x == CULLED) {
      if (!tid)
        output[offsets[group] + index] = stroke;
      continue;
    }
    const int x = stroke.x + 64, y = stroke.y + 64;
    bool needed = false;

    for (int pixel = tid; pixel < 1024; pixel += 128) {
      const int p = (y + pixel / 32) * width + x + pixel % 32;
      needed |= owner[p] == index + 1 && canvas[p] != stroke.color;
    }
    if (__syncthreads_or(needed)) {
      for (int pixel = tid; pixel < 1024; pixel += 128)
        canvas[(y + pixel / 32) * width + x + pixel % 32] = stroke.color;
      if (!tid) {
        output[offsets[group] + index] = stroke;
        kept[group]++;
      }
    } else if (!tid)
      output[offsets[group] + index] = {CULLED, 0, 0};
    __syncthreads();
  }
}

extern "C" __global__ void retain_blank(const Stroke* input, Stroke* output,
  const unsigned* offsets, const unsigned* counts, unsigned* kept) {
  const int group = blockIdx.x;

  if (threadIdx.x || kept[group])
    return;
  for (int i = int(counts[group]) - 1; i >= 0; --i)
    if (input[offsets[group] + i].x != CULLED) {
      output[offsets[group] + i] = input[offsets[group] + i];
      kept[group] = 1;
      return;
    }
}
