// Offline test of src/depth.cpp, no game needed: real depth buffers in the formats games use, drawn on a real device, and what comes out.
//   depth_test  (build\<arch>\depth_test.exe; exit code 0 = all passed)
#include "../src/depth.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace hsf;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

// Left half of the buffer at `left`, right half at `right`, through its depth-stencil view.
void Fill(ID3D11DeviceContext* context, ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT dsv_format, bool multisampled, UINT width, UINT height, float left, float right) {
  D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
  desc.Format = dsv_format;
  desc.ViewDimension = multisampled ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
  ComPtr<ID3D11DepthStencilView> dsv;
  device->CreateDepthStencilView(texture, &desc, &dsv);
  context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, right, 0);
  // Clears ignore the viewport, so the left half is drawn with a depth-only full-screen triangle.
  static const char VS_AT[] = R"(cbuffer C : register(b0) { float d; float3 pad; }; float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), d, 1); })";
  ComPtr<ID3DBlob> code;
  D3DCompile(VS_AT, sizeof(VS_AT) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, nullptr);
  ComPtr<ID3D11VertexShader> vs;
  device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
  const float values[4] = {left, 0, 0, 0};
  const D3D11_BUFFER_DESC buffer_desc{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  const D3D11_SUBRESOURCE_DATA data{values, 0, 0};
  ComPtr<ID3D11Buffer> buffer;
  device->CreateBuffer(&buffer_desc, &data, &buffer);
  D3D11_DEPTH_STENCIL_DESC depth_desc{};
  depth_desc.DepthEnable = TRUE;
  depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  ComPtr<ID3D11DepthStencilState> depth_state;
  device->CreateDepthStencilState(&depth_desc, &depth_state);
  const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width) / 2, static_cast<float>(height), 0, 1};
  context->OMSetRenderTargets(0, nullptr, dsv.Get());
  context->OMSetDepthStencilState(depth_state.Get(), 0);
  context->RSSetViewports(1, &viewport);
  context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context->VSSetShader(vs.Get(), nullptr, 0);
  context->VSSetConstantBuffers(0, 1, buffer.GetAddressOf());
  context->PSSetShader(nullptr, nullptr, 0);
  context->Draw(3, 0);
  context->OMSetRenderTargets(0, nullptr, nullptr);
}

struct Case {
  const char* name;
  DXGI_FORMAT texture, dsv, srv;
  UINT samples;
  float left, right;
  bool want_inverted, want_flat;
};

// A few frames, so the read-back finishes.
ID3D11Texture2D* Run(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11ShaderResourceView* srv, UINT width, UINT height, ID3D11Texture2D* picture = nullptr) {
  ID3D11Texture2D* out = nullptr;
  depth::SetPeriod(0);
  for (int i = 0; i < 40; ++i) {
    out = depth::Frame(device, srv, width, height, picture);
    context->Flush();
    Sleep(5);
  }
  return out;
}

float ReadAt(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* texture, UINT x, UINT y) {
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  desc.Usage = D3D11_USAGE_STAGING;
  desc.BindFlags = 0;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  device->CreateTexture2D(&desc, nullptr, &staging);
  context->CopyResource(staging.Get(), texture);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  const float value = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch)[x];
  context->Unmap(staging.Get(), 0);
  return value;
}

