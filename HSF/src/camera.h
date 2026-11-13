// Exact motion for the still world from the camera. No graphics API in here, so the tests can feed it made-up scenes and the 32-bit add-on and
// the 64-bit helper share it.
//   Add-on, game thread: Scanner looks through what the game writes to its constant buffers for matrices shaped like a camera with this picture's aspect.
//   Helper, presenter thread: Chooser compares each candidate's motion with the optical flow on a small grid, locks the one that explains it, and watches it after that.
//   When no matrix is locked or the locked one stops agreeing: FitClipToPrev finds the same matrix from the flow and depth alone. A pixel's clip position (x, y, raw depth, 1)
//   goes linearly to its last frame's clip position, whatever the camera and depth curve, so 11 numbers describe the still world's motion. It only needs real depth.
// A matrix here maps a column vector: m[4 * row + column]. Streamline's are the transpose.
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>

namespace hsf::camera {

constexpr int CANDIDATES = 16;                 // matrices handed over per frame
constexpr uint32_t SAMPLE_WIDTH = 64, SAMPLE_HEIGHT = 36;
constexpr uint32_t SAMPLES = SAMPLE_WIDTH * SAMPLE_HEIGHT;

struct Mat4 {
  float m[16];
};

inline Mat4 Identity() {
  Mat4 r{};
  r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
  return r;
}

inline Mat4 Mul(const Mat4& a, const Mat4& b) {
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double sum = 0;
      for (int k = 0; k < 4; ++k) sum += static_cast<double>(a.m[4 * i + k]) * b.m[4 * k + j];
      r.m[4 * i + j] = static_cast<float>(sum);
    }
  return r;
}

inline Mat4 Transpose(const Mat4& a) {
  Mat4 r{};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r.m[4 * i + j] = a.m[4 * j + i];
  return r;
}

inline bool Inverse(const Mat4& in, Mat4& out) {
  double a[4][8];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      a[i][j] = in.m[4 * i + j];
      a[i][j + 4] = i == j ? 1.0 : 0.0;
    }
  for (int c = 0; c < 4; ++c) {
    int pivot = c;
    for (int i = c + 1; i < 4; ++i)
      if (std::fabs(a[i][c]) > std::fabs(a[pivot][c])) pivot = i;
    if (!(std::fabs(a[pivot][c]) > 1e-12)) return false;
    if (pivot != c)
      for (int j = 0; j < 8; ++j) std::swap(a[pivot][j], a[c][j]);
    const double scale = 1.0 / a[c][c];
    for (int j = 0; j < 8; ++j) a[c][j] *= scale;
    for (int i = 0; i < 4; ++i) {
      if (i == c) continue;
      const double f = a[i][c];
      for (int j = 0; j < 8; ++j) a[i][j] -= f * a[c][j];
    }
  }
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[4 * i + j] = static_cast<float>(a[i][j + 4]);
  return true;
}

// ---- Recognising a camera ----
// Layout of a candidate's identity, stable from one run of the game to the next, so it can be kept per game. The buffer itself is not part of it: an engine may use a
// pool of same-sized buffers. Occurrence: how many matrices of this kind were written at this place earlier in the frame, as a game can draw several views.
inline int64_t MakeKey(uint32_t byte_width, uint32_t offset, bool transposed, uint32_t occurrence) {
  return (static_cast<int64_t>(byte_width) << 24) | (static_cast<int64_t>(offset) << 4) | (static_cast<int64_t>(transposed ? 1 : 0) << 3) | static_cast<int64_t>(occurrence & 7);
}
inline uint32_t KeyWidth(int64_t key) { return static_cast<uint32_t>(key >> 24); }
inline uint32_t KeyOffset(int64_t key) { return static_cast<uint32_t>((key >> 4) & 0xfffff); }
inline bool KeyTransposed(int64_t key) { return ((key >> 3) & 1) != 0; }
inline uint32_t KeyOccurrence(int64_t key) { return static_cast<uint32_t>(key & 7); }

enum class Shape { no, wrong_aspect, camera };

// What camera-shaped means: the bottom row is the view direction, plus an offset; the top two rows are the side and up directions, at right angles, sized by the field of
// view; the third is the view direction again or next to nothing. Holds for a projection alone, projection x view, and object x view x projection with no scale or shear.
// On `camera`, `out` is the matrix with the view direction's length 1 and without TAA jitter: Streamline's matrices must not have it, and a jittered matrix makes the still
// world shake. The jitter is the share of the view direction in the top two rows, as a projection's jitter adds w x offset to x and y.
inline Shape Recognise(const float* stored, bool transposed, float aspect, Mat4& out, float* found_aspect = nullptr) {
  float r[4][4];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      r[i][j] = transposed ? stored[4 * j + i] : stored[4 * i + j];
      if (!(std::fabs(r[i][j]) < 1e7f)) return Shape::no;
    }
  const float length = std::sqrt(r[3][0] * r[3][0] + r[3][1] * r[3][1] + r[3][2] * r[3][2]);
  if (!(length > 1e-4f)) return Shape::no;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) r[i][j] /= length;
  const auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const float jitter_x = dot3(r[0], r[3]), jitter_y = dot3(r[1], r[3]);
  if (std::fabs(jitter_x) > 0.05f || std::fabs(jitter_y) > 0.05f) return Shape::no;
  for (int j = 0; j < 4; ++j) {
    r[0][j] -= jitter_x * r[3][j];
    r[1][j] -= jitter_y * r[3][j];
  }
  const float a = std::sqrt(dot3(r[0], r[0])), b = std::sqrt(dot3(r[1], r[1]));
  if (!(b > 0.25f && b < 10.f && a > 0.02f)) return Shape::no;
  if (std::fabs(dot3(r[0], r[1])) > 0.02f * a * b) return Shape::no;
  const float z = std::sqrt(dot3(r[2], r[2]));
  if (z > 1e-5f && std::fabs(dot3(r[2], r[3])) < 0.98f * z) return Shape::no;
  if (found_aspect != nullptr) *found_aspect = b / a;
  // Wide, as a game's camera need not have the window's shape: Tomb Raider's is 2.806 in a 2.513 window. Narrow enough to leave out square shadow maps. The Chooser decides the rest.
  if (std::fabs(b / a / aspect - 1.f) > 0.25f) return Shape::wrong_aspect;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[4 * i + j] = r[i][j];
  Mat4 unused;
  return Inverse(out, unused) ? Shape::camera : Shape::no;
}

