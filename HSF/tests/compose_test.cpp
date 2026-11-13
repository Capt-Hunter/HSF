// Offline test of the compose pass (src/compose.cpp), no game needed: a near square with its own motion over a far background, optical flow soft
// across the square's edge, and sharp depth. Checks the pass moves the soft edge the right way and leaves plain areas alone, that it steadies shaking
// motion over time without holding back real changes, then times it at game size on the real GPU. compose_test.exe; exit code 0 = passed.

#include "../src/camera.h"
#include "../src/compose.h"
#include "../src/frame_checks.h"

#include <d3dcompiler.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

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

// Fills a texture in COPY_DEST from tightly packed rows; it ends in `after`.
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

std::vector<float> Compose(hsf::compose::Pass& pass, UINT w, UINT h, const std::vector<float>& motion, const std::vector<float>& depth, bool inverted, bool reset = true) {
  const auto m = Texture(w, h, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto z = Texture(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto out = Texture(w, h, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(m.Get(), motion.data(), w * 8, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(z.Get(), depth.data(), w * 4, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const float scale[2] = {1.f / static_cast<float>(w), 1.f / static_cast<float>(h)};  // a motion texel is one pixel
  pass.Record(g_list.Get(), 0, m.Get(), z.Get(), out.Get(), w, h, inverted, scale, reset);
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
  std::vector<float> result(motion.size());
  char* mapped = nullptr;
  down->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < h; ++y) std::memcpy(&result[2ull * y * w], mapped + y * place.Footprint.RowPitch, w * 8);
  down->Unmap(0, nullptr);
  return result;
}

void Check(bool ok, const char* what, double got, double was) {
  std::printf("%s %s: %.3f (flow in %.3f)\n", ok ? "ok  " : "FAIL", what, got, was);
  if (!ok) ++g_failed;
}

// One frame through the pass with the camera. `colour` is RGBA8, at the motion's size.
std::vector<float> ComposeCamera(hsf::compose::Pass& pass, UINT w, UINT h, const std::vector<float>& motion, const std::vector<float>& depth, const std::vector<uint32_t>& colour,
                                 const float* clip_to_prev, bool reset) {
  const auto m = Texture(w, h, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto z = Texture(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto c = Texture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto out = Texture(w, h, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(m.Get(), motion.data(), w * 8, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(z.Get(), depth.data(), w * 4, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(c.Get(), colour.data(), w * 4, h, D3D12_RESOURCE_STATE_COMMON);
  const float scale[2] = {1.f / static_cast<float>(w), 1.f / static_cast<float>(h)};
  hsf::compose::Camera camera;
  camera.colour = c.Get();
  camera.clip_to_prev = clip_to_prev;
  pass.Record(g_list.Get(), 0, m.Get(), z.Get(), out.Get(), w, h, false, scale, reset, &camera);
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
  std::vector<float> result(motion.size());
  char* mapped = nullptr;
  down->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < h; ++y) std::memcpy(&result[2ull * y * w], mapped + y * place.Footprint.RowPitch, w * 8);
  down->Unmap(0, nullptr);
  return result;
}

void TestCamera() {
  const UINT w = 256, h = 144;
  hsf::compose::Pass pass;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!pass.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R32G32_FLOAT, w, h, what, hr) || !pass.InitCamera(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, w, h, what, hr)) {
    std::printf("FAIL camera pass: %s (0x%08lx)\n", what, hr);
    ++g_failed;
    return;
  }
  // A camera that moves everything 8 pixels sideways: the picture's detail at x was at x + 8 last frame. A matrix in clip space, depth playing no part.
  hsf::camera::Mat4 pan = hsf::camera::Identity();
  pan.m[3] = 2.f * 8.f / static_cast<float>(w);
  // A fine random pattern, so a wrong offset shows. The flow is wrong (2) on the flat wall (x < 40) and on the world (40 and up). The moving thing (120-180) was at x - 4 and
  // its flow is right. The HUD (y from 120) stays put, flow 0.
  const auto pattern = [](int x, int y) {
    const uint32_t v = (static_cast<uint32_t>(x) * 73856093u) ^ (static_cast<uint32_t>(y) * 19349663u);
    return (v ^ (v >> 7)) & 255u;
  };
  const auto grey = [](uint32_t v) { return v | (v << 8) | (v << 16) | 0xff000000u; };
  const auto picture = [&](int frame) {
    std::vector<uint32_t> c(static_cast<size_t>(w) * h);
    for (int y = 0; y < static_cast<int>(h); ++y)
      for (int x = 0; x < static_cast<int>(w); ++x) {
        uint32_t v;
        if (y >= 120) v = pattern(x, y);
        else if (x >= 120 && x < 180) v = pattern(x - 4 * frame + 2000, y + 500);
        else if (x < 40) v = 128;
        else v = pattern(x + 8 * frame + 1000, y);
        c[static_cast<size_t>(y) * w + x] = grey(v);
      }
    return c;
  };
  std::vector<float> motion(2ull * w * h), depth(static_cast<size_t>(w) * h, 0.5f);
  for (int y = 0; y < static_cast<int>(h); ++y)
    for (int x = 0; x < static_cast<int>(w); ++x) {
      float f = 2.f;
      if (y >= 120) f = 0.f;
      else if (x >= 120 && x < 180) f = -4.f;
      motion[2ull * (static_cast<size_t>(y) * w + x)] = f;
    }
  ComposeCamera(pass, w, h, motion, depth, picture(0), pan.m, true);
  const auto o = ComposeCamera(pass, w, h, motion, depth, picture(1), pan.m, false);
  const auto at = [&](const std::vector<float>& v, int x, int y) { return v[2ull * (static_cast<size_t>(y) * w + x)]; };
  Check(std::abs(at(o, 90, 60) - 8.f) < 0.05f, "camera: wrong flow on detailed world takes the camera's motion", at(o, 90, 60), 2.0);
  Check(std::abs(at(o, 20, 60) - 8.f) > 3.f, "camera: a flat surface such as a face keeps the flow, as the picture cannot tell", at(o, 20, 60), 2.0);
  Check(std::abs(at(o, 150, 60) + 4.f) < 0.3f, "camera: a thing moving by itself keeps the flow", at(o, 150, 60), -4.0);
  Check(std::abs(at(o, 90, 130)) < 0.3f, "camera: the HUD keeps the flow", at(o, 90, 130), 0.0);
  // Without a matrix this frame the pass is as before.
  const auto none = ComposeCamera(pass, w, h, motion, depth, picture(2), nullptr, false);
  Check(std::abs(at(none, 90, 60) - 8.f) > 1.f, "camera: no matrix, flow only", at(none, 90, 60), 2.0);

  // Direction and depth: where flow and camera agree, the result is exactly the camera's, with depth in it and y the right way up.
  hsf::compose::Pass pass2;
  pass2.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R32G32_FLOAT, w, h, what, hr);
  pass2.InitCamera(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, w, h, what, hr);
  hsf::camera::Mat4 parallax = hsf::camera::Identity();
  parallax.m[2] = 2.f * 16.f / static_cast<float>(w);  // x offset: 16 pixels x depth
  parallax.m[7] = 2.f * 3.f / static_cast<float>(h);   // clip y 3 pixels up last frame: 3 pixels nearer the picture's top
  std::vector<float> agree(2ull * w * h), steps(static_cast<size_t>(w) * h);
  for (int y = 0; y < static_cast<int>(h); ++y)
    for (int x = 0; x < static_cast<int>(w); ++x) {
      const float d = y < 72 ? 0.25f : 0.5f;
      steps[static_cast<size_t>(y) * w + x] = d;
      agree[2ull * (static_cast<size_t>(y) * w + x)] = 16.f * d + ((x ^ y) & 1 ? 0.3f : -0.3f);
      agree[2ull * (static_cast<size_t>(y) * w + x) + 1] = -3.f;
    }
  const std::vector<uint32_t> flat(static_cast<size_t>(w) * h, grey(100));
  ComposeCamera(pass2, w, h, agree, steps, flat, parallax.m, true);
  const auto q = ComposeCamera(pass2, w, h, agree, steps, flat, parallax.m, false);
  Check(std::abs(at(q, 100, 30) - 4.f) < 0.01f, "camera: motion from depth, one surface", at(q, 100, 30), 4.3);
  Check(std::abs(at(q, 100, 110) - 8.f) < 0.01f, "camera: motion from depth, the other", at(q, 100, 110), 8.3);
  const float qy = q[2ull * (static_cast<size_t>(30) * w + 100) + 1];
  Check(std::abs(qy + 3.f) < 0.01f, "camera: y the right way up", qy, -3.0);

  // The grid the presenter reads.
  hsf::frame_checks::Samples samples;
  if (!samples.Init(g_device.Get(), DXGI_FORMAT_R32G32_FLOAT, what, hr)) {
    std::printf("FAIL camera grid: %s (0x%08lx)\n", what, hr);
    ++g_failed;
    return;
  }
  const auto m = Texture(w, h, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto z = Texture(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  Upload(m.Get(), agree.data(), w * 8, h, D3D12_RESOURCE_STATE_COMMON);
  Upload(z.Get(), steps.data(), w * 4, h, D3D12_RESOURCE_STATE_COMMON);
  const float* grid = nullptr;
  bool answered = false;
  for (int i = 0; i < 50 && !answered; ++i) answered = samples.Run(m.Get(), z.Get(), w, h, grid);  // 3 ms may be short for the very first run
  Check(answered && grid != nullptr, "camera grid: answered", answered ? 1 : 0, 1);
  if (answered) {
    const float* const top = grid + 4 * (2 * hsf::camera::SAMPLE_WIDTH + 10);
    const float* const bottom = grid + 4 * (30 * hsf::camera::SAMPLE_WIDTH + 10);
    Check(std::abs(top[2] - 0.25f) < 1e-5f && std::abs(bottom[2] - 0.5f) < 1e-5f, "camera grid: depth in the right rows", top[2], 0.25);
    Check(std::abs(std::abs(top[0] - 4.f) - 0.3f) < 1e-4f && std::abs(top[1] + 3.f) < 1e-5f, "camera grid: motion", top[0], 4.3);
  }

  // Cost at game size, every pixel settling a disagreement: the worst case.
  const UINT GW = 3440, GH = 1369;
  hsf::compose::Pass big;
  big.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R16G16_FLOAT, GW, GH, what, hr);
  big.InitCamera(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, GW, GH, what, hr);
  std::vector<float> big_depth(static_cast<size_t>(GW) * GH, 0.5f);
  std::vector<unsigned short> big_motion(2ull * GW * GH, static_cast<unsigned short>(0x3c00));  // all 1.0
  std::vector<uint32_t> big_colour(static_cast<size_t>(GW) * GH);
  for (UINT y = 0; y < GH; ++y)
    for (UINT x = 0; x < GW; ++x) big_colour[static_cast<size_t>(y) * GW + x] = grey(pattern(static_cast<int>(x), static_cast<int>(y)));
  const auto gm = Texture(GW, GH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto gz = Texture(GW, GH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto gc = Texture(GW, GH, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto gout = Texture(GW, GH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(gm.Get(), big_motion.data(), GW * 4, GH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(gz.Get(), big_depth.data(), GW * 4, GH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(gc.Get(), big_colour.data(), GW * 4, GH, D3D12_RESOURCE_STATE_COMMON);
  hsf::camera::Mat4 far_pan = hsf::camera::Identity();
  far_pan.m[3] = 2.f * 20.f / static_cast<float>(GW);
  hsf::compose::Camera game;
  game.colour = gc.Get();
  game.clip_to_prev = far_pan.m;
  const float game_scale[2] = {1.f / static_cast<float>(GW), 1.f / static_cast<float>(GH)};
  double ms = -1;
  for (UINT i = 0; i < 64 && ms < 0; ++i) {
    big.Record(g_list.Get(), i % hsf::compose::FRAMES, gm.Get(), gz.Get(), gout.Get(), GW, GH, false, game_scale, false, &game);
    Run();
    if (!big.TakeAverageMs(50, ms)) ms = -1;
  }
  std::printf("cost at %ux%u with the camera, every pixel disagreeing: %.3f ms on the GPU, last picture's copy included\n", GW, GH, ms);
  if (ms < 0) ++g_failed;
}

}  // namespace

// At 4K the pass works at half size and brings the result back by depth. A near square moving 8 pixels over a still background, its left edge at an odd
// x, so one half size texel holds both surfaces: each full size pixel beside the edge must still get its own surface's motion. Then its cost.
void TestHalfSize() {
  const UINT w = 3840, h = 2160;
  hsf::compose::Pass pass;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!pass.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R32G32_FLOAT, w, h, what, hr)) {
    std::printf("FAIL 4K Init: %s (0x%08lx)\n", what, hr);
    ++g_failed;
    return;
  }
  Check(pass.WorkWidth() == w / 2 && pass.WorkHeight() == h / 2, "4K works at half size, width", pass.WorkWidth(), w);
  hsf::compose::Pass ultrawide;
  ultrawide.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R32G32_FLOAT, 3440, 1440, what, hr);
  Check(ultrawide.WorkWidth() == 1720, "3440x1440 works at half size, width", ultrawide.WorkWidth(), 3440);
  using hsf::compose::WorkDivisor;
  Check(WorkDivisor(1920, 1080) == 1 && WorkDivisor(1920, 1200) == 1, "1080p and 1920x1200 stay at full size", WorkDivisor(1920, 1080), 1);
  Check(WorkDivisor(2560, 1440) == 2, "1440p at half size", WorkDivisor(2560, 1440), 2);
  Check(WorkDivisor(3840, 2160) == 2, "4K at half size", WorkDivisor(3840, 2160), 2);
  Check(WorkDivisor(5120, 2880) == 3 && WorkDivisor(7680, 4320) == 3, "5K and 8K at a third", WorkDivisor(7680, 4320), 3);

  const auto inside = [](UINT x, UINT y) { return x >= 1001 && x < 2001 && y >= 601 && y < 1401; };
  std::vector<float> motion(2ull * w * h, 0.f), depth(static_cast<size_t>(w) * h);
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) {
      const bool in = inside(x, y);
      motion[2ull * (static_cast<size_t>(y) * w + x)] = in ? 8.f : 0.f;
      depth[static_cast<size_t>(y) * w + x] = in ? 0.5f : 0.05f;
    }
  const auto o = Compose(pass, w, h, motion, depth, true);
  const auto at = [&](int x, int y) { return o[2ull * (static_cast<size_t>(y) * w + x)]; };
  const int Y = 1000;
  Check(std::abs(at(1500, Y) - 8.f) < 0.01f, "4K: middle of the square stays 8", at(1500, Y), 8.0);
  Check(std::abs(at(100, 100)) < 0.01f, "4K: far background stays 0", at(100, 100), 0.0);
  // Beside an edge the background takes the square's motion (the 3x3's dilation, as at full size), a few pixels wide at half size; past that it is its own.
  Check(at(1001, Y) > 7.f, "4K: square pixel sharing a half size texel with the background keeps the square's motion", at(1001, Y), 8.0);
  Check(at(996, Y) < 1.f, "4K: background 5 pixels from the left edge keeps its own motion", at(996, Y), 0.0);
  Check(at(2000, Y) > 7.f, "4K: square pixel at the right edge keeps the square's motion", at(2000, Y), 8.0);
  Check(at(2005, Y) < 1.f, "4K: background 5 pixels from the right edge keeps its own motion", at(2005, Y), 0.0);
  Check(at(1500, 601) > 7.f, "4K: square pixel at the top edge keeps the square's motion", at(1500, 601), 8.0);
  Check(at(1500, 596) < 1.f, "4K: background 5 pixels above the top edge keeps its own motion", at(1500, 596), 0.0);

  // Cost at 4K in the format games use.
  std::vector<unsigned short> half(2ull * w * h);
  for (size_t i = 0; i < half.size(); ++i) half[i] = static_cast<unsigned short>(0x3c00 + (i % 1024));
  std::vector<float> game_depth(static_cast<size_t>(w) * h);
  for (UINT y = 0; y < h; ++y)
    for (UINT x = 0; x < w; ++x) game_depth[static_cast<size_t>(y) * w + x] = 0.02f + 0.9f * static_cast<float>((x / 64 + y / 64) % 7) / 7.f;
  hsf::compose::Pass pass16;
  if (!pass16.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R16G16_FLOAT, w, h, what, hr)) {
    std::printf("FAIL 4K Init for R16G16_FLOAT: %s (0x%08lx)\n", what, hr);
    ++g_failed;
    return;
  }
  const auto gm = Texture(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto gz = Texture(w, h, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto gout = Texture(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(gm.Get(), half.data(), w * 4, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(gz.Get(), game_depth.data(), w * 4, h, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  double ms = -1;
  const float scale[2] = {1.f / static_cast<float>(w), 1.f / static_cast<float>(h)};
  for (UINT i = 0; i < 64 && ms < 0; ++i) {
    pass16.Record(g_list.Get(), i % hsf::compose::FRAMES, gm.Get(), gz.Get(), gout.Get(), w, h, true, scale, false);
    Run();
    if (!pass16.TakeAverageMs(50, ms)) ms = -1;
  }
  std::printf("cost at %ux%u (works at %ux%u), R16G16_FLOAT: %.3f ms on the GPU\n", w, h, pass16.WorkWidth(), pass16.WorkHeight(), ms);
  if (ms < 0) {
    std::printf("FAIL no 4K timing\n");
    ++g_failed;
  }
}

int wmain() {
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  DXGI_ADAPTER_DESC1 desc{};
  adapter->GetDesc1(&desc);
  if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) {
    std::printf("no Direct3D 12 device\n");
    return 2;
  }
  std::printf("GPU: %ls\n", desc.Description);
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  g_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&g_queue));
  g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
  g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_list));
  g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));

  hsf::compose::Pass pass;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!pass.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R32G32_FLOAT, 128, 128, what, hr)) {
    std::printf("FAIL Init: %s (0x%08lx)\n", what, hr);
    return 1;
  }
  hsf::compose::Pass pass16;
  if (!pass16.Init(g_device.Get(), g_queue.Get(), DXGI_FORMAT_R16G16_FLOAT, 3440, 1369, what, hr)) {  // the format games have so far
    std::printf("FAIL Init for R16G16_FLOAT: %s (0x%08lx)\n", what, hr);
    return 1;
  }

  // A square at x, y 40-87, moving 8 pixels; the background does not move. The flow is that field blurred 4 pixels each way, as flow is soft at edges.
  const UINT W = 128, H = 128;
  const auto inside = [](int x, int y) { return x >= 40 && x < 88 && y >= 40 && y < 88; };
  std::vector<float> motion(2ull * W * H), reversed(W * H), normal(W * H), flat(W * H, 0.3f), uniform(2ull * W * H);
  for (int y = 0; y < static_cast<int>(H); ++y)
    for (int x = 0; x < static_cast<int>(W); ++x) {
      int hits = 0, all = 0;
      for (int b = -4; b <= 4; ++b)
        for (int a = -4; a <= 4; ++a) {
          hits += inside(x + a, y + b) ? 1 : 0;
          ++all;
        }
      motion[2ull * (y * W + x)] = 8.f * static_cast<float>(hits) / static_cast<float>(all);
      uniform[2ull * (y * W + x)] = 3.f;
      uniform[2ull * (y * W + x) + 1] = -2.f;
      reversed[y * W + x] = inside(x, y) ? 0.5f : 0.05f;
      normal[y * W + x] = 1.f - reversed[y * W + x];
    }
  const auto at = [&](const std::vector<float>& m, int x, int y) { return m[2ull * (y * W + x)]; };

  const auto a = Compose(pass, W, H, motion, reversed, true);
  const int Y = 64;
  // Just inside the square the flow is too small; 4 pixels outside it still leaks. Both must move to the truth.
  for (int x : {40, 41, 42}) Check(8.f - at(a, x, Y) < 0.6f * (8.f - at(motion, x, Y)), "inside the near edge moves towards 8", at(a, x, Y), at(motion, x, Y));
  for (int x : {36, 37, 38}) Check(at(a, x, Y) < 0.6f * at(motion, x, Y), "outside the edge moves towards 0", at(a, x, Y), at(motion, x, Y));
  // The pixel beside the edge takes the near surface's motion (the 3x3).
  Check(at(a, 39, Y) > at(motion, 39, Y), "beside the edge takes the near surface", at(a, 39, Y), at(motion, 39, Y));
  Check(std::abs(at(a, 64, Y) - 8.f) < 0.01f, "middle of the square stays 8", at(a, 64, Y), at(motion, 64, Y));
  Check(std::abs(at(a, 10, 10)) < 0.01f, "far background stays 0", at(a, 10, 10), at(motion, 10, 10));

  // The other depth direction gives the same answer.
  const auto b = Compose(pass, W, H, motion, normal, false);
  double worst = 0;
  for (size_t i = 0; i < a.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(a[i] - b[i])));
  Check(worst < 0.01, "normal depth matches reversed, largest difference", worst, 0);

  // Even motion comes out the same, and both channels are kept.
  const auto c = Compose(pass, W, H, uniform, flat, true);
  worst = 0;
  for (size_t i = 0; i < c.size(); ++i) worst = std::max(worst, static_cast<double>(std::abs(c[i] - uniform[i])));
  Check(worst < 0.001, "even motion unchanged, largest difference", worst, 0);

  // Over time. Motion that shakes from frame to frame comes out steadier.
  const auto filled = [&](float value) {
    std::vector<float> m(2ull * W * H, 0.f);
    for (size_t i = 0; i < m.size(); i += 2) m[i] = value;
    return m;
  };
  const int X = 64;
  Compose(pass, W, H, filled(3.f), flat, true, true);
  float low = 0, high = 0;
  for (int i = 0; i < 16; ++i) {
    const auto o = Compose(pass, W, H, filled(i % 2 == 0 ? 3.4f : 2.6f), flat, true, false);
    (i % 2 == 0 ? high : low) = at(o, X, Y);
  }
  Check(high - low < 0.5f * 0.8f, "shaking motion is steadied, swing", high - low, 0.8);

  // A real jump goes through at once.
  Compose(pass, W, H, filled(3.f), flat, true, true);
  for (int i = 0; i < 4; ++i) Compose(pass, W, H, filled(3.f), flat, true, false);
  const auto jump = Compose(pass, W, H, filled(10.f), flat, true, false);
  Check(at(jump, X, Y) > 9.9f, "a jump in motion is not held back", at(jump, X, Y), 10.0);

  // Motion that speeds up steadily is not left far behind.
  Compose(pass, W, H, filled(0.f), flat, true, true);
  float ramp = 0, ramped = 0;
  for (int i = 0; i < 40; ++i) {
    ramp += 0.2f;
    ramped = at(Compose(pass, W, H, filled(ramp), flat, true, false), X, Y);
  }
  Check(ramp - ramped < 0.5f, "steady speeding up trails by little, lag", ramp - ramped, 0);

  // A reset ignores last frame.
  Compose(pass, W, H, filled(3.f), flat, true, true);
  const auto cut = Compose(pass, W, H, filled(3.4f), flat, true, true);
  Check(std::abs(at(cut, X, Y) - 3.4f) < 0.001f, "a reset ignores last frame", at(cut, X, Y), 3.4);

  // Forgetting does too.
  Compose(pass, W, H, filled(3.f), flat, true, true);
  pass.ForgetHistory();
  const auto forgot = Compose(pass, W, H, filled(3.4f), flat, true, false);
  Check(std::abs(at(forgot, X, Y) - 3.4f) < 0.001f, "ForgetHistory ignores last frame", at(forgot, X, Y), 3.4);

  // A new surface here does not take over the old surface's motion, even when the two nearly agree.
  std::vector<float> near_depth(W * H, 0.5f), far_depth(W * H, 0.05f);
  Compose(pass, W, H, filled(3.f), far_depth, true, true);
  const auto other = Compose(pass, W, H, filled(3.4f), near_depth, true, false);
  Check(std::abs(at(other, X, Y) - 3.4f) < 0.001f, "a new surface ignores last frame", at(other, X, Y), 3.4);

  // A square moving right 8 pixels a frame, its motion shaking. The motion points back to where each pixel was, and is followed there: at the front of
  // the square last frame's motion is 8 pixels behind, inside the square, so it is steadied too. Followed the wrong way it would land on the background.
  const auto moving = [&](int frame, float speed, std::vector<float>& m, std::vector<float>& z) {
    const int left = 20 + 8 * frame, right = left + 40;
    for (int y = 0; y < static_cast<int>(H); ++y)
      for (int x = 0; x < static_cast<int>(W); ++x) {
        const bool in = x >= left && x < right && y >= 40 && y < 88;
        m[2ull * (y * W + x)] = in ? -speed : 0.f;
        m[2ull * (y * W + x) + 1] = 0.f;
        z[y * W + x] = in ? 0.5f : 0.05f;
      }
  };
  std::vector<float> mm(2ull * W * H), mz(W * H);
  moving(0, 8.f, mm, mz);
  Compose(pass, W, H, mm, mz, true, true);
  low = high = 0;
  for (int i = 1; i <= 6; ++i) {
    moving(i, i % 2 == 0 ? 8.4f : 7.6f, mm, mz);
    const auto o = Compose(pass, W, H, mm, mz, true, false);
    const int front = 20 + 8 * i + 40 - 5;  // inside the square, within the 8 pixels it has just moved over
    (i % 2 == 0 ? high : low) = at(o, front, Y);
  }
  Check(std::abs(high - low) < 0.6f * 0.8f, "motion is followed to where the pixel was, swing at the front", std::abs(high - low), 0.8);

  // Size and cost: every pixel at the size games have run at, on the real GPU, in the format they use.
  const UINT GW = 3440, GH = 1369;
  std::vector<float> game_depth(static_cast<size_t>(GW) * GH);
  std::vector<float> game_motion(2ull * GW * GH);
  for (UINT y = 0; y < GH; ++y)
    for (UINT x = 0; x < GW; ++x) {
      game_depth[static_cast<size_t>(y) * GW + x] = 0.02f + 0.9f * static_cast<float>((x / 64 + y / 64) % 7) / 7.f;
      game_motion[2ull * (static_cast<size_t>(y) * GW + x)] = static_cast<float>(x % 32);
    }
  std::vector<unsigned short> half(2ull * GW * GH);
  for (size_t i = 0; i < half.size(); ++i) half[i] = static_cast<unsigned short>(0x3c00 + (i % 1024));  // valid half floats from 1 to 2
  const auto gm = Texture(GW, GH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto gz = Texture(GW, GH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto gout = Texture(GW, GH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  Upload(gm.Get(), half.data(), GW * 4, GH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(gz.Get(), game_depth.data(), GW * 4, GH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  double ms = -1;
  for (UINT i = 0; i < 64 && ms < 0; ++i) {
    const float game_scale[2] = {1.f / static_cast<float>(GW), 1.f / static_cast<float>(GH)};
    pass16.Record(g_list.Get(), i % hsf::compose::FRAMES, gm.Get(), gz.Get(), gout.Get(), GW, GH, true, game_scale, false);
    Run();
    if (!pass16.TakeAverageMs(50, ms)) ms = -1;
  }
  std::printf("cost at %ux%u, R16G16_FLOAT: %.3f ms on the GPU\n", GW, GH, ms);
  if (ms < 0) {
    std::printf("FAIL no timing\n");
    ++g_failed;
  }
  TestCamera();
  TestHalfSize();
  std::printf(g_failed == 0 ? "passed\n" : "%d FAILED\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
