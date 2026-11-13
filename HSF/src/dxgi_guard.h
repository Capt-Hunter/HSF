// Guards ReShade's DXGI factory against adapter interfaces it does not know.
//
// ReShade's IDXGIFactory4::EnumAdapterByLuid wraps whatever adapter it returns as an IDXGIAdapter and stores the wrapper in the adapter's private
// data, whatever interface the caller asked for. d3d11.dll asks for an internal adapter interface when it is handed an adapter it cannot use
// directly (RTSS makes a D3D11 device on an adapter from ReShade's factory); that interface has no SetPrivateData, and Windows breaks into the
// debugger, which kills the game. The guard sends only those calls past ReShade's wrapping: ReShade still makes its IDXGIAdapter wrapper, and the
// interface the caller asked for comes from it. Calls for the known adapter interfaces go to ReShade unchanged.
#pragma once

#include <windows.h>

namespace hsf::dxgiguard {

// Patches ReShade's factory once per process. Returns a line for the log, or nullptr when there was nothing to do (already installed).
const char* Install(HMODULE reshade_module);
// Puts ReShade's own method back, as HSF unloads while ReShade stays (ReShade reloading its add-ons). Install can run again after it.
void Uninstall();

}  // namespace hsf::dxgiguard
