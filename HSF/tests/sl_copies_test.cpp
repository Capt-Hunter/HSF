// Offline test of two sl.interposer.dll copies loaded at once, as in Gothic Remake with RenoDX DLSS (Oct 2026 build): a stray copy loads first and is never
// started, the game's own loads second. Uses fake_interposer.dll (next to this exe), copied into folders under <temp>. Two cases, each in its own process
// (HSF's state and the loaded copies cannot be undone in one):
//   started (default): the game started its copy. The route check asks every copy (AnyCopyStarted), not the first loaded, so HSF never writes SlBringIn=2
//     for it; HSF goes to the started copy by itself, takes its hooks off the stray one, and stays there through 6 s with no frames (a game's menus,
//     or its own frame generation off) instead of going round between the copies, as the first fix did in Gothic.
//   --unstarted: neither copy says it was started. HSF leaves the first copy after a few seconds with no frames for the next, once, and stays on the one
//     that gets frames.
//   sl_copies_test [log path]   (exit code 0 = all passed)
#include "../src/sl_game.h"

#include <windows.h>

#include <sl.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

bool Has(const std::string& log, const char* text) { return log.find(text) != std::string::npos; }

size_t Count(const std::string& log, const char* text) {
  size_t n = 0;
  for (size_t at = log.find(text); at != std::string::npos; at = log.find(text, at + 1)) ++n;
  return n;
}

