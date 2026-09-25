#include "archive.h"
#include "miniz.h"

#include <Windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <future>
#include <stdexcept>
#include <streambuf>
#include <vector>

namespace mp42rhm {
  static constexpr uint64_t ZIP32_LIMIT = 0xffff0000;

  template<class Value>
  static Value little_endian(const uint8_t* bytes) {
    Value value;

    std::memcpy(&value, bytes, sizeof(value));
    return value;
  }

  static void seek_file(FILE* file, uint64_t offset) {
    if (offset > INT64_MAX || _fseeki64(file, static_cast<int64_t>(offset), SEEK_SET) != 0)
      throw std::runtime_error("Cannot seek archive header");
  }

  static void normalize_zip64(FILE* file, uint64_t central_offset, uint32_t entries) {
    for (uint32_t entry = 0; entry < entries; ++entry) {
      std::array<uint8_t, 46> header{};

      seek_file(file, central_offset);
      if (fread(header.data(), 1, header.size(), file) != header.size() ||
        little_endian<uint32_t>(header.data()) != 0x02014b50)
        throw std::runtime_error("Invalid ZIP central directory");

      const auto name_size = little_endian<uint16_t>(header.data() + 28);
      const auto extra_size = little_endian<uint16_t>(header.data() + 30);
      const auto comment_size = little_endian<uint16_t>(header.data() + 32);
      uint64_t local_offset = little_endian<uint32_t>(header.data() + 42);
      std::vector<uint8_t> extra(extra_size);

      seek_file(file, central_offset + header.size() + name_size);
      if (fread(extra.data(), 1, extra.size(), file) != extra.size())
        throw std::runtime_error("Cannot read ZIP64 extra fields");
      for (size_t position = 0; position + 4 <= extra.size();) {
        const auto id = little_endian<uint16_t>(extra.data() + position);
        const auto size = little_endian<uint16_t>(extra.data() + position + 2);

        if (position + 4 + size > extra.size())
          throw std::runtime_error("Truncated ZIP extra field");
        if (id == 1) {
          size_t offset = 0;
          const auto compressed = little_endian<uint32_t>(header.data() + 20);
          const auto uncompressed = little_endian<uint32_t>(header.data() + 24);

          if (uncompressed == UINT32_MAX) {
            offset += 8;
            // This miniz revision includes both sizes when the uncompressed size exceeds ZIP32
            if (compressed != UINT32_MAX && size >= 16 &&
              little_endian<uint64_t>(extra.data() + position + 12) == compressed)
              std::memset(header.data() + 20, 0xff, 4);
          }
          if (little_endian<uint32_t>(header.data() + 20) == UINT32_MAX)
            offset += 8;
          if (local_offset == UINT32_MAX) {
            if (offset + 8 > size)
              throw std::runtime_error("Missing ZIP64 local header offset");
            local_offset = little_endian<uint64_t>(extra.data() + position + 4 + offset);
          }
          break;
        }
        position += 4 + size;
      }

      const uint16_t version = 45;
      std::array<uint8_t, 30> local{};

      seek_file(file, local_offset);
      if (fread(local.data(), 1, local.size(), file) != local.size() ||
        little_endian<uint32_t>(local.data()) != 0x04034b50)
        throw std::runtime_error("Invalid ZIP local header");
      if ((local[6] & 8) && little_endian<uint32_t>(header.data() + 20) != UINT32_MAX &&
        little_endian<uint32_t>(header.data() + 24) != UINT32_MAX) {
        // Avoid ambiguous 32-bit descriptors inside ZIP64 archives
        std::memcpy(local.data() + 14, header.data() + 16, 12);
        local[6] &= ~8;
        header[8] &= ~8;
      }
      std::memcpy(header.data() + 6, &version, sizeof(version));
      std::memcpy(local.data() + 4, &version, sizeof(version));
      seek_file(file, central_offset);
      if (fwrite(header.data(), 1, header.size(), file) != header.size())
        throw std::runtime_error("Cannot update ZIP64 central header");
      seek_file(file, local_offset);
      if (fwrite(local.data(), 1, local.size(), file) != local.size())
        throw std::runtime_error("Cannot update ZIP64 local header");
      central_offset += header.size() + name_size + extra_size + comment_size;
    }
    if (fflush(file) != 0)
      throw std::runtime_error("Cannot flush ZIP64 headers");
  }

  void normalize_zip64(const std::filesystem::path& path) {
    FILE* file = nullptr;

    if (_wfopen_s(&file, path.c_str(), L"r+b") != 0)
      throw std::runtime_error("Cannot open archive for ZIP64 normalization");

    mz_zip_archive zip{};

    try {
      if (!mz_zip_reader_init_cfile(&zip, file, 0, 0))
        throw std::runtime_error("Cannot read ZIP64 archive");

      const auto offset = zip.m_central_directory_file_ofs;
      const auto entries = zip.m_total_files;

      mz_zip_reader_end(&zip);
      normalize_zip64(file, offset, entries);
    } catch (...) {
      if (zip.m_pState)
        mz_zip_reader_end(&zip);
      fclose(file);
      throw;
    }
    fclose(file);
  }

