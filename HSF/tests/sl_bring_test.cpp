// A small program with no Streamline of its own, standing in for a game: real device, real window, and a swap chain made the plain way. HSF must turn that swap chain into one made through
// Streamline, from the folder given. Run: sl_bring_test <folder with sl.interposer.dll and its plugins> <log file>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdio>

#include "../src/sl_bring.h"

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    printf("usage: sl_bring_test <folder> <log file> [nvof]\n");
    return 2;
  }
  const wchar_t kClass[] = L"HSFBringTest";
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  RegisterClassW(&wc);
  const HWND hwnd = CreateWindowW(kClass, L"HSF bring test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, 40, 1280, 720, nullptr, nullptr, wc.hInstance, nullptr);
  Check(hwnd != nullptr, "window");

  IDXGIFactory2* first_factory = nullptr;
  Check(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&first_factory))), "a plain factory, made before HSF starts");
  ID3D12Device* device = nullptr;
  Check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))), "device");
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ID3D12CommandQueue* queue = nullptr;
  Check(SUCCEEDED(device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&queue))), "queue");

  // With the real add-on and ReShade beside this program: HSF.ini there says SlBringIn=1, and this only makes a swap chain and presents, like a game with a black picture.
  if (GetEnvironmentVariableW(L"HSF_TEST_ADDON", nullptr, 0) != 0) {
    DXGI_SWAP_CHAIN_DESC1 game_desc{};
    game_desc.Width = 1280;
    game_desc.Height = 720;
    game_desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    game_desc.SampleDesc.Count = 1;
    game_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    game_desc.BufferCount = 3;
    game_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* game_chain = nullptr;
    const HRESULT made = first_factory->CreateSwapChainForHwnd(queue, hwnd, &game_desc, nullptr, nullptr, &game_chain);
    printf("     swap chain 0x%08lx\n", made);
    int shown = 0;
    const ULONGLONG begun = GetTickCount64();
    while (game_chain != nullptr && GetTickCount64() - begun < 75000) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
      if (SUCCEEDED(game_chain->Present(0, 0))) ++shown;
      Sleep(10);
    }
    printf("     presented %d, device %08lx\n", shown, device->GetDeviceRemovedReason());
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
  }

  hsf::slbring::Start(argv[2], argv[1]);
  void* const slot_before = (*reinterpret_cast<void***>(first_factory))[15];
  hsf::slbring::NoteDevice(device);
  Check(hsf::slbring::GetStats().streamline_up, "Streamline started and swap chains are caught");
  Check((*reinterpret_cast<void***>(first_factory))[15] != slot_before, "a factory made earlier is caught too");

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = 1280;
  desc.Height = 720;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 3;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  IDXGISwapChain1* swap_chain = nullptr;
  const HRESULT hr = first_factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swap_chain);
  printf("     swap chain 0x%08lx %p\n", hr, swap_chain);
  Check(SUCCEEDED(hr) && swap_chain != nullptr, "the swap chain is made");
  hsf::slbring::Stats stats = hsf::slbring::GetStats();
  Check(stats.swap_chains == 1 && stats.failed == 0, "it went through Streamline");
  Check(stats.plugin_loaded == 1, "DLSS-G's plugin loaded");
  Check(stats.supported == 0, "DLSS-G supported on this adapter");

  if (swap_chain != nullptr) {
    Check(SUCCEEDED(swap_chain->ResizeBuffers(0, 1024, 600, DXGI_FORMAT_UNKNOWN, 0)), "a resize with the game's own flags");
    int presented = 0;
    for (int i = 0; i < 90; ++i) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
      if (SUCCEEDED(swap_chain->Present(1, 0))) ++presented;
      hsf::slbring::Poll();
    }
    printf("     presented %d of 90\n", presented);
    Check(presented == 90, "frames present through it");

    // Flow and depth, as the effects would leave them, for real frames: DLSS-G must generate from them.
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = 1024;
    tex.Height = 600;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.SampleDesc.Count = 1;
    tex.Format = DXGI_FORMAT_R16G16_FLOAT;
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    ID3D12Resource* motion = nullptr;
    ID3D12Resource* depth = nullptr;
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, read, nullptr, IID_PPV_ARGS(&motion))), "flow texture");
    tex.Format = DXGI_FORMAT_R32_FLOAT;
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, read, nullptr, IID_PPV_ARGS(&depth))), "depth texture");
    IDXGISwapChain3* with_index = nullptr;
    Check(SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&with_index))), "swap chain index");
    // DLSS-G generates nothing for a window without focus, which says nothing about HSF: this needs saying when it fails.
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    int frames_with_focus = 0;
    uint32_t most_shown = 0, most_shown_partial = 0;
    int fed = 0;
    // With "nvof": the first half's flow comes from NVIDIA Optical Flow, off the back buffer, as in a game.
    const bool nvof = argc > 3 && wcscmp(argv[3], L"nvof") == 0;
    int nvof_active = 0;
    const ULONGLONG started = GetTickCount64();
    for (int i = 0; i < 400 && GetTickCount64() - started < 15000; ++i) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
      ID3D12Resource* back = nullptr;
      if (FAILED(with_index->GetBuffer(with_index->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)))) break;
      hsf::slbring::FrameFeed feed;
      feed.device = device;
      feed.queue = queue;
      feed.back_buffer = back;
      feed.back_buffer_state = D3D12_RESOURCE_STATE_COMMON;
      feed.motion = motion;
      feed.depth = depth;
      feed.scale[0] = feed.scale[1] = 1.f;
      feed.depth_inverted = true;
      feed.on = true;
      feed.frames = 1;
      feed.nvof = nvof && i < 200;
      // The second half as from a game's upscaler: part of the textures, a size that changes, jitter, no clean-up.
      const bool partial = i >= 200;
      if (GetForegroundWindow() == hwnd) ++frames_with_focus;
      if (partial) {
        feed.width = 800 + (i % 2) * 16;
        feed.height = 450 + (i % 2) * 9;
        feed.jitter[0] = 0.25f;
        feed.jitter[1] = -0.25f;
        feed.jittered = true;
        feed.compose = false;
        feed.mask = false;
      }
      // In the game this runs under Present, in the wrapper. Here it runs just before.
      if (hsf::slbring::Frame(feed)) ++fed;
      if (hsf::slbring::GetStats().nvof_state == 2) ++nvof_active;
      back->Release();
      swap_chain->Present(0, 0);
      hsf::slbring::Poll();
      if (hsf::slbring::GetStats().shown_per_real > most_shown) most_shown = hsf::slbring::GetStats().shown_per_real;
      if (partial && i > 300 && hsf::slbring::GetStats().shown_per_real > most_shown_partial) most_shown_partial = hsf::slbring::GetStats().shown_per_real;
      Sleep(16);
    }
    printf("     fed %d frames, %d with focus, most shown per real frame %u\n", fed, frames_with_focus, most_shown);
    Check(fed > 100, "inputs go in");
    if (nvof) {
      printf("     NVIDIA Optical Flow gave the flow on %d frames\n", nvof_active);
      Check(nvof_active > 150, "NVIDIA Optical Flow gives the flow, from the back buffer");
    }
    Check(hsf::slbring::GetStats().generating, "DLSS-G is on");
    Check(most_shown >= 2, "DLSS-G generates from them");
    printf("     most shown per real frame with an upscaler's data %u\n", most_shown_partial);
    Check(most_shown_partial >= 2, "DLSS-G generates from part of the textures, at a size that changes");
    if (motion != nullptr) motion->Release();
    if (depth != nullptr) depth->Release();
    if (with_index != nullptr) with_index->Release();
    stats = hsf::slbring::GetStats();
    printf("     DLSS-G status 0x%x, up to %u generated frames\n", static_cast<unsigned>(stats.status), stats.max_frames);
    Check(stats.status != -1, "DLSS-G answers");
    Check(stats.max_frames >= 1, "DLSS-G says how many frames it can make");
  }

  hsf::slbring::Stop(false);
  Check((*reinterpret_cast<void***>(first_factory))[15] == slot_before, "the factory is as it was after Stop");
  printf(g_failures == 0 ? "all ok\n" : "%d failed\n", g_failures);
  fflush(stdout);
  // Streamline stays in the process by design, so this leaves without tearing it down, as a game would be free to.
  TerminateProcess(GetCurrentProcess(), g_failures == 0 ? 0 : 1);
  return 0;
}
