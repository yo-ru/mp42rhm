#pragma once

#include <Windows.h>
#include <cstddef>
#include <string>
#include <vector>

namespace mp42rhm {
  class Process {
  public:
    explicit Process(const std::vector<std::wstring>& arguments);
    ~Process();
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;
    size_t read(void* buffer, size_t size);
    std::string finish();

  private:
    HANDLE process = nullptr;
    HANDLE output = nullptr;
  };
}