// A game may keep the view and the projection in separate places, so no one matrix has both, and Recognise cannot see a view. Only looked for in a recording (camera_record.h).
// `stored`: 16 floats, or 12 with `three_rows` (a view of three rows, the fourth being 0 0 0 1). `out` is the matrix with column vectors.
enum class PartKind { none = 0, view = 1, projection = 2 };
constexpr int64_t PART_KIND_SHIFT = 60;
inline PartKind RecognisePart(const float* stored, bool transposed, bool three_rows, Mat4& out) {
  float r[4][4] = {{}, {}, {}, {0, 0, 0, 1}};
  for (int i = 0; i < (three_rows ? 3 : 4); ++i)
    for (int j = 0; j < 4; ++j) {
      r[i][j] = transposed ? stored[4 * j + i] : stored[4 * i + j];
      if (!(std::fabs(r[i][j]) < 1e7f)) return PartKind::none;
    }
  const auto dot3 = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  PartKind kind = PartKind::none;
  if (std::fabs(r[3][0]) < 1e-5f && std::fabs(r[3][1]) < 1e-5f && std::fabs(r[3][2]) < 1e-5f && std::fabs(r[3][3] - 1.f) < 1e-5f) {
    bool rigid = true;
    for (int i = 0; i < 3 && rigid; ++i) {
      if (std::fabs(dot3(r[i], r[i]) - 1.f) > 0.003f) rigid = false;
      for (int k = i + 1; k < 3 && rigid; ++k)
        if (std::fabs(dot3(r[i], r[k])) > 0.003f) rigid = false;
    }
    // Not "no view at all", which is common and says nothing.
    float off = 0;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 4; ++j) off += std::fabs(r[i][j] - (i == j ? 1.f : 0.f));
    if (rigid && off > 1e-3f) kind = PartKind::view;
  } else if (!three_rows && std::fabs(r[3][0]) < 1e-5f && std::fabs(r[3][1]) < 1e-5f && std::fabs(r[3][3]) < 1e-5f && std::fabs(std::fabs(r[3][2]) - 1.f) < 1e-3f) {
    const bool empty = std::fabs(r[0][1]) < 1e-5f && std::fabs(r[0][3]) < 1e-5f && std::fabs(r[1][0]) < 1e-5f && std::fabs(r[1][3]) < 1e-5f && std::fabs(r[2][0]) < 1e-5f && std::fabs(r[2][1]) < 1e-5f;
    if (empty && r[0][0] > 0.02f && r[0][0] < 20.f && std::fabs(r[1][1]) > 0.25f && std::fabs(r[1][1]) < 20.f && std::fabs(r[0][2]) < 0.2f && std::fabs(r[1][2]) < 0.2f && std::fabs(r[2][3]) > 1e-6f) kind = PartKind::projection;
  }
  if (kind == PartKind::none) return kind;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) out.m[4 * i + j] = r[i][j];
  return kind;
}

// One matrix from one frame, as handed over. Fixed layout: this goes through the shared block.
#pragma pack(push, 8)
struct Candidate {
  int64_t key;
  float m[16];
};
#pragma pack(pop)

// One buffer size within a frame: how often it was written and how much of that was read.
struct WidthUse {
  uint32_t width, writes, bytes, matrices;
};

// What the scan did since the last time it was asked, for the log.
struct ScanStats {
  uint64_t bytes = 0;
  double ms = 0;
  uint32_t frames = 0, writes = 0, unread_for_size = 0, unread_for_total = 0, left_for_size = 0, left_for_total = 0;
  int most = 0;  // most matrices found in one frame
  int widths = 0;
  WidthUse all[64] = {};  // the last scanned frame's sizes, least written first
};

// ---- Add-on side ----
class Scanner {
 public:
  static constexpr uint32_t MAX_WIDTH = 65536;             // a larger buffer is a ring that cannot be read cheaply, and no constant buffer has more
  static constexpr uint32_t BYTES_PER_FRAME = 512 * 1024;  // mapped memory reads slowly
  static constexpr int WIDTHS = 64;
  static constexpr int PARTS = 256;  // views and projections kept in a frame of a recording
  static constexpr int SEEN = 2048;  // room for every size's limit below, so one written late in the frame is never left out by those written before it
  // After ALWAYS_SECONDS with no lock: look only DUTY_FRAMES of every DUTY_PERIOD frames (5%), as reading the game's buffers costs the game thread time
  // (about 2 ms a frame in FFXV, which never locks). Not never: a game whose camera only shows up after long start-up screens and menus still locks later.
  static constexpr int ALWAYS_SECONDS = 30;
  static constexpr uint32_t DUTY_PERIOD = 1200, DUTY_FRAMES = 60;
  static constexpr uint32_t HANDOVER_FRAMES = 8;
  // A size may only fill this many of the frame's matrices, as Tomb Raider's many per-object sizes filled all 128.
  static constexpr uint32_t MATRICES_PER_WIDTH = 8;
  // Handed over every frame: the matrices from the least written sizes. A camera is written a few times a frame, an object's matrix once per object.
  static constexpr int QUIET_SLOTS = 8;

  // What one buffer size may use of a frame. A game can write one size thousands of times, once per object, and in draw order: with one budget for all of them that flood uses
  // it up before a camera written later in the frame is reached (seen in Tomb Raider, Castlevania and AC3). Small buffers get more writes, the largest gets one.
  static constexpr uint32_t WidthBudget(uint32_t byte_width) { return std::clamp<uint32_t>(byte_width * 4, 4096, MAX_WIDTH); }

