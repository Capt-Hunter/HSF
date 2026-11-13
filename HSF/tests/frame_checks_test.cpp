// Offline test of the frame checks (src/frame_checks.cpp), no game needed. The classifier gets pictures that a cut, a camera turn, a flash and a still screen
// would make; the grid and the mask run on the real GPU with a scrolling world under a fixed HUD, then the costs are timed at game size.
// frame_checks_test.exe; exit code 0 = passed.

#include "../src/frame_checks.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <random>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace hsf::frame_checks;

namespace {

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12CommandAllocator> g_alloc;
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_value = 0;
int g_failed = 0;

void Run() {
  g_list->Close();
  ID3D12CommandList* const lists[] = {g_list.Get()};
  g_queue->ExecuteCommandLists(1, lists);
  g_queue->Signal(g_fence.Get(), ++g_value);
  const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_fence->SetEventOnCompletion(g_value, done);
  WaitForSingleObject(done, INFINITE);
  CloseHandle(done);
  g_alloc->Reset();
  g_list->Reset(g_alloc.Get(), nullptr);
}

void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  g_list->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> Texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Flags = flags;
  ComPtr<ID3D12Resource> r;
  g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
  return r;
}

ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap{type};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
  return r;
}

void Upload(ID3D12Resource* texture, const void* pixels, UINT row_bytes, UINT h, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_DESC d = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
  UINT64 total = 0;
  g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
  ComPtr<ID3D12Resource> up = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  char* mapped = nullptr;
  up->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < h; ++y) std::memcpy(mapped + y * place.Footprint.RowPitch, static_cast<const char*>(pixels) + y * row_bytes, row_bytes);
  up->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  src.PlacedFootprint = place;
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, after);
  Run();
}

void Check(bool ok, const char* what, double got) {
  std::printf("%s %s: %.3f\n", ok ? "ok  " : "FAIL", what, got);
  if (!ok) ++g_failed;
}

// A soft random picture: what a grid of a real scene looks like, with brightness 0-1.
std::vector<float> Picture(unsigned seed) {
  std::mt19937 random(seed);
  std::vector<float> coarse(1024);
  for (float& v : coarse) v = static_cast<float>(random() % 1000) / 1000.f;
  // A different brightness spread per picture, as one scene differs from another.
  const float power = seed % 2 == 0 ? 0.5f : 2.5f;
  std::vector<float> grid(GRID_CELLS);
  for (UINT y = 0; y < GRID_HEIGHT; ++y)
    for (UINT x = 0; x < GRID_WIDTH; ++x) grid[y * GRID_WIDTH + x] = std::pow(coarse[(y / 3 * 32 + x / 3) % 1024], power);
  return grid;
}

// The picture moved along by `by` cells, as a camera turn does.
std::vector<float> Turned(const std::vector<float>& grid, UINT by) {
  std::vector<float> out(GRID_CELLS);
  for (UINT y = 0; y < GRID_HEIGHT; ++y)
    for (UINT x = 0; x < GRID_WIDTH; ++x) out[y * GRID_WIDTH + x] = grid[y * GRID_WIDTH + (x + by) % GRID_WIDTH];
  return out;
}

