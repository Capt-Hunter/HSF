// Offline test of src/hsf_log.cpp (HSF.log): a game run starts the file fresh and keeps the last run as HSF.previous.log. A reload in the same
// process appends. A second process (the helper, or a game started by a launcher) appends while another one still has the file open.
// Lines carry the part's tag, and many threads never mix inside a line.
// hsf_log_test   (exit code 0 = all passed)
#include "../src/hsf_log.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

std::string ReadAll(const std::wstring& path) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  std::string text(static_cast<size_t>(size.QuadPart), '\0');
  DWORD got = 0;
  if (!text.empty()) ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
  CloseHandle(file);
  text.resize(got);
  return text;
}

bool Has(const std::string& text, const char* part) { return text.find(part) != std::string::npos; }

// Pretends the first part of HSF in this process was another process (a launcher run, or the game that started the helper).
void MarkOtherProcess(const std::wstring& path) {
  SetEnvironmentVariableW(L"HSF_LOG", (L"4294967280|" + path).c_str());
}

}  // namespace

int main() {
  namespace hl = hsf::hsflog;
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const std::wstring folder = std::wstring(temp) + L"hsf_log_test";
  CreateDirectoryW(folder.c_str(), nullptr);
  const std::wstring path = folder + L"\\HSF.log";
  const std::wstring previous = folder + L"\\HSF.previous.log";
  DeleteFileW(path.c_str());
  DeleteFileW(previous.c_str());
  SetEnvironmentVariableW(L"HSF_LOG", nullptr);

  // The line format.
  SYSTEMTIME t{};
  t.wHour = 7;
  t.wMinute = 5;
  t.wSecond = 3;
  t.wMilliseconds = 42;
  Check(hl::FormatLine(t, hl::STREAMLINE, "plugin loaded\n") == "07:05:03.042 [STREAMLINE] plugin loaded\r\n", "line: time, part, text, one CRLF");
  Check(hl::PreviousPath(path.c_str()) == previous, "previous log sits next to HSF.log");

  // Run 1 of a game, then the same game again: run 1 becomes HSF.previous.log.
  hl::Open(path.c_str());
  hl::Write(hl::HSF, "run one");
  hl::Close();
  Check(Has(ReadAll(path), "HSF.log started") && Has(ReadAll(path), "[HSF] run one"), "run 1: fresh file with a start line and its lines");
  Check(hl::PathFromEnvironment(true) != nullptr && std::wstring(hl::PathFromEnvironment(true)) == path, "run 1: marks the process with the path");

  // ReShade loads the add-on again in the same process: append, nothing moves.
  hl::Open(path.c_str());
  hl::Write(hl::HSF, "after reload");
  hl::Close();
  {
    const std::string text = ReadAll(path);
    Check(Has(text, "run one") && Has(text, "after reload") && Has(text, "same game process"), "reload in the same process appends");
  }
  Check(GetFileAttributesW(previous.c_str()) == INVALID_FILE_ATTRIBUTES, "reload: no previous log made");

  // Next game run (another process, and nobody has the file open): run 1 moves to HSF.previous.log.
  MarkOtherProcess(path);
  hl::Open(path.c_str());
  hl::Write(hl::HSF, "run two");
  {
    const std::string now = ReadAll(path), before = ReadAll(previous);
    Check(Has(now, "run two") && !Has(now, "run one"), "next run: fresh HSF.log");
    Check(Has(before, "run one") && Has(before, "after reload"), "next run: the last run kept in HSF.previous.log");
  }

  // The helper of run two: the game (this module's handle) has the file open, so the helper appends. Played by a second handle and a
  // fresh-looking mark from another pid.
  {
    const HANDLE game = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, 0, nullptr);
    hl::Close();
    Check(hl::OpenElsewhere(path.c_str()), "a writer in another process is seen");
    MarkOtherProcess(path);
    hl::Open(path.c_str());
    hl::Write(hl::HELPER, "helper line");
    const std::string now = ReadAll(path);
    Check(Has(now, "run two") && Has(now, "joins HSF.log") && Has(now, "[HELPER] helper line"), "helper appends while the game has it open");
    Check(!Has(ReadAll(previous), "run two"), "helper: nothing moved to HSF.previous.log");
    CloseHandle(game);
  }

  // Many threads: every line whole.
  {
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
      threads.emplace_back([i] {
        char text[200];
        for (int n = 0; n < 200; ++n) {
          std::snprintf(text, sizeof(text), "thread %d line %d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx end", i, n);
          hl::Write(hl::VULKAN, text);
        }
      });
    for (std::thread& th : threads) th.join();
    const std::string text = ReadAll(path);
    int whole = 0, broken = 0;
    size_t from = 0;
    while (from < text.size()) {
      size_t end = text.find("\r\n", from);
      if (end == std::string::npos) end = text.size();
      const std::string line = text.substr(from, end - from);
      if (Has(line, "[VULKAN]")) (line.size() > 4 && line.compare(line.size() - 4, 4, " end") == 0 && line.find("[VULKAN]", 14) == std::string::npos ? whole : broken)++;
      from = end + 2;
    }
    Check(whole == 1600 && broken == 0, "8 threads x 200 lines: all whole");
  }
  hl::Close();

  // Nothing written before Open or after Close.
  hl::Write(hl::HSF, "lost line");
  Check(!Has(ReadAll(path), "lost line"), "no line after Close");

  DeleteFileW(path.c_str());
  DeleteFileW(previous.c_str());
  RemoveDirectoryW(folder.c_str());
  std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
