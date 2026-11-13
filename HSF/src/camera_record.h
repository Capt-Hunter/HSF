// A short recording of what the camera work sees, to study offline with the same code the game runs (tests/camera_replay.cpp), so one run in a game answers many questions.
// Two files, as the matrices are known in the game's process and the flow in the helper's. They join on the game's frame number.
//   <time>-matrices.bin  add-on: every matrix found in a frame, in the order the game wrote them, not only the ones handed over
//   <time>-flow.bin      helper: the grid of motion and depth for a frame, the matrices it was handed, and what the chooser decided
#pragma once

#include "camera.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace hsf::camera::record {

constexpr uint32_t MATRICES_MAGIC = 0x584D4348, FLOW_MAGIC = 0x4C464348;  // "HCMX", "HCFL"
constexpr uint32_t FORMAT = 1;

// Fixed layouts: the add-on may be 32-bit.
#pragma pack(push, 8)
struct MatricesHeader {
  uint32_t magic, format;
  float aspect;
  uint32_t candidate_size;
};
struct MatricesFrame {
  int64_t frame;
  int32_t count;
  int32_t parts;  // views and projections, after the matrices
};
struct FlowHeader {
  uint32_t magic, format, grid_width, grid_height;
};
struct FlowFrame {
  int64_t camera_frame;
  uint64_t id;
  float scale[2];
  uint32_t width, height, input_width, input_height;
  int32_t reset, depth_inverted, source, handed;
  int64_t locked;
  float share;
  int32_t pad;
};
#pragma pack(pop)
static_assert(sizeof(Candidate) == 72 && sizeof(MatricesHeader) == 16 && sizeof(MatricesFrame) == 16 && sizeof(FlowHeader) == 16 && sizeof(FlowFrame) == 72, "recording layout changed");

// Plain buffered writing, so a frame costs a memory copy nearly always. Flushed now and then: the helper can be ended at any moment.
class File {
 public:
  ~File() { Close(); }
  bool Open(const wchar_t* path) {
    Close();
    if (_wfopen_s(&file_, path, L"wb") != 0 || file_ == nullptr) {
      file_ = nullptr;
      return false;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 1024 * 1024);
    frames_ = 0;
    return true;
  }
  void Write(const void* data, size_t bytes) {
    if (file_ != nullptr && bytes != 0) std::fwrite(data, 1, bytes, file_);
  }
  void EndFrame() {
    if (file_ != nullptr && ++frames_ % 60 == 0) std::fflush(file_);
  }
  void Close() {
    if (file_ != nullptr) std::fclose(file_);
    file_ = nullptr;
  }
  bool IsOpen() const { return file_ != nullptr; }
  uint32_t Frames() const { return frames_; }

 private:
  FILE* file_ = nullptr;
  uint32_t frames_ = 0;
};

class MatricesWriter {
 public:
  bool Open(const wchar_t* path, float aspect) {
    if (!file_.Open(path)) return false;
    const MatricesHeader h{MATRICES_MAGIC, FORMAT, aspect, static_cast<uint32_t>(sizeof(Candidate))};
    file_.Write(&h, sizeof(h));
    return true;
  }
  void Frame(int64_t frame, const Candidate* found, int count, const Candidate* parts = nullptr, int part_count = 0) {
    const MatricesFrame f{frame, count, part_count};
    file_.Write(&f, sizeof(f));
    file_.Write(found, static_cast<size_t>(count) * sizeof(Candidate));
    file_.Write(parts, static_cast<size_t>(part_count) * sizeof(Candidate));
    file_.EndFrame();
  }
  void Close() { file_.Close(); }
  bool IsOpen() const { return file_.IsOpen(); }
  uint32_t Frames() const { return file_.Frames(); }

 private:
  File file_;
};

class FlowWriter {
 public:
  bool Open(const wchar_t* path) {
    if (!file_.Open(path)) return false;
    const FlowHeader h{FLOW_MAGIC, FORMAT, SAMPLE_WIDTH, SAMPLE_HEIGHT};
    file_.Write(&h, sizeof(h));
    return true;
  }
  // `grid`: frame_checks::Samples' answer as it came, sky and all. `frame.handed` matrices follow from `handed`.
  void Frame(const FlowFrame& frame, const Candidate* handed, const float* grid) {
    file_.Write(&frame, sizeof(frame));
    file_.Write(handed, static_cast<size_t>(frame.handed) * sizeof(Candidate));
    file_.Write(grid, 4ull * SAMPLES * sizeof(float));
    file_.EndFrame();
  }
  void Close() { file_.Close(); }
  bool IsOpen() const { return file_.IsOpen(); }
  uint32_t Frames() const { return file_.Frames(); }

 private:
  File file_;
};

// ---- Reading back, offline ----
struct MatricesRecord {
  int64_t frame = 0;
  std::vector<Candidate> found;
  std::vector<Candidate> parts;
};
struct FlowRecord {
  FlowFrame head{};
  std::vector<Candidate> handed;
  std::vector<float> grid;
};

inline bool ReadMatrices(const wchar_t* path, float& aspect, std::vector<MatricesRecord>& out) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
  MatricesHeader h{};
  bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == MATRICES_MAGIC && h.format == FORMAT && h.candidate_size == sizeof(Candidate);
  aspect = h.aspect;
  MatricesFrame head{};
  while (ok && std::fread(&head, sizeof(head), 1, f) == 1) {
    if (head.count < 0 || head.parts < 0) break;
    MatricesRecord r;
    r.frame = head.frame;
    r.found.resize(static_cast<size_t>(head.count));
    // A file cut short by the game ending: keep what is whole.
    if (head.count != 0 && std::fread(r.found.data(), sizeof(Candidate), r.found.size(), f) != r.found.size()) break;
    r.parts.resize(static_cast<size_t>(head.parts));
    if (head.parts != 0 && std::fread(r.parts.data(), sizeof(Candidate), r.parts.size(), f) != r.parts.size()) break;
    out.push_back(std::move(r));
  }
  std::fclose(f);
  return ok;
}

inline bool ReadFlow(const wchar_t* path, std::vector<FlowRecord>& out) {
  FILE* f = nullptr;
  if (_wfopen_s(&f, path, L"rb") != 0 || f == nullptr) return false;
  FlowHeader h{};
  const bool ok = std::fread(&h, sizeof(h), 1, f) == 1 && h.magic == FLOW_MAGIC && h.format == FORMAT && h.grid_width == SAMPLE_WIDTH && h.grid_height == SAMPLE_HEIGHT;
  FlowRecord r;
  while (ok && std::fread(&r.head, sizeof(r.head), 1, f) == 1) {
    if (r.head.handed < 0) break;
    r.handed.resize(static_cast<size_t>(r.head.handed));
    r.grid.resize(4ull * SAMPLES);
    if (r.head.handed != 0 && std::fread(r.handed.data(), sizeof(Candidate), r.handed.size(), f) != r.handed.size()) break;
    if (std::fread(r.grid.data(), sizeof(float), r.grid.size(), f) != r.grid.size()) break;
    out.push_back(r);
  }
  std::fclose(f);
  return ok;
}

}  // namespace hsf::camera::record