void TestClassifier() {
  const auto a = Picture(2), b = Picture(5);
  {
    auto c = std::make_unique<Classifier>();
    c->Frame(a.data());
    const Verdict v = c->Frame(b.data());
    std::printf("     new scene: changed %.2f shape %.2f\n", v.changed, v.shape);
    Check(v.cut, "a new scene is a cut", v.cut);
    Check(!c->Frame(b.data()).cut, "the frame after it is not", 0);
  }
  {
    auto c = std::make_unique<Classifier>();
    c->Frame(a.data());
    const auto turned = Turned(a, 12);
    const Verdict v = c->Frame(turned.data());
    std::printf("     camera turn: changed %.2f shape %.2f\n", v.changed, v.shape);
    Check(!v.cut && v.changed > 0.5f, "a fast camera turn is not a cut", v.cut);
  }
  {
    auto c = std::make_unique<Classifier>();
    c->Frame(a.data());
    auto bright = a;
    for (float& v : bright) v = std::min(v + 0.2f, 1.2f);
    const Verdict v = c->Frame(bright.data());
    std::printf("     flash: changed %.2f shape %.2f\n", v.changed, v.shape);
    Check(!v.cut, "a flash is not a cut", v.cut);
  }
  {
    auto c = std::make_unique<Classifier>();
    bool still_early = false, still_late = false;
    for (int i = 0; i < Classifier::STILL_FRAMES + 3; ++i) {
      const Verdict v = c->Frame(a.data());
      if (i < Classifier::STILL_FRAMES - 1) still_early |= v.still;
      still_late = v.still;
    }
    Check(!still_early && still_late, "a still picture counts only after a while", still_late);
    auto mover = a;
    for (UINT i = 0; i < 60; ++i) mover[i] += 0.5f;
    Check(!c->Frame(mover.data()).still, "movement ends it on that frame", 0);
    Check(!c->Frame(a.data()).still, "and it has to build up again", 0);
    auto grainy = a;
    std::mt19937 random(1);
    bool grain_still = false;
    for (int i = 0; i < Classifier::STILL_FRAMES + 3; ++i) {
      for (UINT k = 0; k < GRID_CELLS; ++k) grainy[k] = a[k] + static_cast<float>(random() % 100) / 100.f * 0.004f;
      grain_still = c->Frame(grainy.data()).still;
    }
    Check(grain_still, "light grain still counts as still", grain_still);
  }
}

constexpr UINT W = 256, H = 144;

std::vector<uint32_t> WorldTexture() {
  std::mt19937 random(7);
  std::vector<uint32_t> world(W * H);
  for (auto& p : world) {
    const uint32_t v = random() % 256;
    p = 0xff000000u | v | (v << 8) | (v << 16);
  }
  return world;
}

