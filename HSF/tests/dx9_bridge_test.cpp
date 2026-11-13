// Offline test of the DX9 bridge's GPU route, no game needed: a D3D9Ex device of its own draws a new colour each frame, bridges it, and reads
// the Direct3D 11 texture back. With the GPU fences the colour read must be this frame's; without them, the frame before's.
// With "stall" one fence signal is left out on purpose: the bridge must notice within a second, let the GPU queues go and carry on without
// fences (the frame before's colour from then on).
// Usage: dx9_bridge_test.exe [frames] [nofence|stall]

#include "../src/dx9_bridge.h"

#include <wrl/client.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace {

D3DCOLOR ColorOf(int frame) {
  return D3DCOLOR_ARGB(255, (frame * 37) & 255, (frame * 91 + 13) & 255, (frame * 151 + 7) & 255);
}

// The D3D11 texture's top-left texel as a D3DCOLOR (the bridge copies A8R8G8B8 into B8G8R8A8: the same bytes).
bool ReadBack(ID3D11Device* device, ID3D11Texture2D* texture, D3DCOLOR& out) {
  ComPtr<ID3D11DeviceContext> context;
  device->GetImmediateContext(&context);
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.MiscFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging))) return false;
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
  std::memcpy(&out, mapped.pData, sizeof(out));
  context->Unmap(staging.Get(), 0);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  int frames = 300;
  bool fence = true, stall = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "nofence") == 0) fence = false;
    else if (std::strcmp(argv[i], "stall") == 0) stall = true;
    else frames = std::atoi(argv[i]);
  }
  const HWND window = CreateWindowExW(0, L"STATIC", L"dx9_bridge_test", WS_OVERLAPPEDWINDOW, 0, 0, 640, 360, nullptr, nullptr, nullptr, nullptr);
  ComPtr<IDirect3D9Ex> d3d;
  if (FAILED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d))) return std::printf("FAIL: no D3D9Ex\n"), 1;
  D3DPRESENT_PARAMETERS pp{};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_A8R8G8B8;
  pp.BackBufferWidth = 640;
  pp.BackBufferHeight = 360;
  pp.hDeviceWindow = window;
  pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
  ComPtr<IDirect3DDevice9Ex> device;
  HRESULT hr = d3d->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, nullptr, &device);
  if (FAILED(hr)) return std::printf("FAIL: no D3D9Ex device (0x%08lx)\n", static_cast<unsigned long>(hr)), 1;

  char error[200] = "";
  hsf::dx9::SetGpuFence(fence);
  hsf::dx9::SetHalfInputs(false);
  if (!hsf::dx9::Start(device.Get(), error, sizeof(error))) return std::printf("FAIL: bridge: %s\n", error), 1;

  int same = 0, behind = 0, wrong = 0, none = 0, resets = 0;
  if (stall) {
    // 20 good frames, then a lost signal. Nothing is read back while HSF's Direct3D 11 context may stand still (a read-back would wait for it).
    int n = 1;
    for (; n <= 20; ++n) {
      device->Clear(0, nullptr, D3DCLEAR_TARGET, ColorOf(n), 1.0f, 0);
      ComPtr<IDirect3DSurface9> back;
      device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
      if (n == 20) hsf::dx9::TestLoseFenceSignal();
      hsf::dx9::Bridge(back.Get(), nullptr, nullptr);
      device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    }
    const DWORD start = GetTickCount();
    int waited = 0;
    while (hsf::dx9::GpuFence() && GetTickCount() - start < 5000) {
      device->Clear(0, nullptr, D3DCLEAR_TARGET, ColorOf(n), 1.0f, 0);
      ComPtr<IDirect3DSurface9> back;
      device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
      hsf::dx9::Bridge(back.Get(), nullptr, nullptr);
      device->PresentEx(nullptr, nullptr, nullptr, nullptr, D3DPRESENT_DONOTWAIT);
      ++n;
      ++waited;
      Sleep(5);
    }
    const DWORD took = GetTickCount() - start;
    const bool let_go = !hsf::dx9::GpuFence();
    // After it: the event-query route, one frame behind, and the read-back must not hang.
    int after_behind = 0, after_other = 0;
    for (int i = 0; i < 100; ++i, ++n) {
      device->Clear(0, nullptr, D3DCLEAR_TARGET, ColorOf(n), 1.0f, 0);
      ComPtr<IDirect3DSurface9> back;
      device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
      const hsf::dx9::Frame f = hsf::dx9::Bridge(back.Get(), nullptr, nullptr);
      D3DCOLOR got = 0;
      if (f.color != nullptr && ReadBack(hsf::dx9::Device(), f.color, got) && got == ColorOf(n - 1)) ++after_behind;
      else ++after_other;
      device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
    }
    const hsf::dx9::Stats s = hsf::dx9::TakeStats();
    std::printf("lost signal: fences let go %s after %lu ms (%d frames); then %d of 100 frames read as the frame before's, %d other\n%s\n", let_go ? "yes" : "NO",
                took, waited, after_behind, after_other, s.lanes);
    hsf::dx9::Stop();
    const bool pass = let_go && took < 2000 && after_behind >= 95;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
  }
  for (int n = 1; n <= frames; ++n) {
    // A device Reset in the middle: the bridge must let go and start again.
    if (n == frames / 2) {
      hsf::dx9::ReleaseGameResources();
      hr = device->ResetEx(&pp, nullptr);
      if (FAILED(hr)) return std::printf("FAIL: ResetEx (0x%08lx)\n", static_cast<unsigned long>(hr)), 1;
      ++resets;
    }
    device->Clear(0, nullptr, D3DCLEAR_TARGET, ColorOf(n), 1.0f, 0);
    ComPtr<IDirect3DSurface9> back;
    device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
    const hsf::dx9::Frame f = hsf::dx9::Bridge(back.Get(), nullptr, nullptr);
    D3DCOLOR got = 0;
    if (f.color == nullptr || !ReadBack(hsf::dx9::Device(), f.color, got)) {
      ++none;
    } else if (got == ColorOf(n)) {
      ++same;
    } else if (got == ColorOf(n - 1)) {
      ++behind;
    } else {
      ++wrong;
      if (wrong <= 5) std::printf("frame %d: read %08lx, this frame %08lx, the one before %08lx\n", n, got, ColorOf(n), ColorOf(n - 1));
    }
    device->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
  }
  const bool on = hsf::dx9::GpuFence();
  const hsf::dx9::Stats s = hsf::dx9::TakeStats();
  std::printf("route: %s, GPU fences %s\n", hsf::dx9::ROUTE_NAMES[static_cast<int>(hsf::dx9::CurrentRoute())], on ? "on" : "off");
  std::printf("frames %d: this frame's %d, the frame before's %d, wrong %d, none %d; resets %d\n", frames, same, behind, wrong, none, resets);
  std::printf("bridge %.3f ms per frame (worst %.3f), failed %u, problem \"%s\"\n%s\n", s.bridge_ms, s.worst_ms, s.failed, s.problem, s.lanes);
  hsf::dx9::Stop();
  // With fences every frame is this frame's. Without, every frame but the first after a start is the one before's.
  const bool pass = wrong == 0 && s.failed == 0 && (fence ? (on && same == frames) : (!on && behind >= frames - 2 * (resets + 1)));
  std::printf("%s\n", pass ? "PASS" : "FAIL");
  return pass ? 0 : 1;
}
