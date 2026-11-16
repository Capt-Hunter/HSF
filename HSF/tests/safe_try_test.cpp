// Offline test of src/safe_try.cpp: a mark cleared after a stable run, a mark left by a launch that died turns its way off for good, and a normal exit clears the marks.
// safe_try_test   (exit code 0 = all passed)
#include "../src/safe_try.h"

#include <windows.h>

#include <cstdio>
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
  Check(st::Allowed("C"), "launch 3: C allowed after a normal exit");

  // A mark from a process still running (a parent that started this one) is not a crash.
  st::InitForTest(file.c_str(), nullptr, GetCurrentProcessId(), 1000);
  st::Begin("D");
  st::InitForTest(file.c_str(), nullptr, GONE_PID, 1000);
  Check(st::Allowed("D"), "a mark from a running process does not turn a way off");

  DeleteFileW(file.c_str());
  std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