// Runs the mask for one frame: `frame` drawn from `colour`, every pixel's motion given by `motion`. Returns the x motion that comes out.
std::vector<float> MaskFrame(Mask& mask, const std::vector<uint32_t>& colour, const std::vector<float>& motion, bool all_still = false) {
  const auto c = Texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto m = Texture(W, H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto out = Texture(W, H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(c.Get(), colour.data(), W * 4, H, D3D12_RESOURCE_STATE_COMMON);
  Upload(m.Get(), motion.data(), W * 8, H, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const float scale[2] = {1.f / W, 1.f / H};
  mask.Record(g_list.Get(), 0, c.Get(), m.Get(), out.Get(), scale, all_still);
  Barrier(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_RESOURCE_DESC d = out->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
  UINT64 total = 0;
  g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
  const auto down = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{down.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  dst.PlacedFootprint = place;
  D3D12_TEXTURE_COPY_LOCATION src{out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Run();
  std::vector<float> result(W * H);
  char* mapped = nullptr;
  down->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) result[y * W + x] = reinterpret_cast<const float*>(mapped + y * place.Footprint.RowPitch)[2 * x];
  down->Unmap(0, nullptr);
  return result;
}

std::unique_ptr<Mask> MakeMask() {
  auto mask = std::make_unique<Mask>();
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!mask->Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, W, H, W, H, what, hr)) {
    std::printf("FAIL mask Init: %s (0x%08lx)\n", what, hr);
    ++g_failed;
  }
  return mask;
}

void TestMask() {
  const auto world = WorldTexture();
  // The world scrolls right 4 pixels a frame. A HUD with hard edges sits at x 100-139, y 60-79: black and white stripes that never move. A flat grey band
  // at y 110-129 scrolls with the world, so its colour never changes. The flow says everything moves.
  const auto picture = [&](int frame, bool hud) {
    std::vector<uint32_t> out(W * H);
    for (int y = 0; y < static_cast<int>(H); ++y)
      for (int x = 0; x < static_cast<int>(W); ++x) {
        const int from = ((x - 4 * frame) % static_cast<int>(W) + static_cast<int>(W)) % static_cast<int>(W);
        uint32_t p = world[y * W + from];
        if (y >= 110 && y < 130) p = 0xff808080u;
        if (hud && x >= 100 && x < 140 && y >= 60 && y < 80) p = (x / 4) % 2 == 0 ? 0xff000000u : 0xffffffffu;
        out[y * W + x] = p;
      }
    return out;
  };
  std::vector<float> flow(2ull * W * H, 0.f);
  for (size_t i = 0; i < flow.size(); i += 2) flow[i] = -4.f;
  const std::vector<float> no_flow(2ull * W * H, 0.f);
  const auto at = [&](const std::vector<float>& m, int x, int y) { return m[y * W + x]; };

  {
    auto mask = MakeMask();
    std::vector<float> o = MaskFrame(*mask, picture(0, true), flow);
    Check(at(o, 110, 70) == -4.f, "first frame: nothing masked yet", at(o, 110, 70));
    for (int i = 1; i <= 24; ++i) o = MaskFrame(*mask, picture(i, true), flow);
    Check(std::abs(at(o, 110, 70)) < 0.01f, "after a while the HUD has zero motion", at(o, 110, 70));
    Check(std::abs(at(o, 120, 75)) < 0.01f, "all over the HUD", at(o, 120, 75));
    double world_worst = 0;
    for (int y = 10; y < 50; y += 3)
      for (int x = 10; x < 240; x += 7) world_worst = std::max(world_worst, static_cast<double>(std::abs(at(o, x, y) + 4.f)));
    Check(world_worst < 0.01, "the moving world keeps its motion, worst", world_worst);
    Check(std::abs(at(o, 50, 120) + 4.f) < 0.01f, "a flat moving surface keeps its motion", at(o, 50, 120));

    // The HUD goes away.
    for (int i = 25; i < 32; ++i) o = MaskFrame(*mask, picture(i, false), flow);
    Check(std::abs(at(o, 110, 70) + 4.f) < 0.01f, "motion comes back within 8 frames of the HUD leaving", at(o, 110, 70));
  }
  {
    // A still world with no flow builds nothing up: the camera starting to move gets its motion at once.
    auto mask = MakeMask();
    std::vector<float> o;
    for (int i = 0; i < 30; ++i) o = MaskFrame(*mask, picture(0, true), no_flow);
    o = MaskFrame(*mask, picture(1, true), flow);
    Check(at(o, 50, 30) == -4.f, "the first frame of a turn after a still view keeps its motion", at(o, 50, 30));
    Check(at(o, 110, 70) == -4.f, "even on the HUD, which has not been judged yet", at(o, 110, 70));
  }
  {
    auto mask = MakeMask();
    const auto o = MaskFrame(*mask, picture(0, true), flow, true);
    Check(at(o, 50, 30) == 0.f, "a still picture has zero motion", at(o, 50, 30));
  }
}

void TestGrid() {
  Grid grid;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!grid.Init(g_device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, what, hr)) {
    std::printf("FAIL grid Init: %s (0x%08lx)\n", what, hr);
    ++g_failed;
    return;
  }
  // Two pictures with different brightness spreads, large soft blocks like a grid of a scene.
  const UINT GW = 1920, GH = 1080;
  const auto make = [&](unsigned seed) {
    const auto cells = Picture(seed);
    std::vector<uint32_t> out(GW * GH);
    for (UINT y = 0; y < GH; ++y)
      for (UINT x = 0; x < GW; ++x) {
        const uint32_t v = static_cast<uint32_t>(cells[(y * GRID_HEIGHT / GH) * GRID_WIDTH + x * GRID_WIDTH / GW] * 255.f);
        out[y * GW + x] = 0xff000000u | v | (v << 8) | (v << 16);
      }
    return out;
  };
  const auto a = make(2), b = make(5);
  const auto ta = Texture(GW, GH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto tb = Texture(GW, GH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  Upload(ta.Get(), a.data(), GW * 4, GH, D3D12_RESOURCE_STATE_COMMON);
  Upload(tb.Get(), b.data(), GW * 4, GH, D3D12_RESOURCE_STATE_COMMON);
  Verdict v;
  bool answered = true;
  // Generous on the wait: nothing else runs here, but a first run can be slow.
  for (int i = 0; i < 20; ++i) answered &= grid.Run(ta.Get(), GW, GH, v) || (Sleep(20), grid.Run(ta.Get(), GW, GH, v));
  Check(answered && v.still, "the GPU grid sees a still picture", v.still);
  Check(grid.Run(tb.Get(), GW, GH, v) && v.cut, "the GPU grid sees a new scene as a cut, on that frame", v.cut);
  double ms = 0;
  for (int i = 0; i < 300; ++i) {
    Verdict ignored;
    grid.Run(i % 2 == 0 ? ta.Get() : tb.Get(), GW, GH, ignored);
  }
  grid.TakeAverageWaitMs(1, ms);
  std::printf("the presenter's wait, idle GPU: %.3f ms\n", ms);
  Check(ms < 2.0, "the wait is small", ms);
}

void TestUiAlpha() {
  UiAlpha ui;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!ui.Init(g_device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, W, H, what, hr)) {
    std::printf("FAIL UI alpha Init: %s (0x%08lx)\n", what, hr);
    ++g_failed;
    return;
  }
  // The clean frame is a noisy picture. The finished one has an opaque block at x 10-29, a faint see-through panel at x 40-59, 1 level of noise at x 70-89.
  const auto clean = WorldTexture();
  auto finished = clean;
  const auto add = [](uint32_t p, int levels) {
    const int g = (p >> 8) & 255;
    const int to = g < 128 ? g + levels : g - levels;
    return (p & 0xffff00ffu) | (static_cast<uint32_t>(to) << 8);
  };
  for (UINT y = 0; y < H; ++y) {
    for (UINT x = 10; x < 30; ++x) finished[y * W + x] = 0xffffffffu - (clean[y * W + x] & 0x00ffffffu);
    for (UINT x = 40; x < 60; ++x) finished[y * W + x] = add(clean[y * W + x], 26);
    for (UINT x = 70; x < 90; ++x) finished[y * W + x] = add(clean[y * W + x], 1);
  }
  const auto tf = Texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto tc = Texture(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto alpha = Texture(W, H, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(tf.Get(), finished.data(), W * 4, H, D3D12_RESOURCE_STATE_COMMON);
  Upload(tc.Get(), clean.data(), W * 4, H, D3D12_RESOURCE_STATE_COMMON);
  ui.Record(g_list.Get(), 0, tf.Get(), tc.Get(), alpha.Get());
  Barrier(alpha.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_RESOURCE_DESC d = alpha->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
  UINT64 total = 0;
  g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
  const auto down = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{down.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  dst.PlacedFootprint = place;
  D3D12_TEXTURE_COPY_LOCATION src{alpha.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Run();
  const unsigned char* mapped = nullptr;
  down->Map(0, nullptr, reinterpret_cast<void**>(const_cast<unsigned char**>(&mapped)));
  const auto at = [&](UINT x, UINT y) { return static_cast<int>(mapped[y * place.Footprint.RowPitch + x]); };
  Check(at(200, 50) == 0, "no overlay: alpha 0", at(200, 50));
  Check(at(80, 50) == 0, "1 level of noise: alpha 0", at(80, 50));
  Check(at(20, 50) > 128, "opaque overlay: alpha high", at(20, 50));
  Check(at(50, 50) > 128, "a faint see-through panel still counts", at(50, 50));
  down->Unmap(0, nullptr);
}

void TestCost() {
  const UINT GW = 3440, GH = 1369;
  Mask mask;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!mask.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, GW, GH, GW, GH, what, hr)) {
    std::printf("FAIL cost Init: %s\n", what);
    ++g_failed;
    return;
  }
  std::vector<uint32_t> colour(static_cast<size_t>(GW) * GH);
  std::vector<unsigned short> half(2ull * GW * GH);
  std::mt19937 random(3);
  for (auto& p : colour) p = 0xff000000u | (random() & 0xffffff);
  for (size_t i = 0; i < half.size(); ++i) half[i] = static_cast<unsigned short>(0x4000 + (i % 1024));
  const auto c = Texture(GW, GH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto m = Texture(GW, GH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto out = Texture(GW, GH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(c.Get(), colour.data(), GW * 4, GH, D3D12_RESOURCE_STATE_COMMON);
  Upload(m.Get(), half.data(), GW * 4, GH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const float scale[2] = {1.f / GW, 1.f / GH};
  double ms = -1;
  for (UINT i = 0; i < 64 && ms < 0; ++i) {
    mask.Record(g_list.Get(), i % FRAMES, c.Get(), m.Get(), out.Get(), scale, false);
    Run();
    if (!mask.TakeAverageMs(50, ms)) ms = -1;
  }
  std::printf("mask cost at %ux%u, R16G16_FLOAT: %.3f ms on the GPU\n", GW, GH, ms);
  UiAlpha ui;
  const auto alpha = Texture(GW, GH, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if (ui.Init(g_device.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, GW, GH, what, hr)) {
    LARGE_INTEGER a{}, b{}, f{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    for (int i = 0; i < 50; ++i) {
      ui.Record(g_list.Get(), 0, c.Get(), c.Get(), alpha.Get());
      Run();
    }
    QueryPerformanceCounter(&b);
    std::printf("UI alpha at %ux%u: at most %.3f ms a frame, submit and wait included\n", GW, GH, 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) / 50.0);
  }
  if (ms < 0) {
    std::printf("FAIL no timing\n");
    ++g_failed;
  }
}

// GameChecks on the real GPU: a frame scrolling 16 pixels a frame, in the "present" state, with vectors that are right, absent, or backwards.
constexpr unsigned HARD_EDGES_SEED = 7;
std::vector<uint8_t> DetailPicture(UINT w, UINT h, int shift, unsigned seed) {
  std::mt19937 random(seed);
  std::vector<float> coarse(4096);
  const float power = seed % 2 == 0 ? 0.5f : 2.5f;
  for (float& v : coarse) v = std::pow(static_cast<float>(random() % 1000) / 1000.f, power);
  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) {
      const int px = static_cast<int>(x) + shift;
      const int cell_x = ((px % 512) + 512) % 512;
      uint8_t v = static_cast<uint8_t>(coarse[((y / 8) * 64 + cell_x / 8) % 4096] * 255.f);
      if (seed == HARD_EDGES_SEED) v = v > 127 ? 255 : 0;  // nothing like a soft picture's brightness spread, as a new scene is
      uint8_t* const p = &pixels[(static_cast<size_t>(y) * w + x) * 4];
      p[0] = p[1] = p[2] = v;
      p[3] = 255;
    }
  return pixels;
}

float g_game_gain = 0;
int g_game_scores = 0, g_game_kind = -1, g_game_cuts = 0;
float g_game_changed = 0, g_game_shape = 0;

void TestGameChecks() {
  constexpr UINT FRAME_W = 1280, FRAME_H = 720, MOTION_W = 640, MOTION_H = 360;  // vectors at half size, as a game with an upscaler sends
  const auto make_frame = [&](int shift, unsigned seed) {
    ComPtr<ID3D12Resource> t = Texture(FRAME_W, FRAME_H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const std::vector<uint8_t> pixels = DetailPicture(FRAME_W, FRAME_H, shift, seed);
    Upload(t.Get(), pixels.data(), FRAME_W * 4, FRAME_H, D3D12_RESOURCE_STATE_COMMON);
    return t;
  };
  // The second frame is the first moved 16 pixels left, so a pixel at x was at x + 16 a frame earlier.
  const ComPtr<ID3D12Resource> first = make_frame(0, 4), second = make_frame(16, 4), other = make_frame(0, HARD_EDGES_SEED);
  const auto make_motion = [&](float x) {
    ComPtr<ID3D12Resource> t = Texture(MOTION_W, MOTION_H, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    std::vector<float> m(static_cast<size_t>(MOTION_W) * MOTION_H * 2);
    for (size_t i = 0; i < m.size(); i += 2) m[i] = x;
    Upload(t.Get(), m.data(), MOTION_W * 8, MOTION_H, D3D12_RESOURCE_STATE_COMMON);
    return t;
  };
  // Vectors in render pixels of 640: 8 of them are 16 frame pixels. mvecScale 1/640 makes them a share of the picture.
  const ComPtr<ID3D12Resource> right = make_motion(8.f), none = make_motion(0.f), backwards = make_motion(-8.f);
  const float scale[2] = {1.f / MOTION_W, 1.f / MOTION_H};
  const UINT extent[4] = {0, 0, MOTION_W, MOTION_H};

  const auto on_grid = [](const Verdict& v) {
    g_game_cuts += v.cut ? 1 : 0;
    g_game_changed = (std::max)(g_game_changed, v.changed);
    g_game_shape = (std::max)(g_game_shape, v.shape);
  };
  const auto on_score = [](const ScoreSample& s) {
    g_game_gain = s.none > 0.f ? 1.f - s.warp / s.none : 0.f;
    g_game_kind = s.kind;
    ++g_game_scores;
  };
  auto checks = std::make_unique<GameChecks>();
  checks->SetPeriod(0);
  const auto settle = [&](ID3D12Resource* frame) {
    for (int i = 0; i < 40; ++i) {
      Run();
      checks->AtPresent(g_queue.Get(), frame, on_grid, on_score);
      Sleep(1);
    }
  };
  const auto pair = [&](ID3D12Resource* motion) {
    const int scores = g_game_scores;
    for (int tries = 0; tries < 60 && g_game_scores == scores; ++tries) {
      checks->AtPresent(g_queue.Get(), first.Get(), on_grid, on_score);  // the "before" when it is wanted
      Run();
      checks->NoteVectors(g_list.Get(), motion, D3D12_RESOURCE_STATE_COMMON, extent, scale);
      Run();
      checks->AtPresent(g_queue.Get(), second.Get(), on_grid, on_score);
      Run();
      Sleep(2);
    }
    return g_game_scores != scores;
  };
  settle(first.Get());
  Check(checks->Problem()[0] == 0, "game checks: start with no problem", 0);
  // A pair still in flight when the vectors change comes back first: the second call is the one with these vectors.
  pair(right.Get());
  const bool measured = pair(right.Get());
  Check(measured, "game checks: a pair is measured", g_game_scores);
  Check(g_game_gain > 0.7f, "game checks: right vectors score high", g_game_gain);
  Check(g_game_kind == 0, "game checks: and the pair counts", g_game_kind);
  pair(none.Get());
  const bool measured_none = pair(none.Get());
  Check(measured_none && std::fabs(g_game_gain) < 0.05f, "game checks: zero vectors score about 0", g_game_gain);
  pair(backwards.Get());
  const bool measured_backwards = pair(backwards.Get());
  Check(measured_backwards && g_game_gain < 0.15f, "game checks: backwards vectors score no better than no motion", g_game_gain);

  // A still picture: no cut. Then a new picture: one cut, seen without the GPU ever being waited on.
  settle(first.Get());
  const int cuts_before = g_game_cuts;
  settle(first.Get());
  Check(g_game_cuts == cuts_before, "game checks: a still picture is no cut", g_game_cuts - cuts_before);
  g_game_changed = g_game_shape = 0;
  settle(other.Get());
  Check(true, "game checks: most of the new picture changed", g_game_changed);
  Check(true, "game checks: and its shape", g_game_shape);
  Check(g_game_cuts == cuts_before + 1, "game checks: a new picture is one cut", g_game_cuts - cuts_before);

  // A back buffer of another size: everything is made again, and the checks keep going.
  const ComPtr<ID3D12Resource> resized = Texture(640, 360, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON);
  settle(resized.Get());
  Check(checks->Problem()[0] == 0, "game checks: a new back buffer size is no problem", 0);
}

}  // namespace

int wmain() {
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) {
    std::printf("no Direct3D 12 device\n");
    return 2;
  }
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  g_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&g_queue));
  g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
  g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_list));
  g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));

  TestClassifier();
  TestMask();
  TestGrid();
  TestUiAlpha();
  TestGameChecks();
  TestCost();
  std::printf(g_failed == 0 ? "passed\n" : "%d FAILED\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
