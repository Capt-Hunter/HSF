// See xmx_model.h. Ported from MFGAdaUnlock-RenoDx by ImDreamt (MIT), commit 8b854ff, which took these parts from dashdogy's MFG Unlocker
// (MIT): the program decoder relay (port/shader_port.h), the model catalog edits (port/model_port.h), the frame-count sites
// (host_unlock.cpp) and the program list (runtime_payload.cpp, tools/prepare_xemfg_inputs.py). Every address below is the reference
// address of libxess_fg.dll 1.3.1.78 (their "fg78" family) and libxell.dll 1.3.2.10; each is checked byte for byte before anything is
// written, and any other bytes leave XeFG at its own 2x.
#include "xmx_model.h"

#include <bcrypt.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "lzma_dec.h"

#pragma comment(lib, "bcrypt.lib")

namespace hsf::xmx::model {
namespace {

using Bytes = std::vector<unsigned char>;
using Programs = std::map<std::string, Bytes>;

// ---- The 54 programs (runtime_payload.cpp, prepare_xemfg_inputs.py) ----
// The program list: decoder key and SHA-256 of each program, from the 54-row manifest MFGAdaUnlock embeds (its SHA-256 d74bd783...).
// 30 distinct programs: some keys share one. Hashes only; the programs themselves come from the user's own mod (FindPrograms).
struct ProgramRow {
  const char* key;
  const char* sha256;
};
constexpr ProgramRow kProgramTable[] = {
    {"34510306390392534204181", "bc9d3fff9f957f756ca6776b50ed96b87314d7ae96213e35011284250ece3f34"},
    {"34510549607087156077416", "1bebcab193714ee264da7dc0a96e313d74ad4c71017fdea5314c8b07f6b56394"},
    {"34510630495729375117249", "6acf82efd2a0c95c342a154f1f13cc8dc8450bcef6044175b3c91b17e21bedc0"},
    {"34511130729005347619443", "8c4a23058529dd0f5a7aab90c42d9d9e50cb0d13d261031d783b11b3f81562bb"},
    {"34511160110267935226759", "0142035fb0845ff49f5c20fe6c5ff533efc68e30b6ceedd6783da8a2a4cb95b7"},
    {"34511460816689149022550", "2f31a5964c891994d8d5c5cfcf2b9fac5f73a78e2549b17944190533c6e95c88"},
    {"34511775204213202565591", "3577b7537200e869aff122f06d1ff9310f07b67b351931b685451de616c6496d"},
    {"34513405466145798375150", "1bebcab193714ee264da7dc0a96e313d74ad4c71017fdea5314c8b07f6b56394"},
    {"34513793361990215758205", "3577b7537200e869aff122f06d1ff9310f07b67b351931b685451de616c6496d"},
    {"34515160591401652990146", "8c4a23058529dd0f5a7aab90c42d9d9e50cb0d13d261031d783b11b3f81562bb"},
    {"34515410463317489640932", "f4aa535a6fc74410ae8b684eb881b9fd511c6a1ac1c784cc95bd8f7fd8ea0f61"},
    {"34515657724417237518993", "f4aa535a6fc74410ae8b684eb881b9fd511c6a1ac1c784cc95bd8f7fd8ea0f61"},
    {"34516380930220257970286", "f4aa535a6fc74410ae8b684eb881b9fd511c6a1ac1c784cc95bd8f7fd8ea0f61"},
    {"34517260297374432353812", "8c4a23058529dd0f5a7aab90c42d9d9e50cb0d13d261031d783b11b3f81562bb"},
    {"34517625535393435596474", "6acf82efd2a0c95c342a154f1f13cc8dc8450bcef6044175b3c91b17e21bedc0"},
    {"34517804162798471891092", "c8b778ce1332e0f4f0c31b6985a4f77f88d01305602e932f78dac0157a7de840"},
    {"34517926239615626135843", "1bebcab193714ee264da7dc0a96e313d74ad4c71017fdea5314c8b07f6b56394"},
    {"34518157918880769121294", "3577b7537200e869aff122f06d1ff9310f07b67b351931b685451de616c6496d"},
    {"3452159554838854843181", "51667b6d9955036a10cbd4e363881252de8ca5d3b6f210a7a3ee6cab719629a6"},
    {"3452703420262894380461", "13abff5d769ef56f99cb9ee7825968872e28395d77a6d344053c06cb08b8064c"},
    {"3452733261465573612279", "f4aa535a6fc74410ae8b684eb881b9fd511c6a1ac1c784cc95bd8f7fd8ea0f61"},
    {"3453044563380101633987", "13abff5d769ef56f99cb9ee7825968872e28395d77a6d344053c06cb08b8064c"},
    {"345403028921287745114", "c8b778ce1332e0f4f0c31b6985a4f77f88d01305602e932f78dac0157a7de840"},
    {"3454723847365348416138", "51667b6d9955036a10cbd4e363881252de8ca5d3b6f210a7a3ee6cab719629a6"},
    {"345629657834658089628", "855ea04032a06d002e0be772a0dc941b9ba1497ad0d93e9be2c8e7f3296e2a51"},
    {"3456611144524852740280", "3577b7537200e869aff122f06d1ff9310f07b67b351931b685451de616c6496d"},
    {"345853420520350049400", "397171dd727f55ab174c574f4c3a87f197c6910ca2087cef66ae7f2e5e698c73"},
    {"3458799740138742970455", "6acf82efd2a0c95c342a154f1f13cc8dc8450bcef6044175b3c91b17e21bedc0"},
    {"3459485083775095487499", "51667b6d9955036a10cbd4e363881252de8ca5d3b6f210a7a3ee6cab719629a6"},
    {"3459900000412734828113", "2f338a641e65601e8a1b528ee001ba455db64f7e370972bcb8051657c2565e4c"},
    {"3461420893142609508058", "402afbef69b75abd60590549fabc2836a76bf78aaf42d86d0308f6a20c1b5cb9"},
    {"346228873022202761969", "45c264e344a6894f65b29f2be5ddc81fafa831daeddeb1b4d8278426929b3d0b"},
    {"34811228111448562003997", "00b1098a80a5e662de426284e6bf7bdf82ed236eb7c8aef1e74f9ebed204019a"},
    {"35110677435103722760616", "8008ab36ad231fe0411e7177a2db2747e0cb7a448f87f6203f7075b0edd9e0b2"},
    {"35110677436203234388827", "df293f6bfd22af772c886dbce7c937d66480b2fc2de26c91bc2bd8a4e6ac4fb0"},
    {"35115408590283900931308", "709f1188aea8481bf0bcfd62406edec731138307c94b9ce67848fd4b9feb0464"},
    {"35115408591383412559519", "d4ab4bbfcc47f219110b7c35c49ece9612e86f434c36d91ca847b5f25ee5f4b8"},
    {"35118300970848558985152", "8bde7db0638cb99e399b6d250b2c0ce5174d28085e25ee8bd4b534c85f96ec9b"},
    {"35118300971948070613363", "e6800a53087cd6cae5818f27645dbbac203c61ca016b2d4e928204e4fd9dd877"},
    {"3515873413746975655382", "d4ab4bbfcc47f219110b7c35c49ece9612e86f434c36d91ca847b5f25ee5f4b8"},
    {"3515873414846487283593", "709f1188aea8481bf0bcfd62406edec731138307c94b9ce67848fd4b9feb0464"},
    {"3517111066626369642692", "df293f6bfd22af772c886dbce7c937d66480b2fc2de26c91bc2bd8a4e6ac4fb0"},
    {"3517111067725881270903", "8008ab36ad231fe0411e7177a2db2747e0cb7a448f87f6203f7075b0edd9e0b2"},
    {"351747859676298538780", "e6800a53087cd6cae5818f27645dbbac203c61ca016b2d4e928204e4fd9dd877"},
    {"351747860775810166991", "8bde7db0638cb99e399b6d250b2c0ce5174d28085e25ee8bd4b534c85f96ec9b"},
    {"3518243144275597942266", "8cb7e938320fb1f189d62959e2d937eb24f9a7a1e0041d45d16b2243db6ea776"},
    {"3518243145375109570477", "f5326c8b3443b6d62911e30012de1497e5d039f8310c0164fd56d1b456d0434a"},
    {"3519166588561883014920", "f5326c8b3443b6d62911e30012de1497e5d039f8310c0164fd56d1b456d0434a"},
    {"3519166589661394643131", "8cb7e938320fb1f189d62959e2d937eb24f9a7a1e0041d45d16b2243db6ea776"},
    {"3523094507705513878191", "1d07fc625b5a62002a5754374778b9d84fc751aeedcc5695926ad65c097da220"},
    {"3524380691093124776041", "f4ce38ed177ab5ce31119037f60dab508938c5fa43a4332c01c5bf62f4ada042"},
    {"3538061477285680095061", "6ea243efa3359800d22d7ce274e5c2ad5178fdf61934b16855ae5ada1df4e979"},
    {"3538061481683726607905", "ea67bb193e1f35c2f636fec5bc597347bd2c6d16619953304b21f5627f6aaeb7"},
    {"3538061483882749864327", "bd0ea2581c98650bce63e5afe3ccbf490f8efda7d695fdd2a11d5eb240cf20f4"},
};
constexpr size_t kProgramCount = std::size(kProgramTable);
static_assert(kProgramCount == 54);

std::string Sha256(const void* data, size_t size) {
  static BCRYPT_ALG_HANDLE algorithm = [] {
    BCRYPT_ALG_HANDLE handle = nullptr;
    return BCryptOpenAlgorithmProvider(&handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 ? handle : nullptr;
  }();
  unsigned char digest[32]{};
  if (algorithm == nullptr ||
      BCryptHash(algorithm, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), digest, sizeof(digest)) < 0)
    throw std::runtime_error("SHA-256 is not available");
  static constexpr char kHex[] = "0123456789abcdef";
  std::string text(64, '0');
  for (size_t i = 0; i < 32; ++i) {
    text[i * 2] = kHex[digest[i] >> 4];
    text[i * 2 + 1] = kHex[digest[i] & 15];
  }
  return text;
}

bool ReadFileBytes(const std::wstring& path, Bytes& out) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < (64ll << 20);
  if (ok) {
    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
  }
  CloseHandle(file);
  return ok;
}

// Every RT_RCDATA resource of a PE32+ file, read from its bytes: the file is never loaded, so none of its code runs.
std::vector<Bytes> DataResources(const Bytes& file) {
  const auto u16 = [&](size_t p) -> uint32_t {
    if (p + 2 > file.size()) throw std::runtime_error("not a valid DLL");
    uint16_t v;
    memcpy(&v, file.data() + p, 2);
    return v;
  };
  const auto u32 = [&](size_t p) -> uint32_t {
    if (p + 4 > file.size()) throw std::runtime_error("not a valid DLL");
    uint32_t v;
    memcpy(&v, file.data() + p, 4);
    return v;
  };
  if (file.size() < 64 || file[0] != 'M' || file[1] != 'Z') throw std::runtime_error("not a DLL");
  const size_t nt = u32(60);
  if (u32(nt) != 0x00004550 || u16(nt + 24) != 0x20B) throw std::runtime_error("not a 64-bit DLL");
  const size_t sections = nt + 24 + u16(nt + 20);
  const uint32_t section_count = u16(nt + 6);
  const auto offset = [&](uint32_t rva, uint32_t size) -> size_t {
    for (uint32_t i = 0; i < section_count; ++i) {
      const size_t p = sections + i * 40;
      const uint32_t va = u32(p + 12), raw_size = u32(p + 16), raw = u32(p + 20);
      if (va <= rva && uint64_t{rva} + size <= uint64_t{va} + raw_size && uint64_t{raw} + (rva - va) + size <= file.size()) return raw + (rva - va);
    }
    throw std::runtime_error("a resource lies outside the file");
  };
  const uint32_t root_rva = u32(nt + 24 + 112 + 16);
  if (root_rva == 0) return {};
  const size_t root = offset(root_rva, 16);
  const auto entries = [&](uint32_t relative) {
    const size_t p = root + relative;
    const uint32_t count = u16(p + 12) + u16(p + 14);
    if (count > 4096) throw std::runtime_error("resource directory too large");
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (uint32_t i = 0; i < count; ++i) out.emplace_back(u32(p + 16 + 8 * i), u32(p + 20 + 8 * i));
    return out;
  };
  std::vector<Bytes> out;
  for (const auto& [type, names] : entries(0)) {
    if (type != 10 || !(names & 0x80000000)) continue;  // RT_RCDATA
    for (const auto& [name, languages] : entries(names & 0x7FFFFFFF)) {
      (void)name;
      if (!(languages & 0x80000000)) continue;
      for (const auto& [language, leaf] : entries(languages & 0x7FFFFFFF)) {
        (void)language;
        if (leaf & 0x80000000) continue;
        const uint32_t rva = u32(root + leaf), size = u32(root + leaf + 4);
        if (size == 0 || size > 16'000'000) continue;
        const size_t at = offset(rva, size);
        out.emplace_back(file.begin() + at, file.begin() + at + size);
      }
    }
  }
  return out;
}

// Every DXBC program inside one blob, by SHA-256: the blob itself when it is one, and each one laid end to end in it (MFG Unlocker's
// unpacked bundles). A DXBC header carries its own total size at offset 24.
void CollectPrograms(const Bytes& blob, std::map<std::string, Bytes>& by_hash) {
  size_t at = 0;
  while (at + 32 <= blob.size()) {
    const auto* hit = static_cast<const unsigned char*>(memchr(blob.data() + at, 'D', blob.size() - at));
    if (hit == nullptr) break;
    at = static_cast<size_t>(hit - blob.data());
    uint32_t size = 0;
    if (at + 32 <= blob.size() && memcmp(blob.data() + at, "DXBC", 4) == 0) memcpy(&size, blob.data() + at + 24, 4);
    if (size >= 32 && size <= blob.size() - at) {
      by_hash.emplace(Sha256(blob.data() + at, size), Bytes(blob.begin() + at, blob.begin() + at + size));
      at += size;
    } else {
      ++at;
    }
  }
}

// The 54 programs from one file, or an empty list with the reason. Plain DXBC resources (MFGAdaUnlock's add-on) are read as they are;
// LZMA-packed resources (MFG Unlocker 1.7.1 and later) are unpacked first, only while programs are still missing.
Programs ProgramsFrom(const Bytes& file, std::string& why) {
  std::map<std::string, Bytes> by_hash;
  const auto resources = DataResources(file);
  const auto complete = [&] {
    for (const auto& row : kProgramTable)
      if (by_hash.find(row.sha256) == by_hash.end()) return false;
    return true;
  };
  for (const auto& resource : resources) CollectPrograms(resource, by_hash);
  std::string unpack_why;
  for (const auto& resource : resources) {
    if (complete()) break;
    if (!lzma::LooksLikeAlone(resource.data(), resource.size())) continue;
    Bytes unpacked;
    if (lzma::DecodeAlone(resource.data(), resource.size(), unpacked, 64u << 20, unpack_why)) CollectPrograms(unpacked, by_hash);
  }
  Programs programs;
  size_t missing = 0;
  for (const auto& row : kProgramTable) {
    const auto found = by_hash.find(row.sha256);
    if (found == by_hash.end()) {
      ++missing;
      continue;
    }
    programs.emplace(row.key, found->second);
  }
  if (missing == kProgramCount) {
    why = "it carries none of the programs this XeMFG port needs";
    if (!unpack_why.empty()) why += " (" + unpack_why + ")";
    return {};
  }
  if (missing != 0) {
    why = "it carries only " + std::to_string(kProgramCount - missing) + " of the 54 programs (another version?)";
    return {};
  }
  return programs;
}

std::string Narrow(const std::wstring& text) {
  char out[MAX_PATH * 2] = "";
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out, sizeof(out), nullptr, nullptr);
  return out;
}

