// Offline test of the motion quality score (src/score.cpp), no game and no window: synthetic frames with known motion, through the same code the game runs.
// Checks that right motion scores high, no motion about zero, wrong motion below zero, that still frames and cuts are left out, that coarse vectors
// score between exact and none, and that the game's compute state is put back.
//   score_test
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../src/score.h"

using Microsoft::WRL::ComPtr;
namespace score = hsf::score;

namespace {

constexpr UINT W = 1280, H = 720;
int g_failures = 0;

// Soft random texture, repeating, so a shift of it is well defined and a wrong shift does not match.
std::vector<uint32_t> MakeTexture(UINT seed) {
  constexpr UINT size = 1024;
  std::srand(seed);
  std::vector<float> grain(size * size);
  for (float& value : grain) value = static_cast<float>(std::rand()) / RAND_MAX;
  std::vector<uint32_t> out(size * size);
  for (UINT y = 0; y < size; ++y)
    for (UINT x = 0; x < size; ++x) {
      float sum = 0;
      for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx) sum += grain[((y + dy + size) % size) * size + (x + dx + size) % size];
      const uint32_t level = static_cast<uint32_t>(std::fmin(std::fmax((sum / 25.f - 0.5f) * 5.f + 0.5f, 0.f), 1.f) * 255.f);
      out[y * size + x] = level | level << 8 | level << 16 | 0xff000000u;
    }
  return out;
}

ComPtr<ID3D11Texture2D> Frame(ID3D11Device* device, const std::vector<uint32_t>& texture, int shift_x, int shift_y, DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM) {
  constexpr int size = 1024;
  std::vector<uint32_t> pixels(W * H);
  for (int y = 0; y < static_cast<int>(H); ++y)
    for (int x = 0; x < static_cast<int>(W); ++x) pixels[y * W + x] = texture[((y + shift_y) % size + size) % size * size + ((x + shift_x) % size + size) % size];
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = W;
  desc.Height = H;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_RENDER_TARGET;
  const D3D11_SUBRESOURCE_DATA data{pixels.data(), W * 4, 0};
  ComPtr<ID3D11Texture2D> out;
  if (FAILED(device->CreateTexture2D(&desc, &data, &out))) std::exit(2);
  return out;
}

// Flat motion field in pixels, with the same value everywhere; `coarse_noise` adds a per-block error, like coarse optical flow.
ComPtr<ID3D11Texture2D> Motion(ID3D11Device* device, UINT width, UINT height, float x, float y, float coarse_noise = 0.f, DXGI_FORMAT format = DXGI_FORMAT_R32G32_FLOAT) {
  std::vector<float> values(static_cast<size_t>(width) * height * 2);
  std::srand(7);
  std::vector<float> blocks(static_cast<size_t>(width / 8 + 1) * (height / 8 + 1) * 2);
  for (float& value : blocks) value = (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * 2.f * coarse_noise;
  for (UINT j = 0; j < height; ++j)
    for (UINT i = 0; i < width; ++i) {
      const size_t block = (static_cast<size_t>(j / 8) * (width / 8 + 1) + i / 8) * 2;
      values[(static_cast<size_t>(j) * width + i) * 2] = x + blocks[block];
      values[(static_cast<size_t>(j) * width + i) * 2 + 1] = y + blocks[block + 1];
    }
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA data{values.data(), width * 8, 0};
  ComPtr<ID3D11Texture2D> out;
  if (FAILED(device->CreateTexture2D(&desc, &data, &out))) std::exit(2);
  return out;
}

// One pair, then frames until its result is read back. Each case gets a fresh history window by shutting the score down.
score::Result Run(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* before, ID3D11Texture2D* after, ID3D11Texture2D* motion, float to_pixels_x, float to_pixels_y) {
  score::Shutdown();
  score::SetPeriod(1000000);
  score::Input inputs[score::MAX_INPUTS];
  inputs[0].motion = motion;
  inputs[0].to_pixels[0] = to_pixels_x;
  inputs[0].to_pixels[1] = to_pixels_y;
  score::Frame(device, before, inputs);
  score::Frame(device, after, inputs);
  for (int i = 0; i < 2000; ++i) {
    context->Flush();
    score::Frame(device, after, inputs);
    const score::Result result = score::Recent(0, 60000);
    if (result.pairs + result.still + result.cuts != 0) return result;
    Sleep(1);
  }
  std::printf("FAILED: no result came back\n");
  ++g_failures;
  return {};
}

void Print(const char* name, const score::Result& r) {
  std::printf("%-34s gain %6.1f%%  warp %6.2f  none %6.2f levels  better %5.1f%%  worse %5.1f%%  moving %5.1f%%  pairs %u still %u cuts %u\n", name, r.gain * 100.f, r.error_warp,
              r.error_none, r.better * 100.f, r.worse * 100.f, r.moving * 100.f, r.pairs, r.still, r.cuts);
}

void Check(bool ok, const char* what) {
  std::printf("  %s: %s\n", ok ? "ok" : "FAILED", what);
  if (!ok) ++g_failures;
}

}  // namespace

