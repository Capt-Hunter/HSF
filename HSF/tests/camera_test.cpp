// Offline test of the camera work (src/camera.h), no game and no GPU: made-up cameras over a made-up scene. Checks that the scanner finds a camera among other data in both
// memory layouts, that jitter comes out, that the chooser locks the right matrix and lets go of it, that the fit finds the still world's motion from flow alone with a moving
// thing and a HUD in the picture, and what that costs. camera_test.exe; exit code 0 = passed.
#include "../src/camera_record.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <vector>

using namespace hsf::camera;

namespace {

int g_failed = 0;

void Check(bool ok, const char* what) {
  std::printf("  %s: %s\n", ok ? "ok" : "FAILED", what);
  if (!ok) ++g_failed;
}

constexpr float W = 1920.f, H = 1080.f;

struct Camera {
  float yaw = 0, pitch = 0, x = 0, y = 2, z = 0;
  float jitter_x = 0, jitter_y = 0;
};

// World to clip, column vectors, left-handed, optionally reversed depth (far = 0).
Mat4 ViewProjection(const Camera& c, bool reversed, float aspect = W / H) {
  const float cy = std::cos(c.yaw), sy = std::sin(c.yaw), cp = std::cos(c.pitch), sp = std::sin(c.pitch);
  const float right[3] = {cy, 0, -sy};
  const float up[3] = {sy * sp, cp, cy * sp};
  const float fwd[3] = {sy * cp, -sp, cy * cp};
  const float pos[3] = {c.x, c.y, c.z};
  const auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
  const float b = 1.5f, a = b / aspect, n = 0.5f, f = 1000.f;
  const float A = reversed ? n / (n - f) : f / (f - n);
  const float B = reversed ? -f * n / (n - f) : -f * n / (f - n);
  Mat4 r{};
  for (int i = 0; i < 3; ++i) {
    r.m[i] = a * right[i] + c.jitter_x * fwd[i];
    r.m[4 + i] = b * up[i] + c.jitter_y * fwd[i];
    r.m[8 + i] = A * fwd[i];
    r.m[12 + i] = fwd[i];
  }
  const float d = -dot(fwd, pos);
  r.m[3] = -a * dot(right, pos) + c.jitter_x * d;
  r.m[7] = -b * dot(up, pos) + c.jitter_y * d;
  r.m[11] = A * d + B;
  r.m[15] = d;
  return r;
}

// A little world of varied depth in front of the camera, at each sample's place: distance from the sample's position, so it is the same world whichever way the camera looks only
// for the frame it is made for. The flow is what the true matrix says, so what is under test is whether it can be found again.
struct Scene {
  std::vector<float> grid;  // motion x, y, depth, unused, per sample
  float scale[2] = {1.f / W, 1.f / H};
};

float Noise(uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return static_cast<float>(state % 2000) / 1000.f - 1.f;
}

// `moving`: a block of the picture with its own motion. `hud`: a strip that does not move. `noise`: the flow's shake, in pixels.
Scene Make(const Mat4& cur, const Mat4& prev, bool reversed, bool moving, bool hud, float noise, uint32_t seed) {
  Scene s;
  s.grid.resize(4ull * SAMPLES);
  Mat4 inv, ctp;
  Inverse(cur, inv);
  ctp = Mul(prev, inv);
  uint32_t state = seed * 2654435761u + 1u;
  for (uint32_t y = 0; y < SAMPLE_HEIGHT; ++y)
    for (uint32_t x = 0; x < SAMPLE_WIDTH; ++x) {
      float* const g = &s.grid[4 * (y * SAMPLE_WIDTH + x)];
      const float u = (x + 0.5f) / SAMPLE_WIDTH, v = (y + 0.5f) / SAMPLE_HEIGHT;
      // Distance varies across the picture, so depth carries real parallax.
      const float distance = 3.f + 40.f * (0.5f + 0.5f * std::sin(u * 17.f) * std::cos(v * 11.f));
      const float n = 0.5f, f = 1000.f;
      const float A = reversed ? n / (n - f) : f / (f - n), B = reversed ? -f * n / (n - f) : -f * n / (f - n);
      const float depth = A + B / distance;
      const float cx = u * 2 - 1, cy = 1 - v * 2;
      const float* const m = ctp.m;
      const float w = m[12] * cx + m[13] * cy + m[14] * depth + m[15];
      const float px = (m[0] * cx + m[1] * cy + m[2] * depth + m[3]) / w, py = (m[4] * cx + m[5] * cy + m[6] * depth + m[7]) / w;
      float mx = (px - cx) * 0.5f * W, my = (cy - py) * 0.5f * H;
      if (moving && u > 0.3f && u < 0.6f && v > 0.3f && v < 0.7f) {
        mx = 40.f;
        my = -12.f;
      }
      if (hud && v > 0.9f) mx = my = 0.f;
      g[0] = mx + noise * Noise(state);
      g[1] = my + noise * Noise(state);
      g[2] = depth;
    }
  return s;
}

// How far off a matrix is from the true one over the picture, in pixels: the worst point.
float WorstPixels(const Mat4& a, const Mat4& b, bool reversed) {
  float worst = 0;
  for (int y = 0; y < 18; ++y)
    for (int x = 0; x < 32; ++x)
      for (int k = 0; k < 4; ++k) {
        const float cx = (x + 0.5f) / 16.f - 1.f, cy = 1.f - (y + 0.5f) / 9.f;
        const float distance = 3.f + 13.f * k;
        const float n = 0.5f, f = 1000.f;
        const float A = reversed ? n / (n - f) : f / (f - n), B = reversed ? -f * n / (n - f) : -f * n / (f - n);
        const float depth = A + B / distance;
        const auto at = [&](const Mat4& t, float& ox, float& oy) {
          const float* const m = t.m;
          const float w = m[12] * cx + m[13] * cy + m[14] * depth + m[15];
          ox = (m[0] * cx + m[1] * cy + m[2] * depth + m[3]) / w;
          oy = (m[4] * cx + m[5] * cy + m[6] * depth + m[7]) / w;
        };
        float ax, ay, bx, by;
        at(a, ax, ay);
        at(b, bx, by);
        worst = std::max(worst, std::hypot((ax - bx) * 0.5f * W, (ay - by) * 0.5f * H));
      }
  return worst;
}

void TestRecognise() {
  std::printf("recognising a camera\n");
  Camera c;
  c.yaw = 0.7f;
  c.pitch = -0.2f;
  c.x = 12;
  c.z = -40;
  Mat4 out;
  const Mat4 vp = ViewProjection(c, false);
  Check(Recognise(vp.m, false, W / H, out) == Shape::camera, "view x projection, as stored");
  const Mat4 flipped = Transpose(vp);
  Check(Recognise(flipped.m, true, W / H, out) == Shape::camera, "the other memory layout");
  Check(Recognise(vp.m, false, 1.f, out) == Shape::wrong_aspect, "a very different aspect is told apart");
  const Mat4 tomb_raider = ViewProjection(c, false, 2.806f);
  Check(Recognise(tomb_raider.m, false, 2.513f, out) == Shape::camera, "a camera of 2.806 in a window of 2.513, as Tomb Raider has");
  const Mat4 shadow_map = ViewProjection(c, false, 1.f);
  Check(Recognise(shadow_map.m, false, 2.513f, out) == Shape::wrong_aspect, "a square shadow map in that window is left out");
  const Mat4 reversed = ViewProjection(c, true);
  Check(Recognise(reversed.m, false, W / H, out) == Shape::camera, "reversed depth");
  Check(Recognise(Identity().m, false, W / H, out) != Shape::camera, "not the identity");
  Mat4 ortho = Identity();
  ortho.m[0] = 0.001f;
  ortho.m[5] = 0.002f;
  ortho.m[3] = -1.f;
  Check(Recognise(ortho.m, false, W / H, out) != Shape::camera, "not an orthographic matrix");
  float junk[16];
  for (int i = 0; i < 16; ++i) junk[i] = std::sin(i * 3.3f) * 40.f;
  Check(Recognise(junk, false, W / H, out) != Shape::camera, "not noise");

  // Jitter: a matrix with jitter comes out equal to the same one without it.
  Camera j = c;
  j.jitter_x = 0.0007f;
  j.jitter_y = -0.0011f;
  Mat4 with, without;
  const Mat4 jittered = ViewProjection(j, false);
  Recognise(jittered.m, false, W / H, with);
  Recognise(vp.m, false, W / H, without);
  float apart = 0;
  for (int i = 0; i < 16; ++i) apart = std::max(apart, std::fabs(with.m[i] - without.m[i]));
  Check(apart < 1e-3f, "jitter comes out");

  Parts p;
  Check(Decompose(without, p), "taken apart");
  Check(std::fabs(p.pos[0] - 12) < 0.01f && std::fabs(p.pos[1] - 2) < 0.01f && std::fabs(p.pos[2] + 40) < 0.01f, "camera position");
  Check(std::fabs(p.near_plane - 0.5f) < 0.01f && std::fabs(p.far_plane - 1000.f) < 5.f, "near and far");
  Check(std::fabs(p.aspect - W / H) < 0.01f && std::fabs(p.fov - 2.f * std::atan(1.f / 1.5f)) < 0.001f, "field of view and aspect");
  Parts q;
  Mat4 rev;
  Recognise(reversed.m, false, W / H, rev);
  Decompose(rev, q);
  Check(std::fabs(q.near_plane - 0.5f) < 0.01f && std::fabs(q.far_plane - 1000.f) < 5.f, "near and far, reversed");
}

void TestScanner() {
  std::printf("scanner\n");
  Scanner scan;
  scan.SetAspect(W / H);
  Camera c;
  c.yaw = 0.4f;
  const Mat4 vp = ViewProjection(c, false);
  // A constant buffer with other data around the matrix, at offset 96.
  std::vector<float> buffer(64);
  for (size_t i = 0; i < buffer.size(); ++i) buffer[i] = std::cos(static_cast<float>(i)) * 3.f;
  std::memcpy(&buffer[24], vp.m, sizeof(vp.m));
  Check(scan.Wants(256), "wants a constant buffer");
  Check(!scan.Wants(1024 * 1024), "not a huge one");
  scan.Write(256, buffer.data(), 0, 256);
  Candidate out[CANDIDATES];
  int64_t frame = 0;
  int count = scan.EndFrame(out, frame);
  Check(count == 1, "finds the one matrix");
  Check(count >= 1 && out[0].key == MakeKey(256, 96, false, 0), "its place");
  // The same buffer written for many draws in a frame only counts a few times.
  for (int i = 0; i < 100; ++i) scan.Write(256, buffer.data(), 0, 256);
  count = scan.EndFrame(out, frame);
  Check(count == 4, "a buffer written for every draw counts four times at most");
  // Locked: only that place is read, and other buffers are not wanted.
  scan.Lock(MakeKey(256, 96, false, 0));
  Check(!scan.Wants(512), "locked: other sizes are left alone");
  scan.Write(256, buffer.data(), 0, 256);
  count = scan.EndFrame(out, frame);
  Check(count == 1, "locked: still reads its own");
  // A flood of one size earlier in the frame must not use up what a camera written later needs.
  scan.Lock(0);
  scan.TakeStats();
  std::vector<char> object(4096);
  for (size_t i = 0; i < object.size(); ++i) object[i] = static_cast<char>(i * 7);
  for (int i = 0; i < 2000; ++i) scan.Write(4096, object.data(), 0, 4096);
  std::vector<float> small(20);
  std::memcpy(small.data(), vp.m, sizeof(vp.m));
  scan.Write(80, small.data(), 0, 80);
  count = scan.EndFrame(out, frame);
  Check(count == 1 && out[0].key == MakeKey(80, 0, false, 0), "a camera written after 2000 writes of another size is still read");
  ScanStats stats = scan.TakeStats();
  Check(stats.writes == 2001 && stats.unread_for_size == 1996 && stats.unread_for_total == 0 && stats.frames == 1, "the log's counts of what went unread");
  Check(stats.widths == 2 && stats.all[0].width == 80 && stats.all[1].width == 4096 && stats.all[1].writes == 2000 && stats.all[1].bytes == 16384, "the log's sizes, least written first");
  // Many sizes: the frame's total still holds.
  for (uint32_t i = 0; i < 40; ++i) {
    std::vector<char> big(65536 - i * 16);
    for (size_t j = 0; j < big.size(); ++j) big[j] = static_cast<char>(j * 13);
    scan.Write(static_cast<uint32_t>(big.size()), big.data(), 0, static_cast<uint32_t>(big.size()));
  }
  scan.EndFrame(out, frame);
  stats = scan.TakeStats();
  Check(stats.bytes == Scanner::BYTES_PER_FRAME && stats.unread_for_total > 0, "no more than the frame's total is read");
  // More matrices than fit in a frame: each one must still come round on frames in a row, the only way it can be scored.
  {
    constexpr int SIZES = 5, MANY = SIZES * Scanner::MATRICES_PER_WIDTH;
    std::vector<char> many(Scanner::MATRICES_PER_WIDTH * 64);
    for (uint32_t i = 0; i < Scanner::MATRICES_PER_WIDTH; ++i) std::memcpy(&many[static_cast<size_t>(i) * 64], vp.m, sizeof(vp.m));
    int pairs[MANY] = {};
    Candidate before[CANDIDATES];
    int before_count = 0;
    for (int f = 0; f < 128; ++f) {
      for (uint32_t w = 0; w < SIZES; ++w) scan.Write(512 + w * 16, many.data(), 0, static_cast<uint32_t>(many.size()));
      const int now = scan.EndFrame(out, frame);
      for (int i = 0; i < now; ++i)
        for (int j = 0; j < before_count; ++j)
          if (out[i].key == before[j].key) ++pairs[(KeyWidth(out[i].key) - 512) / 16 * Scanner::MATRICES_PER_WIDTH + KeyOffset(out[i].key) / 64];
      std::memcpy(before, out, static_cast<size_t>(now) * sizeof(Candidate));
      before_count = now;
    }
    int fewest = 1000000;
    for (const int p : pairs) fewest = std::min(fewest, p);
    std::printf("  40 matrices, 16 handed over a frame: the least seen one was scored on %d of 127 frame pairs\n", fewest);
    // 32 not among the quietest, 8 at a time for 8 frames, so 7 pairs each 32 frames: 28 pairs in 128 frames, less a few where the run starts.
    Check(fewest >= 24, "every matrix gets scored when there are more than fit");
    scan.TakeStats();
    // Sizes written for every object must not push out one written once, however many there are, and must not fill the frame's matrices.
    std::vector<float> lone(20);
    std::memcpy(lone.data(), vp.m, sizeof(vp.m));
    int always = 0;
    for (int f = 0; f < 16; ++f) {
      for (uint32_t w = 0; w < 20; ++w)
        for (int repeat = 0; repeat < 50; ++repeat) scan.Write(1024 + w * 16, many.data(), 0, static_cast<uint32_t>(many.size()));
      scan.Write(80, lone.data(), 0, 80);
      const int now = scan.EndFrame(out, frame);
      for (int i = 0; i < now; ++i)
        if (out[i].key == MakeKey(80, 0, false, 0)) ++always;
    }
    Check(always == 16, "a matrix from a size written once a frame is handed over every frame");
    stats = scan.TakeStats();
    std::printf("  flood of 20 sizes: %u left out for their size's limit, %u for the frame's, up to %d matrices a frame\n", stats.left_for_size, stats.left_for_total, stats.most);
    Check(stats.left_for_size > 0 && stats.left_for_total == 0 && stats.most == 161, "each flooding size stops at its limit, leaving room");
  }
  // Time: a frame's worth of ordinary constant buffers.
  LARGE_INTEGER f, a, b;
  QueryPerformanceFrequency(&f);
  std::vector<float> plain(64);
  for (size_t i = 0; i < plain.size(); ++i) plain[i] = std::sin(static_cast<float>(i) * 0.7f);
  QueryPerformanceCounter(&a);
  for (int i = 0; i < 1000; ++i)
    if (scan.Wants(256)) scan.Write(256, plain.data(), 0, 256);
  QueryPerformanceCounter(&b);
  scan.EndFrame(out, frame);
  std::printf("  1000 buffer writes of 256 bytes (normal memory): %.3f ms\n", 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart));
}