// Loaded modules that export the given function (MFG Unlocker loads under any name: version.dll in D3D12 games, its ProductName next
// to HSF's helper).
std::vector<std::wstring> LoadedWithExport(const char* name) {
  std::vector<std::wstring> paths;
  HMODULE modules[1024];
  DWORD bytes = 0;
  if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return paths;
  for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024); ++i) {
    wchar_t path[MAX_PATH] = L"";
    if (GetProcAddress(modules[i], name) != nullptr && GetModuleFileNameW(modules[i], path, MAX_PATH)) paths.emplace_back(path);
  }
  return paths;
}

// The programs come from either mod the user already has: MFGAdaUnlock's add-on (renodx-mfgunlock.addon64) or dashdogy's MFG Unlocker
// (MFGUnlocker.dll, or version.dll in D3D12 games). The loaded copies first, then copies in the game's folder or HSF_streamline\mods
// (where nothing loads them). Files are read, never loaded.
Programs FindPrograms(const wchar_t* folder, std::string& source) {
  std::vector<std::wstring> paths;
  wchar_t loaded[MAX_PATH] = L"";
  if (HMODULE module = GetModuleHandleW(L"renodx-mfgunlock.addon64"); module != nullptr && GetModuleFileNameW(module, loaded, MAX_PATH))
    paths.emplace_back(loaded);
  for (auto& path : LoadedWithExport("MfgUnlockSingleModuleQuery")) paths.push_back(std::move(path));
  const std::wstring base(folder);
  for (const wchar_t* name : {L"\\renodx-mfgunlock.addon64", L"\\HSF_streamline\\mods\\renodx-mfgunlock.addon64", L"\\MFGUnlocker.dll",
                              L"\\HSF_streamline\\mods\\MFGUnlocker.dll", L"\\version.dll"})
    paths.push_back(base + name);
  std::string reasons;
  for (size_t i = 0; i < paths.size(); ++i) {
    bool seen = false;
    for (size_t j = 0; j < i; ++j) seen = seen || lstrcmpiW(paths[i].c_str(), paths[j].c_str()) == 0;
    Bytes file;
    if (seen || !ReadFileBytes(paths[i], file)) continue;
    std::string why;
    Programs programs;
    try {
      programs = ProgramsFrom(file, why);
    } catch (const std::exception& e) {
      why = e.what();
    }
    if (!programs.empty()) {
      source = Narrow(paths[i]);
      return programs;
    }
    reasons += (reasons.empty() ? "" : "; ") + Narrow(paths[i]) + ": " + why;
  }
  throw std::runtime_error(reasons.empty() ? "3x to 6x needs MFGAdaUnlock's renodx-mfgunlock.addon64 or dashdogy's MFG Unlocker (1.7.1 or later) "
                                             "in the game's folder or HSF_streamline\\mods"
                                           : reasons);
}

