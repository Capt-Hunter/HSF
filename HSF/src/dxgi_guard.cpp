// See dxgi_guard.h.
#include "dxgi_guard.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <atomic>

using Microsoft::WRL::ComPtr;

namespace hsf::dxgiguard {
namespace {

constexpr size_t SLOT_ENUM_ADAPTER_BY_LUID = 26;  // IDXGIFactory4

using EnumAdapterByLuidFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory4*, LUID, REFIID, void**);
EnumAdapterByLuidFn g_reshade_enum = nullptr;
void** g_table = nullptr;  // ReShade's factory method table, while the guard is in it
std::atomic<bool> g_done{false};

bool WriteSlot(void* value) {
  DWORD old = 0;
  if (!VirtualProtect(&g_table[SLOT_ENUM_ADAPTER_BY_LUID], sizeof(void*), PAGE_READWRITE, &old)) return false;
  g_table[SLOT_ENUM_ADAPTER_BY_LUID] = value;
  VirtualProtect(&g_table[SLOT_ENUM_ADAPTER_BY_LUID], sizeof(void*), old, &old);
  return true;
}

bool Known(REFIID riid) {
  return riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIAdapter) || riid == __uuidof(IDXGIAdapter1) ||
         riid == __uuidof(IDXGIAdapter2) || riid == __uuidof(IDXGIAdapter3) || riid == __uuidof(IDXGIAdapter4);
}

HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(IDXGIFactory4* factory, LUID luid, REFIID riid, void** out) {
  if (out == nullptr || Known(riid)) return g_reshade_enum(factory, luid, riid, out);
  *out = nullptr;
  ComPtr<IDXGIAdapter> adapter;
  const HRESULT hr = g_reshade_enum(factory, luid, __uuidof(IDXGIAdapter), reinterpret_cast<void**>(adapter.GetAddressOf()));
  if (FAILED(hr)) return hr;
  // ReShade's adapter wrapper hands interfaces it does not know to the real adapter.
  return adapter->QueryInterface(riid, out);
}

bool Inside(HMODULE module, const void* address) {
  HMODULE owner = nullptr;
  return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &owner) &&
         owner == module;
}

}  // namespace

const char* Install(HMODULE reshade_module) {
  if (g_done.exchange(true)) return nullptr;
  if (reshade_module == nullptr) return "DXGI guard: no ReShade module";
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  const auto create = reinterpret_cast<CreateFactoryFn>(GetProcAddress(reshade_module, "CreateDXGIFactory1"));
  if (create == nullptr) return "DXGI guard: off (ReShade does not make DXGI factories here)";
  ComPtr<IDXGIFactory1> factory1;
  if (FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(factory1.GetAddressOf())))) return "DXGI guard: off (no DXGI factory)";
  ComPtr<IDXGIFactory4> factory;
  if (FAILED(factory1.As(&factory))) return "DXGI guard: off (no IDXGIFactory4)";
  void** const table = *reinterpret_cast<void***>(factory.Get());
  // Only ReShade's own factory wrapper, whose table and method both live in ReShade.
  if (!Inside(reshade_module, table) || !Inside(reshade_module, table[SLOT_ENUM_ADAPTER_BY_LUID]))
    return "DXGI guard: off (the factory is not ReShade's wrapper)";
  if (table[SLOT_ENUM_ADAPTER_BY_LUID] == reinterpret_cast<void*>(&EnumAdapterByLuid)) return nullptr;
  g_reshade_enum = reinterpret_cast<EnumAdapterByLuidFn>(table[SLOT_ENUM_ADAPTER_BY_LUID]);
  g_table = table;
  if (!WriteSlot(reinterpret_cast<void*>(&EnumAdapterByLuid))) {
    g_table = nullptr;
    return "DXGI guard: off (could not patch)";
  }
  // The patched table lives in ReShade, which outlives HSF when it reloads its add-ons: Uninstall puts ReShade's method back as HSF unloads. (Pinning HSF
  // instead made ReShade refuse it when it loaded its add-ons again.)
  return "DXGI guard: on (adapter interfaces ReShade does not know skip its wrapper)";
}

void Uninstall() {
  if (g_table == nullptr) return;
  if (g_table[SLOT_ENUM_ADAPTER_BY_LUID] == reinterpret_cast<void*>(&EnumAdapterByLuid)) WriteSlot(reinterpret_cast<void*>(g_reshade_enum));
  g_table = nullptr;
  g_done = false;
}

}  // namespace hsf::dxgiguard