void TestFit() {
  std::printf("fit from the flow\n");
  LARGE_INTEGER f, a, b;
  QueryPerformanceFrequency(&f);
  for (const bool reversed : {false, true}) {
    Camera c0;
    c0.yaw = 0.3f;
    c0.x = 5;
    Camera c1 = c0;
    c1.yaw += 0.012f;  // about 15 pixels' turn
    c1.x += 0.05f;
    c1.z += 0.15f;
    const Mat4 cur = ViewProjection(c1, reversed), prev = ViewProjection(c0, reversed);
    Mat4 inv;
    Inverse(cur, inv);
    const Mat4 truth = Mul(prev, inv);
    std::vector<Sample> samples(SAMPLES);
    Mat4 fitted;
    float share = 0;

    Scene clean = Make(cur, prev, reversed, false, false, 0.f, 1);
    uint32_t n = MakeSamples(clean.grid.data(), clean.scale, samples.data());
    Check(FitClipToPrev(samples.data(), n, W, H, nullptr, fitted, share), "solved");
    std::printf("  %s, clean flow: explains %.0f%%, worst %.2f pixels off\n", reversed ? "reversed" : "normal", share * 100.f, WorstPixels(fitted, truth, reversed));
    Check(share > 0.98f && WorstPixels(fitted, truth, reversed) < 0.5f, "clean flow gives the true motion");

    Scene noisy = Make(cur, prev, reversed, true, true, 1.5f, 2);
    n = MakeSamples(noisy.grid.data(), noisy.scale, samples.data());
    QueryPerformanceCounter(&a);
    FitClipToPrev(samples.data(), n, W, H, nullptr, fitted, share);
    QueryPerformanceCounter(&b);
    const float worst = WorstPixels(fitted, truth, reversed);
    std::printf("  %s, shaking flow, a moving block and a HUD: explains %.0f%%, worst %.2f pixels off, %.2f ms\n", reversed ? "reversed" : "normal", share * 100.f, worst,
                1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart));
    Check(worst < 1.5f, "a moving block, a HUD and shaking flow do not pull it off");
    Check(share < 0.95f, "and what it leaves out shows");
  }
  // Depth with no information (one value): still a usable answer, not a failure.
  Camera c0, c1;
  c1.yaw = 0.01f;
  const Mat4 cur = ViewProjection(c1, false), prev = ViewProjection(c0, false);
  Scene s = Make(cur, prev, false, false, false, 0.f, 3);
  for (uint32_t i = 0; i < SAMPLES; ++i) s.grid[4 * i + 2] = 0.99f;
  std::vector<Sample> samples(SAMPLES);
  const uint32_t n = MakeSamples(s.grid.data(), s.scale, samples.data());
  Mat4 fitted;
  float share = 0;
  Check(FitClipToPrev(samples.data(), n, W, H, nullptr, fitted, share), "one depth value everywhere is solved");
  // Pure noise: says it explains little, so nothing uses it.
  uint32_t state = 77;
  for (uint32_t i = 0; i < SAMPLES; ++i) {
    s.grid[4 * i] = 60.f * Noise(state);
    s.grid[4 * i + 1] = 60.f * Noise(state);
    s.grid[4 * i + 2] = 0.5f + 0.4f * Noise(state);
  }
  const uint32_t m = MakeSamples(s.grid.data(), s.scale, samples.data());
  FitClipToPrev(samples.data(), m, W, H, nullptr, fitted, share);
  std::printf("  random flow: explains %.0f%%\n", share * 100.f);
  Check(share < Chooser::FIT_SHARE, "random flow is not believed");
}

