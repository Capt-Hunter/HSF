// Takes the game's own HUD out of the picture DLSS-G generates from, on the game's Direct3D 11 device.
// Watch: from the game's draws, the moment its HUD starts: the first blended draw with no depth test on the back buffer since the last pass that was not HUD.
// Remover: the back buffer is copied at that moment, and again when ReShade's effects start. Their difference is the HUD. The finished frame with that difference
// taken away is the HUD-less image, with ReShade's effects kept, whatever they did to the colours.
// Guard: a blended full-screen pass, such as a vignette or a fade, looks like a HUD by its draw state alone. If "the HUD" covers most of the picture for a while,
// it is not used. A full-screen menu then takes the same path, which is harmless.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>

namespace hsf::hud {

class Watch {
 public:
  static constexpr int MOST_COPIES = 8;           // a game that draws its world on the back buffer alternates all frame: that is no HUD to find
  static constexpr uint32_t POST_VERTICES = 6;    // at most this many vertices, opaque, no depth: a full-screen pass, so what came before it is not HUD

  void BeginFrame() {
    open_ = true;
    started_ = overflow_ = false;
    copies_ = 0;
    on_back_draws_ = hud_draws_ = 0;
  }
  // From ReShade's effects on, draws are not the game's.
  void Close() { open_ = false; }
  // One draw on the back buffer. True: copy the back buffer now, before this draw.
  bool Draw(bool blended, bool depth_test, uint32_t vertices) {
    if (!open_) return false;
    ++on_back_draws_;
    if (blended && !depth_test) {
      ++hud_draws_;
      if (started_ || overflow_) return false;
      if (copies_ >= MOST_COPIES) {
        overflow_ = true;
        return false;
      }
      ++copies_;
      started_ = true;
      return true;
    }
    if (depth_test || vertices <= POST_VERTICES) {
      if (started_) hud_draws_ = 0;
      started_ = false;
    }
    return false;
  }
  // A whole pass that may be the HUD, whatever its draws' state. True: copy now.
  bool StartPass() {
    if (!open_ || overflow_) return false;
    if (copies_ >= MOST_COPIES) {
      overflow_ = true;
      return false;
    }
    ++copies_;
    started_ = true;
    return true;
  }
  void CountPassDraw() { ++on_back_draws_; }
  bool HaveCopy() const { return started_ && !overflow_; }
  int Copies() const { return copies_; }
  uint32_t OnBackDraws() const { return on_back_draws_; }
  uint32_t HudDraws() const { return hud_draws_; }

 private:
  bool open_ = false, started_ = false, overflow_ = false;
  int copies_ = 0;
  uint32_t on_back_draws_ = 0, hud_draws_ = 0;
};

class Guard {
 public:
  static constexpr float TOO_MUCH = 0.5f;
  static constexpr int OFF_FRAMES = 8, ON_FRAMES = 30;

  void Feed(float share) {
    if (share > TOO_MUCH) {
      good_ = 0;
      if (++bad_ >= OFF_FRAMES) in_use_ = false;
    } else {
      bad_ = 0;
      if (++good_ >= ON_FRAMES) in_use_ = true;
    }
  }
  bool InUse() const { return in_use_; }
  void Reset() { *this = Guard{}; }

 private:
  bool in_use_ = true;
  int bad_ = 0, good_ = 0;
};

class Remover {
 public:
  // Makes everything for frames like `back`. False and `why` if this frame cannot be done.
  bool Prepare(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& back, const char*& why);
  // `back` must be like the one prepared for. False if it no longer is.
  bool CopyBeforeHud(ID3D11DeviceContext* context, ID3D11Texture2D* back);
  bool CopyAtEffects(ID3D11DeviceContext* context, ID3D11Texture2D* back);
  // A game whose HUD goes on another texture that its last pass then draws to the back buffer: `source` copied at the HUD's start, with a view like the one the
  // last pass reads it by. Drawing that pass again from this copy, into BeforeHudTarget(), gives the back buffer as it would be without the HUD.
  bool CopySource(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const D3D11_SHADER_RESOURCE_VIEW_DESC& view);
  ID3D11ShaderResourceView* SourceCopy() const { return source_srv_.Get(); }
  ID3D11RenderTargetView* BeforeHudTarget() const { return before_rtv_.Get(); }
  ID3D11Texture2D* BeforeHud() const { return before_.Get(); }
  // The HUD-less image from `finished`. Stays valid until the next call.
  ID3D11Texture2D* Make(ID3D11DeviceContext* context, ID3D11Texture2D* finished);
  // The share of the picture the HUD covered on an earlier frame. Never waits; false if none has arrived.
  bool ReadShare(ID3D11DeviceContext* context, float& share);
  void Reset();

 private:
  static constexpr int STAGING = 3;
  bool Same(ID3D11Texture2D* back) const;

  Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> before_, at_effects_, finished_, out_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> before_srv_, at_effects_srv_, finished_srv_;
  Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> out_uav_, count_uav_;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> before_rtv_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> source_;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> source_srv_;
  D3D11_SHADER_RESOURCE_VIEW_DESC source_view_{};
  Microsoft::WRL::ComPtr<ID3D11Buffer> count_, staging_[STAGING];
  D3D11_TEXTURE2D_DESC desc_{};
  bool made_ = false, failed_ = false;
  int write_ = 0, read_ = 0, pending_ = 0;
};

}  // namespace hsf::hud