int main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) return 2;
  const std::vector<uint32_t> texture = MakeTexture(1), other = MakeTexture(2);

  // The picture moved 12 px right and 5 px down, so a pixel's earlier place is at (x - 12, y - 5): the shift's sign in Frame is the opposite.
  const auto before = Frame(device.Get(), texture, 0, 0);
  const auto after = Frame(device.Get(), texture, -12, -5);
  const auto exact = Motion(device.Get(), W, H, -12.f, -5.f);
  const auto zero = Motion(device.Get(), W, H, 0.f, 0.f);
  const auto wrong = Motion(device.Get(), W, H, 12.f, 5.f);
  const auto half_off = Motion(device.Get(), W / 8, H / 8, -6.f, -2.5f);
  const auto eighth = Motion(device.Get(), W / 8, H / 8, -12.f, -5.f);
  const auto noisy = Motion(device.Get(), W, H, -12.f, -5.f, 3.f);

  score::Result r = Run(device.Get(), context.Get(), before.Get(), after.Get(), exact.Get(), 1.f, 1.f);
  Print("exact motion", r);
  Check(r.valid && r.gain > 0.9f, "exact motion gains over 90%");
  Check(r.better > 0.9f && r.worse < 0.02f, "almost all changed patches better");
  const float exact_gain = r.gain;

  r = Run(device.Get(), context.Get(), before.Get(), after.Get(), eighth.Get(), 1.f, 1.f);
  Print("exact motion, 1/8 size texture", r);
  Check(r.valid && std::fabs(r.gain - exact_gain) < 0.02f, "same score from a smaller texture");

  r = Run(device.Get(), context.Get(), before.Get(), after.Get(), zero.Get(), 1.f, 1.f);
  Print("zero motion", r);
  Check(r.valid && std::fabs(r.gain) < 0.02f, "zero motion gains nothing");

  r = Run(device.Get(), context.Get(), before.Get(), after.Get(), wrong.Get(), 1.f, 1.f);
  Print("wrong sign", r);
  Check(r.valid && r.gain < 0.02f, "wrong direction does not gain");

  r = Run(device.Get(), context.Get(), before.Get(), after.Get(), half_off.Get(), 1.f, 1.f);
  Print("half the real motion", r);
  Check(r.valid && r.gain < exact_gain - 0.2f, "half the motion scores clearly below exact");

  r = Run(device.Get(), context.Get(), before.Get(), after.Get(), noisy.Get(), 1.f, 1.f);
  Print("noisy blocks (+-3 px)", r);
  Check(r.valid && r.gain < exact_gain - 0.1f, "noisy motion scores below exact");

  // UV convention: same motion as a share of the frame.
  const auto uv = Motion(device.Get(), W, H, -12.f / W, -5.f / H);
  r = Run(device.Get(), context.Get(), before.Get(), after.Get(), uv.Get(), static_cast<float>(W), static_cast<float>(H));
  Print("exact motion in UV", r);
  Check(r.valid && std::fabs(r.gain - exact_gain) < 0.02f, "UV vectors with their scale score the same");

  // Still frames.
  r = Run(device.Get(), context.Get(), before.Get(), before.Get(), exact.Get(), 1.f, 1.f);
  Print("still picture", r);
  Check(!r.valid && r.still == 1, "a still picture is left out");

  // Cut: a different picture.
  std::vector<uint32_t> dark = other;
  for (uint32_t& pixel : dark) pixel = ((pixel & 0xff) / 4) * 0x010101u | 0xff000000u;
  const auto cut = Frame(device.Get(), dark, 0, 0);
  r = Run(device.Get(), context.Get(), before.Get(), cut.Get(), exact.Get(), 1.f, 1.f);
  Print("scene cut", r);
  Check(!r.valid && r.cuts == 1, "a cut is left out and counted");

  // sRGB frame format.
  const auto before_srgb = Frame(device.Get(), texture, 0, 0, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  const auto after_srgb = Frame(device.Get(), texture, -12, -5, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  r = Run(device.Get(), context.Get(), before_srgb.Get(), after_srgb.Get(), exact.Get(), 1.f, 1.f);
  Print("sRGB frames", r);
  Check(r.valid && r.gain > 0.9f, "sRGB frames work");

  // Views exist.
  for (int view = 1; view < static_cast<int>(score::View::depth); ++view) Check(score::ViewOf(0, static_cast<score::View>(view)) != nullptr, score::VIEW_NAMES[view]);

  // The game's compute state: a shader's worth of bindings must survive.
  {
    ComPtr<ID3D11SamplerState> sampler;
    D3D11_SAMPLER_DESC sampler_desc{};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    device->CreateSamplerState(&sampler_desc, &sampler);
    ComPtr<ID3D11ShaderResourceView> srv;
    device->CreateShaderResourceView(exact.Get(), nullptr, &srv);
    ID3D11SamplerState* const samplers[] = {sampler.Get()};
    ID3D11ShaderResourceView* const views[] = {srv.Get()};
    context->CSSetSamplers(0, 1, samplers);
    context->CSSetShaderResources(1, 1, views);
    Run(device.Get(), context.Get(), before.Get(), after.Get(), exact.Get(), 1.f, 1.f);
    ID3D11SamplerState* sampler_after = nullptr;
    ID3D11ShaderResourceView* view_after = nullptr;
    context->CSGetSamplers(0, 1, &sampler_after);
    context->CSGetShaderResources(1, 1, &view_after);
    Check(sampler_after == sampler.Get() && view_after == srv.Get(), "the game's compute bindings are put back");
    if (sampler_after != nullptr) sampler_after->Release();
    if (view_after != nullptr) view_after->Release();
  }

  score::Shutdown();
  std::printf(g_failures == 0 ? "ALL OK\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
