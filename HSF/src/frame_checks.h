// Cheap fixes that need no game data, on HSF's own Direct3D 12 device, so one version serves every game API and both bitnesses.
// Grid and Classifier: a small luma grid of every frame, read back before the frame is handed to DLSS-G, says whether this frame is a scene cut (DLSS-G's
// reset) or part of a still picture such as a menu or loading screen (zero motion). DLSS-G's own menu detection needs a UI buffer, which HSF does not have.
// Mask: finds what stays put on screen while the optical flow says it moves, which is mostly the HUD, and gives it zero motion.
// UiAlpha: the finished frame against a copy of it from before the overlays were drawn: where they differ is overlay, which DLSS-G then keeps out of generation.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace hsf::frame_checks {

constexpr UINT GRID_WIDTH = 128, GRID_HEIGHT = 72;
constexpr UINT GRID_CELLS = GRID_WIDTH * GRID_HEIGHT;
constexpr UINT FRAMES = 3;  // frames in flight: one set of descriptors and timestamps each

struct Verdict {
  bool cut = false;    // this frame does not follow from the last one
  bool still = false;  // the picture has not changed for a while
  float changed = 0;   // share of the grid's cells that clearly changed
  float shape = 0;     // how far the brightness histogram moved, brightness change removed: 0 = same, 2 = nothing in common
};

// Decides from the grid alone, so a test can feed it pictures.
// A cut changes most cells AND the histogram's shape. A camera turn changes most cells but keeps the shape; a flash changes the brightness but, with the
// mean taken out, keeps the shape too. A wrong cut costs one frame's generation, a missed one a smeared frame.
class Classifier {
 public:
  static constexpr float CUT_CHANGED = 0.6f, CUT_SHAPE = 0.7f;
  static constexpr float CHANGED_LUMA = 0.04f;  // a cell has clearly changed
  static constexpr float STILL_LUMA = 0.008f, STILL_SHARE = 0.002f;  // about 2 levels of 255 in 1 cell of 500: still, allowing for grain
  static constexpr int STILL_FRAMES = 10;

  Verdict Frame(const float* grid) {
    Verdict v;
    float histogram[BINS] = {};
    float mean = 0;
    for (UINT i = 0; i < GRID_CELLS; ++i) mean += grid[i];
    mean /= static_cast<float>(GRID_CELLS);
    for (UINT i = 0; i < GRID_CELLS; ++i) {
      const int bin = static_cast<int>((grid[i] - mean + 1.f) * 0.5f * BINS);
      histogram[bin < 0 ? 0 : bin >= BINS ? BINS - 1 : bin] += 1.f / static_cast<float>(GRID_CELLS);
    }
    if (have_last_) {
      UINT changed = 0, moved = 0;
      for (UINT i = 0; i < GRID_CELLS; ++i) {
        const float d = std::fabs(grid[i] - last_[i]);
        changed += d > CHANGED_LUMA ? 1 : 0;
        moved += d > STILL_LUMA ? 1 : 0;
      }
      v.changed = static_cast<float>(changed) / static_cast<float>(GRID_CELLS);
      for (int b = 0; b < BINS; ++b) v.shape += std::fabs(histogram[b] - last_histogram_[b]);
      v.cut = v.changed > CUT_CHANGED && v.shape > CUT_SHAPE;
      still_frames_ = static_cast<float>(moved) / static_cast<float>(GRID_CELLS) < STILL_SHARE ? still_frames_ + 1 : 0;
      v.still = still_frames_ >= STILL_FRAMES;
    }
    std::memcpy(last_, grid, sizeof(last_));
    std::memcpy(last_histogram_, histogram, sizeof(last_histogram_));
    have_last_ = true;
    return v;
  }

 private:
  static constexpr int BINS = 64;
  float last_[GRID_CELLS] = {};
  float last_histogram_[BINS] = {};
  bool have_last_ = false;
  int still_frames_ = 0;
};