// ---- Memory reads that cannot fault (both ports read XeFG's own structures) ----
bool Peek(uintptr_t address, void* out, size_t size) {
  SIZE_T got = 0;
  return address != 0 && size <= 1'000'000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), out, size, &got) && got == size;
}

// ---- The program decoder relay (shader_port.h) ----
// XeFG builds each compute pipeline from a program its decoder looks up by a numeric key. One call to the decoder (fg 0x1AD217) goes to
// Dispatch through a relay near the module: a key in the 54 gets that program, every other key Intel's own decoder, unchanged.
constexpr uint32_t kDecoderCall = 0x1AD217;
constexpr uint32_t kDecoderCallTarget = 0x11230;
constexpr uint32_t kDecode = 0x19FDA0;
constexpr uint32_t kResize = 0x1A09C0;

struct Relay {
  using Decode = void (*)(void* store, const void* key, void* output);
  using Resize = void (*)(void* vector, size_t size);
  Decode decode{};
  Resize resize{};
  Programs programs;
};
Relay* g_relay = nullptr;  // never freed: XeFG may build pipelines until the process ends
std::atomic<uint32_t> g_supplied{0};

// MSVC's std::string: size at +16, capacity at +24, the characters in place below 16.
bool KeyOf(const void* object, std::string& key) {
  const auto p = reinterpret_cast<uintptr_t>(object);
  size_t size = 0, capacity = 0;
  if (!Peek(p + 16, &size, 8) || !Peek(p + 24, &capacity, 8) || size == 0 || size > 24 || capacity < size) return false;
  uintptr_t text = p;
  if (capacity >= 16 && !Peek(p, &text, 8)) return false;
  key.resize(size);
  return Peek(text, key.data(), size) && key.find_first_not_of("0123456789") == std::string::npos;
}

