// HSF's Vulkan layer (VK_LAYER_HSF_bridge), the fallback for games whose vkCreateDevice the add-on's hook misses (vk_hook.cpp). It does one
// thing: when a game makes its Vulkan device, it adds what HSF's Vulkan bridge needs to hand frames to D3D11 in the same frame (vk_extend.h).
// Everything else passes straight through to the next layer or the driver.
// The add-on asks HsfVkLayerDevice(device) whether a device got them (bit 1: semaphores, bit 2: memory).
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#include "vk_extend.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Dispatchable handles start with the loader's dispatch table pointer: the same for an object and everything made from it.
void* Key(const void* handle) { return *static_cast<void* const*>(handle); }

struct InstanceData {
  VkInstance instance = VK_NULL_HANDLE;
  PFN_vkGetInstanceProcAddr next_gipa = nullptr;
  PFN_vkDestroyInstance destroy = nullptr;
};

struct DeviceData {
  PFN_vkGetDeviceProcAddr next_gdpa = nullptr;
  PFN_vkDestroyDevice destroy = nullptr;
  int flags = 0;
};

std::mutex g_lock;
std::unordered_map<void*, InstanceData> g_instances;
std::unordered_map<void*, DeviceData> g_devices;
std::unordered_map<VkDevice, int> g_device_flags;  // by handle, for the add-on

void Log(const std::string& text) {
  // Same folder as the layer: hsf_vk_layer.log, rewritten by each process that loads the layer.
  static std::once_flag once;
  static wchar_t path[MAX_PATH] = L"";
  std::call_once(once, [] {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Log), &self);
    if (GetModuleFileNameW(self, path, MAX_PATH) == 0) return;
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash == nullptr) return;
    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"hsf_vk_layer.log");
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  });
  if (path[0] == L'\0') return;
  HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return;
  const std::string line = text + "\r\n";
  DWORD written = 0;
  WriteFile(f, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
  CloseHandle(f);
}

