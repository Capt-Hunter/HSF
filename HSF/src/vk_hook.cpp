// See vk_hook.h.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "vk_hook.h"

#include <vulkan/vulkan.h>

#include "vk_extend.h"

#include <MinHook.h>
#include <windows.h>

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hsf::vk {
namespace {

void* g_target = nullptr;  // vulkan-1.dll's vkCreateDevice, while hooked
PFN_vkCreateDevice g_original = nullptr;

// The hook jumps to this stub, not into HSF: it sits in memory of its own that outlives the add-on. ReShade can unload HSF and load it again
// (a game that makes a Vulkan instance, drops it and makes another), and another program (RTSS) may hook vkCreateDevice on top of HSF's jump.
// Taking the hook out then would wipe that program's hook or leave it jumping into freed code, so the hook stays in for good: at unload the
// stub goes straight to the original, and the next HSF finds the stub again (its address in an environment variable of the process) and
// points it back at itself.
struct Stub {
  uint8_t code[8];       // jmp [target]
  void* volatile target;  // OnCreateDevice of the HSF loaded now, or `original`
  void* original;        // MinHook's trampoline to the real vkCreateDevice (MinHook's own memory, never freed: MH_Uninitialize is never called)
  void* hooked;          // the vkCreateDevice it hooked (a vulkan-1.dll loaded again is hooked anew)
  uint32_t magic;
};
constexpr uint32_t STUB_MAGIC = 0x48534656;  // "HSFV"
constexpr wchar_t STUB_VARIABLE[] = L"HSF_VK_CREATE_DEVICE_STUB";
Stub* g_stub = nullptr;

Stub* FindStub() {
  wchar_t text[32] = L"";
  if (GetEnvironmentVariableW(STUB_VARIABLE, text, 32) == 0) return nullptr;
  Stub* const stub = reinterpret_cast<Stub*>(static_cast<uintptr_t>(_wcstoui64(text, nullptr, 16)));
  MEMORY_BASIC_INFORMATION m{};
  if (stub == nullptr || VirtualQuery(stub, &m, sizeof(m)) == 0 || m.State != MEM_COMMIT || stub->magic != STUB_MAGIC) return nullptr;
  return stub;
}

Stub* MakeStub() {
  Stub* const stub = static_cast<Stub*>(VirtualAlloc(nullptr, sizeof(Stub), MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (stub == nullptr) return nullptr;
  const uint8_t jump[8] = {0xFF, 0x25, 0, 0, 0, 0, 0xCC, 0xCC};
  std::memcpy(stub->code, jump, sizeof(jump));
#ifdef _WIN64
  const int32_t to = static_cast<int32_t>(offsetof(Stub, target) - 6);  // RIP-relative, from the end of the jump
#else
  const int32_t to = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&stub->target));  // absolute in 32-bit code
#endif
  std::memcpy(stub->code + 2, &to, sizeof(to));
  stub->target = nullptr;
  stub->original = nullptr;
  stub->hooked = nullptr;
  stub->magic = 0;
  return stub;
}
PFN_vkEnumerateDeviceExtensionProperties g_enumerate = nullptr;
PFN_vkGetPhysicalDeviceProperties g_properties = nullptr;

std::mutex g_lock;
std::unordered_map<const void*, int> g_flags;
std::string g_report;

// RivaTuner Statistics Server's hooks are in the process. A device made with HSF's additions crashes RTSS's Vulkan overlay in some games
// (Valheim: the Vulkan loader aborts inside RTSS's hooks), so with RTSS the device is made as the game asked and frames go one frame behind.
bool RtssIn() { return GetModuleHandleW(L"RTSSHooks64.dll") != nullptr || GetModuleHandleW(L"RTSSHooks.dll") != nullptr; }

VKAPI_ATTR VkResult VKAPI_CALL OnCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* alloc,
                                              VkDevice* device) {
  if (info == nullptr) return g_original(physical, info, alloc, device);
  if (RtssIn()) {
    const VkResult result = g_original(physical, info, alloc, device);
    if (result == VK_SUCCESS) {
      std::lock_guard<std::mutex> lock(g_lock);
      g_flags[*device] = 0;
      g_report = "Vulkan device hook: RTSS is running, device made as the game asked (frames one frame behind; RTSS crashes with HSF's additions)";
    }
    return result;
  }
  std::vector<VkExtensionProperties> offered;
  uint32_t count = 0;
  if (g_enumerate(physical, nullptr, &count, nullptr) == VK_SUCCESS) {
    offered.resize(count);
    if (g_enumerate(physical, nullptr, &count, offered.data()) != VK_SUCCESS) count = 0;
    offered.resize(count);
  }
  VkPhysicalDeviceProperties properties{};
  g_properties(physical, &properties);

  Extended ext;
  Extend(*info, offered, properties.apiVersion, ext);
  VkResult result = g_original(physical, &ext.info, alloc, device);
  bool refused = false;
  if (result != VK_SUCCESS && ext.changed) {
    // Refused with HSF's additions: make the device exactly as the game asked, so the game never fails because of HSF.
    ext.Restore();
    refused = true;
    result = g_original(physical, info, alloc, device);
  }
  if (result != VK_SUCCESS) return result;
  const int flags = refused ? 0 : ext.Flags();
  char line[400];
  if (refused)
    std::snprintf(line, sizeof(line), "Vulkan device hook: the device with HSF's extensions was refused, made as the game asked (frames one frame behind)");
  else
    std::snprintf(line, sizeof(line), "Vulkan device hook: shared semaphores %s, shared memory %s, %s%s", (flags & 1) ? "on" : "OFF", (flags & 2) ? "on" : "OFF",
                  ext.added.empty() ? "no extension added (the game or a layer asked already)" : "added ", ext.added.c_str());
  std::lock_guard<std::mutex> lock(g_lock);
  g_flags[*device] = flags;
  g_report = line;
  return result;
}

}  // namespace