void Dispatch(void* store, const void* key_object, void* output) {
  Relay* relay = g_relay;
  std::string key;
  if (KeyOf(key_object, key)) {
    const auto found = relay->programs.find(key);
    if (found != relay->programs.end()) {
      // The output is a std::vector<unsigned char>: begin, end, capacity.
      relay->resize(output, found->second.size());
      uintptr_t begin = 0, end = 0, capacity = 0;
      const auto o = reinterpret_cast<uintptr_t>(output);
      if (Peek(o, &begin, 8) && Peek(o + 8, &end, 8) && Peek(o + 16, &capacity, 8) && begin != 0 && end - begin == found->second.size() && capacity >= end) {
        memcpy(reinterpret_cast<void*>(begin), found->second.data(), found->second.size());
        g_supplied.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
  }
  relay->decode(store, key_object, output);
}

// The call's rel32 swapped atomically, the page protection put back and checked.
void SwapCall(unsigned char* site, LONG from, LONG to) {
  DWORD old = 0, unused = 0;
  if (!VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &old)) throw std::runtime_error("the program decoder call cannot be made writable");
  const LONG prior = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(site + 1), to, from);
  FlushInstructionCache(GetCurrentProcess(), site, 5);
  VirtualProtect(site, 5, old, &unused);
  if (prior != from) throw std::runtime_error("the program decoder call changed while it was being redirected");
}