Candidate MakeCandidate(int64_t key, const Mat4& m) {
  Candidate c;
  c.key = key;
  Mat4 out;
  Recognise(m.m, false, W / H, out);
  std::memcpy(c.m, out.m, sizeof(c.m));
  return c;
}

void TestChooser() {
  std::printf("chooser\n");
  Chooser chooser;
  const int64_t right = MakeKey(256, 96, false, 0), wrong = MakeKey(256, 160, false, 0), fixed = MakeKey(128, 0, false, 0);
  std::vector<Sample> samples(SAMPLES);
  Camera c;
  c.x = 3;
  Mat4 prev = ViewProjection(c, false);
  int64_t frame = 0;
  int locked_at = -1;
  Decision d;
  Candidate list[3];
  uint32_t n = 0;
  // A camera turning, a shadow-map-like camera that moves its own way, and a projection alone that never moves.
  const auto step = [&](float turn, bool flow_true) {
    Camera next = c;
    next.yaw += turn;
    next.x += 0.03f;
    const Mat4 cur = ViewProjection(next, false);
    Scene s = Make(cur, flow_true ? prev : ViewProjection(next, false), false, true, true, 1.f, static_cast<uint32_t>(frame) + 10);
    if (!flow_true) {
      uint32_t state = static_cast<uint32_t>(frame) * 31 + 5;
      for (uint32_t i = 0; i < SAMPLES; ++i) {
        s.grid[4 * i] = 50.f * Noise(state);
        s.grid[4 * i + 1] = 50.f * Noise(state);
      }
    }
    n = MakeSamples(s.grid.data(), s.scale, samples.data());
    Camera other;
    other.x = static_cast<float>(frame) * 0.7f;
    other.yaw = static_cast<float>(frame) * 0.1f;
    Camera still;
    list[0] = MakeCandidate(wrong, ViewProjection(other, false));
    list[1] = MakeCandidate(fixed, ViewProjection(still, false));
    list[2] = MakeCandidate(right, cur);
    ++frame;
    d = chooser.Frame(samples.data(), n, list, 3, frame, false, W, H);
    c = next;
    prev = cur;
  };
  for (int i = 0; i < 60 && locked_at < 0; ++i) {
    step(0.01f, true);
    if (chooser.LockedKey() != 0) locked_at = i;
  }
  std::printf("  locked after %d frames\n", locked_at);
  Check(chooser.LockedKey() == right, "the matrix that explains the flow wins");
  step(0.01f, true);
  Check(d.source == Source::matrix && d.share > 0.7f, "and is used");
  // The flow stops agreeing: kept for a few frames, as a fast pan does this to a right matrix, then dropped.
  for (int i = 0; i < Chooser::MATRIX_OFF_FRAMES - 1; ++i) step(0.01f, false);
  Check(d.source == Source::matrix, "kept through a few frames of not agreeing");
  step(0.01f, false);
  Check(d.source != Source::matrix, "dropped when it goes on");
  int back = -1;
  for (int i = 0; i < 100 && back < 0; ++i) {
    step(0.01f, true);
    if (d.source == Source::matrix) back = i + 1;
  }
  std::printf("  back after %d good frames\n", back);
  Check(back >= Chooser::MATRIX_ON_FRAMES - 1 && back <= Chooser::MATRIX_ON_FRAMES + 1, "taken back after a few good frames");
  // Long wrong: let go, and looks again.
  for (int i = 0; i < Chooser::BAD_FRAMES + 2; ++i) step(0.01f, false);
  Check(chooser.LockedKey() == 0, "let go after being wrong for a long time");
  // A scene cut: no motion from the matrices across it.
  for (int i = 0; i < 40; ++i) step(0.01f, true);
  Check(chooser.LockedKey() == right, "locks again");
  Camera jump = c;
  jump.yaw += 2.f;
  ++frame;
  list[2] = MakeCandidate(right, ViewProjection(jump, false));
  d = chooser.Frame(samples.data(), n, list, 3, frame, true, W, H);
  Check(d.source == Source::none, "nothing across a reset");

  // No candidates at all: the fit takes over after a second of agreeing, and works without any matrix.
  Chooser only_fit;
  Camera f;
  Mat4 last = ViewProjection(f, false);
  int fit_at = -1;
  for (int i = 0; i < 100 && fit_at < 0; ++i) {
    Camera next = f;
    next.yaw += 0.008f;
    next.z += 0.1f;
    const Mat4 cur = ViewProjection(next, false);
    Scene s = Make(cur, last, false, true, true, 1.f, static_cast<uint32_t>(i) + 100);
    const uint32_t count = MakeSamples(s.grid.data(), s.scale, samples.data());
    const Decision r = only_fit.Frame(samples.data(), count, nullptr, 0, i + 1, false, W, H);
    if (r.source == Source::fit) fit_at = i;
    f = next;
    last = cur;
  }
  std::printf("  fit in use after %d frames\n", fit_at);
  Check(fit_at >= 0 && fit_at <= Chooser::GOOD_RUN + 2, "no matrix in the game: the fit is used");

  // The fit switched off: no matrix means flow only.
  {
    Chooser no_fit;
    no_fit.SetFitAllowed(false);
    Camera n0;
    Mat4 n_last = ViewProjection(n0, false);
    Source seen = Source::none;
    for (int i = 0; i < 100; ++i) {
      Camera next = n0;
      next.yaw += 0.008f;
      const Mat4 cur = ViewProjection(next, false);
      Scene s = Make(cur, n_last, false, false, false, 0.5f, static_cast<uint32_t>(i) + 300);
      const uint32_t count = MakeSamples(s.grid.data(), s.scale, samples.data());
      const Decision r = no_fit.Frame(samples.data(), count, nullptr, 0, i + 1, false, W, H);
      if (r.source != Source::none) seen = r.source;
      n0 = next;
      n_last = cur;
    }
    Check(seen == Source::none, "fit switched off: flow only");
  }

  // The report for the log: what could be judged and how the candidates score.
  {
    Chooser reporting;
    reporting.SetFitAllowed(false);
    Camera r0;
    Mat4 r_last = ViewProjection(r0, false);
    Candidate seen_last{};
    for (int i = 0; i < 10; ++i) {
      Camera next = r0;
      next.yaw += 0.008f;
      const Mat4 cur = ViewProjection(next, false);
      Scene s = Make(cur, r_last, false, false, false, 0.5f, static_cast<uint32_t>(i) + 400);
      const uint32_t count = MakeSamples(s.grid.data(), s.scale, samples.data());
      seen_last.key = MakeKey(80, 0, false, 0);
      std::memcpy(seen_last.m, cur.m, sizeof(cur.m));
      reporting.Frame(samples.data(), count, &seen_last, 1, i + 1, false, W, H);
      r0 = next;
      r_last = cur;
    }
    const Chooser::Report report = reporting.TakeReport();
    std::printf("  report: %u frames, %u judged, %u moving, %u with candidates; best %.0f%% over %d frames, aspect %.3f\n", report.frames, report.usable, report.moving, report.with_candidates,
                report.top[0].mean * 100.f, report.top[0].frames, report.top[0].aspect);
    Check(report.frames == 10 && report.usable == 9 && report.moving == 9 && report.with_candidates == 9, "report: frames counted");
    Check(report.top_count == 1 && report.top[0].key == MakeKey(80, 0, false, 0) && report.top[0].frames == 9 && report.top[0].mean > 0.9f, "report: the candidate and its score");
    Check(std::fabs(report.top[0].aspect - W / H) < 0.02f, "report: the candidate's aspect");
    Check(report.most_scored[0].key == MakeKey(80, 0, false, 0) && report.most_scored[0].frames == 9, "report: the most scored");
    Check(reporting.TakeReport().frames == 0, "report: counts start again");
  }

  // More wrong matrices than the tally holds: the right one, coming late, must still get scored and lock.
  {
    Chooser full;
    full.SetFitAllowed(false);
    Camera f0;
    Mat4 f_last = ViewProjection(f0, false);
    const Mat4 still = ViewProjection(Camera{}, false);
    int64_t frame_number = 0;
    const auto run = [&](const auto& fill, int frames) {
      for (int i = 0; i < frames; ++i) {
        Camera next = f0;
        next.yaw += 0.008f;
        const Mat4 cur = ViewProjection(next, false);
        Scene s = Make(cur, f_last, false, false, false, 0.5f, static_cast<uint32_t>(frame_number) + 500);
        const uint32_t count = MakeSamples(s.grid.data(), s.scale, samples.data());
        Candidate list[CANDIDATES];
        const int listed = fill(list, cur);
        full.Frame(samples.data(), count, list, listed, ++frame_number, false, W, H);
        f0 = next;
        f_last = cur;
      }
    };
    // 10 sets of 16 keys, 160 in all, each scored past its chance.
    for (uint32_t set = 0; set < 10; ++set)
      run([&](Candidate* list, const Mat4&) {
        for (uint32_t k = 0; k < CANDIDATES; ++k) {
          list[k].key = MakeKey(256, (set * CANDIDATES + k) * 16, false, 0);
          std::memcpy(list[k].m, still.m, sizeof(still.m));
        }
        return CANDIDATES;
      }, 22);
    Check(full.LockedKey() == 0, "wrong matrices do not lock");
    Check(full.TakeReport().tallied == Chooser::TALLIES, "the tally is full");
    const int64_t right_key = MakeKey(80, 0, false, 0);
    run([&](Candidate* list, const Mat4& cur) {
      list[0].key = right_key;
      std::memcpy(list[0].m, cur.m, sizeof(cur.m));
      return 1;
    }, 25);
    Check(full.LockedKey() == right_key, "with the tally full, the right matrix still locks");
  }

  // Mostly sky: few samples left, on one thing. Not fitted, as its motion would go to the whole picture.
  {
    Chooser sky;
    std::vector<float> grid(4ull * SAMPLES);
    for (uint32_t i = 0; i < SAMPLES; ++i) {
      const bool thing = i % SAMPLE_WIDTH < 6;  // about 9% of the picture
      grid[4 * i] = thing ? 12.f : 0.f;
      grid[4 * i + 2] = thing ? 0.25f : 1.f;
    }
    const float scale[2] = {1.f / W, 1.f / H};
    const uint32_t few = MakeSamples(grid.data(), scale, samples.data());
    Source last_source = Source::none;
    for (int i = 0; i < 100; ++i) last_source = sky.Frame(samples.data(), few, nullptr, 0, i + 1, false, W, H).source;
    Check(few == SAMPLES * 6 / SAMPLE_WIDTH && last_source == Source::none, "mostly sky: not fitted to the one thing left");
  }

  // A hint: locks sooner on the same test.
  Chooser hinted;
  hinted.SetHint(right);
  chooser = Chooser();
  Camera h;
  Mat4 hp = ViewProjection(h, false);
  int hinted_at = -1;
  for (int i = 0; i < 40 && hinted_at < 0; ++i) {
    Camera next = h;
    next.yaw += 0.01f;
    const Mat4 cur = ViewProjection(next, false);
    Scene s = Make(cur, hp, false, false, false, 0.5f, static_cast<uint32_t>(i) + 200);
    const uint32_t count = MakeSamples(s.grid.data(), s.scale, samples.data());
    Candidate one = MakeCandidate(right, cur);
    hinted.Frame(samples.data(), count, &one, 1, i + 1, false, W, H);
    if (hinted.LockedKey() != 0) hinted_at = i;
    h = next;
    hp = cur;
  }
  std::printf("  with a hint, locked after %d frames\n", hinted_at);
  Check(hinted_at >= 0 && hinted_at < Chooser::LOCK_FRAMES, "a hint locks sooner");
}

