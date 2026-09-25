#pragma once

#include <filesystem>
#include <memory>
#include <ostream>

namespace mp42rhm {
  void normalize_zip64(const std::filesystem::path& path);

  class Archive {
  public:
    Archive(const std::filesystem::path& path, bool zip64);
    ~Archive();
    Archive(const Archive&) = delete;
    Archive& operator=(const Archive&) = delete;
    std::ostream& map();
    void finish_map();
    void add_audio(const std::filesystem::path& path);
    void finish();

  private:
    struct State;
    std::unique_ptr<State> state;
  };
}