  void SetAspect(float aspect) { aspect_ = aspect; }
  // For a short recording (camera_record.h): look at everything, locked or not, and further than usual, at more cost. The chooser keeps getting the locked matrix.
  static constexpr uint32_t RECORD_BUDGET_TIMES = 16, RECORD_MATRICES_PER_WIDTH = 256, RECORD_OCCURRENCES = 7;
  void SetRecording(bool on) {
    std::lock_guard<std::mutex> hold(lock_);
    recording_.store(on, std::memory_order_relaxed);
  }
  // Game thread, before EndFrame: every matrix of this frame, in the order the game wrote them. `out` has room for SEEN.
  int CopyFound(Candidate* out) {
    std::lock_guard<std::mutex> hold(lock_);
    std::memcpy(out, found_, static_cast<size_t>(count_) * sizeof(Candidate));
    return count_;
  }
  // The same for views and projections, `out` with room for PARTS. The kind is in the key's top bits.
  int CopyParts(Candidate* out) {
    std::lock_guard<std::mutex> hold(lock_);
    std::memcpy(out, parts_, static_cast<size_t>(part_count_) * sizeof(Candidate));
    return part_count_;
  }
  // Any thread, the game's. Fast when locked: only a buffer of the locked width costs anything.
  bool Wants(uint32_t byte_width) const {
    if (byte_width == 0 || byte_width > MAX_WIDTH) return false;
    if (recording_.load(std::memory_order_relaxed)) return true;
    const uint32_t locked = locked_width_.load(std::memory_order_relaxed);
    return locked != 0 ? locked == byte_width : scanning_.load(std::memory_order_relaxed);
  }
  // `data` holds the buffer's bytes from `offset`, `size` of them. Must only be called after Wants.
  void Write(uint32_t byte_width, const void* data, uint32_t offset, uint32_t size) {
    std::lock_guard<std::mutex> hold(lock_);
    const int64_t locked = locked_key_;
    const bool recording = recording_.load(std::memory_order_relaxed);
    if (locked != 0 && !recording) {
      const uint32_t place = KeyOffset(locked);
      if (KeyWidth(locked) != byte_width || place < offset || place + 64 > offset + size) return;
      Look(byte_width, static_cast<const char*>(data) + (place - offset), place, KeyTransposed(locked), locked);
      return;
    }
    if (offset % 16 != 0) {
      const uint32_t skip = 16 - offset % 16;
      if (size <= skip) return;
      data = static_cast<const char*>(data) + skip;
      offset += skip;
      size -= skip;
    }
    if (size < 64) return;
    ++writes_;
    WidthUse* use = nullptr;
    for (int i = 0; i < width_count_ && use == nullptr; ++i)
      if (widths_[i].width == byte_width) use = &widths_[i];
    if (use == nullptr && width_count_ < WIDTHS) {
      use = &widths_[width_count_++];
      *use = {byte_width, 0, 0, 0};
    }
    if (use != nullptr) {
      ++use->writes;
      if (use->bytes >= WidthBudget(byte_width) * (recording ? RECORD_BUDGET_TIMES : 1)) {
        ++unread_for_size_;
        return;
      }
    }
    const uint32_t frame_budget = BYTES_PER_FRAME * (recording ? RECORD_BUDGET_TIMES : 1);
    size = std::min(size, frame_budget - std::min(frame_budget, bytes_));
    if (size < 64) {
      ++unread_for_total_;
      return;
    }
    if (use != nullptr) use->bytes += size;
    bytes_ += size;
    const auto started = std::chrono::steady_clock::now();
    // Mapped memory may be slow to read in small pieces: one pass over it.
    std::memcpy(copy_, data, size);
    for (uint32_t at = 0; at + 64 <= size; at += 16) {
      const float* const s = reinterpret_cast<const float*>(copy_ + at);
      // Most data fails here: neither the bottom row nor the last column can be a direction.
      const float row = s[12] * s[12] + s[13] * s[13] + s[14] * s[14], column = s[3] * s[3] + s[7] * s[7] + s[11] * s[11];
      if (row > 1e-8f && row < 1e14f) Look(byte_width, s, offset + at, false, 0);
      if (column > 1e-8f && column < 1e14f) Look(byte_width, s, offset + at, true, 0);
    }
    if (recording)
      for (uint32_t at = 0; at + 48 <= size; at += 16) {
        const float* const s = reinterpret_cast<const float*>(copy_ + at);
        if (at + 64 <= size) {
          LookPart(byte_width, s, offset + at, false, false);
          LookPart(byte_width, s, offset + at, true, false);
        }
        // Three rows, with what follows being something else.
        LookPart(byte_width, s, offset + at, false, true);
      }
    read_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
  }
  // Game thread, at Present. Copies this frame's candidates out and starts the next frame. Returns how many; `frame` numbers the frames, so a gap shows.
  int EndFrame(Candidate* out, int64_t& frame) {
    std::lock_guard<std::mutex> hold(lock_);
    frame = ++frame_;
    int count = 0;
    if (locked_key_ != 0 && recording_.load(std::memory_order_relaxed)) {
      // Only what a locked scan would have found: the recording must not change what the game gets.
      for (int i = 0; i < count_ && count < CANDIDATES; ++i)
        if (found_[i].key == locked_key_) out[count++] = found_[i];
    } else if (count_ <= CANDIDATES) {
      for (; count < count_; ++count) out[count] = found_[count];
    } else {
      // More than fit: the quietest every frame, the others a few at a time, each few kept for some frames, as a candidate is only scored on two frames in a row. In order of the
      // key, so a game writing them in another order does not change who is next.
      struct Ranked {
        uint32_t writes;
        int64_t key;
        int index;
      };
      Ranked ranked[SEEN];
      for (int i = 0; i < count_; ++i) {
        uint32_t writes = 0;
        const uint32_t width = KeyWidth(found_[i].key);
        for (int w = 0; w < width_count_; ++w)
          if (widths_[w].width == width) writes = widths_[w].writes;
        ranked[i] = {writes, found_[i].key, i};
      }
      std::sort(ranked, ranked + count_, [](const Ranked& a, const Ranked& b) { return a.writes != b.writes ? a.writes < b.writes : a.key < b.key; });
      for (; count < QUIET_SLOTS; ++count) out[count] = found_[ranked[count].index];
      std::sort(ranked + QUIET_SLOTS, ranked + count_, [](const Ranked& a, const Ranked& b) { return a.key < b.key; });
      const uint64_t rest = static_cast<uint64_t>(count_ - QUIET_SLOTS), room = static_cast<uint64_t>(CANDIDATES - QUIET_SLOTS);
      const uint64_t start = (frame_ / HANDOVER_FRAMES * room) % rest;
      for (uint64_t i = 0; i < room; ++i) out[count++] = found_[ranked[QUIET_SLOTS + static_cast<int>((start + i) % rest)].index];
    }
    last_count_ = count_;
    most_count_ = std::max(most_count_, count_);
    count_ = 0;
    kinds_ = 0;
    if (bytes_ != 0) {
      ++scanned_frames_;
      last_width_count_ = width_count_;
      std::memcpy(last_widths_, widths_, static_cast<size_t>(width_count_) * sizeof(WidthUse));
    }
    width_count_ = 0;
    part_count_ = 0;
    total_bytes_ += bytes_;
    bytes_ = 0;
    const auto now = std::chrono::steady_clock::now();
    if (frame_ == 1) since_ = now;
    scanning_.store(locked_key_ == 0 && (now - since_ < std::chrono::seconds(ALWAYS_SECONDS) || frame_ % DUTY_PERIOD < DUTY_FRAMES), std::memory_order_relaxed);
    return count;
  }
  void Lock(int64_t key) {
    std::lock_guard<std::mutex> hold(lock_);
    locked_key_ = key;
    locked_width_.store(key != 0 ? KeyWidth(key) : 0, std::memory_order_relaxed);
    scanning_.store(key == 0, std::memory_order_relaxed);
    if (key == 0) frame_ = 0;  // back to looking all the time
  }
  // For the log: what the last frame found, and matrices that were camera-shaped but for a different aspect (the game draws to another shape, or this is a shadow map).
  int LastCount() const { return last_count_; }
  uint32_t TakeWrongAspect(float& aspect) {
    std::lock_guard<std::mutex> hold(lock_);
    const uint32_t n = wrong_aspect_;
    aspect = last_wrong_aspect_;
    wrong_aspect_ = 0;
    return n;
  }
  ScanStats TakeStats() {
    std::lock_guard<std::mutex> hold(lock_);
    ScanStats s;
    s.bytes = total_bytes_;
    s.ms = read_ms_;
    s.frames = scanned_frames_;
    s.writes = writes_;
    s.unread_for_size = unread_for_size_;
    s.unread_for_total = unread_for_total_;
    s.left_for_size = left_for_size_;
    s.left_for_total = left_for_total_;
    s.most = most_count_;
    s.widths = last_width_count_;
    std::memcpy(s.all, last_widths_, static_cast<size_t>(last_width_count_) * sizeof(WidthUse));
    std::sort(s.all, s.all + last_width_count_, [](const WidthUse& a, const WidthUse& b) { return a.writes < b.writes; });
    total_bytes_ = 0;
    read_ms_ = 0;
    scanned_frames_ = writes_ = unread_for_size_ = unread_for_total_ = left_for_size_ = left_for_total_ = 0;
    most_count_ = 0;
    return s;
  }