  class PipeBuffer : public std::streambuf {
  public:
    HANDLE reader = nullptr;
    HANDLE writer = nullptr;
    uint64_t bytes = 0;
    uint64_t limit;
    std::vector<char> buffer;

    explicit PipeBuffer(bool zip64) : limit(zip64 ? UINT64_MAX : ZIP32_LIMIT), buffer(1024 * 1024) {
      if (!CreatePipe(&reader, &writer, nullptr, 1024 * 1024))
        throw std::runtime_error("Cannot create archive compression pipe");
      setp(buffer.data(), buffer.data() + buffer.size());
    }

    ~PipeBuffer() {
      close_writer();
      if (reader)
        CloseHandle(reader);
    }

    void close_writer() {
      if (writer) {
        CloseHandle(writer);
        writer = nullptr;
      }
    }

  protected:
    int sync() override {
      const auto count = static_cast<DWORD>(pptr() - pbase());
      DWORD sent = 0;

      if (bytes > limit - count)
        throw std::runtime_error("Map exceeds the ZIP32 size limit");
      while (sent < count) {
        DWORD written = 0;

        if (!WriteFile(writer, pbase() + sent, count - sent, &written, nullptr) || written == 0)
          throw std::runtime_error("Archive compression failed while writing map data");
        sent += written;
      }
      bytes += count;
      setp(buffer.data(), buffer.data() + buffer.size());
      return 0;
    }

    int_type overflow(int_type ch) override {
      sync();
      if (!traits_type::eq_int_type(ch, traits_type::eof())) {
        *pptr() = traits_type::to_char_type(ch);
        pbump(1);
      }
      return traits_type::not_eof(ch);
    }
  };

  struct Archive::State {
    FILE* file = nullptr;
    mz_zip_archive zip{};
    PipeBuffer buffer;
    std::ostream stream;
    std::future<void> worker;
    bool zip64_enabled;

    State(const std::filesystem::path& path, bool zip64) : buffer(zip64), stream(&buffer), zip64_enabled(zip64) {
      stream.exceptions(std::ios::failbit | std::ios::badbit);
      if (_wfopen_s(&file, path.c_str(), L"w+b") != 0)
        throw std::runtime_error("Cannot open output archive");
      if (!mz_zip_writer_init_cfile(&zip, file, zip64 ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0)) {
        fclose(file);
        throw std::runtime_error("Cannot initialize output archive");
      }
      try {
        worker = std::async(std::launch::async, [this] {
          struct Reader {
            HANDLE pipe;
            bool failed = false;
          } reader{buffer.reader};
          const auto callback = [](void* opaque, mz_uint64, void* destination, size_t capacity) -> size_t {
            auto& input = *static_cast<Reader*>(opaque);
            DWORD count = 0;

            if (!ReadFile(input.pipe, destination, static_cast<DWORD>(capacity), &count, nullptr)) {
              if (GetLastError() != ERROR_BROKEN_PIPE)
                input.failed = true;
              return 0;
            }
            return count;
          };
          const bool success = mz_zip_writer_add_read_buf_callback(&zip, "map", callback, &reader,
            buffer.limit, nullptr, nullptr, 0, MZ_BEST_SPEED | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE,
            nullptr, 0, nullptr, 0) != 0;

          CloseHandle(buffer.reader);
          buffer.reader = nullptr;
          if (!success || reader.failed)
            throw std::runtime_error("Cannot compress map into RHM archive");
        });
      } catch (...) {
        mz_zip_writer_end(&zip);
        fclose(file);
        throw;
      }
    }

    ~State() {
      buffer.close_writer();
      if (worker.valid())
        worker.wait();
      mz_zip_writer_end(&zip);
      fclose(file);
    }
  };

  Archive::Archive(const std::filesystem::path& path, bool zip64) : state(std::make_unique<State>(path, zip64)) {}
  Archive::~Archive() = default;

  std::ostream& Archive::map() {
    return state->stream;
  }

  void Archive::finish_map() {
    state->stream.flush();
    state->buffer.close_writer();
    state->worker.get();
  }

  void Archive::add_audio(const std::filesystem::path& path) {
    FILE* input = nullptr;

    if (_wfopen_s(&input, path.c_str(), L"rb") != 0)
      throw std::runtime_error("Cannot open encoded audio");
    const auto size = std::filesystem::file_size(path);
    const bool success = mz_zip_writer_add_cfile(&state->zip, "audio", input, size, nullptr,
      nullptr, 0, MZ_BEST_SPEED | MZ_ZIP_FLAG_WRITE_HEADER_SET_SIZE, nullptr, 0, nullptr, 0) != 0;

    fclose(input);
    if (!success)
      throw std::runtime_error("Cannot add audio to RHM archive");
  }

  void Archive::finish() {
    if (!mz_zip_writer_finalize_archive(&state->zip) || fflush(state->file) != 0)
      throw std::runtime_error("Cannot finalize RHM archive");
    if (state->zip64_enabled)
      normalize_zip64(state->file, state->zip.m_central_directory_file_ofs, state->zip.m_total_files);
    state.reset();
  }
}