// Reads the grid of a frame on a compute queue of its own, so it does not queue behind anything else, and waits for it: DLSS-G's reset is set before the
// frame goes in, so an answer that comes a frame later is no use.
class Grid {
 public:
  // `colour_format` must be readable from the frame's texture as a shader resource.
  bool Init(ID3D12Device* device, DXGI_FORMAT colour_format, const char*& what, HRESULT& hr);
  // Measures `colour` (in COMMON). False if the GPU did not answer in time: no verdict for this frame.
  bool Run(ID3D12Resource* colour, UINT width, UINT height, Verdict& verdict);
  // True once `every` waits have been counted: `ms` is their mean, and the count starts again.
  bool TakeAverageWaitMs(UINT every, double& ms);

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer_, readback_;
  HANDLE event_ = nullptr;
  UINT64 value_ = 0;
  const float* mapped_ = nullptr;
  UINT descriptor_size_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  Classifier classifier_;
  LONGLONG ticks_per_second_ = 0;
  double sum_ms_ = 0;
  UINT count_ = 0;
};

// Reads the motion and depth on a small grid for the camera work (camera.h), the same way: on a compute queue of its own, and the presenter waits, as the matrix it leads to is
// for this frame. Each point: motion x, motion y, depth, unused.
class Samples {
 public:
  bool Init(ID3D12Device* device, DXGI_FORMAT motion_format, const char*& what, HRESULT& hr);
  // `motion` and `depth` (R32_FLOAT) in COMMON, the same size. False if the GPU did not answer in time. `grid` stays valid until the next Run.
  bool Run(ID3D12Resource* motion, ID3D12Resource* depth, UINT width, UINT height, const float*& grid);

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc_;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer_, readback_;
  HANDLE event_ = nullptr;
  UINT64 value_ = 0;
  const float* mapped_ = nullptr;
  UINT descriptor_size_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
};

// Follows each pixel over time. A pixel with detail, whose colour is the same as last frame's in place but not at the place its motion points to, is
// staying put while the flow says it moves. After about 16 such frames in a row it gets zero motion. It lets go over a few frames once its colour changes.
// So a still world does not build up (no motion, no evidence) and a flat moving surface does not either (no detail, and its motion does no harm).
class Mask {
 public:
  bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT motion_format, DXGI_FORMAT colour_resource_format, DXGI_FORMAT colour_format,
            UINT frame_width, UINT frame_height, UINT motion_width, UINT motion_height, const char*& what, HRESULT& hr);

  // `colour` and `motion` must be readable as shader resources (COMMON promotes, or already NON_PIXEL_SHADER_RESOURCE), `out` in UNORDERED_ACCESS and it
  // stays there. `scale` is DLSS-G's mvecScale. `all_still`: zero motion everywhere this frame. Leaves the list's compute state set.
  void Record(ID3D12GraphicsCommandList* list, UINT frame, ID3D12Resource* colour, ID3D12Resource* motion, ID3D12Resource* out, const float scale[2], bool all_still);

  // True once `every` finished passes have been timed: `ms` is their mean on the GPU, and the count starts again.
  bool TakeAverageMs(UINT every, double& ms);

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
  Microsoft::WRL::ComPtr<ID3D12Resource> times_buffer_;
  Microsoft::WRL::ComPtr<ID3D12Resource> last_colour_;
  Microsoft::WRL::ComPtr<ID3D12Resource> still_[2];  // this frame's is written from last frame's; they swap
  const UINT64* times_ = nullptr;
  bool timed_[FRAMES] = {};
  UINT current_ = 0;
  bool have_last_ = false;
  UINT descriptor_size_ = 0;
  DXGI_FORMAT motion_format_ = DXGI_FORMAT_UNKNOWN, colour_format_ = DXGI_FORMAT_UNKNOWN;
  UINT frame_width_ = 0, frame_height_ = 0, motion_width_ = 0, motion_height_ = 0;
  double ticks_per_ms_ = 0;
  double sum_ms_ = 0;
  UINT count_ = 0;
};

// Writes DLSS-G's UI alpha: 0 where the frame with overlays matches the clean frame, rising with the difference elsewhere.
class UiAlpha {
 public:
  bool Init(ID3D12Device* device, DXGI_FORMAT colour_format, UINT width, UINT height, const char*& what, HRESULT& hr);
  // `finished` and `clean` must be readable as shader resources (COMMON promotes), `alpha` (R8_UNORM) in UNORDERED_ACCESS, and it stays there.
  // Leaves the list's compute state set.
  void Record(ID3D12GraphicsCommandList* list, UINT frame, ID3D12Resource* finished, ID3D12Resource* clean, ID3D12Resource* alpha);

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  UINT descriptor_size_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  UINT width_ = 0, height_ = 0;
};