enum class DecoderCall { Stock, Foreign };
DecoderCall CheckDecoderCall(HMODULE fg) {
  const auto base = reinterpret_cast<uintptr_t>(fg);
  auto* site = reinterpret_cast<unsigned char*>(base + kDecoderCall);
  MEMORY_BASIC_INFORMATION info{};
  if (VirtualQuery(site, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != fg || info.Type != MEM_IMAGE || info.Protect != PAGE_EXECUTE_READ ||
      site[0] != 0xE8)
    throw std::runtime_error("libxess_fg.dll's program decoder call is not the expected one");
  LONG displacement = 0;
  memcpy(&displacement, site + 1, 4);
  return reinterpret_cast<uintptr_t>(site + 5) + displacement == base + kDecoderCallTarget ? DecoderCall::Stock : DecoderCall::Foreign;
}

void InstallRelay(HMODULE fg, Programs programs) {
  static constexpr unsigned char kDecodeStart[]{0x48, 0x89, 0x5C, 0x24, 0x20, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0, 0, 0};
  static constexpr unsigned char kResizeStart[]{0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8B, 0x49, 0x08};
  const auto base = reinterpret_cast<uintptr_t>(fg);
  if (memcmp(reinterpret_cast<const void*>(base + kDecode), kDecodeStart, sizeof(kDecodeStart)) != 0 ||
      memcmp(reinterpret_cast<const void*>(base + kResize), kResizeStart, sizeof(kResizeStart)) != 0)
    throw std::runtime_error("libxess_fg.dll's program decoder is not the expected one");
  auto* site = reinterpret_cast<unsigned char*>(base + kDecoderCall);
  LONG original = 0;
  memcpy(&original, site + 1, 4);
  // The relay must be within a rel32 of the call: the first free 64 KB in reach.
  const auto call_end = reinterpret_cast<uintptr_t>(site + 5);
  const uintptr_t low = call_end > 0x7FFE0000 ? call_end - 0x7FFE0000 : 0x10000, high = call_end + 0x7FFE0000;
  void* stub = nullptr;
  for (uintptr_t p = (low + 0xFFFF) & ~uintptr_t{0xFFFF}; p < high && stub == nullptr;) {
    MEMORY_BASIC_INFORMATION region{};
    if (VirtualQuery(reinterpret_cast<void*>(p), &region, sizeof(region)) != sizeof(region)) break;
    const auto end = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
    if (region.State == MEM_FREE && end - p >= 0x10000) stub = VirtualAlloc(reinterpret_cast<void*>(p), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (end <= p) break;
    p = (end + 0xFFFF) & ~uintptr_t{0xFFFF};
  }
  if (stub == nullptr) throw std::runtime_error("no memory near libxess_fg.dll for the program relay");
  unsigned char jump[14]{0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]; the address follows
  const auto destination = reinterpret_cast<uintptr_t>(&Dispatch);
  memcpy(jump + 6, &destination, 8);
  memcpy(stub, jump, sizeof(jump));
  DWORD prior = 0;
  const intptr_t distance = reinterpret_cast<intptr_t>(stub) - static_cast<intptr_t>(call_end);
  if (!VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &prior) || distance < INT32_MIN || distance > INT32_MAX) {
    VirtualFree(stub, 0, MEM_RELEASE);
    throw std::runtime_error("the program relay could not be set up");
  }
  FlushInstructionCache(GetCurrentProcess(), stub, sizeof(jump));
  auto* relay = new Relay;
  relay->decode = reinterpret_cast<Relay::Decode>(base + kDecode);
  relay->resize = reinterpret_cast<Relay::Resize>(base + kResize);
  relay->programs = std::move(programs);
  g_relay = relay;
  SwapCall(site, original, static_cast<LONG>(distance));
}

// ---- The model catalog edits (model_port.h): model 15 (Intel's multi-frame model for XMX hardware) becomes model 16, laid out for
// non-XMX hardware with the 2x model's (18) scheduling. The trained weights are untouched. ----
constexpr uint32_t kModelCatalog = 0x15BE920;

class ModelPort {
  struct GraphNode {
    uint32_t op;
    std::string key;
    bool flag;
  };
  static_assert(sizeof(std::string) == 32 && sizeof(GraphNode) == 48, "XeFG's catalog uses MSVC's release std::string");
  struct VectorView {
    const void* begin;
    const void* end;
    const void* capacity;
  };
  struct Edit {
    uintptr_t address;
    Bytes before, after;
  };
  template <size_t N>
  struct Descriptor {
    std::string name;
    std::array<unsigned char, N> body;
  };
  std::vector<Edit> edits;
  std::vector<GraphNode> graph;
  VectorView graph_view{};
  std::unordered_map<std::string, Descriptor<0xE0>> reconstruction;
  std::unordered_map<std::string, Descriptor<0xD8>> output;
  std::vector<std::string> output_inputs;

  static Bytes Read(uintptr_t address, size_t size) {
    Bytes bytes(size);
    if (size == 0 || !Peek(address, bytes.data(), size)) throw std::runtime_error("model_port: unreadable");
    return bytes;
  }
  template <class T>
  static T Value(uintptr_t p) {
    T v{};
    if (!Peek(p, &v, sizeof(T))) throw std::runtime_error("model_port: unreadable");
    return v;
  }
  static std::string Name(uintptr_t p) {
    const auto size = Value<size_t>(p + 16), capacity = Value<size_t>(p + 24);
    if (size == 0 || size > 128 || capacity < size || capacity > 65536) throw std::runtime_error("model_port: string shape");
    const auto bytes = Read(capacity >= 16 ? Value<uintptr_t>(p) : p, size);
    return std::string(bytes.begin(), bytes.end());
  }
  // MSVC's std::unordered_map: max load factor, then the node list (head, size).
  static std::map<std::string, uintptr_t> Table(uintptr_t p, size_t expected = 0) {
    if (Value<float>(p) != 1.0f) throw std::runtime_error("model_port: map shape");
    const auto head = Value<uintptr_t>(p + 8), count = Value<size_t>(p + 16);
    if (!count || count > 1024 || (expected && count != expected)) throw std::runtime_error("model_port: map count");
    std::map<std::string, uintptr_t> out;
    auto node = Value<uintptr_t>(head), prior = head;
    while (node != head) {
      if (out.size() >= count || Value<uintptr_t>(node + 8) != prior || !out.emplace(Name(node + 16), node).second)
        throw std::runtime_error("model_port: map links");
      prior = node;
      node = Value<uintptr_t>(node);
    }
    if (out.size() != count || Value<uintptr_t>(head + 8) != prior) throw std::runtime_error("model_port: map length");
    return out;
  }
  template <class T>
  void Plan(uintptr_t p, T expected, T replacement) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (Value<T>(p) != expected) throw std::runtime_error("model_port: unexpected value");
    if (expected == replacement) return;
    for (const auto& e : edits)
      if (p < e.address + e.after.size() && e.address < p + sizeof(T)) throw std::runtime_error("model_port: overlapping edits");
    Edit e{p, Read(p, sizeof(T)), Bytes(sizeof(T))};
    memcpy(e.after.data(), &replacement, sizeof(T));
    edits.push_back(std::move(e));
  }
  static void Write(const Edit& e, bool restore) {
    const auto& before = restore ? e.after : e.before;
    const auto& after = restore ? e.before : e.after;
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(reinterpret_cast<void*>(e.address), &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT || m.Protect != PAGE_READWRITE ||
        m.Type != MEM_PRIVATE || e.address + after.size() > reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize || Read(e.address, before.size()) != before)
      throw std::runtime_error("model_port: write ownership");
    if (after.size() == 8 && e.address % 8 == 0) {
      LONG64 expected{}, replacement{};
      memcpy(&expected, before.data(), 8);
      memcpy(&replacement, after.data(), 8);
      if (InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(e.address), replacement, expected) != expected)
        throw std::runtime_error("model_port: pointer exchange");
    } else if (after.size() == 4 && e.address % 4 == 0) {
      LONG expected{}, replacement{};
      memcpy(&expected, before.data(), 4);
      memcpy(&replacement, after.data(), 4);
      if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(e.address), replacement, expected) != expected)
        throw std::runtime_error("model_port: value exchange");
    } else {
      throw std::runtime_error("model_port: edit alignment");
    }
    if (Read(e.address, after.size()) != after) throw std::runtime_error("model_port: write readback");
  }

 public:
  // True when the catalog already has a model 16: another mod (MFG Unlocker, MFGAdaUnlock) published it in this process.
  static bool HasModel16(HMODULE module) {
    for (const auto& [key, p] : Table(reinterpret_cast<uintptr_t>(module) + kModelCatalog, 3))
      if (Value<uint32_t>(p + 0x50) == 16) return true;
    return false;
  }

  explicit ModelPort(HMODULE module) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    auto models = Table(base + kModelCatalog, 3);
    const auto donor = models.at("i0"), fallback = models.at("i65");
    if (Value<uint32_t>(donor + 0x50) != 15 || Value<uint32_t>(fallback + 0x50) != 18 || Value<uint32_t>(models.at("i62") + 0x50) != 17 ||
        Name(donor + 0x30) != "468cb740f9dd46bc919741718b1fe210")
      throw std::runtime_error("model_port: the model catalog is not the expected one");
    // The numeric model lookup scans the string-keyed catalog.
    Plan<uint32_t>(donor + 0x50, 15, 16);
    const auto donor_table = [&](unsigned i) { return Value<uintptr_t>(donor + 0x78 + i * 8); };
    const auto fallback_table = [&](unsigned i) { return Value<uintptr_t>(fallback + 0x78 + i * 8); };
    {
      // Reconstruction texture: family 351 writes the texture-array layout family 344 reads, with the i61 residual input.
      const auto stage = Table(donor_table(3), 1).at("i40");
      const auto reference = Table(fallback_table(3), 1).at("i66");
      const auto resources = Value<uintptr_t>(stage + 0xD8);
      const auto reference_resources = Value<uintptr_t>(reference + 0xD8);
      if (Value<uintptr_t>(stage + 0xE0) - resources != 5 * 64 || Value<uintptr_t>(reference + 0xE0) - reference_resources != 5 * 64 ||
          Name(resources) != "i41" || Name(reference_resources) != "i41")
        throw std::runtime_error("model_port: reconstruction resource shape");
      for (size_t off : {40u, 44u, 48u, 52u})
        if (Value<uint32_t>(resources + off) != Value<uint32_t>(reference_resources + off)) throw std::runtime_error("model_port: reconstruction resource");
      if (Value<uint32_t>(reference_resources + 32) != 4 || Value<uint32_t>(reference_resources + 36) != 0)
        throw std::runtime_error("model_port: reconstruction reference type");
      Plan<uint32_t>(resources + 32, 0, 4);
      Plan<uint32_t>(resources + 36, 1, 0);
    }
    // Scheduling and layout metadata of the shared reconstruction layers, from the 2x model; the trained MFG weights stay in the donor's
    // table zero. Shallow borrowed vectors stay alive in the unmodified fallback catalog.
    for (const auto& [key, p] : Table(fallback_table(1), 12)) {
      Descriptor<0xE0> d;
      d.name = Name(p + 0x30);
      const auto bytes = Read(p + 0x50, d.body.size());
      memcpy(d.body.data(), bytes.data(), bytes.size());
      for (size_t offset : {0x58u, 0x78u}) {
        const auto s = reinterpret_cast<uintptr_t>(d.body.data() + offset);
        if (Value<size_t>(s + 16) == 0) continue;
        if (Name(s).starts_with("i66::")) {
          if (Value<size_t>(s + 24) != 15) throw std::runtime_error("model_port: inline reference shape");
          memcpy(d.body.data() + offset, "i40", 3);
        }
      }
      reconstruction.emplace(key, std::move(d));
    }
    {
      const auto out_source = Table(fallback_table(2), 1).at("i59");
      Descriptor<0xD8> d;
      d.name = Name(out_source + 0x30);
      const auto bytes = Read(out_source + 0x50, d.body.size());
      memcpy(d.body.data(), bytes.data(), bytes.size());
      const auto inputs_begin = Value<uintptr_t>(out_source + 0xE0), inputs_end = Value<uintptr_t>(out_source + 0xE8);
      if (inputs_end - inputs_begin != 5 * sizeof(std::string)) throw std::runtime_error("model_port: output inputs");
      for (auto p = inputs_begin; p < inputs_end; p += sizeof(std::string)) {
        auto ref = Name(p);
        if (ref.starts_with("i66::")) ref.replace(0, 3, "i40");
        output_inputs.push_back(std::move(ref));
      }
      const VectorView inputs_view{output_inputs.data(), output_inputs.data() + output_inputs.size(), output_inputs.data() + output_inputs.size()};
      memcpy(d.body.data() + 0x90, &inputs_view, sizeof(inputs_view));
      output.emplace("i59", std::move(d));
    }
    // HSF's STL must match the release-map layout XeFG reads before it is handed over.
    Table(reinterpret_cast<uintptr_t>(&reconstruction), 12);
    Table(reinterpret_cast<uintptr_t>(&output), 1);
    Plan<uintptr_t>(donor + 0x80, donor_table(1), reinterpret_cast<uintptr_t>(&reconstruction));
    Plan<uintptr_t>(donor + 0x88, donor_table(2), reinterpret_cast<uintptr_t>(&output));
    for (unsigned t : {4u, 5u, 6u, 7u}) {
      auto target = Table(donor_table(t));
      auto reference = Table(fallback_table(t));
      for (const auto& [key, p] : target) {
        if (t == 5 && key == "i4") continue;  // the MFG-specific HLSL stage
        std::string ref_key = key;
        if (key == "i18" || key == "i19") ref_key = "i17";
        if (key == "i33" || key == "i34") ref_key = "i32";
        const auto r = reference.at(ref_key);
        // Six scheduling scalars only: never flags, padding, stage indices, channel counts or weights.
        for (unsigned word = 1; word <= 6; ++word) Plan<uint32_t>(p + 0x50 + word * 4, Value<uint32_t>(p + 0x50 + word * 4), Value<uint32_t>(r + 0x50 + word * 4));
        // The shared downsampling stages need the HLSL stride.
        if (t == 6) Plan<uint32_t>(p + 0x8C, Value<uint32_t>(p + 0x8C), Value<uint32_t>(r + 0x8C));
        // CM and HLSL encode the intermediate tensors differently: only the two format fields of each buffer. Copying a vector's owning
        // pointers would alias the fallback's allocations and double-free them at SDK shutdown.
        const size_t resource_offset = t == 4 ? 0x98 : t == 5 ? 0x78 : t == 6 ? 0xA0 : 0x80;
        const auto a = Value<uintptr_t>(p + resource_offset), b = Value<uintptr_t>(p + resource_offset + 8);
        const auto ra = Value<uintptr_t>(r + resource_offset), rb = Value<uintptr_t>(r + resource_offset + 8);
        if (b <= a || b - a != rb - ra || (b - a) % 64 || b - a > 128) throw std::runtime_error("model_port: tensor vector shape");
        if (Value<uintptr_t>(p + resource_offset + 16) < b || Value<uintptr_t>(r + resource_offset + 16) < rb)
          throw std::runtime_error("model_port: tensor vector capacity");
        for (size_t off = 0; off < b - a; off += 64) {
          if (Name(a + off) != Name(ra + off)) throw std::runtime_error("model_port: tensor name");
          for (size_t field = 40; field < 56; field += 4)
            if (Value<uint32_t>(a + off + field) != Value<uint32_t>(ra + off + field)) throw std::runtime_error("model_port: tensor resource");
          if (Value<unsigned char>(a + off + 56) != Value<unsigned char>(ra + off + 56)) throw std::runtime_error("model_port: tensor flag");
          if (Value<uint32_t>(a + off + 32) > 15 || Value<uint32_t>(ra + off + 32) > 15 || Value<uint32_t>(a + off + 36) > 1 || Value<uint32_t>(ra + off + 36) > 1)
            throw std::runtime_error("model_port: tensor format");
          for (size_t field : {32u, 36u}) Plan<uint32_t>(a + off + field, Value<uint32_t>(a + off + field), Value<uint32_t>(ra + off + field));
        }
      }
    }
    const auto old_view = donor_table(9);
    const auto begin = Value<uintptr_t>(old_view), end = Value<uintptr_t>(old_view + 8);
    if (end <= begin || end - begin != 45 * sizeof(GraphNode)) throw std::runtime_error("model_port: graph shape");
    const std::map<uint32_t, uint32_t> translate{{9, 7}, {13, 10}, {12, 10}, {23, 11}, {19, 21}, {20, 22}, {14, 15}, {3, 1}, {4, 5}};
    graph.reserve(46);
    for (auto p = begin; p < end; p += sizeof(GraphNode)) {
      const auto op = Value<uint32_t>(p);
      auto key = Name(p + 8);
      const auto flag = Value<unsigned char>(p + 40);
      if (flag > 1) throw std::runtime_error("model_port: graph flag");
      if (key == "i59") graph.push_back({1, "i61", true});
      graph.push_back({translate.contains(op) ? translate.at(op) : op, std::move(key), flag != 0});
    }
    if (graph.size() != 46) throw std::runtime_error("model_port: graph length");
    graph_view = {graph.data(), graph.data() + graph.size(), graph.data() + graph.size()};
    Plan<uintptr_t>(donor + 0xC0, old_view, reinterpret_cast<uintptr_t>(&graph_view));
    // All or nothing, before the first context.
    size_t applied = 0;
    try {
      for (const auto& e : edits) {
        Write(e, false);
        ++applied;
      }
    } catch (...) {
      try {
        while (applied) Write(edits[--applied], true);
      } catch (...) {
        TerminateProcess(GetCurrentProcess(), 91);  // half a catalog would crash XeFG later, far from the cause
      }
      throw;
    }
  }
};

