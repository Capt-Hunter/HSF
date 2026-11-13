// Studies a recording of the camera work (src/camera_record.h) with the code the game runs, no game needed.
//   camera_replay.exe <time>-matrices.bin <time>-flow.bin [more pairs]
// What it answers, for each pair:
//   - does the chooser, fed what it was fed in the game, do what it did in the game
//   - how much of the flow could any one camera explain at all (a fit from the flow and depth): low means the flow or the depth is the trouble, not the matrices
//   - how much each matrix explains, whether its motion is the flow's with a wrong scale or sign, and whether it matches a neighbouring frame's flow better
//   - whether taking a buffer's writes by anything other than a fixed place in the frame's order helps
//   - on how many frames at least one matrix is good: can a choice per frame work
#include "../src/camera_record.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <unordered_map>
#include <vector>

using namespace hsf::camera;
using namespace hsf::camera::record;

namespace {

constexpr float LOCK = Chooser::LOCK_SHARE, MOVING = Chooser::MOVING_PIXELS;

struct Per {
  std::vector<float> shares;
  std::vector<float> scales, sizes;  // per frame, see Compare
  float aspect = 0;
};

void KeyText(int64_t key, char* text, size_t size) {
  std::snprintf(text, size, "%5u bytes at %5u %s write %u", KeyWidth(key), KeyOffset(key), KeyTransposed(key) ? "cols" : "rows", KeyOccurrence(key));
}

float Mean(const std::vector<float>& v) {
  if (v.empty()) return 0;
  double sum = 0;
  for (float f : v) sum += f;
  return static_cast<float>(sum / v.size());
}

float Quantile(std::vector<float> v, float q) {
  if (v.empty()) return 0;
  const size_t at = std::min(v.size() - 1, static_cast<size_t>(q * v.size()));
  std::nth_element(v.begin(), v.begin() + at, v.end());
  return v[at];
}

float MedianSpeed(const Sample* s, uint32_t n, float width, float height) {
  std::vector<float> speed(n);
  for (uint32_t i = 0; i < n; ++i) {
    const float x = (s[i].to_x - s[i].x) * width * 0.5f, y = (s[i].to_y - s[i].y) * height * 0.5f;
    speed[i] = std::sqrt(x * x + y * y);
  }
  return Quantile(speed, 0.5f);
}

bool PairMatrix(const Candidate& cur, const Candidate& prev, Mat4& out) {
  Mat4 c, p, inverse;
  std::memcpy(c.m, cur.m, sizeof(c.m));
  std::memcpy(p.m, prev.m, sizeof(p.m));
  if (!Inverse(c, inverse)) return false;
  out = Mul(p, inverse);
  return true;
}

// The matrix's motion against the flow on one frame, by medians over the points, as a moving object can be far larger than the rest: the scale that maps one on the other
// (1 = right, negative = wrong way), and the matrix's motion in pixels.
void Compare(const Sample* samples, uint32_t n, const Mat4& ctp, float width, float height, Per& per) {
  const float* const m = ctp.m;
  std::vector<float> ratio, size;
  ratio.reserve(n);
  size.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const Sample& s = samples[i];
    const float w = m[12] * s.x + m[13] * s.y + m[14] * s.depth + m[15];
    if (!(w > 1e-6f)) continue;
    const float px = ((m[0] * s.x + m[1] * s.y + m[2] * s.depth + m[3]) / w - s.x) * width * 0.5f, py = ((m[4] * s.x + m[5] * s.y + m[6] * s.depth + m[7]) / w - s.y) * height * 0.5f;
    const float fx = (s.to_x - s.x) * width * 0.5f, fy = (s.to_y - s.y) * height * 0.5f;
    const float flow_sq = fx * fx + fy * fy;
    size.push_back(std::sqrt(px * px + py * py));
    if (flow_sq >= 1.f) ratio.push_back((px * fx + py * fy) / flow_sq);
  }
  if (!ratio.empty()) per.scales.push_back(Quantile(ratio, 0.5f));
  per.sizes.push_back(Quantile(size, 0.5f));
}

const Candidate* Find(const std::vector<Candidate>& list, int64_t key) {
  for (const Candidate& c : list)
    if (c.key == key) return &c;
  return nullptr;
}

// One matrix per object: a key names a place in the frame's order, and another object can be there next frame. For a still object cur = VP W and prev = VP' W, so prev * inverse(cur) is
// VP' * inverse(VP), the camera's motion, whatever W is, and every still object gives the same answer. So: pair each matrix with the most alike one of the last frame, and take the
// answer most of them agree on. The flow is not asked at all, only used here to check the answer.
struct Consensus {
  Mat4 ctp = Identity();
  Candidate cur{}, prev{};  // the pair that gave the answer
  int agree = 0, tried = 0;
  bool found = false;
};

