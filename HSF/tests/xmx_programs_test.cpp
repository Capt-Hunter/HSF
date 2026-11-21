// Offline check of where XeMFG's 54 programs come from (src/xmx_model.cpp), no game needed: build\x64\xmx_programs_test.exe <file>...
// Each file (renodx-mfgunlock.addon64, MFGUnlocker.dll) is read exactly as FindPrograms reads it; PASS when all 54 programs are found.
// The mods are the user's own files: none is in the repo.
#include "../src/xmx_model.cpp"

#include <cstdio>

using namespace hsf::xmx::model;

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) {
    std::printf("usage: xmx_programs_test <mod file>...\n");
    return 2;
  }
  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    Bytes file;
    std::string why;
    Programs programs;
    const DWORD start = GetTickCount();
    try {
      if (!ReadFileBytes(argv[i], file)) why = "cannot read the file";
      else programs = ProgramsFrom(file, why);
    } catch (const std::exception& e) {
      why = e.what();
    }
    const bool ok = programs.size() == kProgramCount;
    std::printf("%s %ls: %zu programs, %lu ms%s%s\n", ok ? "PASS" : "FAIL", argv[i], programs.size(), GetTickCount() - start, why.empty() ? "" : ", ",
                why.c_str());
    failures += ok ? 0 : 1;
  }
  return failures == 0 ? 0 : 1;
}
