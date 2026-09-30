#pragma once

#include <Windows.h>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace mp42rhm {
  class TemporaryDirectory {
  public:
    std::filesystem::path path;

    explicit TemporaryDirectory(const std::filesystem::path& parent) {
      for (uint32_t suffix = 0; suffix < 1000; ++suffix) {
        path = parent / (L".mp42rhm-" + std::to_wstring(GetCurrentProcessId()) +
          L"-" + std::to_wstring(suffix));
        if (std::filesystem::create_directory(path))
          return;
      }
      throw std::runtime_error("Cannot create temporary conversion directory");
    }

    ~TemporaryDirectory() {
      std::error_code error;

      // File scanners can briefly hold staging files after export
      for (unsigned attempt = 0; attempt <= 20; ++attempt) {
        std::filesystem::remove_all(path, error);
        if (!error)
          return;
        if (attempt == 20 || (error.value() != ERROR_ACCESS_DENIED &&
          error.value() != ERROR_SHARING_VIOLATION && error.value() != ERROR_DIR_NOT_EMPTY))
          break;
        Sleep(100);
      }
      std::cerr << "Warning: Cannot remove temporary directory " << path.u8string()
        << " (" << error.value() << ": " << error.message() << ")\n";
    }
  };
}