Consensus FindConsensus(const std::vector<Candidate>& cur, const std::vector<Candidate>& prev, const Sample* samples, uint32_t n, float width, float height, size_t room) {
  Consensus out;
  if (cur.empty() || prev.empty() || n < 64) return out;
  constexpr uint32_t PROBES = 24;
  const uint32_t step = std::max<uint32_t>(1, n / PROBES);
  const size_t stride = std::max<size_t>(1, cur.size() / room);
  std::vector<Mat4> answers;
  std::vector<std::pair<const Candidate*, const Candidate*>> pairs;
  std::vector<float> where;  // the probes' places for each answer, in pixels
  uint32_t probes = 0;
  for (uint32_t i = 0; i < n && probes < PROBES; i += step) ++probes;
  for (size_t c = 0; c < cur.size(); c += stride) {
    double nearest = 1e300;
    const Candidate* match = nullptr;
    for (const Candidate& p : prev) {
      double d = 0;
      for (int k = 0; k < 16; ++k) {
        const double e = static_cast<double>(cur[c].m[k]) - p.m[k];
        d += e * e;
      }
      if (d < nearest) {
        nearest = d;
        match = &p;
      }
    }
    Mat4 ctp;
    if (match == nullptr || !PairMatrix(cur[c], *match, ctp)) continue;
    bool good = true;
    const size_t at = where.size();
    where.resize(at + 2ull * probes);
    uint32_t place = 0;
    for (uint32_t i = 0; i < n && place < probes; i += step, ++place) {
      const Sample& s = samples[i];
      const float* const m = ctp.m;
      const float w = m[12] * s.x + m[13] * s.y + m[14] * s.depth + m[15];
      if (!(w > 1e-6f)) {
        good = false;
        break;
      }
      where[at + 2ull * place] = (m[0] * s.x + m[1] * s.y + m[2] * s.depth + m[3]) / w * width * 0.5f;
      where[at + 2ull * place + 1] = (m[4] * s.x + m[5] * s.y + m[6] * s.depth + m[7]) / w * height * 0.5f;
    }
    if (!good) {
      where.resize(at);
      continue;
    }
    answers.push_back(ctp);
    pairs.push_back({&cur[c], match});
  }
  out.tried = static_cast<int>(answers.size());
  for (size_t a = 0; a < answers.size(); ++a) {
    int agree = 0;
    for (size_t b = 0; b < answers.size(); ++b) {
      float worst = 0;
      for (uint32_t i = 0; i < 2 * probes; ++i) worst = std::max(worst, std::fabs(where[a * 2 * probes + i] - where[b * 2 * probes + i]));
      if (worst < 1.5f) ++agree;
    }
    if (agree > out.agree) {
      out.agree = agree;
      out.ctp = answers[a];
      out.cur = *pairs[a].first;
      out.prev = *pairs[a].second;
      out.found = true;
    }
  }
  return out;
}

// A matrix with the camera's turn and lens but not where it stands: objects placed relative to the camera, so the place is not in the matrices at all. Then the turn and lens are exact and
// only the camera's move is unknown: three numbers, far fewer than a whole camera's, found from the flow and depth. A point X = inverse(cur) * (x, y, depth, 1) was at prev * (X + move) last frame,
// which is linear in the move. Robust: only the points that agree count, each round. Returns the matrix, from this frame's clip space to the last frame's.
bool TurnAndMove(const Candidate& cur, const Candidate& prev, const Sample* samples, uint32_t n, float width, float height, Mat4& ctp, double move[3]) {
  Mat4 c, inverse;
  std::memcpy(c.m, cur.m, sizeof(c.m));
  if (!Inverse(c, inverse)) return false;
  const float* const inv = inverse.m;
  const float* const p = prev.m;
  move[0] = move[1] = move[2] = 0;
  const auto make = [&]() {
    // prev * translate(move) * inverse(cur)
    Mat4 shift = Identity(), pm;
    shift.m[3] = static_cast<float>(move[0]);
    shift.m[7] = static_cast<float>(move[1]);
    shift.m[11] = static_cast<float>(move[2]);
    std::memcpy(pm.m, p, sizeof(pm.m));
    ctp = Mul(Mul(pm, shift), inverse);
  };
  make();
  const float half_w = width * 0.5f, half_h = height * 0.5f;
  for (int round = 0; round < 8; ++round) {
    double a[3][3] = {}, b[3] = {};
    for (uint32_t i = 0; i < n; ++i) {
      const Sample& s = samples[i];
      const double W = inv[12] * s.x + inv[13] * s.y + inv[14] * s.depth + inv[15];
      if (std::fabs(W) < 1e-12) continue;
      const double X = (inv[0] * s.x + inv[1] * s.y + inv[2] * s.depth + inv[3]) / W, Y = (inv[4] * s.x + inv[5] * s.y + inv[6] * s.depth + inv[7]) / W, Z = (inv[8] * s.x + inv[9] * s.y + inv[10] * s.depth + inv[11]) / W;
      // Weight by the last round's answer; the first round has only the turn, so wide.
      const float* const m = ctp.m;
      const float w = m[12] * s.x + m[13] * s.y + m[14] * s.depth + m[15];
      if (!(w > 1e-6f)) continue;
      const float dx = ((m[0] * s.x + m[1] * s.y + m[2] * s.depth + m[3]) / w - s.to_x) * half_w, dy = ((m[4] * s.x + m[5] * s.y + m[6] * s.depth + m[7]) / w - s.to_y) * half_h;
      const float fx = (s.to_x - s.x) * half_w, fy = (s.to_y - s.y) * half_h;
      const float limit = (round < 3 ? 16.f : round < 6 ? 6.f : 3.f) * (AGREE_PIXELS + AGREE_SHARE * std::sqrt(fx * fx + fy * fy));
      const float d = (dx * dx + dy * dy) / (limit * limit);
      if (d >= 1.f) continue;
      const double weight = (1.0 - d) * (1.0 - d);
      for (int axis = 0; axis < 2; ++axis) {
        const float* const r = p + 4 * axis;
        const double to = axis == 0 ? s.to_x : s.to_y;
        const double row[3] = {r[0] - to * p[12], r[1] - to * p[13], r[2] - to * p[14]};
        const double rhs = -((r[0] - to * p[12]) * X + (r[1] - to * p[13]) * Y + (r[2] - to * p[14]) * Z + (r[3] - to * p[15]));
        const double pixels = (axis == 0 ? half_w : half_h) * (axis == 0 ? half_w : half_h);
        for (int j = 0; j < 3; ++j) {
          for (int k = 0; k < 3; ++k) a[j][k] += weight * pixels * row[j] * row[k];
          b[j] += weight * pixels * row[j] * rhs;
        }
      }
    }
    const double pull = 1e-9 * (a[0][0] + a[1][1] + a[2][2]) + 1e-12;
    for (int j = 0; j < 3; ++j) a[j][j] += pull;
    // Gauss.
    for (int j = 0; j < 3; ++j) {
      int pivot = j;
      for (int k = j + 1; k < 3; ++k)
        if (std::fabs(a[k][j]) > std::fabs(a[pivot][j])) pivot = k;
      if (std::fabs(a[pivot][j]) < 1e-30) return false;
      if (pivot != j) {
        std::swap(a[pivot], a[j]);
        std::swap(b[pivot], b[j]);
      }
      for (int k = j + 1; k < 3; ++k) {
        const double f = a[k][j] / a[j][j];
        for (int l = j; l < 3; ++l) a[k][l] -= f * a[j][l];
        b[k] -= f * b[j];
      }
    }
    for (int j = 2; j >= 0; --j) {
      for (int k = j + 1; k < 3; ++k) b[j] -= a[j][k] * b[k];
      b[j] /= a[j][j];
      move[j] = b[j];
    }
    make();
  }
  return true;
}

