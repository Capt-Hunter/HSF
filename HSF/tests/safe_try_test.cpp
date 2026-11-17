// Offline test of src/safe_try.cpp: a mark cleared after a stable run, a mark left by a launch that died turns its way off for good, and a normal exit clears the marks.
// safe_try_test   (exit code 0 = all passed)
#include "../src/safe_try.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

constexpr uint32_t GONE_PID = 0x7FFFFFF0;  // no such process

}  // namespace

int main() {
  wchar_t temp[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  const std::wstring file = std::wstring(temp) + L"hsf_safe_try_test.ini";
  DeleteFileW(file.c_str());
  namespace st = hsf::safetry;

  // Launch 1 (a process that will be gone): A runs long enough, B is in use when the launch dies, C is never begun.
  st::InitForTest(file.c_str(), nullptr, GONE_PID, 1000);
  Check(st::Allowed("A") && st::Allowed("B") && st::Allowed("C"), "launch 1: every way allowed at first");
  st::Begin("A");
  st::Tick(1000 + st::STABLE_MS);
  st::Begin("B");

  // Launch 2 (this process).
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  Check(st::Allowed("A"), "launch 2: A ran long enough, still allowed");
  Check(!st::Allowed("B"), "launch 2: B was in use when launch 1 died, now off");
  Check(st::Allowed("C"), "launch 2: C was never tried, allowed");
  st::Begin("C");
  st::ClearAll();

  // Launch 3: B stays off, C's mark went with the normal exit.
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  Check(!st::Allowed("B"), "launch 3: B stays off");
  Check(st::State("B") == st::OFF_AFTER_CRASH, "switch: B shows OFF after a crash");
  Check(st::Allowed("C"), "launch 3: C allowed after a normal exit");

  // A mark from a process still running (a parent that started this one) is not a crash.
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  st::Begin("D");
  st::InitForTest(file.c_str(), nullptr, GONE_PID, 1000);
  Check(st::Allowed("D"), "a mark from a running process does not turn a way off");

  // The panel's list and its TURN BACK ON button.
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  char names[8][32];
  const int count = st::ListOff(names, 8);
  Check(count == 1 && std::strcmp(names[0], "B") == 0, "list: only B is off");
  st::TurnBackOn("B");
  Check(st::ListOff(names, 8) == 0, "list: empty after B is turned back on");
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  Check(st::Allowed("B"), "next launch: B allowed again");
  Check(std::strcmp(st::Describe("T2_ngx_game_streamline"), "NGX MOTION") == 0 && std::strcmp(st::Describe("Z"), "Z") == 0, "plain names");
  const st::Step* steps = nullptr;
  const int step_count = st::Steps(&steps);
  bool steps_ok = step_count == 4;
  for (int i = 0; i < step_count; ++i) steps_ok = steps_ok && steps[i].tip != nullptr && steps[i].tip[0] != 0;
  Check(steps_ok, "every step has a name and an explanation");

  // The panel's ON / OFF switch: a crash shows as OFF_AFTER_CRASH (B above), the player's OFF as OFF_BY_PLAYER, and either keeps the way off.
  Check(st::State("B") == st::ON, "switch: B on after TURN BACK ON");
  st::TurnOff("G");
  Check(st::State("G") == st::OFF_BY_PLAYER, "switch: G off by the player");
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  Check(!st::Allowed("G"), "next launch: G stays off");
  st::TurnBackOn("G");
  Check(st::State("G") == st::ON, "switch: G back on");

  // SafeTry=0: a way left marked by a dead launch is not turned off, and nothing is marked.
  st::InitForTest(file.c_str(), nullptr, GONE_PID, 1000);
  st::Begin("E");
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  st::SetEnabled(false);
  Check(st::Allowed("E"), "safe try off: E allowed although its launch died");
  st::Begin("F");
  st::SetEnabled(true);
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  Check(st::Allowed("F"), "safe try off: F was never marked");
  Check(!st::Allowed("E"), "safe try back on: E's old mark now turns it off");

  DeleteFileW(file.c_str());
  std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