// ---- The frame-count sites (host_unlock.cpp), for 5 generated frames ----
struct Site {
  const char* name;
  bool fg;
  uint32_t rva;
  Bytes before;
  int count_at;  // where the count goes in `before`, or -1 for a site that is all or nothing
  Bytes after;   // for count_at -1
};
std::vector<Site> Sites() {
  return {
      {"override clamp", true, 0x1A45C2, {0xC7, 0x87, 0x6C, 0x01, 0, 0, 0x01, 0, 0, 0}, 6, {}},  // mov [rdi+16Ch],1
      {"default ceiling", true, 0x1A517D, {0xBB, 0x03, 0, 0, 0}, 1, {}},                       // mov ebx,3
      {"reported maximum", true, 0x20973B, {0xB8, 0x01, 0, 0, 0}, 1, {}},                      // mov eax,1
      // cmp eax,1 / cmp edx,1 -> 0: a live count change to or from 1 no longer re-initializes XeFG.
      {"live count", true, 0x21B86B, {0x83, 0xF8, 0x01, 0x74, 0x0F, 0x83, 0xFA, 0x01, 0x74, 0x0A}, -1, {0x83, 0xF8, 0x00, 0x74, 0x0F, 0x83, 0xFA, 0x00, 0x74, 0x0A}},
      {"XeLL frame count gate", false, 0xD1C9, {0x83, 0xFB, 0x03, 0x76, 0x07}, 2, {}},  // cmp ebx,3
  };
}