std::string ReadLog(const wchar_t* path) {
  std::ifstream file(path, std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  return text.str();
}

using GetTokenFn = sl::Result (*)(sl::FrameToken*&, const uint32_t*);

// The first bytes of a copy's slSetConstants: HSF's hook starts it with a jump (0xE9).
const uint8_t* Constants(HMODULE module) { return reinterpret_cast<const uint8_t*>(GetProcAddress(module, "slSetConstants")); }

struct Copy {
  wchar_t folder[MAX_PATH] = L"";
  wchar_t path[MAX_PATH] = L"";
  HMODULE module = nullptr;
  uint8_t bytes[8] = {};
};

bool Load(const wchar_t* fake, const wchar_t* root, const wchar_t* name, Copy& c) {
  swprintf_s(c.folder, L"%s\\%s", root, name);
  CreateDirectoryW(c.folder, nullptr);
  swprintf_s(c.path, L"%s\\sl.interposer.dll", c.folder);
  if (!CopyFileW(fake, c.path, FALSE)) return false;
  c.module = LoadLibraryW(c.path);
  if (c.module == nullptr) return false;
  std::memcpy(c.bytes, Constants(c.module), 8);
  return true;
}

void Unload(Copy& c) {
  if (c.module != nullptr) FreeLibrary(c.module);
  DeleteFileW(c.path);
  RemoveDirectoryW(c.folder);
}

bool Hooked(const Copy& c) { return Constants(c.module)[0] == 0xE9; }
bool BytesBack(const Copy& c) { return std::memcmp(c.bytes, Constants(c.module), 8) == 0; }

// Polls HSF for ms, the game making a frame every 16 ms through `frames_through` (none if null).
void Run(ULONGLONG ms, const Copy* frames_through) {
  const auto token_fn = frames_through != nullptr ? reinterpret_cast<GetTokenFn>(GetProcAddress(frames_through->module, "slGetNewFrameToken")) : nullptr;
  const ULONGLONG start = GetTickCount64();
  while (GetTickCount64() - start < ms) {
    if (token_fn != nullptr) {
      sl::FrameToken* token = nullptr;
      token_fn(token, nullptr);
    }
    hsf::slgame::Poll();
    Sleep(16);
  }
}

void Started(const wchar_t* fake, const wchar_t* root, const wchar_t* log_path) {
  Copy stray, game;
  // The stray copy loads first (RenoDX's, from the game's exe folder), never started.
  Check(Load(fake, root, L"stray", stray), "stray copy loads");
  Check(!hsf::slgame::AnyCopyStarted(), "only the stray copy loaded: no copy started");
  Check(Load(fake, root, L"game", game) && game.module != stray.module, "game's copy loads as a second module");
  if (stray.module == nullptr || game.module == nullptr) return;
  Check(GetModuleHandleW(L"sl.interposer.dll") == stray.module, "the name finds the stray copy first (what the old route check asked)");
  Check(hsf::slgame::AnyCopyStarted(), "the game's copy counts as started: InGameDlssg, not SlBringIn=2");

  hsf::slgame::Start(log_path, false);
  Check(hsf::slgame::HooksInstalled() == 7, "all seven hooks in place on the first copy");
  Check(Hooked(stray), "the stray copy is hooked first, as in Gothic (no asking under the loader lock)");

  // The game's menus: no frames at all, for twice the move-on time.
  Run(6500, nullptr);
  Check(Hooked(game), "HSF went to the started copy without any frame");
  Check(BytesBack(stray), "the stray copy's bytes are back (no hooks left behind)");
  Check(hsf::slgame::HooksInstalled() == 7, "seven hooks, all on the game's copy");

  // Play: frames through the game's copy.
  Run(1000, &game);
  Check(hsf::slgame::GameFrames() > 0, "the game's frames are counted");
  Check(hsf::slgame::AllCopiesTried(), "the route pick may go on");
  hsf::slgame::Stop(false);
  Check(hsf::slgame::HooksInstalled() == 0, "no hooks after Stop");
  Check(BytesBack(game), "the game's copy's bytes are back after Stop");

  const std::string log = ReadLog(log_path);
  Check(Has(log, "sl.interposer.dll copy 1 of 2") && Has(log, "sl.interposer.dll copy 2 of 2"), "log: both copies named");
  Check(Count(log, "sl.interposer.dll changed:") == 1, "log: one change only, no going round in the menu");
  Check(!Has(log, "got no frame from the game"), "log: no move for lack of frames from a started copy");
  Unload(game);
  Unload(stray);
}

void Unstarted(const wchar_t* fake, const wchar_t* root, const wchar_t* log_path) {
  Copy first, second;
  Check(Load(fake, root, L"stray_first", first) && Load(fake, root, L"stray_second", second), "two copies, neither started, load");
  if (first.module == nullptr || second.module == nullptr) return;
  Check(!hsf::slgame::AnyCopyStarted(), "no copy started");
  hsf::slgame::Start(log_path, false);
  Check(Hooked(first), "the first copy is hooked first");
  Check(!hsf::slgame::AllCopiesTried(), "the route pick waits");
  // The game's frames go through the second copy only.
  const ULONGLONG start = GetTickCount64();
  ULONGLONG moved_at = 0;
  while (GetTickCount64() - start < 8000 && moved_at == 0) {
    Run(100, &second);
    if (Hooked(second)) moved_at = GetTickCount64() - start;
  }
  std::printf("  moved to the second copy after %llu ms\n", moved_at);
  Check(moved_at != 0 && moved_at < 6000, "HSF moved to the copy with frames within 6 s");
  Run(5000, &second);
  Check(Hooked(second) && BytesBack(first), "HSF stays on the copy with frames");
  Check(hsf::slgame::GameFrames() > 0 && hsf::slgame::AllCopiesTried(), "frames counted, the route pick may go on");
  hsf::slgame::Stop(false);
  Check(BytesBack(second), "bytes back after Stop");
  const std::string log = ReadLog(log_path);
  Check(Count(log, "got no frame from the game") == 1, "log: one move for lack of frames");
  Check(Count(log, "sl.interposer.dll changed:") == 1, "log: one change only");
  Unload(second);
  Unload(first);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const bool unstarted = argc >= 2 && wcscmp(argv[argc - 1], L"--unstarted") == 0;
  const wchar_t* const given_log = argc >= 2 && wcscmp(argv[1], L"--unstarted") != 0 ? argv[1] : nullptr;
  wchar_t exe[MAX_PATH] = L"";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  wchar_t fake[MAX_PATH];
  wcscpy_s(fake, exe);
  *(std::wcsrchr(fake, L'\\') + 1) = L'\0';
  wcscat_s(fake, L"fake_interposer.dll");
  wchar_t temp[MAX_PATH] = L"";
  GetTempPathW(MAX_PATH, temp);
  wchar_t root[MAX_PATH], log_path[MAX_PATH];
  swprintf_s(root, L"%shsf_sl_copies_%lu", temp, GetCurrentProcessId());
  CreateDirectoryW(root, nullptr);
  if (given_log != nullptr) swprintf_s(log_path, L"%s%s", given_log, unstarted ? L".unstarted" : L"");
  else swprintf_s(log_path, L"%s\\sl_copies.log", root);
  DeleteFileW(log_path);

  if (unstarted) {
    std::printf("case: neither copy started\n");
    Unstarted(fake, root, log_path);
  } else {
    std::printf("case: the game started its copy, a stray copy loaded first\n");
    Started(fake, root, log_path);
    // The other case in a process of its own.
    wchar_t command[MAX_PATH * 3];
    swprintf_s(command, L"\"%s\" %s%s%s --unstarted", exe, given_log != nullptr ? L"\"" : L"", given_log != nullptr ? given_log : L"", given_log != nullptr ? L"\"" : L"");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::fflush(stdout);
    DWORD code = 1;
    if (CreateProcessW(nullptr, command, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process)) {
      WaitForSingleObject(process.hProcess, 60000);
      GetExitCodeProcess(process.hProcess, &code);
      CloseHandle(process.hThread);
      CloseHandle(process.hProcess);
    }
    Check(code == 0, "case without a started copy passed");
  }
  if (given_log == nullptr) DeleteFileW(log_path);
  RemoveDirectoryW(root);
  std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
