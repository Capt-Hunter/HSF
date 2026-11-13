// Offline test of src/hud.cpp, no game needed: when the HUD is found, and what comes out with it taken away, on a real device.
//   hud_test  (build\<arch>\hud_test.exe; exit code 0 = all passed)
#include "../src/hud.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace hsf::hud;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

constexpr UINT W = 64, H = 48;
constexpr uint8_t EFFECT = 20;  // what the "effects" add to every pixel

bool InHud(UINT x, UINT y) { return x >= 8 && x < 24 && y >= 8 && y < 24; }
ComPtr<ID3D11Texture2D> Picture(ID3D11Device* device, DXGI_FORMAT format, bool hud, uint8_t add) {
  std::vector<uint32_t> pixels(W * H);
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      uint32_t r = 40 + x, g = 60 + y, b = 90;
      if (hud && InHud(x, y)) {
        r = 200;
        g = 180;
        b = 30;
      }
      r += add;
      g += add;
      b += add;
      pixels[y * W + x] = format == DXGI_FORMAT_R8G8B8A8_UNORM ? (r | g << 8 | b << 16 | 255u << 24) : (b | g << 8 | r << 16 | 255u << 24);
    }
  const D3D11_TEXTURE2D_DESC d{W, H, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
  const D3D11_SUBRESOURCE_DATA data{pixels.data(), W * 4, 0};
  ComPtr<ID3D11Texture2D> t;
  device->CreateTexture2D(&d, &data, &t);
  return t;
}

std::vector<uint32_t> Read(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* t) {
  D3D11_TEXTURE2D_DESC d{};
  t->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING;
  d.BindFlags = 0;
  d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  device->CreateTexture2D(&d, nullptr, &staging);
  context->CopyResource(staging.Get(), t);
  D3D11_MAPPED_SUBRESOURCE map{};
  context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map);
  std::vector<uint32_t> out(W * H);
  for (UINT y = 0; y < H; ++y) std::memcpy(&out[y * W], static_cast<const char*>(map.pData) + y * map.RowPitch, W * 4);
  context->Unmap(staging.Get(), 0);
  return out;
}

void TestWatch() {
  std::printf("Finding the HUD\n");
  Watch w;
  w.BeginFrame();
  Check(!w.Draw(false, false, 3), "a full-screen pass is no HUD");
  Check(w.Draw(true, false, 6), "the first blended draw with no depth test: copy now");
  Check(!w.Draw(true, false, 600) && !w.Draw(true, false, 6), "the rest of the HUD: no more copies");
  Check(w.HaveCopy() && w.Copies() == 1, "one copy, and it stands");

  w.BeginFrame();
  Check(!w.HaveCopy(), "a new frame starts with none");
  w.Draw(false, false, 3);
  Check(!w.HaveCopy(), "no blended draw: no HUD");

  w.BeginFrame();
  w.Draw(true, false, 6);
  Check(!w.Draw(false, false, 3) && !w.HaveCopy(), "a full-screen pass afterwards: that copy is not before the HUD");
  Check(w.Draw(true, false, 6) && w.HaveCopy() && w.Copies() == 2, "the next blended draw copies again");

  w.BeginFrame();
  w.Draw(true, false, 6);
  w.Draw(false, false, 600);
  Check(w.HaveCopy(), "an opaque draw of many vertices is part of the HUD");

  w.BeginFrame();
  Check(!w.Draw(true, true, 6), "blended with a depth test is the world, such as particles");
  w.Draw(true, false, 6);
  Check(!w.Draw(false, true, 3000) && !w.HaveCopy(), "the world drawn afterwards: that copy is not before the HUD");

  w.BeginFrame();
  for (int i = 0; i < 20; ++i) {
    w.Draw(true, false, 6);
    w.Draw(false, true, 3000);
  }
  Check(w.Copies() == Watch::MOST_COPIES, "a game alternating all frame costs a limited number of copies");
  w.Draw(true, false, 6);
  Check(!w.HaveCopy(), "and finds no HUD");

  w.BeginFrame();
  Check(w.StartPass() && w.HaveCopy(), "a whole pass that may be the HUD: copy now");
  Check(!w.Draw(false, false, 3) && !w.HaveCopy(), "a full-screen pass after it: that copy is not before the HUD");
  Check(w.StartPass() && w.HaveCopy() && w.Copies() == 2, "the next one copies again");

  w.BeginFrame();
  w.Close();
  Check(!w.Draw(true, false, 6) && !w.HaveCopy(), "draws from ReShade's effects on do not count");
}

