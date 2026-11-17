// HSF.log, see hsf_log.h.
#include "hsf_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <iterator>

#include "version.h"

namespace hsf::hsflog {
namespace {

// "<pid>|<path>": set by the first part of HSF that opens HSF.log in a process. Child processes (the helper) inherit it.
constexpr const wchar_t* MARK = L"HSF_LOG";

SRWLOCK g_lock = SRWLOCK_INIT;  // exclusive for Open and Close, shared for Write
HANDLE g_file = INVALID_HANDLE_VALUE;
wchar_t g_path[MAX_PATH] = L"";
volatile LONG64 g_bytes = 0;
volatile LONG g_full = 0;

#ifdef _WIN64
constexpr const char* BITS = "64-bit";
#else
constexpr const char* BITS = "32-bit";
#endif

// The mark's pid and path, or false when there is none.
bool ReadMark(DWORD& pid, wchar_t* path, size_t size) {
  wchar_t mark[MAX_PATH + 16] = L"";
  const DWORD length = GetEnvironmentVariableW(MARK, mark, static_cast<DWORD>(std::size(mark)));
  if (length == 0 || length >= std::size(mark)) return false;
  const wchar_t* bar = std::wcschr(mark, L'|');
  if (bar == nullptr || bar[1] == L'\0') return false;
  pid = std::wcstoul(mark, nullptr, 10);
  wcsncpy_s(path, size, bar + 1, _TRUNCATE);
  return true;
}

void WriteRaw(const std::string& line) {
  DWORD written = 0;
  WriteFile(g_file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
}

}  // namespace

std::wstring PreviousPath(const wchar_t* path) {
  std::wstring previous = path;
  const size_t slash = previous.find_last_of(L"\\/");
  previous.resize(slash == std::wstring::npos ? 0 : slash + 1);
  return previous + L"HSF.previous.log";
}

bool OpenElsewhere(const wchar_t* path) {
  // Every writer opens HSF.log for appending. An open that allows only other readers fails while any writer has it.
  const HANDLE probe = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (probe != INVALID_HANDLE_VALUE) {
    CloseHandle(probe);
    return false;
  }
  return GetLastError() == ERROR_SHARING_VIOLATION;
}

std::string FormatLine(const SYSTEMTIME& time, const char* part, const char* text) {
  size_t length = std::strlen(text);
  while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r')) --length;
  char head[48];
  std::snprintf(head, sizeof(head), "%02u:%02u:%02u.%03u [%s] ", time.wHour, time.wMinute, time.wSecond, time.wMilliseconds, part);
  std::string line = head;
  line.append(text, length);
  line += "\r\n";
  return line;
}

const wchar_t* PathFromEnvironment(bool this_process_only) {
  static wchar_t path[MAX_PATH] = L"";
  DWORD pid = 0;
  if (!ReadMark(pid, path, MAX_PATH)) return nullptr;
  if (this_process_only && pid != GetCurrentProcessId()) return nullptr;
  return path;
}

const wchar_t* Path() { return g_path; }

void Open(const wchar_t* path) {
  if (path == nullptr || path[0] == L'\0') return;
  AcquireSRWLockExclusive(&g_lock);
  if (g_file != INVALID_HANDLE_VALUE) {
    ReleaseSRWLockExclusive(&g_lock);
    return;
  }
  DWORD mark_pid = 0;
  wchar_t mark_path[MAX_PATH] = L"";
  const bool have_mark = ReadMark(mark_pid, mark_path, MAX_PATH);
  const bool again = have_mark && mark_pid == GetCurrentProcessId();
  // Fresh only for the first part in this process, and only when no other process is writing the file: a launcher's or a crash
  // reporter's start must not wipe a game that is still running.
  const bool fresh = !again && !OpenElsewhere(path);
  if (fresh) MoveFileExW(path, PreviousPath(path).c_str(), MOVEFILE_REPLACE_EXISTING);
  if (!again) {
    wchar_t mark[MAX_PATH + 16];
    std::swprintf(mark, std::size(mark), L"%lu|%ls", GetCurrentProcessId(), path);
    SetEnvironmentVariableW(MARK, mark);
  }
  g_file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) {
    ReleaseSRWLockExclusive(&g_lock);
    return;
  }
  wcsncpy_s(g_path, path, _TRUNCATE);
  InterlockedExchange64(&g_bytes, 0);
  InterlockedExchange(&g_full, 0);
  wchar_t exe[MAX_PATH] = L"?";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  SYSTEMTIME now;
  GetLocalTime(&now);
  char text[MAX_PATH * 3 + 200];
  if (fresh)
    std::snprintf(text, sizeof(text), "HSF %s (%s): HSF.log started %04u-%02u-%02u by %ls (pid %lu). The last run is in HSF.previous.log.",
                  HSF_VERSION, BITS, now.wYear, now.wMonth, now.wDay, exe, GetCurrentProcessId());
  else if (again)
    std::snprintf(text, sizeof(text), "HSF %s (%s): loaded again in the same game process (pid %lu), so the log goes on", HSF_VERSION, BITS,
                  GetCurrentProcessId());
  else
    std::snprintf(text, sizeof(text), "HSF %s (%s): %ls (pid %lu) joins HSF.log, which another process has open", HSF_VERSION, BITS, exe,
                  GetCurrentProcessId());
  WriteRaw(FormatLine(now, HSF, text));
  ReleaseSRWLockExclusive(&g_lock);
}

void Write(const char* part, const char* text) {
  if (text == nullptr) return;
  SYSTEMTIME now;
  GetLocalTime(&now);
  const std::string line = FormatLine(now, part, text);
  AcquireSRWLockShared(&g_lock);
  if (g_file != INVALID_HANDLE_VALUE && g_full == 0) {
    if (static_cast<unsigned long long>(InterlockedExchangeAdd64(&g_bytes, static_cast<LONG64>(line.size()))) + line.size() <= MAX_BYTES)
      WriteRaw(line);
    else if (InterlockedExchange(&g_full, 1) == 0)
      WriteRaw(FormatLine(now, part, "HSF.log is full (64 MB from this part of HSF): its later lines are dropped"));
  }
  ReleaseSRWLockShared(&g_lock);
}

void Writef(const char* part, const char* format, ...) {
  char text[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  Write(part, text);
}

void Close() {
  AcquireSRWLockExclusive(&g_lock);
  if (g_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
  ReleaseSRWLockExclusive(&g_lock);
}

}  // namespace hsf::hsflog