 private:
  void Look(uint32_t byte_width, const void* place, uint32_t offset, bool transposed, int64_t must_be) {
    float s[16];
    std::memcpy(s, place, sizeof(s));
    Mat4 m;
    float found_aspect = 0;
    const Shape shape = Recognise(s, transposed, aspect_, m, &found_aspect);
    if (shape == Shape::wrong_aspect) {
      ++wrong_aspect_;
      last_wrong_aspect_ = found_aspect;
    }
    if (shape != Shape::camera) return;
    WidthUse* use = nullptr;
    if (must_be == 0) {
      for (int i = 0; i < width_count_ && use == nullptr; ++i)
        if (widths_[i].width == byte_width) use = &widths_[i];
      if (use != nullptr && use->matrices >= (recording_.load(std::memory_order_relaxed) ? RECORD_MATRICES_PER_WIDTH : MATRICES_PER_WIDTH)) {
        ++left_for_size_;
        return;
      }
    }
    const int64_t kind = MakeKey(byte_width, offset, transposed, 0);
    uint32_t occurrence = 0;
    int k = 0;
    for (; k < kinds_; ++k)
      if (kind_[k] == kind) break;
    if (k < kinds_) {
      occurrence = ++kind_seen_[k];
      if (occurrence > (recording_.load(std::memory_order_relaxed) ? RECORD_OCCURRENCES : 3u)) return;  // written for every draw: not a camera for the frame
    } else if (kinds_ < SEEN) {
      kind_[kinds_] = kind;
      kind_seen_[kinds_] = 0;
      ++kinds_;
    } else {
      return;
    }
    const int64_t key = MakeKey(byte_width, offset, transposed, occurrence);
    if (must_be != 0 && key != must_be) return;
    if (count_ >= SEEN) {
      ++left_for_total_;
      return;
    }
    if (use != nullptr) ++use->matrices;
    Candidate& c = found_[count_++];
    c.key = key;
    std::memcpy(c.m, m.m, sizeof(c.m));
  }

  void LookPart(uint32_t byte_width, const float* s, uint32_t offset, bool transposed, bool three_rows) {
    Mat4 m;
    const PartKind kind = RecognisePart(s, transposed, three_rows, m);
    if (kind == PartKind::none || part_count_ >= PARTS) return;
    const int64_t base = MakeKey(byte_width, offset, transposed, 0) | (static_cast<int64_t>(kind) << PART_KIND_SHIFT);
    uint32_t occurrence = 0;
    for (int i = 0; i < part_count_; ++i)
      if ((parts_[i].key & ~static_cast<int64_t>(7)) == base) ++occurrence;
    if (occurrence > 7) return;
    Candidate& c = parts_[part_count_++];
    c.key = base | occurrence;
    std::memcpy(c.m, m.m, sizeof(c.m));
  }

  std::mutex lock_;
  alignas(16) char copy_[MAX_WIDTH];
  Candidate parts_[PARTS] = {};
  int part_count_ = 0;
  std::atomic<uint32_t> locked_width_{0};
  std::atomic<bool> scanning_{true};
  std::atomic<bool> recording_{false};
  int64_t locked_key_ = 0;
  float aspect_ = 16.f / 9.f;
  Candidate found_[SEEN] = {};
  int count_ = 0, last_count_ = 0;
  int64_t kind_[SEEN] = {};
  uint32_t kind_seen_[SEEN] = {};
  int kinds_ = 0;
  WidthUse widths_[WIDTHS] = {}, last_widths_[WIDTHS] = {};
  int width_count_ = 0, last_width_count_ = 0, most_count_ = 0;
  uint32_t bytes_ = 0, wrong_aspect_ = 0, scanned_frames_ = 0, writes_ = 0, unread_for_size_ = 0, unread_for_total_ = 0, left_for_size_ = 0, left_for_total_ = 0;
  float last_wrong_aspect_ = 0;
  uint64_t total_bytes_ = 0;
  double read_ms_ = 0;
  uint64_t frame_ = 0;
  std::chrono::steady_clock::time_point since_ = std::chrono::steady_clock::now();  // when looking all the time began (frame 1)
};