void TestGuard() {
  std::printf("Guarding against a full-screen pass\n");
  Guard g;
  Check(g.InUse(), "in use to begin with");
  for (int i = 0; i < 100; ++i) g.Feed(0.1f);
  Check(g.InUse(), "a small HUD stays in use");
  for (int i = 0; i < Guard::OFF_FRAMES - 1; ++i) g.Feed(0.9f);
  Check(g.InUse(), "a few frames of it do not change that");
  g.Feed(0.9f);
  Check(!g.InUse(), "most of the picture for a while: not used");
  for (int i = 0; i < Guard::ON_FRAMES - 1; ++i) g.Feed(0.1f);
  Check(!g.InUse(), "does not come back at once");
  g.Feed(0.1f);
  Check(g.InUse(), "and then does");
}

void TestRemover(ID3D11Device* device, ID3D11DeviceContext* context, DXGI_FORMAT format, const char* name) {
  std::printf("Taking the HUD away, %s\n", name);
  const auto before = Picture(device, format, false, 0);
  const auto at_effects = Picture(device, format, true, 0);
  const auto finished = Picture(device, format, true, EFFECT);
  const auto expected = Picture(device, format, false, EFFECT);
  D3D11_TEXTURE2D_DESC d{};
  finished->GetDesc(&d);
  Remover r;
  const char* why = nullptr;
  if (!r.Prepare(device, d, why)) {
    std::printf("  FAIL not prepared: %s\n", why != nullptr ? why : "");
    ++g_failures;
    return;
  }
  Check(r.CopyBeforeHud(context, before.Get()) && r.CopyAtEffects(context, at_effects.Get()), "both copies");
  ID3D11Texture2D* const out = r.Make(context, finished.Get());
  Check(out != nullptr, "an image comes out");
  if (out == nullptr) return;
  const auto got = Read(device, context, out), want = Read(device, context, expected.Get());
  int worst = 0, wrong_outside = 0;
  for (UINT i = 0; i < W * H; ++i)
    for (int shift = 0; shift < 24; shift += 8) {
      const int a = (got[i] >> shift) & 255, b = (want[i] >> shift) & 255;
      const int off = a > b ? a - b : b - a;
      if (off > worst) worst = off;
      if (off != 0 && !InHud(i % W, i / W)) ++wrong_outside;
    }
  std::printf("        worst %d of 255 away from the same picture with the effects and no HUD\n", worst);
  Check(worst <= 1, "the HUD is gone and the effects are kept");
  Check(wrong_outside == 0, "outside the HUD, exactly the finished frame");
  float share = -1.f;
  for (int i = 0; i < 1000 && !r.ReadShare(context, share); ++i) Sleep(1);
  // Counted on every 8th pixel each way: 2 x 2 of the 8 x 6 points fall in the HUD.
  std::printf("        share %.3f\n", share);
  Check(std::fabs(share - 4.f / 48.f) < 0.001f, "the share of the picture is right");

  // No HUD: the finished frame as it is.
  r.CopyBeforeHud(context, before.Get());
  r.CopyAtEffects(context, before.Get());
  const auto plain = Picture(device, format, false, EFFECT);
  ID3D11Texture2D* const same = r.Make(context, plain.Get());
  Check(same != nullptr && Read(device, context, same) == Read(device, context, plain.Get()), "no difference: the finished frame, exactly");
  share = -1.f;
  for (int i = 0; i < 1000 && !r.ReadShare(context, share); ++i) Sleep(1);
  Check(share == 0.f, "and a share of 0");

  const auto other = [&] {
    D3D11_TEXTURE2D_DESC o = d;
    o.Width = W / 2;
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&o, nullptr, &t);
    return t;
  }();
  Check(!r.CopyBeforeHud(context, other.Get()) && r.Make(context, other.Get()) == nullptr, "another size is refused");
}

}  // namespace

int main() {
  TestWatch();
  TestGuard();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context))) {
    std::printf("no device\n");
    return 2;
  }
  TestRemover(device.Get(), context.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8");
  TestRemover(device.Get(), context.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8");
  std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