// A bright block in the top left of the picture, and a near block at the same place in the depth, or mirrored to the bottom left.
void UpsideDownCases(ID3D11Device* device, ID3D11DeviceContext* context) {
  const UINT size_x = 640, size_y = 360;
  D3D11_TEXTURE2D_DESC picture_desc{size_x, size_y, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
  ComPtr<ID3D11Texture2D> picture;
  ComPtr<ID3D11RenderTargetView> rtv;
  device->CreateTexture2D(&picture_desc, nullptr, &picture);
  device->CreateRenderTargetView(picture.Get(), nullptr, &rtv);
  const float black[4] = {0, 0, 0, 1};
  context->ClearRenderTargetView(rtv.Get(), black);
  std::vector<uint32_t> white(160 * 90, 0xffffffffu);
  const D3D11_BOX block{80, 45, 0, 240, 135, 1};
  context->UpdateSubresource(picture.Get(), 0, &block, white.data(), 160 * 4, 0);

  static const char VS[] = R"(float4 main(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * float2(2, -2) + float2(-1, 1), 0.25, 1); })";
  ComPtr<ID3DBlob> code;
  D3DCompile(VS, sizeof(VS) - 1, "vs", nullptr, nullptr, "main", "vs_5_0", 0, 0, &code, nullptr);
  ComPtr<ID3D11VertexShader> vs;
  device->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &vs);
  D3D11_DEPTH_STENCIL_DESC depth_desc{};
  depth_desc.DepthEnable = TRUE;
  depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
  depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
  ComPtr<ID3D11DepthStencilState> depth_state;
  device->CreateDepthStencilState(&depth_desc, &depth_state);

  struct Way { const char* name; bool mirrored; int setting; bool want; };
  const Way ways[] = {{"upright depth, found", false, -1, false}, {"upside-down depth, found", true, -1, true}, {"upright depth, forced upside down", false, 1, true}, {"upside-down depth, forced as it is", true, 0, false}};
  for (const Way& way : ways) {
    std::printf("%s\n", way.name);
    depth::Shutdown();
    depth::SetUpsideDown(way.setting);
    D3D11_TEXTURE2D_DESC desc{size_x, size_y, 1, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0};
    ComPtr<ID3D11Texture2D> texture;
    device->CreateTexture2D(&desc, nullptr, &texture);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = DXGI_FORMAT_D32_FLOAT;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    device->CreateDepthStencilView(texture.Get(), &dsv_desc, &dsv);
    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 1.f, 0);
    const D3D11_VIEWPORT viewport{80, way.mirrored ? 225.f : 45.f, 160, 90, 0, 1};
    context->OMSetRenderTargets(0, nullptr, dsv.Get());
    context->OMSetDepthStencilState(depth_state.Get(), 0);
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->Draw(3, 0);
    context->OMSetRenderTargets(0, nullptr, nullptr);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R32_FLOAT;
    view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    device->CreateShaderResourceView(texture.Get(), &view, &srv);
    ID3D11Texture2D* const out = Run(device, context, srv.Get(), size_x, size_y, picture.Get());
    const depth::Status s = depth::Current();
    std::printf("        problem \"%s\", upside down %d, edges %.5f, mirrored %.5f\n", depth::Problem(), s.upside_down, s.edges_same, s.edges_mirrored);
    Check(s.upside_down == way.want, "way up as expected");
    Check(out != nullptr, "depth comes out");
    if (out == nullptr) continue;
    // The near block sits at the top when the copy is upright against what was drawn, at the bottom when it is flipped.
    const bool block_at_top = way.mirrored == way.want;
    Check(std::abs(ReadAt(device, context, out, 160, block_at_top ? 90 : 270) - 0.25f) < 0.001f, "the near block is where it should be");
    Check(std::abs(ReadAt(device, context, out, 160, block_at_top ? 270 : 90) - 1.f) < 0.001f, "and not in the other half");
  }
  depth::SetUpsideDown(-1);
}

// A buffer that has only ever been flat is called out after a while; one that has held real depth is not (that flat is a menu or a loading screen).
void FlatForLongCases(ID3D11Device* device, ID3D11DeviceContext* context) {
  const UINT size_x = 640, size_y = 360;
  D3D11_TEXTURE2D_DESC desc{size_x, size_y, 1, 1, DXGI_FORMAT_R32_TYPELESS, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, 0};
  ComPtr<ID3D11Texture2D> texture;
  device->CreateTexture2D(&desc, nullptr, &texture);
  D3D11_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_FLOAT;
  view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  view.Texture2D.MipLevels = 1;
  ComPtr<ID3D11ShaderResourceView> srv;
  device->CreateShaderResourceView(texture.Get(), &view, &srv);

  std::printf("flat from the start\n");
  depth::Shutdown();
  Fill(context, device, texture.Get(), DXGI_FORMAT_D32_FLOAT, false, size_x, size_y, 1.f, 1.f);
  Run(device, context, srv.Get(), size_x, size_y);
  Check(depth::Current().flat && !depth::Current().flat_for_long, "not called out at once");
  for (int i = 0; i < 4 && !depth::Current().flat_for_long; ++i) Run(device, context, srv.Get(), size_x, size_y);
  Check(depth::Current().flat_for_long, "called out after a while");

  std::printf("real depth arrives\n");
  Fill(context, device, texture.Get(), DXGI_FORMAT_D32_FLOAT, false, size_x, size_y, 0.5f, 0.99f);
  Run(device, context, srv.Get(), size_x, size_y);
  Check(!depth::Current().flat && !depth::Current().flat_for_long, "no longer called out");

  std::printf("flat again after real depth\n");
  Fill(context, device, texture.Get(), DXGI_FORMAT_D32_FLOAT, false, size_x, size_y, 1.f, 1.f);
  for (int i = 0; i < 5; ++i) Run(device, context, srv.Get(), size_x, size_y);
  Check(depth::Current().flat && !depth::Current().flat_for_long, "not called out");
  depth::Shutdown();
}

}  // namespace