// ---- Helper side ----
// One point of the flow: where it is in clip space, and where the flow says it was one frame earlier.
struct Sample {
  float x, y, depth, to_x, to_y;
};

// The GPU's grid (frame_checks::Samples: motion x, motion y, depth, unused) into samples; points with depth at either end of the range are left out: the sky has no
// surface to trust the flow of. `scale` is DLSS-G's mvecScale. Returns how many.
inline uint32_t MakeSamples(const float* grid, const float scale[2], Sample* out) {
  uint32_t n = 0;
  for (uint32_t y = 0; y < SAMPLE_HEIGHT; ++y)
    for (uint32_t x = 0; x < SAMPLE_WIDTH; ++x) {
      const float* const g = grid + 4 * (y * SAMPLE_WIDTH + x);
      const float depth = g[2];
      if (!(depth > 1e-6f && depth < 1.f - 1e-6f) || !(std::fabs(g[0]) < 1e6f) || !(std::fabs(g[1]) < 1e6f)) continue;
      const float u = (static_cast<float>(x) + 0.5f) / SAMPLE_WIDTH, v = (static_cast<float>(y) + 0.5f) / SAMPLE_HEIGHT;
      Sample& s = out[n++];
      s.x = u * 2.f - 1.f;
      s.y = 1.f - v * 2.f;
      s.depth = depth;
      s.to_x = (u + g[0] * scale[0]) * 2.f - 1.f;
      s.to_y = 1.f - (v + g[1] * scale[1]) * 2.f;
    }
  return n;
}

// Close enough, in frame pixels. Flow shakes about a pixel, more when fast.
constexpr float AGREE_PIXELS = 1.f, AGREE_SHARE = 0.15f;

// The share of the samples whose flow this matrix explains, on a picture of `width` x `height`.
inline float Agreement(const Sample* samples, uint32_t n, const Mat4& clip_to_prev, float width, float height) {
  if (n == 0) return 0;
  uint32_t good = 0;
  const float* const m = clip_to_prev.m;
  const float half_w = width * 0.5f, half_h = height * 0.5f;
  for (uint32_t i = 0; i < n; ++i) {
    const Sample& s = samples[i];
    const float w = m[12] * s.x + m[13] * s.y + m[14] * s.depth + m[15];
    if (!(w > 1e-6f)) continue;
    const float dx = ((m[0] * s.x + m[1] * s.y + m[2] * s.depth + m[3]) / w - s.to_x) * half_w;
    const float dy = ((m[4] * s.x + m[5] * s.y + m[6] * s.depth + m[7]) / w - s.to_y) * half_h;
    const float fx = (s.to_x - s.x) * half_w, fy = (s.to_y - s.y) * half_h;
    const float limit = AGREE_PIXELS + AGREE_SHARE * std::sqrt(fx * fx + fy * fy);
    if (dx * dx + dy * dy < limit * limit) ++good;
  }
  return static_cast<float>(good) / static_cast<float>(n);
}

namespace detail {

constexpr int UNKNOWNS = 11;

// The unknowns are the matrix's difference from "no motion": top row, second row, then the first three of the w row. Depth is centred and scaled, as it often sits in a narrow
// range; a small pull towards no motion keeps it solvable when depth is one value or the samples are few.
struct Fit {
  double h[UNKNOWNS] = {};
  double residual_x(double x, double y, double z, double to_x) const {
    const double w = h[8] * x + h[9] * y + h[10] * z + 1.0;
    return ((1.0 + h[0]) * x + h[1] * y + h[2] * z + h[3]) / w - to_x;
  }
  double residual_y(double x, double y, double z, double to_y) const {
    const double w = h[8] * x + h[9] * y + h[10] * z + 1.0;
    return (h[4] * x + (1.0 + h[5]) * y + h[6] * z + h[7]) / w - to_y;
  }
};

// Weighted least squares, or a few chosen samples. False if it cannot be solved.
inline bool Solve(const Sample* samples, const uint32_t* chosen, uint32_t n, const float* weights, double centre, double spread, Fit& fit) {
  double a[UNKNOWNS][UNKNOWNS] = {};
  double b[UNKNOWNS] = {};
  for (uint32_t k = 0; k < n; ++k) {
    const uint32_t i = chosen != nullptr ? chosen[k] : k;
    const double weight = weights != nullptr ? weights[i] : 1.0;
    if (weight <= 0.0) continue;
    const Sample& s = samples[i];
    const double x = s.x, y = s.y, z = (s.depth - centre) * spread, X = s.to_x, Y = s.to_y;
    const double row_x[UNKNOWNS] = {x, y, z, 1, 0, 0, 0, 0, -X * x, -X * y, -X * z};
    const double row_y[UNKNOWNS] = {0, 0, 0, 0, x, y, z, 1, -Y * x, -Y * y, -Y * z};
    const double to_x = X - x, to_y = Y - y;
    for (int p = 0; p < UNKNOWNS; ++p) {
      for (int q = p; q < UNKNOWNS; ++q) a[p][q] += weight * (row_x[p] * row_x[q] + row_y[p] * row_y[q]);
      b[p] += weight * (row_x[p] * to_x + row_y[p] * to_y);
    }
  }
  for (int p = 0; p < UNKNOWNS; ++p) a[p][p] += 1e-5;
  // Cholesky.
  for (int p = 0; p < UNKNOWNS; ++p) {
    for (int q = p; q < UNKNOWNS; ++q) {
      double sum = a[p][q];
      for (int k = 0; k < p; ++k) sum -= a[k][p] * a[k][q];
      if (q == p) {
        if (!(sum > 1e-14)) return false;
        a[p][p] = std::sqrt(sum);
      } else {
        a[p][q] = sum / a[p][p];
      }
    }
  }
  double y[UNKNOWNS];
  for (int p = 0; p < UNKNOWNS; ++p) {
    double sum = b[p];
    for (int k = 0; k < p; ++k) sum -= a[k][p] * y[k];
    y[p] = sum / a[p][p];
  }
  for (int p = UNKNOWNS - 1; p >= 0; --p) {
    double sum = y[p];
    for (int k = p + 1; k < UNKNOWNS; ++k) sum -= a[p][k] * fit.h[k];
    fit.h[p] = sum / a[p][p];
  }
  return true;
}

inline double Limit(const Sample& s, double half_w, double half_h) {
  const double fx = (s.to_x - s.x) * half_w, fy = (s.to_y - s.y) * half_h;
  return AGREE_PIXELS + AGREE_SHARE * std::sqrt(fx * fx + fy * fy);
}

// How many samples this fit explains.
inline uint32_t CountGood(const Sample* samples, uint32_t n, const Fit& fit, double centre, double spread, double half_w, double half_h) {
  uint32_t good = 0;
  for (uint32_t i = 0; i < n; ++i) {
    const Sample& s = samples[i];
    const double z = (s.depth - centre) * spread;
    const double w = fit.h[8] * s.x + fit.h[9] * s.y + fit.h[10] * z + 1.0;
    if (w < 1e-3) continue;
    const double dx = fit.residual_x(s.x, s.y, z, s.to_x) * half_w, dy = fit.residual_y(s.x, s.y, z, s.to_y) * half_h;
    const double limit = Limit(s, half_w, half_h);
    if (dx * dx + dy * dy < limit * limit) ++good;
  }
  return good;
}

}  // namespace detail