// A recording can be written and read back, and recording changes nothing for the chooser.
void TestRecording() {
  std::printf("recording\n");
  Camera c;
  const Mat4 real = ViewProjection(c, false);
  c.yaw = 1.f;
  const Mat4 other = ViewProjection(c, false);
  Scanner scanner;
  scanner.SetAspect(W / H);
  float buffer[20] = {};
  const auto write = [&](const Mat4& m, uint32_t width) {
    std::memcpy(buffer, m.m, sizeof(m.m));
    scanner.Write(width, buffer, 0, 80);
  };
  const int64_t key = MakeKey(80, 0, false, 0);
  scanner.Lock(key);
  Check(!scanner.Wants(128), "locked, another size is not read");
  scanner.SetRecording(true);
  Check(scanner.Wants(128), "recording, every size is read");
  write(other, 128);
  write(real, 80);
  std::vector<Candidate> found(Scanner::SEEN);
  const int count = scanner.CopyFound(found.data());
  Check(count == 2 && found[0].key == MakeKey(128, 0, false, 0) && found[1].key == key, "recording, all of them are kept in the game's order");
  Candidate handed[CANDIDATES];
  int64_t frame = 0;
  const int given = scanner.EndFrame(handed, frame);
  Check(given == 1 && handed[0].key == key, "recording, the chooser still gets only the locked one");
  // A view and a projection in separate places, found only while recording.
  {
    Scanner parts;
    parts.SetAspect(W / H);
    parts.SetRecording(true);
    const float cy = std::cos(0.3f), sy = std::sin(0.3f);
    const float view[16] = {cy, 0, -sy, 1, 0, 1, 0, 2, sy, 0, cy, 3, 0, 0, 0, 1};
    const float projection[16] = {0.8f, 0, 0, 0, 0, 1.5f, 0, 0, 0, 0, 1.0005f, -0.5f, 0, 0, 1, 0};
    const float world[12] = {cy, 0, -sy, 7, 0, 1, 0, 8, sy, 0, cy, 9};
    float none[16] = {};
    none[0] = none[5] = none[10] = none[15] = 1.f;
    float data[16 * 4 + 12] = {};
    std::memcpy(data, view, sizeof(view));
    std::memcpy(data + 16, projection, sizeof(projection));
    std::memcpy(data + 32, none, sizeof(none));
    std::memcpy(data + 48, world, sizeof(world));
    parts.Write(1024, data, 0, sizeof(data));
    std::vector<Candidate> got(Scanner::PARTS);
    const int n = parts.CopyParts(got.data());
    int views = 0, projections = 0;
    for (int i = 0; i < n; ++i) {
      const int kind = static_cast<int>(got[i].key >> PART_KIND_SHIFT);
      if (kind == static_cast<int>(PartKind::view)) ++views;
      if (kind == static_cast<int>(PartKind::projection)) ++projections;
    }
    std::printf("  %d views, %d projections\n", views, projections);
    Check(views >= 2 && projections == 1, "recording, a view (four rows and three) and a projection are found, no view at all is not");
  }
  scanner.SetRecording(false);
  Check(!scanner.Wants(128), "stopped, back to the locked size");

  wchar_t folder[MAX_PATH], matrices_path[MAX_PATH], flow_path[MAX_PATH];
  GetTempPathW(MAX_PATH, folder);
  GetTempFileNameW(folder, L"hcm", 0, matrices_path);
  GetTempFileNameW(folder, L"hcf", 0, flow_path);
  {
    record::MatricesWriter m;
    Check(m.Open(matrices_path, 2.5f), "matrices file opens");
    m.Frame(7, found.data(), 2, found.data(), 1);
    m.Frame(8, found.data(), 0);
    m.Close();
    record::FlowWriter f;
    Check(f.Open(flow_path), "flow file opens");
    record::FlowFrame head{};
    head.camera_frame = 7;
    head.handed = 1;
    head.locked = key;
    std::vector<float> grid(4ull * SAMPLES, 0.25f);
    f.Frame(head, found.data(), grid.data());
    f.Close();
  }
  float aspect = 0;
  std::vector<record::MatricesRecord> matrices;
  std::vector<record::FlowRecord> flow;
  Check(record::ReadMatrices(matrices_path, aspect, matrices) && aspect == 2.5f && matrices.size() == 2 && matrices[0].frame == 7 && matrices[0].found.size() == 2 && matrices[0].parts.size() == 1 && matrices[0].found[1].key == key && matrices[1].found.empty(), "matrices read back");
  Check(record::ReadFlow(flow_path, flow) && flow.size() == 1 && flow[0].head.locked == key && flow[0].handed.size() == 1 && flow[0].grid.size() == 4ull * SAMPLES && flow[0].grid[5] == 0.25f, "flow reads back");
  DeleteFileW(matrices_path);
  DeleteFileW(flow_path);
}