// Every matrix of this frame with every one of the last, as a check of the idea: is a right answer in there at all? Slow, so only some frames, on some of the grid.
float BestOfAllPairs(const std::vector<Candidate>& cur, const std::vector<Candidate>& prev, const Sample* samples, uint32_t n, float width, float height) {
  std::vector<Sample> some;
  for (uint32_t i = 0; i < n; i += 16) some.push_back(samples[i]);
  float best = 0;
  for (const Candidate& c : cur)
    for (const Candidate& p : prev) {
      Mat4 ctp;
      if (PairMatrix(c, p, ctp)) best = std::max(best, Agreement(some.data(), static_cast<uint32_t>(some.size()), ctp, width, height));
    }
  return best;
}

void Study(const wchar_t* matrices_path, const wchar_t* flow_path) {
  float aspect = 0;
  std::vector<MatricesRecord> matrices;
  std::vector<FlowRecord> flow;
  if (!ReadMatrices(matrices_path, aspect, matrices)) {
    std::wprintf(L"cannot read %ls\n", matrices_path);
    return;
  }
  if (!ReadFlow(flow_path, flow)) {
    std::wprintf(L"cannot read %ls\n", flow_path);
    return;
  }
  std::wprintf(L"\n==== %ls\n     %ls\n", matrices_path, flow_path);
  if (flow.empty() || matrices.empty()) {
    std::printf("empty: %zu frames of matrices, %zu of flow\n", matrices.size(), flow.size());
    return;
  }
  std::unordered_map<int64_t, size_t> by_frame;
  size_t most = 0, total = 0;
  for (size_t i = 0; i < matrices.size(); ++i) {
    by_frame[matrices[i].frame] = i;
    most = std::max(most, matrices[i].found.size());
    total += matrices[i].found.size();
  }
  const FlowFrame& first = flow.front().head;
  const float width = static_cast<float>(first.width), height = static_cast<float>(first.height);
  std::printf("matrices: frames %lld to %lld (%zu), %.1f a frame, most %zu, window aspect %.3f\n", static_cast<long long>(matrices.front().frame), static_cast<long long>(matrices.back().frame), matrices.size(),
              static_cast<double>(total) / matrices.size(), most, aspect);
  std::printf("flow: frames %lld to %lld (%zu), picture %ux%u, input %ux%u, scale %g %g, depth inverted %d\n", static_cast<long long>(flow.front().head.camera_frame), static_cast<long long>(flow.back().head.camera_frame), flow.size(), first.width,
              first.height, first.input_width, first.input_height, first.scale[0], first.scale[1], first.depth_inverted);

  std::vector<Sample> samples(SAMPLES);
  std::map<int64_t, Per> keys;
  std::vector<float> best_shares, fit_shares, speeds;
  std::map<int64_t, int> winners;
  std::map<int64_t, std::vector<float>> kind_best, kind_last;
  uint32_t reset_frames = 0, joined = 0, moving = 0, resets_or_gaps = 0;
  uint64_t points = 0;
  uint32_t run = 0, longest_run = 0;
  Chooser chooser;
  int64_t chooser_locked = 0, game_locked = 0;
  const auto text_of = [](int64_t key) {
    static char text[80];
    if (key == 0) return "none";
    KeyText(key, text, sizeof(text));
    return static_cast<const char*>(text);
  };
  struct Second {
    double speed = 0, fit = 0, best = 0;
    uint32_t frames = 0, good = 0;
  };
  std::vector<Second> timeline;
  Mat4 last_fit;
  bool have_fit = false;
  for (size_t f = 0; f < flow.size(); ++f) {
    const FlowRecord& r = flow[f];
    const uint32_t n = MakeSamples(r.grid.data(), r.head.scale, samples.data());
    points += n;
    if (r.head.reset != 0) ++reset_frames;
    // The chooser, as in the game.
    chooser.Frame(samples.data(), n, r.handed.data(), static_cast<int>(r.handed.size()), r.head.camera_frame, r.head.reset != 0, width, height);
    if (chooser.LockedKey() != chooser_locked) {
      chooser_locked = chooser.LockedKey();
      std::printf("replayed chooser, frame %zu of the flow: locked: %s\n", f, text_of(chooser_locked));
    }
    if (r.head.locked != game_locked) {
      game_locked = r.head.locked;
      std::printf("in the game, frame %zu of the flow: locked: %s\n", f, text_of(game_locked));
    }
    const auto cur_at = by_frame.find(r.head.camera_frame), prev_at = by_frame.find(r.head.camera_frame - 1);
    if (cur_at == by_frame.end() || prev_at == by_frame.end() || r.head.reset != 0) {
      ++resets_or_gaps;
      have_fit = false;
      continue;
    }
    ++joined;
    const float speed = MedianSpeed(samples.data(), n, width, height);
    if (speed < MOVING || n < 64) continue;
    ++moving;
    const std::vector<Candidate>& cur = matrices[cur_at->second].found;
    const std::vector<Candidate>& prev = matrices[prev_at->second].found;
    float best = 0;
    int64_t best_key = 0;
    std::map<int64_t, float> frame_kind_best, frame_kind_last;
    std::map<int64_t, uint32_t> frame_kind_last_occurrence;
    for (const Candidate& c : cur) {
      const Candidate* const p = Find(prev, c.key);
      if (p == nullptr) continue;
      Mat4 ctp;
      if (!PairMatrix(c, *p, ctp)) continue;
      const float share = Agreement(samples.data(), n, ctp, width, height);
      Per& per = keys[c.key];
      per.shares.push_back(share);
      Compare(samples.data(), n, ctp, width, height, per);
      const float across = std::sqrt(c.m[0] * c.m[0] + c.m[1] * c.m[1] + c.m[2] * c.m[2]), up = std::sqrt(c.m[4] * c.m[4] + c.m[5] * c.m[5] + c.m[6] * c.m[6]);
      per.aspect = across > 0 ? up / across : 0;
      if (share > best) {
        best = share;
        best_key = c.key;
      }
      const int64_t kind = c.key & ~static_cast<int64_t>(7);
      frame_kind_best[kind] = std::max(frame_kind_best[kind], share);
      if (frame_kind_last_occurrence.find(kind) == frame_kind_last_occurrence.end() || KeyOccurrence(c.key) >= frame_kind_last_occurrence[kind]) {
        frame_kind_last_occurrence[kind] = KeyOccurrence(c.key);
        frame_kind_last[kind] = share;
      }
    }
    for (const auto& k : frame_kind_best) kind_best[k.first].push_back(k.second);
    for (const auto& k : frame_kind_last) kind_last[k.first].push_back(k.second);
    best_shares.push_back(best);
    if (best_key != 0) ++winners[best_key];
    run = best >= LOCK ? run + 1 : 0;
    longest_run = std::max(longest_run, run);
    // What one camera could explain at best.
    Mat4 fitted;
    float fit_share = 0;
    if (n >= Chooser::FIT_SAMPLES && FitClipToPrev(samples.data(), n, width, height, have_fit ? &last_fit : nullptr, fitted, fit_share)) {
      last_fit = fitted;
      have_fit = true;
    } else {
      have_fit = false;
    }
    fit_shares.push_back(fit_share);
    speeds.push_back(speed);
    const size_t second = f / 50;
    if (timeline.size() <= second) timeline.resize(second + 1);
    Second& t = timeline[second];
    t.speed += speed;
    t.fit += fit_share;
    t.best += best;
    ++t.frames;
    if (best >= LOCK) ++t.good;
  }
  std::printf("frames: %u joined, %u reset or missing a matrix frame, %u moving (median over %.0f pixels); %.0f of %u grid points have depth; %u resets\n", joined, resets_or_gaps, moving, MOVING, static_cast<double>(points) / flow.size(), SAMPLES,
              reset_frames);
  if (moving == 0) return;
  std::printf("speed: median %.1f pixels a frame, 90%% under %.1f\n", Quantile(speeds, 0.5f), Quantile(speeds, 0.9f));
  std::printf("any one camera (fit from the flow and depth): explains %.0f%% on the mean, median %.0f%%. Low here means flow or depth is the trouble, not the matrices.\n", Mean(fit_shares) * 100.f, Quantile(fit_shares, 0.5f) * 100.f);
  uint32_t over_lock = 0, over_keep = 0;
  for (float s : best_shares) {
    if (s >= LOCK) ++over_lock;
    if (s >= Chooser::KEEP_SHARE) ++over_keep;
  }
  std::printf("best matrix each frame: explains %.0f%% on the mean; at least %.0f%% on %.0f%% of moving frames, at least %.0f%% on %.0f%%; longest run at the first %u frames; %zu different winners\n", Mean(best_shares) * 100.f, LOCK * 100.f,
              100.0 * over_lock / moving, Chooser::KEEP_SHARE * 100.f, 100.0 * over_keep / moving, longest_run, winners.size());

  std::vector<std::pair<int64_t, float>> order;
  for (const auto& k : keys)
    if (k.second.shares.size() * 20 >= moving) order.push_back({k.first, Mean(k.second.shares)});  // on at least one moving frame in 20
  std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::printf("%zu matrices seen; those on at least 5%% of moving frames, best first (scale: of the flow; 1 = right, negative = wrong way; moves: its median motion in pixels):\n", keys.size());
  char text[80];
  for (size_t i = 0; i < order.size() && i < 16; ++i) {
    const Per& p = keys[order[i].first];
    KeyText(order[i].first, text, sizeof(text));
    std::printf("  %s aspect %.3f: on %5zu frames, explains mean %3.0f%% median %3.0f%% top tenth %3.0f%%; scale %6.2f moves %6.1f\n", text, p.aspect, p.shares.size(), Mean(p.shares) * 100.f, Quantile(p.shares, 0.5f) * 100.f, Quantile(p.shares, 0.9f) * 100.f,
                Quantile(p.scales, 0.5f), Quantile(p.sizes, 0.5f));
  }
  std::printf("the most frequent winners of a frame:\n");
  std::vector<std::pair<int64_t, int>> by_wins(winners.begin(), winners.end());
  std::sort(by_wins.begin(), by_wins.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  for (size_t i = 0; i < by_wins.size() && i < 8; ++i) {
    KeyText(by_wins[i].first, text, sizeof(text));
    std::printf("  %s: %d frames\n", text, by_wins[i].second);
  }

  // A buffer written more than once a frame: is its place in the frame's order the trouble?
  std::vector<std::pair<int64_t, float>> kinds;
  for (const auto& k : kind_best)
    if (k.second.size() * 20 >= moving) kinds.push_back({k.first, Mean(k.second)});
  std::sort(kinds.begin(), kinds.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
  std::printf("by place in the buffer, any write of the frame (best of them, and the last of them):\n");
  for (size_t i = 0; i < kinds.size() && i < 8; ++i) {
    KeyText(kinds[i].first, text, sizeof(text));
    text[std::strlen(text) - 8] = 0;  // no write number
    std::printf("  %s: best %3.0f%%, last %3.0f%%, on %zu frames\n", text, kinds[i].second * 100.f, Mean(kind_last[kinds[i].first]) * 100.f, kind_best[kinds[i].first].size());
  }

  // Is the matrix out of step with the flow?
  std::printf("the best matrices against the flow of neighbouring frames (flow frame + shift):\n");
  for (size_t i = 0; i < order.size() && i < 5; ++i) {
    KeyText(order[i].first, text, sizeof(text));
    std::printf("  %s:", text);
    for (int shift = -2; shift <= 2; ++shift) {
      std::vector<float> shares;
      for (const FlowRecord& r : flow) {
        const auto cur_at = by_frame.find(r.head.camera_frame + shift), prev_at = by_frame.find(r.head.camera_frame + shift - 1);
        if (cur_at == by_frame.end() || prev_at == by_frame.end()) continue;
        const uint32_t n = MakeSamples(r.grid.data(), r.head.scale, samples.data());
        if (n < 64 || MedianSpeed(samples.data(), n, width, height) < MOVING) continue;
        const Candidate* const c = Find(matrices[cur_at->second].found, order[i].first);
        const Candidate* const p = Find(matrices[prev_at->second].found, order[i].first);
        Mat4 ctp;
        if (c != nullptr && p != nullptr && PairMatrix(*c, *p, ctp)) shares.push_back(Agreement(samples.data(), n, ctp, width, height));
      }
      std::printf(" %+d: %3.0f%%", shift, Mean(shares) * 100.f);
    }
    std::printf("\n");
  }

  // What the game did, and how the first matrix does by speed and across the picture.
  {
    uint32_t on_matrix = 0, on_fit = 0, usable = 0;
    for (const FlowRecord& r : flow) {
      ++usable;
      if (r.head.source == static_cast<int32_t>(Source::matrix)) ++on_matrix;
      if (r.head.source == static_cast<int32_t>(Source::fit)) ++on_fit;
    }
    std::printf("in the game: matrix in use on %.0f%% of frames, the fit on %.0f%%\n", 100.0 * on_matrix / usable, 100.0 * on_fit / usable);
  }
  if (!order.empty()) {
    const int64_t key = order[0].first;
    struct Bin {
      float from, to;
      std::vector<float> shares, errors, fits;
    } bins[] = {{1, 10, {}, {}, {}}, {10, 30, {}, {}, {}}, {30, 80, {}, {}, {}}, {80, 160, {}, {}, {}}, {160, 1e9f, {}, {}, {}}};
    uint32_t map_good[6][12] = {}, map_all[6][12] = {};
    for (const FlowRecord& r : flow) {
      const auto cur_at = by_frame.find(r.head.camera_frame), prev_at = by_frame.find(r.head.camera_frame - 1);
      if (cur_at == by_frame.end() || prev_at == by_frame.end() || r.head.reset != 0) continue;
      const Candidate* const c = Find(matrices[cur_at->second].found, key);
      const Candidate* const p = Find(matrices[prev_at->second].found, key);
      Mat4 ctp;
      if (c == nullptr || p == nullptr || !PairMatrix(*c, *p, ctp)) continue;
      const uint32_t n = MakeSamples(r.grid.data(), r.head.scale, samples.data());
      if (n < 64) continue;
      const float speed = MedianSpeed(samples.data(), n, width, height);
      std::vector<float> errors;
      errors.reserve(n);
      const float* const m = ctp.m;
      for (uint32_t i = 0; i < n; ++i) {
        const Sample& s = samples[i];
        const float w = m[12] * s.x + m[13] * s.y + m[14] * s.depth + m[15];
        if (!(w > 1e-6f)) continue;
        const float dx = ((m[0] * s.x + m[1] * s.y + m[2] * s.depth + m[3]) / w - s.to_x) * width * 0.5f, dy = ((m[4] * s.x + m[5] * s.y + m[6] * s.depth + m[7]) / w - s.to_y) * height * 0.5f;
        const float fx = (s.to_x - s.x) * width * 0.5f, fy = (s.to_y - s.y) * height * 0.5f;
        const float error = std::sqrt(dx * dx + dy * dy), limit = AGREE_PIXELS + AGREE_SHARE * std::sqrt(fx * fx + fy * fy);
        errors.push_back(error);
        if (speed >= MOVING) {
          const int my = std::clamp(static_cast<int>((1.f - s.y) * 0.5f * 6.f), 0, 5), mx = std::clamp(static_cast<int>((s.x + 1.f) * 0.5f * 12.f), 0, 11);
          ++map_all[my][mx];
          if (error < limit) ++map_good[my][mx];
        }
      }
      for (Bin& b : bins)
        if (speed >= b.from && speed < b.to) {
          b.shares.push_back(Agreement(samples.data(), n, ctp, width, height));
          b.errors.push_back(Quantile(errors, 0.5f));
        }
    }
    KeyText(key, text, sizeof(text));
    std::printf("the best matrix (%s) by the picture's speed:\n", text);
    for (const Bin& b : bins)
      if (!b.shares.empty()) std::printf("  %5.0f to %5.0f px: %5zu frames, explains %3.0f%% of the points, its median difference from the flow %5.1f px\n", b.from, std::min(b.to, 99999.f), b.shares.size(), Mean(b.shares) * 100.f, Quantile(b.errors, 0.5f));
    std::printf("the same across the picture, percent of points it explains:\n");
    for (int y = 0; y < 6; ++y) {
      std::printf("  ");
      for (int x = 0; x < 12; ++x) std::printf("%4u", map_all[y][x] != 0 ? 100 * map_good[y][x] / map_all[y][x] : 0);
      std::printf("\n");
    }
  }

  // One matrix per object: pair them by likeness and let them agree.
  {
    std::vector<float> shares, all_pairs;
    uint32_t good_frames = 0, none = 0, frames = 0;
    double agreeing = 0, tried = 0;
    for (size_t f = 0; f < flow.size(); ++f) {
      const FlowRecord& r = flow[f];
      const auto cur_at = by_frame.find(r.head.camera_frame), prev_at = by_frame.find(r.head.camera_frame - 1);
      if (cur_at == by_frame.end() || prev_at == by_frame.end() || r.head.reset != 0) continue;
      const uint32_t n = MakeSamples(r.grid.data(), r.head.scale, samples.data());
      if (n < 64 || MedianSpeed(samples.data(), n, width, height) < MOVING) continue;
      ++frames;
      const std::vector<Candidate>& cur = matrices[cur_at->second].found;
      const std::vector<Candidate>& prev = matrices[prev_at->second].found;
      const Consensus c = FindConsensus(cur, prev, samples.data(), n, width, height, 200);
      if (!c.found) {
        ++none;
        continue;
      }
      const float share = Agreement(samples.data(), n, c.ctp, width, height);
      shares.push_back(share);
      if (share >= LOCK) ++good_frames;
      agreeing += c.agree;
      tried += c.tried;
      if (f % 100 == 0) all_pairs.push_back(BestOfAllPairs(cur, prev, samples.data(), n, width, height));
    }
    std::printf("matrices paired by likeness, the answer most agree on, no flow asked: on %u moving frames, none on %u; explains %.0f%% on the mean, median %.0f%%; at least %.0f%% on %.0f%% of frames; %.0f of %.0f matrices agree on a frame\n", frames, none, Mean(shares) * 100.f, Quantile(shares, 0.5f) * 100.f, LOCK * 100.f,
                frames != 0 ? 100.0 * good_frames / frames : 0.0, shares.empty() ? 0.0 : agreeing / shares.size(), shares.empty() ? 0.0 : tried / shares.size());
    std::printf("the best of every pair of matrices, on every 100th frame, on a sixteenth of the grid: explains %.0f%% on the mean, median %.0f%%, over %zu frames\n", Mean(all_pairs) * 100.f, Quantile(all_pairs, 0.5f) * 100.f, all_pairs.size());
  }

  // Does the picture sit in the window the way the matrices think? The agreed answer, with the window's clip space scaled to the game's, and with depth read other ways.
  {
    struct Frame {
      std::vector<Sample> some;
      Mat4 ctp;
      float speed;
    };
    std::vector<Frame> kept;
    bool printed = false;
    for (size_t f = 0; f < flow.size(); f += 5) {
      const FlowRecord& r = flow[f];
      const auto cur_at = by_frame.find(r.head.camera_frame), prev_at = by_frame.find(r.head.camera_frame - 1);
      if (cur_at == by_frame.end() || prev_at == by_frame.end() || r.head.reset != 0) continue;
      const uint32_t n = MakeSamples(r.grid.data(), r.head.scale, samples.data());
      if (n < 64) continue;
      const float speed = MedianSpeed(samples.data(), n, width, height);
      if (speed < 3.f) continue;
      const Consensus c = FindConsensus(matrices[cur_at->second].found, matrices[prev_at->second].found, samples.data(), n, width, height, 200);
      if (!c.found) continue;
      Frame k;
      k.ctp = c.ctp;
      k.speed = speed;
      for (uint32_t i = 0; i < n; i += 4) k.some.push_back(samples[i]);
      kept.push_back(std::move(k));
      if (!printed && speed > 8.f && speed < 40.f) {
        printed = true;
        Mat4 fitted;
        float fit_share = 0;
        if (FitClipToPrev(samples.data(), n, width, height, nullptr, fitted, fit_share)) {
          std::printf("one frame, speed %.1f px; the agreed answer (explains %.0f%%) and the fit from the flow (%.0f%%), clip to previous clip:\n", speed, Agreement(samples.data(), n, c.ctp, width, height) * 100.f, fit_share * 100.f);
          for (int row = 0; row < 4; ++row)
            std::printf("  %9.5f %9.5f %9.5f %9.5f    %9.5f %9.5f %9.5f %9.5f\n", c.ctp.m[4 * row], c.ctp.m[4 * row + 1], c.ctp.m[4 * row + 2], c.ctp.m[4 * row + 3], fitted.m[4 * row], fitted.m[4 * row + 1], fitted.m[4 * row + 2], fitted.m[4 * row + 3]);
          float dmin = 1, dmax = 0;
          for (uint32_t i = 0; i < n; ++i) {
            dmin = std::min(dmin, samples[i].depth);
            dmax = std::max(dmax, samples[i].depth);
          }
          std::printf("  depth on this frame from %.5f to %.5f\n", dmin, dmax);
        }
      }
    }
    const auto mean_share = [&](float sx, float sy, int depth_way) {
      double sum = 0;
      for (const Frame& k : kept) {
        uint32_t good = 0;
        const float* const m = k.ctp.m;
        for (const Sample& s : k.some) {
          const float x = s.x * sx, y = s.y * sy, d = depth_way == 0 ? s.depth : depth_way == 1 ? 1.f - s.depth : 0.5f;
          const float w = m[12] * x + m[13] * y + m[14] * d + m[15];
          if (!(w > 1e-6f)) continue;
          const float dx = ((m[0] * x + m[1] * y + m[2] * d + m[3]) / w / sx - s.to_x) * width * 0.5f, dy = ((m[4] * x + m[5] * y + m[6] * d + m[7]) / w / sy - s.to_y) * height * 0.5f;
          const float fx = (s.to_x - s.x) * width * 0.5f, fy = (s.to_y - s.y) * height * 0.5f;
          const float limit = AGREE_PIXELS + AGREE_SHARE * std::sqrt(fx * fx + fy * fy);
          if (dx * dx + dy * dy < limit * limit) ++good;
        }
        sum += k.some.empty() ? 0.0 : static_cast<double>(good) / k.some.size();
      }
      return kept.empty() ? 0.f : static_cast<float>(sum / kept.size());
    };
    float best = -1, best_x = 1, best_y = 1;
    int best_way = 0;
    for (int way = 0; way < 3; ++way)
      for (int ix = 70; ix <= 140; ix += 2)
        for (int iy = 70; iy <= 140; iy += 2) {
          const float share = mean_share(ix * 0.01f, iy * 0.01f, way);
          if (share > best) {
            best = share;
            best_x = ix * 0.01f;
            best_y = iy * 0.01f;
            best_way = way;
          }
        }
    static const char* const WAYS[] = {"as it is", "1 - depth", "one depth for all"};
    std::printf("the agreed answer over %zu frames: as things are explains %.0f%%; with depth 1 - d %.0f%%; with one depth for all %.0f%%; best picture scale x %.2f y %.2f, depth %s: %.0f%%\n", kept.size(), mean_share(1, 1, 0) * 100.f, mean_share(1, 1, 1) * 100.f, mean_share(1, 1, 2) * 100.f, best_x, best_y, WAYS[best_way], best * 100.f);
  }

  // The turn and lens from the matrices, the move from the flow, against a whole camera from the flow; and how each moves from frame to frame, as a camera has weight and shake does not.
  {
    std::vector<float> turn_share, fit_share_all, turn_shake, fit_shake;
    float last_turn[2] = {}, last_centre[2] = {};
    bool have_turn = false, have_fit_last = false;
    Mat4 start;
    bool have_start = false;
    int64_t last_frame = -2;
    const auto at_centre = [](const Mat4& ctp, float* out) {
      const float* const m = ctp.m;
      const float depth = 0.98f, w = m[14] * depth + m[15];
      out[0] = (m[2] * depth + m[3]) / w;
      out[1] = (m[6] * depth + m[7]) / w;
    };
    for (size_t f = 0; f < flow.size(); ++f) {
      const FlowRecord& r = flow[f];
      const auto cur_at = by_frame.find(r.head.camera_frame), prev_at = by_frame.find(r.head.camera_frame - 1);
      if (cur_at == by_frame.end() || prev_at == by_frame.end() || r.head.reset != 0) continue;
      const uint32_t n = MakeSamples(r.grid.data(), r.head.scale, samples.data());
      if (n < 64 || MedianSpeed(samples.data(), n, width, height) < MOVING) continue;
      const bool next = r.head.camera_frame == last_frame + 1;
      last_frame = r.head.camera_frame;
      const Consensus c = FindConsensus(matrices[cur_at->second].found, matrices[prev_at->second].found, samples.data(), n, width, height, 200);
      Mat4 ctp, fitted;
      double move[3];
      float here[2];
      if (c.found && TurnAndMove(c.cur, c.prev, samples.data(), n, width, height, ctp, move)) {
        turn_share.push_back(Agreement(samples.data(), n, ctp, width, height));
        at_centre(ctp, here);
        if (next && have_turn) turn_shake.push_back(std::hypot((here[0] - last_turn[0]) * width * 0.5f, (here[1] - last_turn[1]) * height * 0.5f));
        last_turn[0] = here[0];
        last_turn[1] = here[1];
        have_turn = true;
      } else {
        have_turn = false;
      }
      float share = 0;
      if (FitClipToPrev(samples.data(), n, width, height, have_start && next ? &start : nullptr, fitted, share)) {
        start = fitted;
        have_start = true;
        fit_share_all.push_back(share);
        at_centre(fitted, here);
        if (next && have_fit_last) fit_shake.push_back(std::hypot((here[0] - last_centre[0]) * width * 0.5f, (here[1] - last_centre[1]) * height * 0.5f));
        last_centre[0] = here[0];
        last_centre[1] = here[1];
        have_fit_last = true;
      } else {
        have_start = have_fit_last = false;
      }
    }
    std::printf("turn and lens from the matrices, move from the flow: explains %.0f%% on the mean, median %.0f%%; its motion at the centre changes %.1f px from frame to frame (median), 90%% under %.1f\n", Mean(turn_share) * 100.f, Quantile(turn_share, 0.5f) * 100.f, Quantile(turn_shake, 0.5f), Quantile(turn_shake, 0.9f));
    std::printf("a whole camera from the flow:                     explains %.0f%% on the mean, median %.0f%%; its motion at the centre changes %.1f px from frame to frame (median), 90%% under %.1f\n", Mean(fit_share_all) * 100.f, Quantile(fit_share_all, 0.5f) * 100.f, Quantile(fit_shake, 0.5f), Quantile(fit_shake, 0.9f));
  }

  // The first matrix under other rules for when it is used.
  if (!order.empty()) {
    const int64_t key = order[0].first;
    struct Rule {
      const char* name;
      float off, on;
      int off_frames, on_frames;
      bool use = false;
      int low = 0, high = 0;
      uint32_t used = 0, changes = 0;
    } rules[] = {{"as now: off under 40% at once, on after 60 frames over 40%", 0.4f, 0.4f, 1, 60}, {"off after 10 frames under 40%, on after 10 over 40%", 0.4f, 0.4f, 10, 10}, {"off after 30 frames under 25%, on after 10 over 40%", 0.25f, 0.4f, 30, 10}};
    uint32_t frames = 0;
    for (const FlowRecord& r : flow) {
      const auto cur_at = by_frame.find(r.head.camera_frame), prev_at = by_frame.find(r.head.camera_frame - 1);
      if (cur_at == by_frame.end() || prev_at == by_frame.end() || r.head.reset != 0) continue;
      const Candidate* const c = Find(matrices[cur_at->second].found, key);
      const Candidate* const p = Find(matrices[prev_at->second].found, key);
      Mat4 ctp;
      if (c == nullptr || p == nullptr || !PairMatrix(*c, *p, ctp)) continue;
      const uint32_t n = MakeSamples(r.grid.data(), r.head.scale, samples.data());
      if (n < 64) continue;
      const float share = Agreement(samples.data(), n, ctp, width, height);
      ++frames;
      for (Rule& rule : rules) {
        rule.low = share < rule.off ? rule.low + 1 : 0;
        rule.high = share >= rule.on ? rule.high + 1 : 0;
        const bool was = rule.use;
        if (rule.use && rule.low >= rule.off_frames) rule.use = false;
        if (!rule.use && rule.high >= rule.on_frames) rule.use = true;
        if (rule.use) ++rule.used;
        if (rule.use != was) ++rule.changes;
      }
    }
    for (const Rule& rule : rules) std::printf("rule, %s: in use on %.0f%% of %u frames, %u changes\n", rule.name, frames != 0 ? 100.0 * rule.used / frames : 0.0, frames, rule.changes);
  }

  // The view and the projection in separate places: try each view found with each projection found, and with the lens of the matrices found, either way around.
  {
    struct Lens {
      int64_t key;
      Mat4 cur, prev;
    };
    std::map<int64_t, Per> combos;
    std::vector<float> best_each;
    double views_seen = 0, projections_seen = 0, frames = 0;
    std::map<int64_t, std::pair<float, float>> lens_shape;
    for (size_t f = 0; f < flow.size(); f += 3) {
      const FlowRecord& r = flow[f];
      const auto cur_at = by_frame.find(r.head.camera_frame), prev_at = by_frame.find(r.head.camera_frame - 1);
      if (cur_at == by_frame.end() || prev_at == by_frame.end() || r.head.reset != 0) continue;
      const uint32_t n_all = MakeSamples(r.grid.data(), r.head.scale, samples.data());
      if (n_all < 64 || MedianSpeed(samples.data(), n_all, width, height) < MOVING) continue;
      std::vector<Sample> some;
      for (uint32_t i = 0; i < n_all; i += 4) some.push_back(samples[i]);
      const MatricesRecord& cur = matrices[cur_at->second];
      const MatricesRecord& prev = matrices[prev_at->second];
      std::vector<Lens> lenses;
      int views = 0, projections = 0;
      for (const Candidate& c : cur.parts) {
        const int kind = static_cast<int>(c.key >> PART_KIND_SHIFT);
        if (kind == static_cast<int>(PartKind::view)) ++views;
        if (kind != static_cast<int>(PartKind::projection)) continue;
        ++projections;
        const Candidate* const p = Find(prev.parts, c.key);
        if (p == nullptr || lenses.size() >= 6) continue;
        Lens l;
        l.key = c.key;
        std::memcpy(l.cur.m, c.m, sizeof(c.m));
        std::memcpy(l.prev.m, p->m, sizeof(p->m));
        lens_shape[c.key] = {c.m[0], c.m[5]};
        lenses.push_back(l);
      }
      // No projection anywhere: the lens from a matrix found, for a view space of right, up, forward, and the same looking down -z.
      if (!cur.found.empty()) {
        Mat4 m;
        std::memcpy(m.m, cur.found[0].m, sizeof(m.m));
        Parts parts;
        if (Decompose(m, parts)) {
          Lens l;
          l.key = 1;
          l.cur = l.prev = parts.view_to_clip;
          lenses.push_back(l);
          l.key = 2;
          for (int i = 0; i < 4; ++i) {
            l.cur.m[4 * i + 2] = -l.cur.m[4 * i + 2];
            l.prev.m[4 * i + 2] = -l.prev.m[4 * i + 2];
          }
          lenses.push_back(l);
        }
      }
      views_seen += views;
      projections_seen += projections;
      ++frames;
      float best = 0;
      for (const Candidate& c : cur.parts) {
        if (static_cast<int>(c.key >> PART_KIND_SHIFT) != static_cast<int>(PartKind::view)) continue;
        const Candidate* const p = Find(prev.parts, c.key);
        if (p == nullptr) continue;
        Mat4 vc, vp, vc_inverse, vp_inverse;
        std::memcpy(vc.m, c.m, sizeof(vc.m));
        std::memcpy(vp.m, p->m, sizeof(vp.m));
        if (!Inverse(vc, vc_inverse) || !Inverse(vp, vp_inverse)) continue;
        for (const Lens& l : lenses) {
          Mat4 lens_inverse;
          if (!Inverse(l.cur, lens_inverse)) continue;
          for (int way = 0; way < 2; ++way) {
            // World to view, or view to world.
            const Mat4 ctp = way == 0 ? Mul(Mul(Mul(l.prev, vp), vc_inverse), lens_inverse) : Mul(Mul(Mul(l.prev, vp_inverse), vc), lens_inverse);
            const float share = Agreement(some.data(), static_cast<uint32_t>(some.size()), ctp, width, height);
            best = std::max(best, share);
            const int64_t combo = (c.key & ((static_cast<int64_t>(1) << PART_KIND_SHIFT) - 1)) ^ (static_cast<int64_t>(way) << 62) ^ (static_cast<int64_t>(&l - lenses.data()) << 58);
            combos[combo].shares.push_back(share);
          }
        }
      }
      best_each.push_back(best);
    }
    std::printf("views and projections apart: %.1f views and %.1f projections a frame over %.0f frames; best combination each frame explains %.0f%% on the mean, median %.0f%%\n", frames != 0 ? views_seen / frames : 0.0, frames != 0 ? projections_seen / frames : 0.0, frames, Mean(best_each) * 100.f, Quantile(best_each, 0.5f) * 100.f);
    for (const auto& s : lens_shape) {
      KeyText(s.first & ((static_cast<int64_t>(1) << PART_KIND_SHIFT) - 1), text, sizeof(text));
      std::printf("  a projection: %s, %.3f and %.3f on its diagonal, aspect %.3f\n", text, s.second.first, s.second.second, s.second.first != 0.f ? std::fabs(s.second.second / s.second.first) : 0.f);
    }
    std::vector<std::pair<int64_t, float>> combo_order;
    for (const auto& k : combos)
      if (k.second.shares.size() * 20 >= static_cast<size_t>(frames)) combo_order.push_back({k.first, Mean(k.second.shares)});
    std::sort(combo_order.begin(), combo_order.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < combo_order.size() && i < 10; ++i) {
      const int64_t combo = combo_order[i].first;
      const int way = static_cast<int>((combo >> 62) & 1), lens = static_cast<int>((combo >> 58) & 15);
      KeyText(combo & ((static_cast<int64_t>(1) << 58) - 1), text, sizeof(text));
      std::printf("  view %s, %s, lens %d: on %zu frames, explains mean %3.0f%%, median %3.0f%%, top tenth %3.0f%%\n", text, way == 0 ? "world to view" : "view to world", lens, combos[combo].shares.size(), combo_order[i].second * 100.f, Quantile(combos[combo].shares, 0.5f) * 100.f, Quantile(combos[combo].shares, 0.9f) * 100.f);
    }
  }

  std::printf("timeline, about one line a second of a 50 fps game (moving frames only): speed, any one camera, best matrix, frames with a good matrix\n");
  for (size_t s = 0; s < timeline.size(); ++s)
    if (timeline[s].frames != 0)
      std::printf("  %3zu: %6.1f px %4.0f%% %4.0f%% %3u of %u\n", s, timeline[s].speed / timeline[s].frames, 100.0 * timeline[s].fit / timeline[s].frames, 100.0 * timeline[s].best / timeline[s].frames, timeline[s].good, timeline[s].frames);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc < 3 || argc % 2 == 0) {
    std::printf("camera_replay.exe <time>-matrices.bin <time>-flow.bin [more pairs]\n");
    return 2;
  }
  for (int i = 1; i + 1 < argc; i += 2) Study(argv[i], argv[i + 1]);
  return 0;
}