// Finds the matrix from the flow, with whatever is not still world (moving things, the HUD, the flow's mistakes) left out. `start`: last frame's answer, usually close already.
// Tries a few matrices made from a handful of samples each, as a least squares fit of everything would be pulled off by a large moving thing, then refines the best.
inline bool FitClipToPrev(const Sample* samples, uint32_t n, float width, float height, const Mat4* start, Mat4& out, float& share) {
  using namespace detail;
  if (n < 64) return false;
  double sum = 0, sum2 = 0;
  for (uint32_t i = 0; i < n; ++i) {
    sum += samples[i].depth;
    sum2 += static_cast<double>(samples[i].depth) * samples[i].depth;
  }
  const double centre = sum / n;
  const double deviation = std::sqrt(std::max(0.0, sum2 / n - centre * centre));
  const double spread = deviation > 1e-5 ? 1.0 / deviation : 1.0;
  const double half_w = width * 0.5, half_h = height * 0.5;

  Fit best;  // no motion
  uint32_t best_good = CountGood(samples, n, best, centre, spread, half_w, half_h);
  if (start != nullptr) {
    // Back from the matrix to the unknowns, in this frame's scaling of depth.
    const float* const m = start->m;
    const double w3 = m[15] + m[14] * centre;
    if (std::fabs(w3) > 1e-6) {
      Fit f;
      const double to = 1.0 / w3;
      f.h[0] = m[0] * to - 1.0;
      f.h[1] = m[1] * to;
      f.h[2] = m[2] / spread * to;
      f.h[3] = (m[3] + m[2] * centre) * to;
      f.h[4] = m[4] * to;
      f.h[5] = m[5] * to - 1.0;
      f.h[6] = m[6] / spread * to;
      f.h[7] = (m[7] + m[6] * centre) * to;
      f.h[8] = m[12] * to;
      f.h[9] = m[13] * to;
      f.h[10] = m[14] / spread * to;
      const uint32_t good = CountGood(samples, n, f, centre, spread, half_w, half_h);
      if (good > best_good) {
        best = f;
        best_good = good;
      }
    }
  }
  // A fixed sequence, so a run can be repeated.
  uint32_t state = 0x9e3779b9u ^ n;
  const auto next = [&state]() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  };
  for (int attempt = 0; attempt < 24; ++attempt) {
    uint32_t chosen[8];
    for (uint32_t& c : chosen) c = next() % n;
    Fit f;
    if (!Solve(samples, chosen, 8, nullptr, centre, spread, f)) continue;
    const uint32_t good = CountGood(samples, n, f, centre, spread, half_w, half_h);
    if (good > best_good) {
      best = f;
      best_good = good;
    }
  }
  // Refine on what agrees; each round's answer decides what agrees next. Twice the limit, tapering, so a sample near it does not flip the answer.
  static thread_local float weights[SAMPLES];
  for (int round = 0; round < 4; ++round) {
    for (uint32_t i = 0; i < n; ++i) {
      const Sample& s = samples[i];
      const double z = (s.depth - centre) * spread;
      const double dx = best.residual_x(s.x, s.y, z, s.to_x) * half_w, dy = best.residual_y(s.x, s.y, z, s.to_y) * half_h;
      const double limit = 2.0 * Limit(s, half_w, half_h);
      const double d = (dx * dx + dy * dy) / (limit * limit);
      weights[i] = d < 1.0 ? static_cast<float>((1.0 - d) * (1.0 - d)) : 0.f;
    }
    Fit f;
    if (!Solve(samples, nullptr, n, weights, centre, spread, f)) break;
    const uint32_t good = CountGood(samples, n, f, centre, spread, half_w, half_h);
    if (good < best_good) break;
    best = f;
    best_good = good;
  }
  // Back to raw depth.
  const double* const h = best.h;
  Mat4 m = Identity();
  m.m[0] = static_cast<float>(1.0 + h[0]);
  m.m[1] = static_cast<float>(h[1]);
  m.m[2] = static_cast<float>(h[2] * spread);
  m.m[3] = static_cast<float>(h[3] - h[2] * spread * centre);
  m.m[4] = static_cast<float>(h[4]);
  m.m[5] = static_cast<float>(1.0 + h[5]);
  m.m[6] = static_cast<float>(h[6] * spread);
  m.m[7] = static_cast<float>(h[7] - h[6] * spread * centre);
  m.m[12] = static_cast<float>(h[8]);
  m.m[13] = static_cast<float>(h[9]);
  m.m[14] = static_cast<float>(h[10] * spread);
  m.m[15] = static_cast<float>(1.0 - h[10] * spread * centre);
  out = m;
  share = static_cast<float>(best_good) / static_cast<float>(n);
  return true;
}

enum class Source : int32_t { none = 0, fit = 1, matrix = 2 };

struct Decision {
  Source source = Source::none;
  Mat4 clip_to_prev = Identity();
  Mat4 world_to_clip = Identity();  // this frame's camera, without jitter; only with Source::matrix
  float share = 0;                  // how much of the flow it explains
  bool changed = false;             // a different source or a new lock this frame
};

