// Reflex's latency report (slReflexGetState's frameReport) boiled down to a few averages for the overlay. 64-bit only (Streamline): used by
// the helper, which passes the numbers on through dlssg_link.h.
// The driver fills the report from the PCL markers (simulation start/end, render submit, present) and its own GPU times. A report at all
// means the driver sees HSF's markers on the game's frames, which is the best sign that Reflex is really working.
#pragma once

#include <sl.h>
#include <sl_reflex.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace hsf::reflex_report {

// Averages in microseconds over the frames that were new since the last call. `frames` = 0: nothing new (no report, or no valid frames).
struct Summary {
  int32_t frames = 0;
  int32_t total_us = 0;    // simulation start -> GPU render end, or -> present end for a frame without GPU times (gpu_us = 0: none had any)
  int32_t game_us = 0;     // simulation start -> simulation end: the game's own CPU frame (until its Present)
  int32_t submit_us = 0;   // render submit start -> end: HSF's hand-off copy on the game thread
  int32_t handoff_us = 0;  // render submit end -> present start: until HSF's DLSS-G presenter picks the frame up
  int32_t present_us = 0;  // present start -> present end: DLSS-G's Present call
  int32_t gpu_us = 0;      // present end -> GPU render end: the render queue and the GPU (frame generation included)
  int32_t foreign = 0;     // entries with a frame ID HSF never issued: never counted, and they must not move last_frame
  char raw[440] = {};      // what the driver sent this time and why frames were not counted, for the log
};

// `last_frame` keeps the newest frame already counted, between calls; `newest_issued` is the highest frame ID HSF has given out. One entry with
// a higher ID would otherwise lift last_frame above every real frame and end the report for good (seen in AC3 and Tomb Raider). A frame counts once the driver has its simulation start and present end:
// that is the driver seeing HSF's markers. Its GPU times count only when the GPU's end comes after the frame's present end.
inline Summary Summarize(const sl::ReflexState& state, uint64_t& last_frame, uint64_t newest_issued) {
  Summary s;
  if (!state.latencyReportAvailable) {
    std::snprintf(s.raw, sizeof(s.raw), "no report: the driver's latency call failed");
    return s;
  }
  int64_t total = 0, game = 0, submit = 0, handoff = 0, present = 0, gpu = 0;
  int32_t gpu_frames = 0, entries = 0, fresh = 0, no_sim = 0, no_present = 0, no_gpu = 0, early_gpu = 0, too_long = 0;
  uint64_t newest = last_frame, foreign_id = 0;
  const sl::ReflexReport* latest = nullptr;
  const auto span = [](uint64_t from, uint64_t to) -> int64_t { return from != 0 && to > from ? static_cast<int64_t>(to - from) : 0; };
  for (const sl::ReflexReport& r : state.frameReport) {
    if (r.frameID == 0) continue;
    if (r.frameID > newest_issued) {
      ++s.foreign;
      foreign_id = r.frameID > foreign_id ? r.frameID : foreign_id;
      continue;
    }
    ++entries;
    if (latest == nullptr || r.frameID > latest->frameID) latest = &r;
    if (r.frameID <= last_frame) continue;
    ++fresh;
    if (r.simStartTime == 0) {
      ++no_sim;
      continue;
    }
    if (r.presentEndTime <= r.simStartTime) {
      ++no_present;
      continue;
    }
    const bool has_gpu = r.gpuRenderEndTime >= r.presentEndTime;
    const int64_t t = span(r.simStartTime, has_gpu ? r.gpuRenderEndTime : r.presentEndTime);
    if (t > 1000000) {  // over a second: not a real frame's latency (a pause, or a mismatched frame)
      ++too_long;
      continue;
    }
    if (r.gpuRenderEndTime == 0)
      ++no_gpu;
    else if (!has_gpu)
      ++early_gpu;
    newest = r.frameID > newest ? r.frameID : newest;
    ++s.frames;
    total += t;
    game += span(r.simStartTime, r.simEndTime);
    submit += span(r.renderSubmitStartTime, r.renderSubmitEndTime);
    handoff += span(r.renderSubmitEndTime, r.presentStartTime);
    present += span(r.presentStartTime, r.presentEndTime);
    if (has_gpu) {
      gpu += span(r.presentEndTime, r.gpuRenderEndTime);
      ++gpu_frames;
    }
  }
  last_frame = newest;
  const int written = std::snprintf(s.raw, sizeof(s.raw),
                                    "%d entries, %d new, %d counted; left out: no sim start %d, no present end %d, over 1 s %d; GPU end missing %d, too early %d; "
                                    "foreign IDs %d (largest %llu, ours up to %llu)",
                                    entries, fresh, s.frames, no_sim, no_present, too_long, no_gpu, early_gpu, s.foreign,
                                    static_cast<unsigned long long>(foreign_id), static_cast<unsigned long long>(newest_issued));
  if (latest != nullptr && written > 0 && written < static_cast<int>(sizeof(s.raw))) {
    // The newest frame's stamps in microseconds from its simulation start. "none" stays out of the numbers: -1 means the driver has no stamp.
    const uint64_t base = latest->simStartTime;
    const auto at = [base](uint64_t t) -> long long { return t == 0 ? -1 : static_cast<long long>(t) - static_cast<long long>(base); };
    std::snprintf(s.raw + written, sizeof(s.raw) - static_cast<size_t>(written),
                  "; frame %llu%s: sim end %lld, submit %lld to %lld, present %lld to %lld, GPU %lld to %lld us (-1 = none)",
                  static_cast<unsigned long long>(latest->frameID), base == 0 ? " (no sim start)" : "", at(latest->simEndTime),
                  at(latest->renderSubmitStartTime), at(latest->renderSubmitEndTime), at(latest->presentStartTime), at(latest->presentEndTime),
                  at(latest->gpuRenderStartTime), at(latest->gpuRenderEndTime));
  }
  if (s.frames == 0) return s;
  s.total_us = static_cast<int32_t>(total / s.frames);
  s.game_us = static_cast<int32_t>(game / s.frames);
  s.submit_us = static_cast<int32_t>(submit / s.frames);
  s.handoff_us = static_cast<int32_t>(handoff / s.frames);
  s.present_us = static_cast<int32_t>(present / s.frames);
  s.gpu_us = gpu_frames != 0 ? static_cast<int32_t>(gpu / gpu_frames) : 0;
  return s;
}

}  // namespace hsf::reflex_report