const char* HookCreateDevice() {
  if (g_target != nullptr) return "Vulkan device hook: already in";
  const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  if (loader == nullptr) return nullptr;  // not a Vulkan game: nothing to say
  void* const target = reinterpret_cast<void*>(GetProcAddress(loader, "vkCreateDevice"));
  g_enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(GetProcAddress(loader, "vkEnumerateDeviceExtensionProperties"));
  g_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(GetProcAddress(loader, "vkGetPhysicalDeviceProperties"));
  if (target == nullptr || g_enumerate == nullptr || g_properties == nullptr) return "Vulkan device hook: vulkan-1.dll lacks vkCreateDevice, not hooked";
  if (RtssIn() && FindStub() == nullptr) return "Vulkan device hook: not hooked, RTSS is running (frames one frame behind)";
  if (Stub* const stub = FindStub(); stub != nullptr && stub->hooked == target) {  // an earlier HSF in this process hooked it already
    g_original = reinterpret_cast<PFN_vkCreateDevice>(stub->original);
    stub->target = reinterpret_cast<void*>(&OnCreateDevice);
    g_stub = stub;
    g_target = target;
    return "Vulkan device hook: in again (the hook of an earlier HSF in this process now leads here)";
  }
  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return "Vulkan device hook: MinHook did not start, not hooked";
  Stub* const stub = MakeStub();
  if (stub == nullptr) return "Vulkan device hook: no memory for its stub, not hooked";
  if (MH_CreateHook(target, stub->code, reinterpret_cast<void**>(&g_original)) != MH_OK) {
    VirtualFree(stub, 0, MEM_RELEASE);
    return "Vulkan device hook: could not hook vkCreateDevice";
  }
  stub->original = reinterpret_cast<void*>(g_original);
  stub->hooked = target;
  stub->target = reinterpret_cast<void*>(&OnCreateDevice);
  stub->magic = STUB_MAGIC;
  if (MH_EnableHook(target) != MH_OK) {
    MH_RemoveHook(target);
    VirtualFree(stub, 0, MEM_RELEASE);
    return "Vulkan device hook: could not turn the vkCreateDevice hook on";
  }
  wchar_t text[32];
  swprintf_s(text, L"%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(stub)));
  SetEnvironmentVariableW(STUB_VARIABLE, text);
  g_stub = stub;
  g_target = target;
  return "Vulkan device hook: in (vkCreateDevice of vulkan-1.dll)";
}

void UnhookCreateDevice() {
  if (g_target == nullptr) return;
  if (g_stub != nullptr) g_stub->target = g_stub->original;  // the hook stays; vkCreateDevice goes straight to the original from now on
  g_stub = nullptr;
  g_target = nullptr;
}

int HookFlags(const void* vk_device) {
  std::lock_guard<std::mutex> lock(g_lock);
  const auto it = g_flags.find(vk_device);
  return it != g_flags.end() ? it->second : 0;
}

const char* HookReport() {
  std::lock_guard<std::mutex> lock(g_lock);
  static std::string copy;
  copy = g_report;
  return copy.c_str();
}

}  // namespace hsf::vk
