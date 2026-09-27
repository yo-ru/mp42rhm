#include "cuda_brush.h"
#include "cuda_source.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

namespace mp42rhm {
  static_assert(sizeof(BrushStroke) == 12);

  struct CudaBrushEncoder::Impl {
    using Device = uint64_t;
    HMODULE dll = nullptr;
    HMODULE compiler = nullptr;
    Options options;
    void* context = nullptr;
    void* module = nullptr;
    void* kernel = nullptr;
    void* cleanup_kernel = nullptr;
    void* blank_kernel = nullptr;
    std::mutex mutex;
    std::condition_variable available;
    struct Slot {
      bool busy = false;
      void* stream = nullptr;
      Device owner = 0, canvas = 0, kept = 0;
      Device input = 0, fallback = 0, offsets = 0, counts = 0, target = 0, coverage = 0;
    };
    std::vector<Slot> slots;
    size_t capacity, target_bytes, coverage_bytes;

    template<typename... Args> int invoke(HMODULE library, const char* name, Args... args) {
      auto function = reinterpret_cast<int(__stdcall*)(Args...)>(GetProcAddress(library, name));

      if (!function)
        throw std::runtime_error(std::string("Missing CUDA function: ") + name);
      return function(args...);
    }

    template<typename... Args> void discard(HMODULE library, const char* name, Args... args) noexcept {
      auto function = reinterpret_cast<int(__stdcall*)(Args...)>(GetProcAddress(library, name));

      if (function)
        function(args...);
    }

    template<typename... Args> void call(const char* name, Args... args) {
      const int status = invoke(dll, name, args...);

      if (status) {
        const char* message = nullptr;

        discard(dll, "cuGetErrorString", status, &message);
        throw std::runtime_error(std::string(name) + ": " + (message ? message : "CUDA failure"));
      }
    }

    template<typename... Args> void compile_call(const char* name, Args... args) {
      const int status = invoke(compiler, name, args...);

      if (status)
        throw std::runtime_error(std::string(name) + ": NVRTC error " + std::to_string(status));
    }

    std::string compile_kernel() {
      for (const auto* name : {L"nvrtc64_130_0.dll", L"nvrtc64_120_0.dll"}) {
        compiler = LoadLibraryW(name);
        if (compiler)
          break;
      }
      if (!compiler)
        throw std::runtime_error("Experimental CUDA requires CUDA 12/13 NVRTC and its matching builtins DLL beside mp42rhm.exe or on PATH");
      void* program = nullptr;

      compile_call("nvrtcCreateProgram", &program, CUDA_SOURCE, "cuda_brush.cu", 0,
        static_cast<const char**>(nullptr), static_cast<const char**>(nullptr));
      try {
        int major = 0, minor = 0, count = 0;

        call("cuDeviceGetAttribute", &major, 75, 0);
        call("cuDeviceGetAttribute", &minor, 76, 0);
        compile_call("nvrtcGetNumSupportedArchs", &count);
        std::vector<int> architectures(count);

        compile_call("nvrtcGetSupportedArchs", architectures.data());
        int architecture = 0;

        for (const int candidate : architectures)
          if (candidate <= major * 10 + minor)
            architecture = std::max(architecture, candidate);
        if (!architecture)
          throw std::runtime_error("This GPU is not supported by the installed CUDA runtime compiler");
        const std::string target = "--gpu-architecture=compute_" + std::to_string(architecture);
        const char* arguments[] = {target.c_str(), "--std=c++17"};

        if (invoke(compiler, "nvrtcCompileProgram", program, 2, arguments)) {
          size_t size = 0;

          compile_call("nvrtcGetProgramLogSize", program, &size);
          std::string log(size, '\0');

          compile_call("nvrtcGetProgramLog", program, log.data());
          throw std::runtime_error("CUDA compilation failed: " + log);
        }
        size_t size = 0;

        compile_call("nvrtcGetPTXSize", program, &size);
        std::string ptx(size, '\0');

        compile_call("nvrtcGetPTX", program, ptx.data());
        discard(compiler, "nvrtcDestroyProgram", &program);
        return ptx;
      } catch (...) {
        discard(compiler, "nvrtcDestroyProgram", &program);
        throw;
      }
    }

    explicit Impl(const Options& value) : options(value) {}