// A 16:9 game's DLSS vectors (one value everywhere, in pixels) into a 21:9 copy: in the middle in DLSS-G's units, no motion in the bars.
void FitMotionCases(ID3D11Device* device, ID3D11DeviceContext* context) {
  std::printf("vectors into the picture's middle\n");
  depth::Shutdown();
  const UINT source_width = 160, source_height = 90, width = 210, height = 90;
  std::vector<uint16_t> texels(source_width * source_height * 2);
  const uint16_t ten = 0x4900, minus_five = 0xC500;  // 10.0 and -5.0 as half floats
  for (size_t i = 0; i < texels.size(); i += 2) {
    texels[i] = ten;
    texels[i + 1] = minus_five;
  }
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = source_width;
  desc.Height = source_height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  const D3D11_SUBRESOURCE_DATA data{texels.data(), source_width * 4, 0};
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11ShaderResourceView> srv;
  device->CreateTexture2D(&desc, &data, &texture);
  device->CreateShaderResourceView(texture.Get(), nullptr, &srv);
  const float to_pixels[2] = {1.f, 1.f};
  ID3D11Texture2D* const out = depth::FitMotion(device, srv.Get(), 0, 0, source_width, source_height, to_pixels, width, height);
  Check(out != nullptr, "fitted vectors come out");
  if (out == nullptr) return;
  D3D11_TEXTURE2D_DESC out_desc{};
  out->GetDesc(&out_desc);
  out_desc.Usage = D3D11_USAGE_STAGING;
  out_desc.BindFlags = 0;
  out_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> staging;
  device->CreateTexture2D(&out_desc, nullptr, &staging);
  context->CopyResource(staging.Get(), out);
  D3D11_MAPPED_SUBRESOURCE mapped{};
  context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  const auto half = [&](UINT x, UINT y, int c) {
    const uint16_t h = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch)[x * 2 + c];
    const int exponent = (h >> 10) & 31, mantissa = h & 1023;
    const float value = exponent == 0 ? mantissa / 1024.f / 16384.f : std::ldexp(1.f + mantissa / 1024.f, exponent - 15);
    return (h & 0x8000) != 0 ? -value : value;
  };
  // 160 of 210 wide is the picture: 10 px of 160 is 1/16 of the picture, 160/210 of the copy. -5 px of 90 is all of the height.
  const float want_x = 10.f / 160.f * 160.f / 210.f, want_y = -5.f / 90.f;
  Check(std::abs(half(width / 2, height / 2, 0) - want_x) < want_x * 0.01f && std::abs(half(width / 2, height / 2, 1) - want_y) < -want_y * 0.01f, "middle in UV of the copy");
  Check(half(5, height / 2, 0) == 0.f && half(width - 5, height / 2, 1) == 0.f, "no motion in the bars");
  context->Unmap(staging.Get(), 0);

  // The flow scaled up, blended: same shape, so the whole copy, and one value everywhere stays that value at the edges too.
  const UINT big_width = 640, big_height = 360;
  ID3D11Texture2D* const big = depth::FitMotion(device, srv.Get(), 0, 0, source_width, source_height, to_pixels, big_width, big_height, true);
  Check(big != nullptr, "smooth fitted vectors come out");
  if (big == nullptr) return;
  big->GetDesc(&out_desc);
  out_desc.Usage = D3D11_USAGE_STAGING;
  out_desc.BindFlags = 0;
  out_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  staging.Reset();
  device->CreateTexture2D(&out_desc, nullptr, &staging);
  context->CopyResource(staging.Get(), big);
  context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  const float flat_x = 10.f / 160.f, flat_y = -5.f / 90.f;
  bool flat = true;
  for (const UINT x : {0u, 3u, big_width / 2, big_width - 1})
    for (const UINT y : {0u, 2u, big_height / 2, big_height - 1})
      flat = flat && std::abs(half(x, y, 0) - flat_x) < flat_x * 0.01f && std::abs(half(x, y, 1) - flat_y) < -flat_y * 0.01f;
  Check(flat, "smooth: the whole copy, edges too");
  context->Unmap(staging.Get(), 0);
}

