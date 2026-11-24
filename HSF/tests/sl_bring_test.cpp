// A small program with no Streamline of its own, standing in for a game: real device, real window, and a swap chain made the plain way. HSF must turn that swap chain into one made through
// Streamline, from the folder given. Run: sl_bring_test <folder with sl.interposer.dll and its plugins> <log file>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <share.h>
#include <wrl/client.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

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
    printf("usage: sl_bring_test <folder> <log file> [nvof|apex|apex-missing|apex-inactive|apex-addon|apex-addon-no-depth|apex-waitable|apex-addon-waitable] [0|2|4] [frame sleep ms] [3] [2 for frame latency]\n");
    return 2;
  }
  const std::wstring mode = argc > 3 ? argv[3] : L"";
  const UINT target_sync = argc > 4 && wcscmp(argv[4], L"0") == 0 ? 0u :
                           argc > 4 && wcscmp(argv[4], L"4") == 0 ? 4u : argc > 4 && wcscmp(argv[4], L"2") == 0 ? 2u : 1u;
  const DWORD frame_sleep = argc > 5 && wcscmp(argv[5], L"24") == 0 ? 24u : 16u;
  const bool multi = argc > 6 && wcscmp(argv[6], L"3") == 0;
  const UINT frame_latency = argc > 7 && wcscmp(argv[7], L"2") == 0 ? 2u : 1u;
  const bool waitable = mode == L"apex-waitable" || mode == L"apex-addon-waitable";
  const bool no_depth = mode == L"apex-addon-no-depth";
  const bool addon = mode == L"apex-addon" || mode == L"apex-addon-waitable" || no_depth;
  const bool missing = mode == L"apex-missing";
  const bool inactive = mode == L"apex-inactive" || no_depth;
  const bool apex = addon || waitable || mode == L"apex" || missing || inactive;
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

  hsf::slbring::StartOptions options;
  if (apex) {
    options.apex = options.apex_debug = true;
    options.apex_allow_tearing = target_sync == 0;
    options.apex_dll = std::wstring(argv[1]) + (missing ? L"\\missing.dll" : L"\\nvngx_dlssg.dll");
  }
  if (!addon) hsf::slbring::Start(argv[2], argv[1], options);
  void* const slot_before = (*reinterpret_cast<void***>(first_factory))[15];
  if (!addon) hsf::slbring::NoteDevice(device);
  if (addon) {
    Check(GetModuleHandleW(L"hsf.addon64") != nullptr, "ReShade loaded the compiled HSF add-on");
  } else {
    Check(hsf::slbring::GetStats().streamline_up, apex ? "Apex is ready to catch swap chains" : "Streamline started and swap chains are caught");
    Check((*reinterpret_cast<void***>(first_factory))[15] != slot_before, "a factory made earlier is caught too");
  }

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = 1280;
  desc.Height = 720;
  desc.Format = apex ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 3;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  if (waitable) {
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    Microsoft::WRL::ComPtr<IDXGIFactory5> tearing;
    BOOL supported = FALSE;
    if (SUCCEEDED(first_factory->QueryInterface(IID_PPV_ARGS(&tearing))) &&
        SUCCEEDED(tearing->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &supported, sizeof(supported))) && supported)
      desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }
  IDXGISwapChain1* swap_chain = nullptr;
  const HRESULT hr = first_factory->CreateSwapChainForHwnd(queue, hwnd, &desc, nullptr, nullptr, &swap_chain);
  printf("     swap chain 0x%08lx %p\n", hr, swap_chain);
  Check(SUCCEEDED(hr) && swap_chain != nullptr, "the swap chain is made");
  hsf::slbring::Stats stats = hsf::slbring::GetStats();
  if (!addon) Check(stats.swap_chains == 1 && stats.failed == 0, apex ? "Apex attached to native DXGI" : "it went through Streamline");
  if (!addon && apex) Check(stats.plugin_loaded == -1 && !GetModuleHandleW(L"nvngx_dlssg.dll"),
                           "Apex provider stays unloaded until there is an eligible frame");
  if (!apex) Check(stats.plugin_loaded == 1, "DLSS-G's plugin loaded");
  if (!apex) Check(stats.supported == 0, "DLSS-G supported on this adapter");

  if (swap_chain != nullptr) {
    Check(SUCCEEDED(swap_chain->ResizeBuffers(0, 1024, 600, DXGI_FORMAT_UNKNOWN, desc.Flags)), "a resize with the game's own flags");
    Microsoft::WRL::ComPtr<IDXGISwapChain2> latency_chain;
    struct WaitHandle {
      HANDLE handle = nullptr;
      ~WaitHandle() { if (handle) CloseHandle(handle); }
    } latency;
    if (waitable) {
      Check(SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&latency_chain))) &&
            SUCCEEDED(latency_chain->SetMaximumFrameLatency(frame_latency)), "game set maximum frame latency");
      if (latency_chain) latency.handle = latency_chain->GetFrameLatencyWaitableObject();
      Check(latency.handle != nullptr, "game acquired its frame latency waitable object");
      if (!latency.handle) return 1;
    }
    const auto ready_to_render = [&] {
      if (!latency.handle || WaitForSingleObjectEx(latency.handle, 2000, FALSE) == WAIT_OBJECT_0) return true;
      Check(false, "game's waitable object signalled before rendering");
      return false;
    };
    int presented = 0;
    for (int i = 0; i < 90; ++i) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
      if (!ready_to_render()) break;
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
    // HDR10 on the game's swap chain, as RenoDX sets it (SetColorSpace1, then Special K's private data). HSF logs whether the swap chain under Streamline's agrees.
    const DXGI_COLOR_SPACE_TYPE space = apex ? DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 : DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
    static constexpr GUID SKID = {0x18b57e4, 0x1493, 0x4953, {0xad, 0xf2, 0xde, 0x6d, 0x99, 0xcc, 0x5, 0xe5}};
    const HRESULT space_hr = with_index->SetColorSpace1(space);
    with_index->SetPrivateData(SKID, sizeof(space), &space);
    printf("     game's swap chain colour space %u: 0x%08lx\n", static_cast<unsigned>(space), space_hr);
    if (apex) Check(SUCCEEDED(space_hr), "DXGI accepted scRGB");
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetForegroundWindow(hwnd);
    int frames_with_focus = 0;
    uint32_t most_shown = 0, most_shown_partial = 0;
    int fed = 0;
    // With "nvof": the first half's flow comes from NVIDIA Optical Flow, off the back buffer, as in a game.
    const bool nvof = argc > 3 && wcscmp(argv[3], L"nvof") == 0;
    int nvof_active = 0;
    int generated_presents = 0;
    bool native_format = true, passthrough = true;
    const bool test_toggle = apex && !addon && !inactive && !missing;
    constexpr int toggle_off = 150, toggle_on = 175;
    int real_only_while_off = 0;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> render_allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> render_list;
    Microsoft::WRL::ComPtr<ID3D12Fence> render_fence;
    const HANDLE draw_done = apex ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    UINT64 render_value = 0;
    if (apex) {
      D3D12_DESCRIPTOR_HEAP_DESC h{};
      h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
      h.NumDescriptors = 1;
      Check(SUCCEEDED(device->CreateDescriptorHeap(&h, IID_PPV_ARGS(&rtv_heap))) &&
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&render_allocator))) &&
            SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, render_allocator.Get(), nullptr, IID_PPV_ARGS(&render_list))) &&
            SUCCEEDED(render_list->Close()) && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&render_fence))) &&
            draw_done, "synthetic FP16 render commands");
      if (g_failures) return 1;
    }
    const ULONGLONG started = GetTickCount64();
    const ULONGLONG test_ms = target_sync == 4 ? 30000 : 20000;
    const int frames_to_test = missing || inactive ? 24 : 400;
    for (int i = 0; i < frames_to_test && GetTickCount64() - started < test_ms; ++i) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&msg);
      if (!ready_to_render()) break;
      ID3D12Resource* back = nullptr;
      if (FAILED(with_index->GetBuffer(with_index->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back)))) break;
      if (apex) {
        native_format &= back->GetDesc().Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (render_value != 0 && render_fence->GetCompletedValue() < render_value) {
          if (FAILED(render_fence->SetEventOnCompletion(render_value, draw_done)) || WaitForSingleObject(draw_done, 2000) != WAIT_OBJECT_0) {
            Check(false, "synthetic draw fence");
            back->Release();
            break;
          }
        }
        const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(back, nullptr, rtv);
        render_allocator->Reset();
        render_list->Reset(render_allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition = {back, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET};
        render_list->ResourceBarrier(1, &b);
        const float background[4] = {-0.125f, 0.3f, 2.0f, 1.f};
        const float block[4] = {4.f, -0.25f, 0.1f, 1.f};
        const LONG x = (i * 4) % 900;
        const D3D12_RECT rectangle{x, 180, x + 120, 340};
        render_list->ClearRenderTargetView(rtv, background, 0, nullptr);
        render_list->ClearRenderTargetView(rtv, block, 1, &rectangle);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        render_list->ResourceBarrier(1, &b);
        render_list->Close();
        ID3D12CommandList* const commands[] = {render_list.Get()};
        queue->ExecuteCommandLists(1, commands);
        queue->Signal(render_fence.Get(), ++render_value);
      }
      hsf::slbring::FrameFeed feed;
      feed.device = device;
      feed.queue = queue;
      feed.back_buffer = back;
      feed.back_buffer_state = D3D12_RESOURCE_STATE_COMMON;
      feed.motion = motion;
      feed.depth = depth;
      feed.scale[0] = feed.scale[1] = 1.f;
      feed.depth_inverted = true;
      feed.on = !inactive;
      feed.frames = multi ? 2u : 1u;
      feed.nvof = nvof && i < 200;
      feed.colour_space = static_cast<uint32_t>(space);
      // The second half as from a game's upscaler: part of the textures, a size that changes, jitter, no clean-up.
      const bool partial = i >= 200;
      const bool skip_inputs = apex && (i == 35 || i == 85);
      const UINT sync = apex && i != 40 ? target_sync : 0;
      const UINT present_flags = apex && target_sync == 0 && i != 40 ? DXGI_PRESENT_ALLOW_TEARING : 0;
      if (apex && i == 65) feed.reset = true;
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
      if (test_toggle && i == toggle_on) hsf::slbring::SetApexEnabled(true);
      if (!addon && !skip_inputs && hsf::slbring::Frame(feed)) ++fed;
      if (test_toggle && i == toggle_off) hsf::slbring::SetApexEnabled(false);
      if (hsf::slbring::GetStats().nvof_state == 2) ++nvof_active;
      back->Release();
      UINT before = 0, after = 0;
      if (apex) swap_chain->GetLastPresentCount(&before);
      const HRESULT presented_hr = swap_chain->Present(sync, present_flags);
      if (apex && presented_hr != S_OK) {
        printf("     Present 0x%08lx, device 0x%08lx\n", presented_hr, device->GetDeviceRemovedReason());
        Check(false, "each real frame presents");
        break;
      }
      if (apex && SUCCEEDED(swap_chain->GetLastPresentCount(&after))) {
        if (after - before == (multi ? 3u : 2u)) ++generated_presents;
        if ((sync == 0 && present_flags == 0) || (!addon && (skip_inputs || feed.reset))) passthrough &= after - before == 1;
        if (test_toggle && i >= toggle_off && i <= toggle_on && after - before == 1) ++real_only_while_off;
      }
      hsf::slbring::Poll();
      if (hsf::slbring::GetStats().shown_per_real > most_shown) most_shown = hsf::slbring::GetStats().shown_per_real;
      if (partial && i > 300 && hsf::slbring::GetStats().shown_per_real > most_shown_partial) most_shown_partial = hsf::slbring::GetStats().shown_per_real;
      Sleep(frame_sleep);
    }
    if (apex) {
      printf("     generated pairs or triplets: %d (app sync interval %u)\n", generated_presents, target_sync);
      Check(native_format, "every swap-chain resource remains R16G16B16A16_FLOAT");
      if (missing || inactive) {
        Check(generated_presents == 0, "only real frames were accepted by DXGI");
        const auto expected = missing ? 0 : -1;
        Check(hsf::slbring::GetStats().plugin_loaded == expected && !GetModuleHandleW(L"nvngx_dlssg.dll"),
              missing ? "missing provider falls back without loading another runtime" :
              no_depth ? "Apex stays unloaded when ReShade depth is unavailable" : "Apex stays unloaded with Frame Gen Off");
      } else {
        Check(generated_presents > 100, "DXGI observed the expected number of presents for generated frames");
      }
      Check(passthrough, addon ? "unsupported Present presents one real frame" : "missing inputs, unsupported Present and reset each present one real frame");
      if (test_toggle) Check(real_only_while_off == toggle_on - toggle_off + 1, "Off discards queued work, presents real frames, and resets history on resume");
      Check(device->GetDeviceRemovedReason() == S_OK, "D3D12 device remained valid");
      if (waitable) {
        UINT max_latency = 0;
        DXGI_SWAP_CHAIN_DESC1 actual{};
        Check(SUCCEEDED(latency_chain->GetMaximumFrameLatency(&max_latency)) && max_latency == frame_latency,
              "game's maximum frame latency is preserved");
        Check(SUCCEEDED(latency_chain->GetDesc1(&actual)) && actual.Flags == desc.Flags,
              "game's waitable swap-chain flags are preserved");
      }
      if (draw_done) CloseHandle(draw_done);
    }
    if (!addon && !inactive && !missing) {
      printf("     fed %d frames, %d with focus, most shown per real frame %u\n", fed, frames_with_focus, most_shown);
      Check(fed > 100, "inputs go in");
      if (nvof) {
        printf("     NVIDIA Optical Flow gave the flow on %d frames\n", nvof_active);
        Check(nvof_active > 150, "NVIDIA Optical Flow gives the flow, from the back buffer");
      }
      Check(hsf::slbring::GetStats().generating, apex ? "Apex generation is on" : "DLSS-G is on");
      Check(most_shown >= 2, apex ? "Apex generates from motion and depth" : "DLSS-G generates from them");
      printf("     most shown per real frame with an upscaler's data %u\n", most_shown_partial);
      Check(most_shown_partial >= 2, "generation uses a changing subregion of the input textures");
      stats = hsf::slbring::GetStats();
      printf("     DLSS-G status 0x%x, up to %u generated frames\n", static_cast<unsigned>(stats.status), stats.max_frames);
      Check(stats.status != -1, "DLSS-G answers");
      Check(stats.max_frames >= 1, "DLSS-G says how many frames it can make");
      if (!apex) Check(!stats.fp16, "an R10G10B10A2 swap chain is not taken for FP16");
    }
    if (motion != nullptr) motion->Release();
    if (depth != nullptr) depth->Release();
    if (with_index != nullptr) with_index->Release();
  }

  if (addon) {
    if (swap_chain) swap_chain->Release();
    queue->Release();
    device->Release();
    first_factory->Release();
    DestroyWindow(hwnd);
    return g_failures != 0;
  }
  if (apex) hsf::slbring::ShutdownForExit();
  hsf::slbring::Stop(false);
  Check((*reinterpret_cast<void***>(first_factory))[15] == slot_before, "the factory is as it was after Stop");
  // The log, closed by Stop: the colour space line says what the swap chain under Streamline has.
  std::string space_line;
  // Opened sharing everything, with a few tries: _wfopen_s shares only reading and failed with EACCES (13) now and then right after Stop closed
  // the log, while something else had it open (seen more often the longer the log). Nothing in the test writes to it any more.
  FILE* log = nullptr;
  errno_t opened = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    log = _wfsopen(argv[2], L"r", _SH_DENYNO);
    opened = log != nullptr ? 0 : errno;
    if (log != nullptr) break;
    Sleep(50);
  }
  int lines = 0;
  if (opened == 0 && log != nullptr) {
    char line[512];
    while (fgets(line, sizeof(line), log) != nullptr) {
      ++lines;
      if (strstr(line, "colour space:") != nullptr) space_line = line;
    }
    fclose(log);
  }
  if (apex) {
    Check(hsf::slbring::GetStats().apex, "Apex backend selected");
  } else {
    if (space_line.empty()) printf("     no colour space line (open %d, %d lines read)\n", static_cast<int>(opened), lines);
    else printf("     %s", space_line.c_str());
    Check(space_line.find("the game's is 12;") != std::string::npos && space_line.find("they differ") == std::string::npos,
          "the colour space line names the game's HDR10, and the swap chain DLSS-G presents on does not disagree");
  }
  printf(g_failures == 0 ? "all ok\n" : "%d failed\n", g_failures);
  fflush(stdout);
  // Streamline stays in the process by design, so this leaves without tearing it down, as a game would be free to.
  if (!apex) TerminateProcess(GetCurrentProcess(), g_failures == 0 ? 0 : 1);
  if (swap_chain) swap_chain->Release();
  queue->Release();
  device->Release();
  first_factory->Release();
  DestroyWindow(hwnd);
  return g_failures != 0;
}
