#pragma once
// GPU time of a few spans of D3D11 work spread over one frame, summed per frame. A report only: each frame's queries are read back FRAMES frames
// later and never waited for. All calls on the thread that owns the immediate context.

#include <d3d11.h>
#include <wrl/client.h>

namespace hsf::gpu {

class Spans {
 public:
  static constexpr unsigned FRAMES = 4, SPANS = 6;

  // Starts a frame. Reads the frame that used this set FRAMES frames ago, if the GPU has finished it.
  void BeginFrame(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (failed_ || open_) return;
    Set& s = sets_[frame_++ % FRAMES];
    if (s.disjoint == nullptr && !Make(device, s)) return;
    if (s.used) Read(context, s);
    s.used = true;
    s.spans = 0;
    context->Begin(s.disjoint.Get());
    current_ = &s;
    open_ = true;
  }
  // A span of work; Begin and End in pairs. Spans past SPANS in a frame are not timed.
  void Begin(ID3D11DeviceContext* context) {
    if (!open_ || in_span_ || current_->spans >= SPANS) return;
    context->End(current_->begin[current_->spans].Get());
    in_span_ = true;
  }
  void End(ID3D11DeviceContext* context) {
    if (!in_span_) return;
    context->End(current_->end[current_->spans++].Get());
    in_span_ = false;
  }
  void EndFrame(ID3D11DeviceContext* context) {
    if (!open_) return;
    End(context);
    context->End(current_->disjoint.Get());
    open_ = false;
  }
  // The mean per frame since the last Take, over `frames` frames read back. False if none.
  bool Take(double& ms, unsigned& frames) {
    frames = count_;
    ms = count_ != 0 ? sum_ / count_ : 0.0;
    sum_ = 0;
    count_ = 0;
    return frames != 0;
  }
  bool Open() const { return open_; }
  // For a new device: the queries are made again on the next frame.
  void Reset() { *this = Spans{}; }

 private:
  struct Set {
    Microsoft::WRL::ComPtr<ID3D11Query> disjoint, begin[SPANS], end[SPANS];
    unsigned spans = 0;
    bool used = false;
  };

  bool Make(ID3D11Device* device, Set& s) {
    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
    bool ok = SUCCEEDED(device->CreateQuery(&disjoint, &s.disjoint));
    for (unsigned i = 0; ok && i < SPANS; ++i) ok = SUCCEEDED(device->CreateQuery(&stamp, &s.begin[i])) && SUCCEEDED(device->CreateQuery(&stamp, &s.end[i]));
    if (!ok) failed_ = true;
    return ok;
  }

  void Read(ID3D11DeviceContext* context, Set& s) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    if (context->GetData(s.disjoint.Get(), &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || d.Disjoint || d.Frequency == 0) return;
    UINT64 ticks = 0;
    for (unsigned i = 0; i < s.spans; ++i) {
      UINT64 a = 0, b = 0;
      if (context->GetData(s.begin[i].Get(), &a, sizeof(a), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK ||
          context->GetData(s.end[i].Get(), &b, sizeof(b), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
        return;
      if (b > a) ticks += b - a;
    }
    sum_ += static_cast<double>(ticks) * 1000.0 / static_cast<double>(d.Frequency);
    ++count_;
  }

  Set sets_[FRAMES];
  Set* current_ = nullptr;
  unsigned frame_ = 0;
  bool open_ = false, in_span_ = false, failed_ = false;
  double sum_ = 0;
  unsigned count_ = 0;
};

}  // namespace hsf::gpu
