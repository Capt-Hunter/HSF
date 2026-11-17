#include "safe_try.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>

namespace hsf::safetry {
namespace {

SRWLOCK g_lock = SRWLOCK_INIT;
wchar_t g_file[MAX_PATH] = L"";
wchar_t g_section[MAX_PATH] = L"";  // the game's exe name
LogFunction g_log = nullptr;
uint32_t g_pid = 0;
uint64_t g_test_now = 0;  // the test's clock for Begin, 0 = the real one
bool g_enabled = true;

struct Path {
  char name[32];
  bool read;      // the file was read for it
  bool allowed;
  bool marked;    // this launch marked it and the mark is still there
  uint64_t since;
};
Path g_paths[16];

struct Lock {
  Lock() { AcquireSRWLockExclusive(&g_lock); }
  ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

void Say(const char* format, ...) {
  if (g_log == nullptr) return;
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  g_log(text);
}

Path* Find(const char* name, bool add) {
  for (Path& p : g_paths)
    if (p.name[0] != 0 && std::strcmp(p.name, name) == 0) return &p;
  if (!add) return nullptr;
  for (Path& p : g_paths)
    if (p.name[0] == 0) {
      p = Path{};
      strncpy_s(p.name, name, _TRUNCATE);
      return &p;
    }
  return nullptr;
}

void Key(const char* name, const wchar_t* what, wchar_t* key, size_t size) { std::swprintf(key, size, L"%hs_%ls", name, what); }

bool Alive(uint32_t pid) {
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
  CloseHandle(process);
  return alive;
}

void ClearMark(Path& p) {
  wchar_t key[64];
  Key(p.name, L"trying", key, 64);
  WritePrivateProfileStringW(g_section, key, nullptr, g_file);
  p.marked = false;
}

void Setup(const wchar_t* file, LogFunction log, uint32_t pid) {
  Lock lock;
  wcsncpy_s(g_file, file != nullptr ? file : L"", _TRUNCATE);
  g_log = log;
  g_pid = pid;
  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const wchar_t* const slash = std::wcsrchr(exe, L'\\');
  wcsncpy_s(g_section, slash != nullptr ? slash + 1 : exe, _TRUNCATE);
  for (Path& p : g_paths) p = Path{};
}

}  // namespace

void Init(const wchar_t* file, LogFunction log) { Setup(file, log, GetCurrentProcessId()); }

void InitForTest(const wchar_t* file, LogFunction log, uint32_t process_id, uint64_t now_ms) {
  Setup(file, log, process_id);
  g_test_now = now_ms;
}

void SetEnabled(bool on) {
  Lock lock;
  g_enabled = on;
}

bool Enabled() {
  Lock lock;
  return g_enabled;
}

int ListOff(char (*names)[32], int max) {
  Lock lock;
  if (g_file[0] == 0 || max <= 0) return 0;
  static wchar_t s_section[4096];
  const DWORD length = GetPrivateProfileSectionW(g_section, s_section, static_cast<DWORD>(std::size(s_section)), g_file);
  int count = 0;
  for (const wchar_t* line = s_section; *line != 0 && line < s_section + length && count < max; line += std::wcslen(line) + 1) {
    const wchar_t* const equals = std::wcschr(line, L'=');
    if (equals == nullptr || equals - line < 5 || std::wcsncmp(equals - 4, L"_off", 4) != 0 || _wtoi(equals + 1) == 0) continue;
    const size_t name_length = static_cast<size_t>(equals - 4 - line);
    if (name_length >= 32) continue;
    for (size_t i = 0; i < name_length; ++i) names[count][i] = static_cast<char>(line[i]);
    names[count][name_length] = 0;
    ++count;
  }
  return count;
}

void TurnBackOn(const char* name) {
  Lock lock;
  if (g_file[0] == 0) return;
  wchar_t key[64];
  Key(name, L"off", key, 64);
  WritePrivateProfileStringW(g_section, key, nullptr, g_file);
  Key(name, L"trying", key, 64);
  WritePrivateProfileStringW(g_section, key, nullptr, g_file);
  Say("safe try: %s turned back on for this game from the panel", name);
}

void TurnOff(const char* name) {
  Lock lock;
  if (g_file[0] == 0) return;
  wchar_t key[64];
  Key(name, L"off", key, 64);
  WritePrivateProfileStringW(g_section, key, L"2", g_file);
  Say("safe try: %s turned off for this game from the panel", name);
}

int State(const char* name) {
  Lock lock;
  if (g_file[0] == 0) return ON;
  wchar_t key[64];
  Key(name, L"off", key, 64);
  const UINT value = GetPrivateProfileIntW(g_section, key, 0, g_file);
  return value == 0 ? ON : value == 2 ? OFF_BY_PLAYER : OFF_AFTER_CRASH;
}

namespace {
const Step STEPS[] = {
    {"T2_ngx_game_streamline", "NGX MOTION",
     "Games with their own Streamline (the in-game route).\n\n"
     "ON: HSF also reads the game's DLSS upscaler calls (NGX) and takes the motion vectors,\n"
     "depth and jitter from there. DLSS-G then gets the game's real motion even when\n"
     "the game does not hand it to frame generation itself.\n\n"
     "OFF: HSF uses only what the game sends its Streamline. Games that send their own\n"
     "motion lose nothing; others fall back to HSF's flow.\n\n"
     "Applies after a game restart."},
    {"H1_plugin", "PLUGIN HOOK",
     "Games with their own Streamline.\n\n"
     "ON: HSF takes DLSS-G's settings call where Streamline asks DLSS-G's plugin\n"
     "(sl.dlss_g.dll, or NVIDIA's copy from the driver) for it. DLSS-G's own code is\n"
     "never changed, so mods that patch it (MFG-Unlocker and others) keep working.\n\n"
     "OFF: HSF patches DLSS-G's settings call directly, the older way. This can clash\n"
     "with a mod that patches the same place.\n\n"
     "Applies after a game restart."},
    {"H2_load_notify", "LOAD WATCH",
     "All routes that use Streamline.\n\n"
     "ON: Windows tells HSF the moment a Streamline file loads, so HSF hooks it before\n"
     "the game makes its first call. Needed by games that start Streamline early.\n\n"
     "OFF: HSF looks for Streamline once a second instead. It still finds it, but a\n"
     "game may make its first calls before HSF is in.\n\n"
     "Applies after a game restart."},
    {"H3_decoder", "HOOK DECODER",
     "All routes.\n\n"
     "ON: when HSF meets a function start it does not know, a full instruction decoder\n"
     "works out how many bytes to move, so the hook can still go in.\n\n"
     "OFF: HSF hooks only function starts on its own short list. Unknown ones are left\n"
     "alone, and the feature that needed that hook may not work in this game.\n\n"
     "Applies after a game restart."},
};
}  // namespace

int Steps(const Step** out) {
  *out = STEPS;
  return static_cast<int>(std::size(STEPS));
}

const char* Describe(const char* name) {
  for (const Step& s : STEPS)
    if (std::strcmp(s.path, name) == 0) return s.label;
  return name;
}

bool Allowed(const char* name) {
  Lock lock;
  if (g_file[0] == 0 || !g_enabled) return true;
  Path* const p = Find(name, true);
  if (p == nullptr) return true;
  if (p->read) return p->allowed;
  p->read = true;
  wchar_t key[64];
  Key(name, L"off", key, 64);
  p->allowed = GetPrivateProfileIntW(g_section, key, 0, g_file) == 0;
  // A mark left by a launch that is gone: it ended while trying this way.
  Key(name, L"trying", key, 64);
  const uint32_t pid = GetPrivateProfileIntW(g_section, key, 0, g_file);
  if (pid != 0 && pid != g_pid && !Alive(pid)) {
    WritePrivateProfileStringW(g_section, key, nullptr, g_file);
    if (p->allowed) {
      p->allowed = false;
      Key(name, L"off", key, 64);
      WritePrivateProfileStringW(g_section, key, L"1", g_file);
      Say("safe try: the last launch of %ls ended while trying %s, so %s stays off for this game from now on (the old way is used)", g_section, name, name);
    }
  }
  if (!p->allowed) {
    Key(name, L"off", key, 64);
    if (GetPrivateProfileIntW(g_section, key, 0, g_file) == 2) Say("safe try: %s is off for this game (turned off in the panel): the old way is used", name);
    else Say("safe try: %s is off for this game (an earlier launch ended while trying it): the old way is used", name);
  }
  return p->allowed;
}

void Begin(const char* name) {
  Lock lock;
  if (g_file[0] == 0 || !g_enabled) return;
  Path* const p = Find(name, true);
  if (p == nullptr || p->marked || p->since != 0) return;
  wchar_t key[64], value[16];
  Key(name, L"trying", key, 64);
  std::swprintf(value, 16, L"%u", g_pid);
  WritePrivateProfileStringW(g_section, key, value, g_file);
  p->marked = true;
  p->since = g_test_now != 0 ? g_test_now : GetTickCount64();
}

void Tick(uint64_t now_ms) {
  if (now_ms == 0) now_ms = GetTickCount64();
  // Cheap check first: no lock unless a mark is due.
  static uint64_t s_next = 0;
  if (now_ms < s_next && g_test_now == 0) return;
  s_next = now_ms + 1000;
  Lock lock;
  for (Path& p : g_paths)
    if (p.marked && now_ms - p.since >= STABLE_MS) {
      ClearMark(p);
      Say("safe try: %s ran %llu s without trouble", p.name, static_cast<unsigned long long>(STABLE_MS / 1000));
    }
}

void ClearAll() {
  Lock lock;
  for (Path& p : g_paths)
    if (p.marked) ClearMark(p);
}

}  // namespace hsf::safetry