// One pair of frames as the score measured it (score.h says what it rates). kind: 0 counted, 1 almost nothing changed, 2 the whole picture changed and the motion did not explain it.
struct ScoreSample {
  float warp = 0, none = 0;        // mean luma error (0-1) with the motion and without
  float better = 0, worse = 0;     // share of the changed patches where the motion clearly helped, clearly hurt
  float moving = 0;                // share of all patches that changed
  int kind = 0;
};

// The checks for a native D3D12 game with its own DLSS-G: on the game's own queue, next to its work, and they never wait for the GPU, so answers come a few frames late.
// So the grid cannot set DLSS-G's reset, which goes in with a frame's constants before the frame exists, and Streamline refuses a second slSetConstants for a frame. The grid says afterwards
// that a cut happened. The score copies two frames in a row and the vectors between them a few times a second. NoteVectors is on the thread with the game's open command list, AtPresent on the present thread.
class GameChecks {
 public:
  using GridFn = void (*)(const Verdict&);
  using ScoreFn = void (*)(const ScoreSample&);
  GameChecks();
  ~GameChecks();  // waits briefly for work in flight
  GameChecks(const GameChecks&) = delete;
  GameChecks& operator=(const GameChecks&) = delete;

  // `state` is what the game says the vectors are in, and they go back to it. extent: left, top, width, height. Records a copy only for the one frame the score wants.
  void NoteVectors(ID3D12GraphicsCommandList* list, ID3D12Resource* motion, UINT state, const UINT extent[4], const float scale[2]);
  // `back_buffer` in PRESENT, the frame finished. Answers that have arrived go to the callbacks. False once it has given up: Problem says why.
  bool AtPresent(ID3D12CommandQueue* queue, ID3D12Resource* back_buffer, GridFn on_grid, ScoreFn on_score);
  const char* Problem() const { return problem_; }
  void SetPeriod(UINT ms) { period_ms_ = ms; }  // from one measured pair to the next, default 250. Tests use 0.

 private:
  static constexpr UINT RING = 4;
  static constexpr UINT DESCRIPTORS = 6;  // grid: colour and buffer. Score: two frames, vectors and buffer.
  struct Slot {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Resource> grid, grid_back;
    const float* grid_mapped = nullptr;
    UINT64 value = 0;
    bool in_use = false, has_score = false;
  };
  enum class Stage { wait, before_taken, reading };
  bool Init(ID3D12CommandQueue* queue);
  bool MakeFrames(const D3D12_RESOURCE_DESC& desc);
  bool MakeMotion();
  void Drain(GridFn on_grid, ScoreFn on_score);
  void Fail(const char* what, HRESULT hr);

  SRWLOCK lock_ = SRWLOCK_INIT;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> grid_root_, score_root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> grid_pso_, score_pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> before_, after_, motion_, cells_, cells_back_;
  Slot slots_[RING];
  UINT next_ = 0;
  UINT64 value_ = 0;
  UINT descriptor_size_ = 0;
  const float* cells_mapped_ = nullptr;
  UINT width_ = 0, height_ = 0, grid_width_ = 0, grid_height_ = 0;
  DXGI_FORMAT frame_format_ = DXGI_FORMAT_UNKNOWN, motion_format_ = DXGI_FORMAT_UNKNOWN;
  UINT motion_capacity_width_ = 0, motion_capacity_height_ = 0;
  DXGI_FORMAT want_motion_format_ = DXGI_FORMAT_UNKNOWN;
  UINT want_motion_width_ = 0, want_motion_height_ = 0;
  Stage stage_ = Stage::wait;
  bool vectors_taken_ = false;
  UINT motion_width_ = 0, motion_height_ = 0;
  float scale_[2] = {};
  ULONGLONG next_pair_ = 0;
  UINT period_ms_ = 250;
  bool failed_ = false;
  char problem_[128] = "";
  Classifier classifier_;
};

}  // namespace hsf::frame_checks
