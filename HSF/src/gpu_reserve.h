#pragma once
// GPU reserve: steps in only when frame generation starves the game. At full load the game's real frames can wait behind frame generation
// and stutter. When the card is full AND the game's real frames stutter, the reserve lowers the FPS cap HSF already sends through Reflex a step
// at a time until the stutter stops, then tries to give the frames back. A full card with even frames costs nothing: the reserve never acts
// on load alone (in RE Village a 10% load reserve cost about 10% of the frame rate and gave little back; Reflex already keeps the queue short).
// The idea of a governor comes from kibblerz's DLSS5 ReShade AIO; this code is HSF's own.
//
// Two parts: LoadReader reads the whole card's load from Windows' own counters (the ones Task Manager shows) on a thread of its own, once a
// second; Governor turns those readings and the game's frame times into a cap. Governor has no Windows calls, so tests can drive it.

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "pdh.lib")

namespace hsf::reserve {

// What the governor is told each time a new load reading arrives (about once a second).
struct Input {
  double seconds = 0;      // a steady clock, in seconds
  float load = -1;         // the whole card's busiest engine, percent; below 0 = no reading
  float spikes = 0;        // share of the game's real frames since the last reading that took far longer than usual (Spikes())
  bool generating = false; // frame generation is on and making frames
  bool hold = false;       // keep the cap as it is, decide nothing (a menu is open)
  float out_fps = 0;       // frames on screen per second now (real + generated)
  uint32_t multiplier = 1; // frames on screen per real frame
  uint32_t user_cap = 0;   // the player's own FPS cap, on screen (0 = none)
  bool on = true;          // the reserve is switched on
};

// Share of frame times that are spikes: over SPIKE_RATIO times the median and SPIKE_MS longer than it. 0 with too few frames to judge.
inline float Spikes(const float* ms, size_t count) {
  constexpr float SPIKE_RATIO = 1.5f, SPIKE_MS = 4.f;
  if (count < 10) return 0.f;
  std::vector<float> sorted(ms, ms + count);
  std::nth_element(sorted.begin(), sorted.begin() + count / 2, sorted.end());
  const float median = sorted[count / 2];
  size_t spikes = 0;
  for (size_t i = 0; i < count; ++i)
    if (ms[i] > median * SPIKE_RATIO && ms[i] > median + SPIKE_MS) ++spikes;
  return static_cast<float>(spikes) / static_cast<float>(count);
}

class Governor {
 public:
  static constexpr double SETTLE_S = 5;    // after generation starts or the multiplier or cap changes, wait this long before judging
  static constexpr float FULL_LOAD = 95;   // the card counts as full from this load
  static constexpr float STUTTER = 0.03f;  // stutter: at least this share of real frames are spikes
  static constexpr double BAD_S = 3;       // full and stuttering this long: one step down
  static constexpr double UP_S = 10;       // no stutter this long: one step up (doubles each time the stutter comes back after a step up)
  static constexpr double UP_MAX_S = 80;
  static constexpr float DOWN = 0.95f;     // a step down: 5% fewer frames
  static constexpr float UP = 1.03f;       // a step up: 3% more
  static constexpr uint32_t BASE_FLOOR = 30;  // the reserve never takes the game's own frame rate under this

  // Feeds one reading. Returns true when the cap changed.
  bool Update(const Input& in) {
    const uint32_t before = cap_;
    const double dt = last_ > 0 ? std::clamp(in.seconds - last_, 0.0, 2.0) : 0.0;
    last_ = in.seconds;
    load_ = in.load;
    spikes_ = in.spikes;
    // Generation off for two readings in a row (one can fall on a short gap, such as a scene cut): let go.
    stopped_ = !in.on || in.multiplier < 2 ? 2 : !in.generating ? stopped_ + 1 : 0;
    if (stopped_ >= 2) {
      Clear();
      return cap_ != before;
    }
    if (in.multiplier != multiplier_ || in.user_cap != user_cap_) {
      // The cap counts frames on screen, so a cap found at one multiplier is wrong at another (115 found at 2x is 19 a second for the game
      // at 6x): a new multiplier starts again from no cap.
      if (in.multiplier != multiplier_) {
        cap_ = 0;
        full_ = false;
        up_wait_ = UP_S;
      }
      multiplier_ = in.multiplier;
      user_cap_ = in.user_cap;
      settle_until_ = in.seconds + SETTLE_S;
      bad_ = good_ = 0;
      if (cap_ != 0 && user_cap_ != 0 && cap_ >= user_cap_) cap_ = 0;  // the player's own cap is now the lower one
    }
    if (!in.generating || in.hold || in.seconds < settle_until_) return cap_ != before;
    const uint32_t floor = BASE_FLOOR * in.multiplier;
    const bool starving = in.load >= FULL_LOAD && in.spikes >= STUTTER;  // stutter on a card with room is not frame generation's doing
    if (starving) {
      bad_ += dt;
      good_ = 0;
      if (bad_ >= BAD_S) {
        bad_ = 0;
        if (raised_) up_wait_ = std::min(up_wait_ * 2, UP_MAX_S);  // the last step up brought it back: wait longer before the next
        raised_ = false;
        // Start from the cap in force, or from what is on screen when no cap holds the game.
        float from = static_cast<float>(cap_ != 0 ? cap_ : user_cap_);
        if (in.out_fps > 1.f && (from == 0 || in.out_fps < from)) from = in.out_fps;
        if (from > 0) {
          const uint32_t next = static_cast<uint32_t>(std::floor(from * DOWN));
          full_ = next < floor;
          if (!full_ || cap_ == 0 || cap_ > floor) cap_ = std::max(next, floor);
          if (in.user_cap != 0 && cap_ >= in.user_cap) cap_ = 0;
        }
      }
    } else if (in.spikes < STUTTER / 2) {
      good_ += dt;
      bad_ = 0;
      full_ = false;
      if (cap_ != 0 && good_ >= up_wait_) {
        good_ = 0;
        raised_ = true;
        const uint32_t next = std::max(cap_ + 1, static_cast<uint32_t>(std::ceil(static_cast<float>(cap_) * UP)));
        // Back to the player's cap once it is reached; with no cap of the player's, let go once the game no longer reaches this one
        // (something else holds it back, so the cap does nothing).
        if ((in.user_cap != 0 && next >= in.user_cap) || (in.user_cap == 0 && in.out_fps > 1.f && in.out_fps < static_cast<float>(cap_) * 0.9f)) cap_ = 0;
        else cap_ = next;
      }
      if (cap_ == 0) up_wait_ = UP_S;
    } else {
      bad_ = good_ = 0;
    }
    if (cap_ != before) ++changes_;
    return cap_ != before;
  }