// A made-up recording to try camera_replay on: a real camera in an 80-byte buffer's second write, a first write that is not the camera, and an unrelated matrix.
void MakeRecording(const wchar_t* matrices_path, const wchar_t* flow_path) {
  record::MatricesWriter m;
  record::FlowWriter f;
  if (!m.Open(matrices_path, W / H) || !f.Open(flow_path)) return;
  Camera c, fixed;
  fixed.yaw = 1.f;
  Mat4 prev = ViewProjection(c, false);
  Candidate list[3];
  for (int i = 1; i <= 300; ++i) {
    c.yaw += 0.002f;
    fixed.yaw -= 0.0015f;
    const Mat4 cur = ViewProjection(c, false), decoy = ViewProjection(fixed, false);
    list[0].key = MakeKey(80, 0, false, 0);
    std::memcpy(list[0].m, decoy.m, sizeof(decoy.m));
    list[1].key = MakeKey(80, 0, false, 1);
    std::memcpy(list[1].m, cur.m, sizeof(cur.m));
    list[2].key = MakeKey(1920, 848, false, 0);
    std::memcpy(list[2].m, decoy.m, sizeof(decoy.m));
    m.Frame(i, list, 3);
    const Scene s = Make(cur, prev, false, true, false, 0.2f, static_cast<uint32_t>(i));
    record::FlowFrame head{};
    head.camera_frame = i;
    head.id = static_cast<uint64_t>(i);
    head.scale[0] = s.scale[0];
    head.scale[1] = s.scale[1];
    head.width = static_cast<uint32_t>(W);
    head.height = static_cast<uint32_t>(H);
    head.input_width = head.width;
    head.input_height = head.height;
    head.handed = 3;
    f.Frame(head, list, s.grid.data());
    prev = cur;
  }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  if (argc == 4 && wcscmp(argv[1], L"--make-recording") == 0) {
    MakeRecording(argv[2], argv[3]);
    return 0;
  }
  TestRecording();
  TestRecognise();
  TestScanner();
  TestFit();
  TestChooser();
  std::printf(g_failed == 0 ? "passed\n" : "%d FAILED\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
