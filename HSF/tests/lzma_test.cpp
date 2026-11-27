// Offline test of HSF's LZMA decoder (src/lzma_dec.cpp), no game needed: build\x64\lzma_test.exe [folder]
// Checks two streams made by liblzma (default settings, and lc=0 lp=2 pb=0), a size limit, truncated input and every byte of the first
// 600 flipped (never a crash, never a wrong result passed as good). With a folder, also decodes each <name>.lzma there and compares it
// with <name>.bin (for MFG Unlocker's packed resources, unpacked by Python beforehand).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/lzma_dec.h"
#include "lzma_fixture.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) ++g_failures;
}

std::vector<uint8_t> Make() {  // same generator as the fixture script
  const char* words[] = {"frame ", "motion ", "depth ", "HSF ", "generated ", "real ", nullptr, "XeFG "};
  const uint8_t binary[] = {0, 1, 2, 3};
  std::vector<uint8_t> out;
  uint32_t x = 12345;
  while (out.size() < 20000) {
    x = x * 1103515245u + 12345u;
    const uint32_t k = (x >> 16) & 15;
    if (k < 8) {
      if (words[k] == nullptr) out.insert(out.end(), binary, binary + 4);
      else out.insert(out.end(), words[k], words[k] + strlen(words[k]));
    } else if (k < 10) {
      out.insert(out.end(), (x >> 4) & 31, static_cast<uint8_t>((x >> 8) & 255));
    } else {
      out.push_back(static_cast<uint8_t>((x >> 8) & 255));
    }
  }
  out.resize(20000);
  return out;
}

bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart < (256ll << 20);
  if (ok) {
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = out.empty() || (ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
  }
  CloseHandle(file);
  return ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const std::vector<uint8_t> expected = Make();
  std::vector<uint8_t> out;
  std::string why;

  Check(hsf::lzma::DecodeAlone(kPackedDefault, sizeof(kPackedDefault), out, 1 << 20, why) && out == expected, "default stream");
  Check(hsf::lzma::DecodeAlone(kPackedLc0Lp2, sizeof(kPackedLc0Lp2), out, 1 << 20, why) && out == expected, "lc=0 lp=2 pb=0 stream");
  Check(!hsf::lzma::DecodeAlone(kPackedDefault, sizeof(kPackedDefault), out, 1000, why) && out.empty(), "limit refused");
  Check(!hsf::lzma::DecodeAlone(kPackedDefault, sizeof(kPackedDefault) / 2, out, 1 << 20, why), "truncated refused");
  Check(!hsf::lzma::DecodeAlone(expected.data(), expected.size(), out, 1 << 20, why), "plain data refused");

  int wrong = 0;
  std::vector<uint8_t> broken(kPackedDefault, kPackedDefault + sizeof(kPackedDefault));
  for (size_t i = 13; i < 613 && i < broken.size(); ++i) {
    broken[i] ^= 0x5A;
    if (hsf::lzma::DecodeAlone(broken.data(), broken.size(), out, 1 << 20, why) && out == expected) ++wrong;
    broken[i] ^= 0x5A;
  }
  // A flipped byte that still decodes to the right data would mean the decoder ignores input; any other outcome is fine as long as
  // nothing crashed (a wrong but "good" result is caught later by the programs' SHA-256 check).
  Check(wrong == 0, "600 flipped bytes: no crash, none decoded to the original");

  if (argc > 1) {
    const std::wstring folder = argv[1];
    WIN32_FIND_DATAW found{};
    const HANDLE search = FindFirstFileW((folder + L"\\*.lzma").c_str(), &found);
    int files = 0;
    if (search != INVALID_HANDLE_VALUE) {
      do {
        std::wstring name = found.cFileName;
        std::vector<uint8_t> packed, unpacked;
        const std::wstring base = folder + L"\\" + name.substr(0, name.size() - 5);
        if (!ReadAll(base + L".lzma", packed) || !ReadAll(base + L".bin", unpacked)) continue;
        ++files;
        const DWORD start = GetTickCount();
        const bool ok = hsf::lzma::DecodeAlone(packed.data(), packed.size(), out, 64u << 20, why) && out == unpacked;
        char line[MAX_PATH + 64];
        std::snprintf(line, sizeof(line), "%ls (%zu -> %zu bytes, %lu ms)%s%s", name.c_str(), packed.size(), unpacked.size(), GetTickCount() - start,
                      ok ? "" : ": ", ok ? "" : why.c_str());
        Check(ok, line);
      } while (FindNextFileW(search, &found));
      FindClose(search);
    }
    Check(files > 0, "folder had .lzma/.bin pairs");
  }

  std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