void WriteCode(unsigned char* address, const Bytes& to) {
  DWORD prior = 0, unused = 0;
  if (!VirtualProtect(address, to.size(), PAGE_EXECUTE_READWRITE, &prior)) throw std::runtime_error("a frame-count site cannot be made writable");
  memcpy(address, to.data(), to.size());
  FlushInstructionCache(GetCurrentProcess(), address, to.size());
  VirtualProtect(address, to.size(), prior, &unused);
  if (memcmp(address, to.data(), to.size()) != 0) throw std::runtime_error("a frame-count site did not take the write");
}

// Raises every site to `wanted` generated frames. A site another mod already raised stays as it is; the result is the lowest count found.
uint32_t Unlock(HMODULE fg, HMODULE ll, uint8_t wanted) {
  struct Target {
    unsigned char* address;
    Bytes to;
  };
  std::vector<Target> targets;
  uint32_t lowest = wanted;
  for (const auto& site : Sites()) {
    const HMODULE module = site.fg ? fg : ll;
    auto* address = reinterpret_cast<unsigned char*>(module) + site.rva;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.AllocationBase != module || info.Type != MEM_IMAGE ||
        info.Protect != PAGE_EXECUTE_READ)
      throw std::runtime_error(std::string(site.name) + ": not in the module's code");
    Bytes now(address, address + site.before.size());
    if (now == site.before) {
      Bytes to = site.count_at < 0 ? site.after : site.before;
      if (site.count_at >= 0) to[site.count_at] = wanted;
      targets.push_back({address, std::move(to)});
      continue;
    }
    // Raised already: the same bytes but the count (2 to 5), or the all-or-nothing site's own result.
    Bytes masked = now, expected = site.before;
    if (site.count_at >= 0) {
      const uint8_t found = now[site.count_at];
      masked[site.count_at] = expected[site.count_at] = 0;
      if (masked == expected && found >= 2 && found <= 5) {
        if (found < lowest) lowest = found;
        continue;
      }
    } else if (now == site.after) {
      continue;
    }
    throw std::runtime_error(std::string(site.name) + " (" + (site.fg ? "libxess_fg" : "libxell") + ".dll) has unexpected bytes");
  }
  size_t done = 0;
  std::vector<Bytes> originals;
  try {
    for (; done < targets.size(); ++done) {
      originals.emplace_back(targets[done].address, targets[done].address + targets[done].to.size());
      WriteCode(targets[done].address, targets[done].to);
    }
  } catch (...) {
    while (done--) {
      try {
        WriteCode(targets[done].address, originals[done]);
      } catch (...) {
      }
    }
    throw;
  }
  return lowest;
}

}  // namespace

Result Publish(HMODULE fg, HMODULE ll, const wchar_t* folder) {
  Result result;
  try {
    if (fg == nullptr || ll == nullptr) throw std::runtime_error("libxell.dll is not loaded");
    if (ModelPort::HasModel16(fg)) {
      // Another mod published the model in this process, and it supplies the programs: use it as it is.
      result.source = "the multi-frame model another mod already published";
    } else {
      if (CheckDecoderCall(fg) == DecoderCall::Foreign) throw std::runtime_error("another mod redirected XeFG's program decoder without publishing model 16");
      Programs programs = FindPrograms(folder, result.source);
      InstallRelay(fg, std::move(programs));
      new ModelPort(fg);  // never destroyed: XeFG's catalog points into it
    }
    result.max_frames = Unlock(fg, ll, 5);
  } catch (const std::exception& e) {
    result.max_frames = 1;
    result.why = e.what();
  }
  return result;
}

uint32_t ProgramsSupplied() { return g_supplied.load(std::memory_order_relaxed); }

}  // namespace hsf::xmx::model
