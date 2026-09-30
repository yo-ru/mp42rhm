#include "process.h"

#include <algorithm>
#include <stdexcept>

namespace mp42rhm {
  static std::wstring quote_argument(const std::wstring& argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;

    for (wchar_t ch : argument) {
      if (ch == L'\\') {
        ++slashes;
        continue;
      }
      result.append(slashes * (ch == L'"' ? 2 : 1), L'\\');
      if (ch == L'"')
        result += L'\\';
      result += ch;
      slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
  }

  Process::Process(const std::vector<std::wstring>& arguments) {
    std::wstring command;

    for (const auto& argument : arguments)
      command += quote_argument(argument) + L" ";

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE write_pipe = nullptr;

    if (!CreatePipe(&output, &write_pipe, &security, 0))
      throw std::runtime_error("Cannot create media process pipe");
    if (!SetHandleInformation(output, HANDLE_FLAG_INHERIT, 0)) {
      CloseHandle(output);
      CloseHandle(write_pipe);
      throw std::runtime_error("Cannot configure media process pipe");
    }

    HANDLE null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
      &security, OPEN_EXISTING, 0, nullptr);
    HANDLE error_output = nullptr;
    HANDLE current = GetCurrentProcess();

    if (!DuplicateHandle(current, GetStdHandle(STD_ERROR_HANDLE), current,
      &error_output, 0, TRUE, DUPLICATE_SAME_ACCESS))
      error_output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr);

    STARTUPINFOW startup{};
    PROCESS_INFORMATION info{};

    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_input;
    startup.hStdOutput = write_pipe;
    startup.hStdError = error_output;

    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr,
      TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info);
    const DWORD error = GetLastError();

    CloseHandle(write_pipe);
    CloseHandle(null_input);
    CloseHandle(error_output);
    if (!started) {
      CloseHandle(output);
      throw std::runtime_error("Cannot start FFmpeg/ffprobe (Win32 error " +
        std::to_string(error) + "). Install them on PATH or use --ffmpeg/--ffprobe.");
    }
    CloseHandle(info.hThread);
    process = info.hProcess;
  }

  Process::~Process() {
    if (process) {
      TerminateProcess(process, 1);
      WaitForSingleObject(process, INFINITE);
      CloseHandle(process);
    }
    if (output)
      CloseHandle(output);
  }

  size_t Process::read(void* buffer, size_t size) {
    DWORD count = 0;

    if (!ReadFile(output, buffer, static_cast<DWORD>(std::min<size_t>(size, MAXDWORD)), &count, nullptr)) {
      if (GetLastError() == ERROR_BROKEN_PIPE)
        return 0;
      throw std::runtime_error("Cannot read media process output");
    }
    return count;
  }

  std::string Process::finish() {
    std::string result;
    char buffer[4096];
    size_t count = 0;

    while ((count = read(buffer, sizeof(buffer))) != 0) {
      if (result.size() + count > 1024 * 1024)
        throw std::runtime_error("Unexpectedly large media process response");
      result.append(buffer, count);
    }
    WaitForSingleObject(process, INFINITE);

    DWORD exit_code = 1;

    GetExitCodeProcess(process, &exit_code);
    CloseHandle(process);
    process = nullptr;
    if (exit_code != 0)
      throw std::runtime_error("FFmpeg/ffprobe failed; see its diagnostic above");
    return result;
  }
}