    ~Impl() {
      if (context) {
        discard(dll, "cuCtxSetCurrent", context);
        for (auto& slot : slots) {
          if (slot.stream)
            discard(dll, "cuStreamSynchronize", slot.stream);
          for (const Device buffer : {slot.input, slot.fallback, slot.offsets, slot.counts,
            slot.target, slot.coverage, slot.owner, slot.canvas, slot.kept})
            if (buffer)
              discard(dll, "cuMemFree_v2", buffer);
          if (slot.stream)
            discard(dll, "cuStreamDestroy_v2", slot.stream);
        }
        if (module)
          discard(dll, "cuModuleUnload", module);
        discard(dll, "cuDevicePrimaryCtxRelease_v2", 0);
      }
      if (compiler)
        FreeLibrary(compiler);
      if (dll)
        FreeLibrary(dll);
    }

    void initialize() {
      dll = LoadLibraryW(L"nvcuda.dll");
      if (!dll)
        throw std::runtime_error("Experimental CUDA requires an NVIDIA GPU and driver");
      const size_t padding = options.brush_size * 4;
      const size_t width = options.width + padding, height = options.height + padding;

      capacity = 16 * (size_t(options.width) * options.height + options.width + options.height + padding);
      target_bytes = width * height * 4;
      coverage_bytes = 16 * ((width + 63) / 64 + 1) * height * 8;
      call("cuInit", 0U);
      call("cuDevicePrimaryCtxRetain", &context, 0);
      call("cuCtxSetCurrent", context);
      const auto ptx = compile_kernel();
      size_t free_bytes = 0, total_bytes = 0;

      call("cuMemGetInfo_v2", &free_bytes, &total_bytes);
      const size_t slot_bytes = capacity * sizeof(BrushStroke) + target_bytes * 33 + coverage_bytes + 384;
      // Leave VRAM for the desktop and other applications
      const size_t frames = std::min(size_t(32), (free_bytes - free_bytes / 4) / slot_bytes);

      if (!frames)
        throw std::runtime_error("Not enough available VRAM for this resolution; reduce the resolution or omit --experimental-cuda");
      slots.resize(frames);
      call("cuModuleLoadData", &module, ptx.c_str());
      call("cuModuleGetFunction", &kernel, module, "compact");
      call("cuModuleGetFunction", &cleanup_kernel, module, "cleanup");
      call("cuModuleGetFunction", &blank_kernel, module, "retain_blank");
      for (auto& slot : slots) {
        call("cuStreamCreate", &slot.stream, 1U);
        call("cuMemAlloc_v2", &slot.input, capacity * sizeof(BrushStroke));
        call("cuMemAlloc_v2", &slot.fallback, size_t(16 * sizeof(BrushStroke)));
        call("cuMemAlloc_v2", &slot.offsets, size_t(64));
        call("cuMemAlloc_v2", &slot.counts, size_t(64));
        call("cuMemAlloc_v2", &slot.target, target_bytes);
        call("cuMemAlloc_v2", &slot.coverage, coverage_bytes);
        call("cuMemAlloc_v2", &slot.owner, target_bytes * 16);
        call("cuMemAlloc_v2", &slot.canvas, target_bytes * 16);
        call("cuMemAlloc_v2", &slot.kept, size_t(64));
      }
    }