template <typename Info, VkStructureType Type>
Info* FindLink(const void* next, VkLayerFunction function) {
  for (auto* p = static_cast<const VkBaseInStructure*>(next); p != nullptr; p = p->pNext)
    if (p->sType == Type && reinterpret_cast<const Info*>(p)->function == function) return const_cast<Info*>(reinterpret_cast<const Info*>(p));
  return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks* alloc, VkInstance* instance) {
  auto* link = FindLink<VkLayerInstanceCreateInfo, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO>(info->pNext, VK_LAYER_LINK_INFO);
  if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  const auto create = reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
  if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const VkResult result = create(info, alloc, instance);
  if (result != VK_SUCCESS) return result;
  InstanceData data;
  data.instance = *instance;
  data.next_gipa = next_gipa;
  data.destroy = reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*instance, "vkDestroyInstance"));
  std::lock_guard<std::mutex> lock(g_lock);
  g_instances[Key(*instance)] = data;
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
  if (instance == VK_NULL_HANDLE) return;
  PFN_vkDestroyInstance destroy = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_instances.find(Key(instance));
    if (it == g_instances.end()) return;
    destroy = it->second.destroy;
    g_instances.erase(it);
  }
  if (destroy != nullptr) destroy(instance, alloc);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info, const VkAllocationCallbacks* alloc, VkDevice* device) {
  auto* link = FindLink<VkLayerDeviceCreateInfo, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO>(info->pNext, VK_LAYER_LINK_INFO);
  if (link == nullptr || link->u.pLayerInfo == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  const PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  const PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  VkLayerDeviceLink* const below = link->u.pLayerInfo;  // each layer below moves this on again, so a second try puts it back

  VkInstance instance = VK_NULL_HANDLE;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_instances.find(Key(physical));  // a physical device carries its instance's dispatch table
    if (it != g_instances.end()) instance = it->second.instance;
    else if (!g_instances.empty()) instance = g_instances.begin()->second.instance;
  }
  const auto create = reinterpret_cast<PFN_vkCreateDevice>(next_gipa(instance, "vkCreateDevice"));
  if (create == nullptr) return VK_ERROR_INITIALIZATION_FAILED;

  // What the GPU offers.
  std::vector<VkExtensionProperties> offered;
  uint32_t api = VK_API_VERSION_1_0;
  if (auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(next_gipa(instance, "vkEnumerateDeviceExtensionProperties"))) {
    uint32_t count = 0;
    if (enumerate(physical, nullptr, &count, nullptr) == VK_SUCCESS) {
      offered.resize(count);
      if (enumerate(physical, nullptr, &count, offered.data()) != VK_SUCCESS) offered.clear();
      offered.resize(count);
    }
  }
  if (auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(next_gipa(instance, "vkGetPhysicalDeviceProperties"))) {
    VkPhysicalDeviceProperties p{};
    properties(physical, &p);
    api = p.apiVersion;
  }

  // The game's request plus what HSF's bridge needs.
  hsf::vk::Extended ext;
  hsf::vk::Extend(*info, offered, api, ext);
  VkResult result = create(physical, &ext.info, alloc, device);
  bool refused = false;
  if (result != VK_SUCCESS && ext.changed) {
    // The driver refused our additions: make the device exactly as the game asked, so the game never fails because of HSF.
    Log("the device with HSF's extensions was refused (" + std::to_string(result) + "); made as the game asked, without them");
    ext.Restore();
    link->u.pLayerInfo = below;
    result = create(physical, info, alloc, device);
    refused = true;
  }
  if (result != VK_SUCCESS) return result;
  const int flags = refused ? 0 : ext.Flags();
  DeviceData data;
  data.next_gdpa = next_gdpa;
  data.destroy = reinterpret_cast<PFN_vkDestroyDevice>(next_gdpa(*device, "vkDestroyDevice"));
  data.flags = flags;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    g_devices[Key(*device)] = data;
    g_device_flags[*device] = data.flags;
  }
  Log(std::string("device made: shared semaphores ") + ((flags & 1) ? "on" : "OFF") + ", shared memory " + ((flags & 2) ? "on" : "OFF") +
      (ext.added.empty() ? std::string(", no extension added (the game, HSF's add-on or another layer asked already)") : ", added " + ext.added));
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
  if (device == VK_NULL_HANDLE) return;
  PFN_vkDestroyDevice destroy = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_devices.find(Key(device));
    if (it == g_devices.end()) return;
    destroy = it->second.destroy;
    g_devices.erase(it);
    g_device_flags.erase(device);
  }
  if (destroy != nullptr) destroy(device, alloc);
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* name) {
  if (std::strcmp(name, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&GetInstanceProcAddr);
  if (std::strcmp(name, "vkCreateInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateInstance);
  if (std::strcmp(name, "vkDestroyInstance") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyInstance);
  if (std::strcmp(name, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&CreateDevice);
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
  if (std::strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice);
  if (instance == VK_NULL_HANDLE) return nullptr;
  PFN_vkGetInstanceProcAddr next = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_instances.find(Key(instance));
    if (it != g_instances.end()) next = it->second.next_gipa;
  }
  return next != nullptr ? next(instance, name) : nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* name) {
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&GetDeviceProcAddr);
  if (std::strcmp(name, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(&DestroyDevice);
  if (device == VK_NULL_HANDLE) return nullptr;
  PFN_vkGetDeviceProcAddr next = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lock);
    auto it = g_devices.find(Key(device));
    if (it != g_devices.end()) next = it->second.next_gdpa;
  }
  return next != nullptr ? next(device, name) : nullptr;
}

}  // namespace

// vk_layer.h declares the loader entry point without dllexport, so it is exported by name here.
#pragma comment(linker, "/EXPORT:vkNegotiateLoaderLayerInterfaceVersion")

extern "C" {

VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* version) {
  if (version == nullptr || version->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
  if (version->loaderLayerInterfaceVersion > 2) version->loaderLayerInterfaceVersion = 2;
  if (version->loaderLayerInterfaceVersion < 2) return VK_ERROR_INITIALIZATION_FAILED;
  version->pfnGetInstanceProcAddr = &GetInstanceProcAddr;
  version->pfnGetDeviceProcAddr = &GetDeviceProcAddr;
  version->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}

// For HSF's add-on: what this layer turned on for `device` (bit 1: shared semaphores with timeline, bit 2: shared memory); 0 if unknown.
__declspec(dllexport) int HsfVkLayerDevice(VkDevice device) {
  std::lock_guard<std::mutex> lock(g_lock);
  auto it = g_device_flags.find(device);
  return it != g_device_flags.end() ? it->second : 0;
}

}  // extern "C"