// Decides per frame which matrix to trust, all from how well each explains the flow. Drops a locked one after some frames in a row of not agreeing, and takes it back after as many of
// agreeing. Not at once: in AC3 a right matrix explains under 40% of the picture on fast pans, as the flow fails there, and dropping it at once kept it in use on 24% of frames. The
// compose pass still asks last frame's picture at each pixel, so a few frames of a wrong matrix do little.
class Chooser {
 public:
  static constexpr float LOCK_SHARE = 0.55f, KEEP_SHARE = 0.4f, FIT_SHARE = 0.6f;
  static constexpr int LOCK_FRAMES = 20, HINT_FRAMES = 5, GOOD_RUN = 60, BAD_FRAMES = 300;
  static constexpr int MATRIX_OFF_FRAMES = 10, MATRIX_ON_FRAMES = 10;
  static constexpr float MOVING_PIXELS = 1.f;  // below this the flow says too little about a camera
  static constexpr uint32_t FIT_SAMPLES = SAMPLES / 4;  // a picture that is mostly sky leaves too little: the fit would describe one object and hand its motion to everything
  static constexpr int TALLIES = 128;

  // For the log: what happened since the last time it was asked, and the best candidates so far.
  struct Scored {
    int64_t key;
    int frames;
    float mean, last, aspect;
  };
  struct Report {
    uint32_t frames = 0, usable = 0, moving = 0, with_candidates = 0;
    uint64_t samples = 0;
    int tallied = 0, top_count = 0;
    Scored top[5] = {};         // the best
    Scored most_scored[5] = {};  // scored on the most frames: the quiet ones, so a low score here means a low score with plenty of chances
  };
  Report TakeReport() {
    Report r = report_;
    report_ = Report{};
    r.tallied = tallies_;
    Scored all[TALLIES];
    for (int t = 0; t < tallies_; ++t) all[t] = {tally_[t].key, tally_[t].frames, tally_[t].frames > 0 ? tally_[t].sum / static_cast<float>(tally_[t].frames) : 0.f, tally_[t].last, tally_[t].aspect};
    const auto end = std::partial_sort_copy(all, all + tallies_, r.top, r.top + 5, [](const Scored& a, const Scored& b) { return a.mean > b.mean; });
    r.top_count = static_cast<int>(end - r.top);
    std::partial_sort_copy(all, all + tallies_, r.most_scored, r.most_scored + 5, [](const Scored& a, const Scored& b) { return a.frames > b.frames; });
    return r;
  }

  // `hint`: the key that won in this game before, or 0. It locks sooner, on the same test.
  void SetHint(int64_t hint) { hint_ = hint; }
  // Off: no fit, so with no matrix locked the frame is flow only. A fit carries the flow's shake into the whole picture at once, which Tomb Raider showed as a shaking camera.
  void SetFitAllowed(bool on) {
    fit_allowed_ = on;
    if (!on) {
      have_fit_ = fit_on_ = false;
      fit_run_ = 0;
    }
  }
  int64_t LockedKey() const { return locked_; }

  Decision Frame(const Sample* samples, uint32_t n, const Candidate* candidates, int count, int64_t frame, bool reset, float width, float height) {
    Decision d;
    const bool consecutive = !reset && have_last_ && frame == last_frame_ + 1;
    const float moving = MedianSpeed(samples, n, width, height);
    ++report_.frames;
    report_.samples += n;
    if (consecutive && n >= 64) {
      ++report_.usable;
      if (moving >= MOVING_PIXELS) {
        ++report_.moving;
        if (count > 0) ++report_.with_candidates;
      }
      if (locked_ != 0) {
        Candidate cur, prev;
        if (Find(candidates, count, locked_, cur) && Find(last_, last_count_, locked_, prev)) {
          absent_ = 0;
          Mat4 ctp;
          const float share = ClipToPrev(samples, n, cur, prev, width, height, ctp);
          const bool good = share >= KEEP_SHARE;
          good_run_ = good ? good_run_ + 1 : 0;
          low_run_ = good ? 0 : low_run_ + 1;
          if (!good) {
            if (low_run_ >= MATRIX_OFF_FRAMES) matrix_on_ = false;
            if (moving >= MOVING_PIXELS) ++bad_;
          } else {
            bad_ = 0;
            if (good_run_ >= MATRIX_ON_FRAMES) matrix_on_ = true;
          }
          if (matrix_on_) {
            d.source = Source::matrix;
            d.clip_to_prev = ctp;
            std::memcpy(d.world_to_clip.m, cur.m, sizeof(cur.m));
            d.share = share;
          }
        } else {
          matrix_on_ = false;
          good_run_ = low_run_ = 0;
          ++absent_;
        }
        // Gone for good: another level, another mode of the game. Look again.
        if (bad_ >= BAD_FRAMES || absent_ >= BAD_FRAMES) Unlock();
      } else if (moving >= MOVING_PIXELS) {
        Tally(samples, n, candidates, count, width, height);
      }
    }
    if (!fit_allowed_) {
      // Nothing to do.
    } else if (d.source == Source::none && consecutive) {
      Mat4 fitted;
      float share = 0;
      if (n < FIT_SAMPLES) {
        have_fit_ = fit_on_ = false;
        fit_run_ = 0;
      } else if (FitClipToPrev(samples, n, width, height, have_fit_ ? &fit_ : nullptr, fitted, share)) {
        fit_ = fitted;
        have_fit_ = share >= FIT_SHARE;
        fit_run_ = share >= FIT_SHARE ? fit_run_ + 1 : 0;
        if (share < FIT_SHARE) fit_on_ = false;
        else if (fit_run_ >= GOOD_RUN) fit_on_ = true;
        if (fit_on_) {
          d.source = Source::fit;
          d.clip_to_prev = fitted;
          d.share = share;
        }
      }
    } else if (!consecutive) {
      have_fit_ = false;
    }
    if (d.source != last_source_ || locked_ != last_locked_) d.changed = true;
    last_source_ = d.source;
    last_locked_ = locked_;
    have_last_ = true;
    last_frame_ = frame;
    last_count_ = std::min(count, CANDIDATES);
    std::memcpy(last_, candidates, static_cast<size_t>(last_count_) * sizeof(Candidate));
    return d;
  }

 private:
  static bool Find(const Candidate* list, int count, int64_t key, Candidate& out) {
    for (int i = 0; i < count; ++i)
      if (list[i].key == key) {
        out = list[i];
        return true;
      }
    return false;
  }