    void compact(std::vector<std::vector<BrushStroke>>& candidates,
      const std::vector<uint8_t>& pixels) {
      int brush = options.brush_size, first = 0, steps = 512, groups = 16;
      const int margin = brush * 2;
      int width = options.width + margin * 2, height = options.height + margin * 2;
      std::array<uint32_t, 16> offsets{}, counts{}, kept{};
      std::vector<BrushStroke> input;
      std::vector<uint32_t> target(size_t(width) * height, options.background);
      std::unique_lock<std::mutex> lock(mutex);
      Slot* slot = nullptr;

      available.wait(lock, [&] {
        for (auto& candidate : slots)
          if (!candidate.busy) {
            slot = &candidate;
            return true;
          }
        return false;
      });
      slot->busy = true;
      lock.unlock();
      const auto release = [&] {
        std::lock_guard<std::mutex> guard(mutex);

        slot->busy = false;
        available.notify_one();
      };
      try {
        call("cuCtxSetCurrent", context);

        for (size_t i = 0; i < candidates.size(); ++i) {
          offsets[i] = static_cast<uint32_t>(input.size());
          counts[i] = static_cast<uint32_t>(candidates[i].size());
          input.insert(input.end(), candidates[i].begin(), candidates[i].end());
        }
        if (input.size() > capacity)
          throw std::runtime_error("CUDA brush capacity exceeded");
        for (uint32_t y = 0; y < options.height; ++y)
          for (uint32_t x = 0; x < options.width; ++x) {
            const size_t p = (size_t(y) * options.width + x) * 3;

            target[size_t(y + margin) * width + x + margin] = (uint32_t(pixels[p]) << 16) |
              (uint32_t(pixels[p + 1]) << 8) | pixels[p + 2];
          }
        const auto upload = [&](Device dest, const void* source, size_t bytes) {
          call("cuMemcpyHtoDAsync_v2", dest, source, bytes, slot->stream);
        };

        upload(slot->input, input.data(), input.size() * sizeof(BrushStroke));
        upload(slot->offsets, offsets.data(), sizeof(offsets));
        upload(slot->counts, counts.data(), sizeof(counts));
        upload(slot->target, target.data(), target_bytes);
        call("cuMemsetD8Async", slot->coverage, static_cast<unsigned char>(0), coverage_bytes, slot->stream);
        call("cuMemsetD8Async", slot->owner, static_cast<unsigned char>(0), target_bytes * 16, slot->stream);
        call("cuMemsetD32Async", slot->canvas, options.background, target_bytes * 4, slot->stream);
        call("cuMemsetD8Async", slot->kept, static_cast<unsigned char>(0), size_t(64), slot->stream);
        void* args[] = {&slot->input, &slot->input, &slot->offsets, &slot->counts, &slot->target,
          &slot->coverage, &width, &height, &brush, &first, &steps, &groups, &slot->owner, &slot->fallback};
        const int maximum = *std::max_element(counts.begin(), counts.end());

        for (; first < maximum; first += steps)
          call("cuLaunchKernel", kernel, 16U, 1U, 1U, 128U, 1U, 1U, 0U, slot->stream, args, static_cast<void**>(nullptr));
        void* cleanup_args[] = {&slot->input, &slot->input, &slot->offsets, &slot->counts,
          &slot->owner, &slot->canvas, &slot->kept, &width, &height, &brush, &first, &steps};
        for (first = 0; first < maximum; first += steps)
          call("cuLaunchKernel", cleanup_kernel, 16U, 1U, 1U, 128U, 1U, 1U, 0U, slot->stream, cleanup_args, static_cast<void**>(nullptr));
        void* blank_args[] = {&slot->fallback, &slot->input, &slot->offsets, &slot->counts, &slot->kept};
        call("cuLaunchKernel", blank_kernel, 16U, 1U, 1U, 1U, 1U, 1U, 0U, slot->stream, blank_args, static_cast<void**>(nullptr));

        call("cuMemcpyDtoHAsync_v2", kept.data(), slot->kept, sizeof(kept), slot->stream);
        call("cuStreamSynchronize", slot->stream);
        std::array<size_t, 16> order{};

        for (size_t i = 0; i < order.size(); ++i)
          order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return kept[a] < kept[b]; });
        candidates.clear();
        candidates.resize(2);
        for (size_t rank = 0; rank < candidates.size(); ++rank) {
          const size_t best = order[rank];

          call("cuMemcpyDtoHAsync_v2", input.data(), slot->input + size_t(offsets[best]) * sizeof(BrushStroke),
            size_t(counts[best]) * sizeof(BrushStroke), slot->stream);
          call("cuStreamSynchronize", slot->stream);
          candidates[rank].reserve(kept[best]);
          for (size_t n = 0; n < counts[best]; ++n)
            if (input[n].x != INT32_MIN)
              candidates[rank].push_back(input[n]);
        }
      } catch (...) {
        discard(dll, "cuStreamSynchronize", slot->stream);
        release();
        throw;
      }
      release();
    }
  };

  CudaBrushEncoder::CudaBrushEncoder(const Options& options) : impl(std::make_unique<Impl>(options)) {
    impl->initialize();
  }

  CudaBrushEncoder::~CudaBrushEncoder() = default;

  size_t CudaBrushEncoder::parallel_frames() const {
    return impl->slots.size();
  }

  void CudaBrushEncoder::compact(std::vector<std::vector<BrushStroke>>& candidates,
    const std::vector<uint8_t>& pixels) {
    impl->compact(candidates, pixels);
  }
}