  // The cap the reserve holds, on screen; 0 = not acting.
  uint32_t Cap() const { return cap_; }
  // The cap in force: the reserve's while it is lower than the player's.
  uint32_t InForce(uint32_t user_cap) const { return cap_ != 0 && (user_cap == 0 || cap_ < user_cap) ? cap_ : user_cap; }
  // Still full and stuttering with the game already at the floor: only a lower multiplier or lower settings help.
  bool Full() const { return full_; }
  float Load() const { return load_; }
  float SpikeShare() const { return spikes_; }
  uint32_t Changes() const { return changes_; }

  void Clear() {
    cap_ = 0;
    full_ = raised_ = false;
    bad_ = good_ = 0;
    up_wait_ = UP_S;
    multiplier_ = 0;  // the next start settles first
  }

 private:
  uint32_t cap_ = 0, multiplier_ = 0, user_cap_ = 0, changes_ = 0, stopped_ = 0;
  double last_ = 0, settle_until_ = 0, bad_ = 0, good_ = 0, up_wait_ = UP_S;
  float load_ = -1, spikes_ = 0;
  bool full_ = false, raised_ = false;
};

// The whole card's load as Task Manager works it out: per engine, the sum over every process; the busiest engine of the busiest card.
// Read from "\GPU Engine(*)\Utilization Percentage" once a second on a thread of its own (a read can take a few milliseconds with many
// processes, never on a game thread). Started on first use and never stopped: the thread holds a reference to HSF's module, so the add-on
// is never unloaded under it, and the reader must outlive it (make it once with new and keep it).
class LoadReader {
 public:
  // Readings only while wanted; the thread sleeps otherwise.
  void Want(bool want) {
    want_.store(want, std::memory_order_relaxed);
    if (want) Start();
  }
  void Start() {
    if (started_.exchange(true)) return;
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Thread), &self)) {
      failed_ = true;
      return;
    }
    self_ = self;
    HANDLE const thread = CreateThread(nullptr, 0, &Thread, this, 0, nullptr);
    if (thread == nullptr) {
      FreeLibrary(self);
      failed_ = true;
      return;
    }
    CloseHandle(thread);
  }
  // The last reading in percent, and a number that goes up with each new one. Below 0: no reading yet.
  float Load(uint32_t* serial = nullptr) const {
    if (serial != nullptr) *serial = serial_.load(std::memory_order_acquire);
    return load_.load(std::memory_order_relaxed);
  }
  bool Failed() const { return failed_.load(); }

  // Instance names look like "pid_1234_luid_0x00000000_0x0000D1F2_phys_0_eng_0_engtype_3D": the engine is everything from "luid_" on.
  static float Busiest(const std::vector<std::pair<std::wstring, double>>& rows) {
    std::map<std::wstring, double> engines;
    for (const auto& [name, value] : rows) {
      const size_t at = name.find(L"luid_");
      if (at == std::wstring::npos || !(value > 0)) continue;
      engines[name.substr(at)] += value;
    }
    double most = 0;
    for (const auto& [engine, sum] : engines) most = std::max(most, sum);
    return static_cast<float>(std::min(most, 100.0));
  }

 private:
  static DWORD WINAPI Thread(void* self) {
    auto* reader = static_cast<LoadReader*>(self);
    reader->Run();
    FreeLibraryAndExitThread(reader->self_, 0);
  }

  void Run() {
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER counter = nullptr;
    if (PdhOpenQueryW(nullptr, 0, &query) != ERROR_SUCCESS ||
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &counter) != ERROR_SUCCESS) {
      if (query != nullptr) PdhCloseQuery(query);
      failed_ = true;
      return;
    }
    PdhCollectQueryData(query);  // a rate needs two readings
    std::vector<unsigned char> buffer;
    std::vector<std::pair<std::wstring, double>> rows;
    for (;;) {
      Sleep(1000);
      if (!want_.load(std::memory_order_relaxed)) {
        load_.store(-1.f, std::memory_order_relaxed);
        continue;
      }
      if (PdhCollectQueryData(query) != ERROR_SUCCESS) continue;
      DWORD size = 0, count = 0;
      if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr) != PDH_MORE_DATA || size == 0) continue;
      buffer.resize(size);
      auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
      if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items) != ERROR_SUCCESS) continue;
      rows.clear();
      for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
          rows.emplace_back(items[i].szName, items[i].FmtValue.doubleValue);
      load_.store(Busiest(rows), std::memory_order_relaxed);
      serial_.fetch_add(1, std::memory_order_release);
    }
  }

  HMODULE self_ = nullptr;
  std::atomic<bool> started_{false}, failed_{false}, want_{false};
  std::atomic<float> load_{-1.f};
  std::atomic<uint32_t> serial_{0};
};

}  // namespace hsf::reserve