int main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level{};
  const D3D_FEATURE_LEVEL wanted = D3D_FEATURE_LEVEL_11_0;
  if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &wanted, 1, D3D11_SDK_VERSION, &device, &level, &context))) {
    std::printf("no device\n");
    return 2;
  }
  const Case cases[] = {
      {"D24S8, normal", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 1, 0.5f, 0.99f, false, false},
      {"D32F, reversed", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 1, 0.25f, 0.01f, true, false},
      {"D32FS8, normal", DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS, 1, 0.75f, 0.98f, false, false},
      {"D16, normal", DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_R16_UNORM, 1, 0.5f, 0.95f, false, false},
      {"D24S8 4x MSAA, normal", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, 4, 0.5f, 0.99f, false, false},
      {"flat 1.0 (nothing drawn)", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT, 1, 1.f, 1.f, false, true},
  };
  // The game's depth is larger than what goes to DLSS-G here, as with motion vectors of another size.
  const UINT source_width = 640, source_height = 360, width = 320, height = 180;
  for (const Case& c : cases) {
    std::printf("%s\n", c.name);
    depth::Shutdown();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source_width;
    desc.Height = source_height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = c.texture;
    desc.SampleDesc.Count = c.samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) {
      Check(false, "the test's depth buffer");
      continue;
    }
    Fill(context.Get(), device.Get(), texture.Get(), c.dsv, c.samples > 1, source_width, source_height, c.left, c.right);
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = c.srv;
    view.ViewDimension = c.samples > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    if (FAILED(device->CreateShaderResourceView(texture.Get(), &view, &srv))) {
      Check(false, "the test's depth view");
      continue;
    }
    ID3D11Texture2D* const out = Run(device.Get(), context.Get(), srv.Get(), width, height);
    const depth::Status s = depth::Current();
    std::printf("        problem \"%s\", measured %d, low %.4f median %.4f high %.4f, inverted %d, flat %d\n", depth::Problem(), s.measured, s.lowest, s.median, s.highest, s.inverted, s.flat);
    Check(s.source && s.measured, "measured");
    Check(s.flat == c.want_flat, "flat as expected");
    if (c.want_flat) {
      Check(out == nullptr, "a flat buffer is not sent");
      continue;
    }
    Check(s.inverted == c.want_inverted, "reversed or not as expected");
    Check(out != nullptr, "depth comes out");
    Check(depth::View() != nullptr, "debug view is there");
    if (out == nullptr) continue;
    D3D11_TEXTURE2D_DESC out_desc{};
    out->GetDesc(&out_desc);
    Check(out_desc.Format == DXGI_FORMAT_R32_FLOAT && out_desc.Width == width && out_desc.Height == height && out_desc.SampleDesc.Count == 1, "R32_FLOAT at the size asked for");
    Check(std::abs(ReadAt(device.Get(), context.Get(), out, width / 4, height / 2) - c.left) < 0.001f, "left half holds its value");
    Check(std::abs(ReadAt(device.Get(), context.Get(), out, width * 3 / 4, height / 2) - c.right) < 0.001f, "right half holds its value");
  }
  FlatForLongCases(device.Get(), context.Get());
  std::printf("no source\n");
  Check(depth::Frame(device.Get(), nullptr, width, height, nullptr) == nullptr && !depth::Current().source, "no depth buffer: nothing comes out");
  Check(!depth::Current().flat_for_long, "no depth buffer: not called flat");
  UpsideDownCases(device.Get(), context.Get());
  FitMotionCases(device.Get(), context.Get());
  depth::Shutdown();
  std::printf(g_failures == 0 ? "ALL PASSED\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