  static float ClipToPrev(const Sample* samples, uint32_t n, const Candidate& cur, const Candidate& prev, float width, float height, Mat4& out) {
    Mat4 c, p, inv;
    std::memcpy(c.m, cur.m, sizeof(c.m));
    std::memcpy(p.m, prev.m, sizeof(p.m));
    if (!Inverse(c, inv)) return 0;
    out = Mul(p, inv);
    return Agreement(samples, n, out, width, height);
  }

  static float MedianSpeed(const Sample* samples, uint32_t n, float width, float height) {
    if (n == 0) return 0;
    static thread_local float speed[SAMPLES];
    for (uint32_t i = 0; i < n; ++i) {
      const float x = (samples[i].to_x - samples[i].x) * width * 0.5f, y = (samples[i].to_y - samples[i].y) * height * 0.5f;
      speed[i] = x * x + y * y;
    }
    std::nth_element(speed, speed + n / 2, speed + n);
    return std::sqrt(speed[n / 2]);
  }

  void Tally(const Sample* samples, uint32_t n, const Candidate* candidates, int count, float width, float height) {
    for (int i = 0; i < count; ++i) {
      Candidate prev;
      if (!Find(last_, last_count_, candidates[i].key, prev)) continue;
      Mat4 ctp;
      const float share = ClipToPrev(samples, n, candidates[i], prev, width, height, ctp);
      int t = 0;
      for (; t < tallies_; ++t)
        if (tally_[t].key == candidates[i].key) break;
      if (t == tallies_) {
        if (tallies_ == TALLIES) {
          // Full: the one with the worst score that has had its chance makes room. Otherwise a game with many matrices fills this and a later one is never scored.
          int worst = -1;
          float worst_mean = KEEP_SHARE;
          for (int u = 0; u < tallies_; ++u) {
            if (tally_[u].frames < LOCK_FRAMES) continue;
            const float mean = tally_[u].sum / static_cast<float>(tally_[u].frames);
            if (mean < worst_mean) {
              worst_mean = mean;
              worst = u;
            }
          }
          if (worst < 0) continue;
          t = worst;
          tally_[t] = {candidates[i].key, 0.f, 0, 0.f, 0.f};
        } else {
          tally_[tallies_++] = {candidates[i].key, 0.f, 0, 0.f, 0.f};
        }
      }
      const float* const m = candidates[i].m;
      const float across = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]), up = std::sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
      tally_[t].aspect = across > 0.f ? up / across : 0.f;
      tally_[t].last = share;
      tally_[t].sum += share;
      if (++tally_[t].frames > 120) {  // the recent past counts most
        tally_[t].sum *= 0.5f;
        tally_[t].frames /= 2;
      }
    }
    int best = -1;
    float best_mean = 0;
    for (int t = 0; t < tallies_; ++t) {
      if (tally_[t].frames < (tally_[t].key == hint_ ? HINT_FRAMES : LOCK_FRAMES)) continue;
      const float mean = tally_[t].sum / static_cast<float>(tally_[t].frames);
      if (mean > best_mean) {
        best_mean = mean;
        best = t;
      }
    }
    if (best >= 0 && best_mean >= LOCK_SHARE) {
      locked_ = tally_[best].key;
      matrix_on_ = true;
      good_run_ = GOOD_RUN;
      low_run_ = bad_ = absent_ = 0;
    }
  }

  void Unlock() {
    locked_ = 0;
    matrix_on_ = false;
    good_run_ = low_run_ = bad_ = absent_ = 0;
    tallies_ = 0;
  }

  struct TallyEntry {
    int64_t key;
    float sum;
    int frames;
    float last, aspect;
  };
  Report report_;
  int low_run_ = 0;  // locked matrix's frames in a row of not agreeing
  Candidate last_[CANDIDATES] = {};
  int last_count_ = 0;
  bool have_last_ = false;
  int64_t last_frame_ = 0;
  int64_t locked_ = 0, last_locked_ = 0, hint_ = 0;
  bool matrix_on_ = false;
  int good_run_ = 0, bad_ = 0, absent_ = 0;
  TallyEntry tally_[TALLIES] = {};
  int tallies_ = 0;
  Mat4 fit_ = Identity();
  bool have_fit_ = false, fit_on_ = false, fit_allowed_ = true;
  int fit_run_ = 0;
  Source last_source_ = Source::none;
};

// ---- DLSS-G's camera constants ----
struct Parts {
  float pos[3], up[3], right[3], fwd[3];
  float near_plane, far_plane, fov, aspect;
  Mat4 view_to_clip;  // for a view space of (right, up, forward)
};

// `m` from Recognise: the bottom row's direction has length 1. Holds for projection x view; with an object matrix in it the position is that object's frame's.
inline bool Decompose(const Mat4& m, Parts& p) {
  const float* const v = m.m;
  const auto length = [](const float* a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); };
  const float a = length(v), b = length(v + 4);
  if (!(a > 1e-6f) || !(b > 1e-6f)) return false;
  for (int i = 0; i < 3; ++i) {
    p.right[i] = v[i] / a;
    p.up[i] = v[4 + i] / b;
    p.fwd[i] = v[12 + i];
  }
  // The camera is where x, y and w of the clip position are all 0: the directions are at right angles, so each row gives its share.
  for (int i = 0; i < 3; ++i) p.pos[i] = -(v[3] / a * p.right[i] + v[7] / b * p.up[i] + v[15] * p.fwd[i]);
  // Depth at distance D along the view: A + B / D.
  const float A = v[8] * p.fwd[0] + v[9] * p.fwd[1] + v[10] * p.fwd[2];
  const float B = v[8] * p.pos[0] + v[9] * p.pos[1] + v[10] * p.pos[2] + v[11];
  const float at_0 = std::fabs(A) > 1e-7f ? -B / A : 1e6f, at_1 = std::fabs(1.f - A) > 1e-7f ? B / (1.f - A) : 1e6f;
  p.near_plane = std::max(std::min(at_0, at_1), 1e-4f);
  p.far_plane = std::min(std::max(at_0, at_1), 1e7f);
  p.fov = 2.f * std::atan(1.f / b);
  p.aspect = b / a;
  p.view_to_clip = Mat4{};
  p.view_to_clip.m[0] = a;
  p.view_to_clip.m[5] = b;
  p.view_to_clip.m[10] = A;
  p.view_to_clip.m[11] = B;
  p.view_to_clip.m[14] = 1.f;
  return true;
}

}  // namespace hsf::camera
