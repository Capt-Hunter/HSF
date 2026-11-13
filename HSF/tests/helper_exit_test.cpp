// Proves hsf-dlssg-helper64.exe never outlives its game. 32-bit stand-in game set up like dlssg_remote.cpp: a
// window, a Direct3D 11 device, shared frame slots and fences duplicated into the helper, helper started suspended in a KILL_ON_JOB_CLOSE job, then
// a frame handed over every 16 ms so the helper really runs (child window, DLSS-G swap chain, presents). After `ms` milliseconds the stand-in
// ends itself with TerminateProcess (like a crash: no clean-up code runs). Prints the helper's pid; tools/helper_exit_test.ps1 checks it is gone.
// Usage: helper_exit_test <helper.exe> <ms> [nojob|stop]   (nojob = the helper's own watchdog only; stop = set the stop flag like the add-on's
// Stop, keep the game alive 5 s more and report whether the helper ended by itself, then die)
// HDR10 eye test: `hdr10` hands over R10G10B10A2 frames in HDR10 (ST.2084), left half 100 nits, right half 400 nits, and tells the helper the
// game's colour space is HDR10 (dlssg_link.h, version 14). With Windows HDR on, the right half must glow clearly brighter than the left.
// `hdr10bug` sends the same frames as sRGB, as before version 14: both halves look like flat greys.
#include <windows.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>

#include "../src/dlssg_link.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

using Microsoft::WRL::ComPtr;

static uint64_t Give(HANDLE process, HANDLE handle) {
  HANDLE theirs = nullptr;
  DuplicateHandle(GetCurrentProcess(), handle, process, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs));
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) return 1;
  const bool use_job = argc < 4 || wcscmp(argv[3], L"nojob") != 0;
  const bool stop_first = argc >= 4 && wcscmp(argv[3], L"stop") == 0;
  const DWORD run_ms = static_cast<DWORD>(_wtoi(argv[2]));
  const bool hdr = argc >= 4 && wcsncmp(argv[3], L"hdr10", 5) == 0;
  const bool hdr_bug = hdr && wcscmp(argv[3], L"hdr10bug") == 0;
  const DXGI_FORMAT format = hdr ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
  const UINT W = hdr ? 1280 : 640, H = hdr ? 720 : 360;

  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"HSF_EXIT_TEST";
  RegisterClassW(&wc);
  RECT rect{0, 0, static_cast<LONG>(W), static_cast<LONG>(H)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  const HWND window = CreateWindowW(L"HSF_EXIT_TEST", L"HSF helper exit test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, rect.right - rect.left,
                                    rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);

  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &immediate))) return 1;
  ComPtr<ID3D11Device5> device5;
  ComPtr<ID3D11DeviceContext4> context;
  device.As(&device5);
  immediate.As(&context);
  ComPtr<IDXGIDevice> dxgi;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adesc{};
  device.As(&dxgi);
  dxgi->GetAdapter(&adapter);
  adapter->GetDesc(&adesc);

  wchar_t name[96];
  swprintf_s(name, L"Local\\HSF_DLSSG_TEST_%lu", GetCurrentProcessId());
  const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(hsf::dlssg_link::Block), name);
  auto* b = static_cast<hsf::dlssg_link::Block*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hsf::dlssg_link::Block)));
  std::memset(b, 0, sizeof(*b));
  b->magic = hsf::dlssg_link::MAGIC;
  b->version = hsf::dlssg_link::VERSION;
  b->game_pid = GetCurrentProcessId();
  b->width = W;
  b->height = H;
  b->format = format;
  b->color_space = hdr && !hdr_bug ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  b->parent_window = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window));
  b->adapter_luid_low = adesc.AdapterLuid.LowPart;
  b->adapter_luid_high = adesc.AdapterLuid.HighPart;
  b->on = 1;
  b->frames = 1;

  std::wstring exe = argv[1];
  std::wstring folder = exe.substr(0, exe.find_last_of(L'\\'));
  std::wstring command = L"\"" + exe + L"\" " + name;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, folder.c_str(), &si, &pi)) {
    std::printf("CreateProcess failed %lu\n", GetLastError());
    return 1;
  }
  if (use_job) {
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    const bool ok = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) && AssignProcessToJobObject(job, pi.hProcess);
    std::printf("job %s\n", ok ? "ok" : "FAILED");
  }
  ComPtr<ID3D11Fence> ready, consumed;
  HANDLE h = nullptr;
  device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&ready));
  ready->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
  b->ready_fence = Give(pi.hProcess, h);
  device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&consumed));
  consumed->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
  b->consumed_fence = Give(pi.hProcess, h);
  ComPtr<ID3D11Texture2D> slots[hsf::dlssg_link::RING];
  const D3D11_TEXTURE2D_DESC desc{W, H, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0,
                                  D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  for (UINT i = 0; i < hsf::dlssg_link::RING; ++i) {
    ComPtr<IDXGIResource1> shared;
    device5->CreateTexture2D(&desc, nullptr, &slots[i]);
    slots[i].As(&shared);
    shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &h);
    b->slots[i] = Give(pi.hProcess, h);
  }
  ResumeThread(pi.hThread);
  std::printf("helper pid %lu\n", pi.dwProcessId);
  std::fflush(stdout);

  // Frames every 16 ms (a coloured clear, slot check like the add-on), window messages pumped.
  const ULONGLONG end = GetTickCount64() + run_ms;
  UINT64 id = 1;
  while (GetTickCount64() < end) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
    if (id <= hsf::dlssg_link::RING || consumed->GetCompletedValue() >= id - hsf::dlssg_link::RING) {
      ComPtr<ID3D11RenderTargetView> rtv;
      device5->CreateRenderTargetView(slots[id % hsf::dlssg_link::RING].Get(), nullptr, &rtv);
      if (hdr) {
        // ST.2084 code values: 0.5081 = 100 nits, 0.6517 = 400 nits.
        const float left[4] = {0.5081f, 0.5081f, 0.5081f, 1.f}, right[4] = {0.6517f, 0.6517f, 0.6517f, 1.f};
        const D3D11_RECT left_rect{0, 0, static_cast<LONG>(W / 2), static_cast<LONG>(H)}, right_rect{static_cast<LONG>(W / 2), 0, static_cast<LONG>(W), static_cast<LONG>(H)};
        context->ClearView(rtv.Get(), left, &left_rect, 1);
        context->ClearView(rtv.Get(), right, &right_rect, 1);
      } else {
        const float colour[4] = {static_cast<float>(id % 60) / 60.f, 0.2f, 0.5f, 1.f};
        context->ClearRenderTargetView(rtv.Get(), colour);
      }
      context->Signal(ready.Get(), id++);
      context->Flush();
    }
    Sleep(16);
  }
  std::printf("helper state %d, frames handed over %llu, helper presented %d\n", b->state, static_cast<unsigned long long>(id - 1), b->real);
  if (stop_first) {
    const ULONGLONG asked = GetTickCount64();
    b->stop = 1;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    std::printf("stop flag: helper %s after %llu ms (exit code %lu)\n", wait == WAIT_OBJECT_0 ? "ended by itself" : "STILL RUNNING",
                static_cast<unsigned long long>(GetTickCount64() - asked), code);
  }
  std::fflush(stdout);
  TerminateProcess(GetCurrentProcess(), 9);
  return 0;
}
