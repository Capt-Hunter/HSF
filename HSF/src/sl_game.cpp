// See sl_game.h. Hooks are written here, in HSF's own code: a 5-byte jump at the start of each function goes to a relay in a page HSF allocates near the module, which
// jumps to the hook; the hook calls a trampoline holding the moved prologue, which jumps back. Anything unexpected at a function's start means no hook there, only a log line.
#include "sl_game.h"

#include <windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_dlss.h>
#include <sl_reflex.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <new>

namespace hsf::slgame {
namespace {

// ---- Reading the game's memory without ever taking the game down ----

// No C++ objects in these: they use structured exceptions.
bool SafeCopy(void* to, const void* from, size_t bytes) {
  __try {
    std::memcpy(to, from, bytes);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool SafeDesc(void* native, D3D12_RESOURCE_DESC* out) {
  __try {
    *out = static_cast<ID3D12Resource*>(native)->GetDesc();
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

uint32_t SafeToken(const sl::FrameToken* token) {
  __try {
    return static_cast<uint32_t>(*token);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return UINT32_MAX;
  }
}

uint32_t SafeViewport(const sl::ViewportHandle* viewport) {
  __try {
    return static_cast<uint32_t>(*viewport);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return UINT32_MAX;
  }
}

// ---- The log ----

constexpr long MAX_LOG_BYTES = 32 * 1024 * 1024;

SRWLOCK g_lock = SRWLOCK_INIT;  // guards the log and all the counters below
FILE* g_file = nullptr;

void (*g_line_sink)(const char* text) = nullptr;  // under g_lock: where Line goes during HookFunction
void Line(const char* format, ...) {
  if (g_line_sink != nullptr) {
    char text[400];
    va_list sink_args;
    va_start(sink_args, format);
    vsnprintf(text, sizeof(text), format, sink_args);
    va_end(sink_args);
    g_line_sink(text);
    return;
  }
  if (g_file == nullptr) return;
  if (std::ftell(g_file) > MAX_LOG_BYTES) return;
  char text[2048];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::fprintf(g_file, "%10.2f [%5lu] %s\r\n", static_cast<double>(GetTickCount64() % 100000000) / 1000.0, GetCurrentThreadId(), text);
  std::fflush(g_file);
}

const char* BufferName(uint32_t type) {
  switch (type) {
    case sl::kBufferTypeDepth: return "Depth";
    case sl::kBufferTypeMotionVectors: return "MotionVectors";
    case sl::kBufferTypeHUDLessColor: return "HUDLessColor";
    case sl::kBufferTypeScalingInputColor: return "ScalingInputColor";
    case sl::kBufferTypeScalingOutputColor: return "ScalingOutputColor";
    case sl::kBufferTypeUIColorAndAlpha: return "UIColorAndAlpha";
    case sl::kBufferTypeExposure: return "Exposure";
    case sl::kBufferTypeHiResDepth: return "HiResDepth";
    case sl::kBufferTypeOpaqueColor: return "OpaqueColor";
    case sl::kBufferTypeBiasCurrentColorHint: return "BiasCurrentColorHint";
    case sl::kBufferTypeReactiveMaskHint: return "ReactiveMaskHint";
    case sl::kBufferTypeTransparencyAndCompositionMaskHint: return "TransparencyCompositionMaskHint";
    default: return "other";
  }
}

const char* LifecycleName(uint32_t lifecycle) {
  switch (lifecycle) {
    case sl::eOnlyValidNow: return "OnlyValidNow";
    case sl::eValidUntilPresent: return "ValidUntilPresent";
    case sl::eValidUntilEvaluate: return "ValidUntilEvaluate";
    default: return "?";
  }
}

const char* FeatureName(uint32_t feature) {
  switch (feature) {
    case sl::kFeatureDLSS: return "DLSS";
    case sl::kFeatureNIS: return "NIS";
    case sl::kFeatureReflex: return "Reflex";
    case sl::kFeaturePCL: return "PCL";
    case sl::kFeatureDeepDVC: return "DeepDVC";
    case sl::kFeatureDLSS_G: return "DLSS-G";
    case sl::kFeatureDLSS_RR: return "DLSS-RR";
    case sl::kFeatureDLSS_NR: return "DLSS-NR";
    case sl::kFeatureCommon: return "Common";
    default: return "other";
  }
}

// ---- What has been seen ----

enum Kind : uint32_t { KIND_SET_TAG, KIND_TAG_FOR_FRAME, KIND_EVALUATE, KIND_COUNT };
const char* const KIND_NAMES[KIND_COUNT] = {"slSetTag", "slSetTagForFrame", "slEvaluateFeature input"};

// One tag type sent through one call on one viewport. A line is written when anything but the texture itself changes, because pools swap textures every frame.
struct TagSeen {
  uint32_t kind, viewport, type;
  bool used;
  uint32_t lines;
  uint64_t count, count_period;
  uint32_t null_count;
  uint32_t width, height, format, state, lifecycle, extent_top, extent_left, extent_width, extent_height;
  bool has_cmd;
  void* natives[16];
  uint32_t native_count;
};
constexpr uint32_t MAX_TAG_LINES = 60;
TagSeen g_tags[64];

struct FeatureSeen {
  uint32_t feature;
  bool used;
  uint64_t count, count_period;
  uint32_t lines;
};
FeatureSeen g_features[16];

uint64_t g_constants = 0, g_constants_period = 0, g_resets = 0;
uint64_t g_frames = 0, g_calls_failed = 0;
std::atomic<uint64_t> g_frames_public{0};  // g_frames, readable without the lock
uint32_t g_last_token = UINT32_MAX;
ULONGLONG g_next_summary = 0;
bool g_tracing = false;
uint32_t g_trace_left = 0;
constexpr uint64_t TRACE_EVERY_FRAMES = 1800;
constexpr uint32_t TRACE_FRAMES = 3;

// A new frame token: counts frames, and every so often turns on a line per call for a few frames, which shows the order of calls and their threads.
void SeeToken(uint32_t token) {
  if (token == UINT32_MAX || token == g_last_token) return;
  g_last_token = token;
  ++g_frames;
  // The real frame count for the menu counts only the game's token moving forward. With optical motion HSF's own tags carry another token (0 in Witcher 3), and
  // counting every switch between the two gave about twice the real frames, more or fewer as the mouse moved. A token far behind the highest seen is
  // the game starting its count again (a load), and counts as a new start.
  static uint32_t s_highest = 0;
  if (token != 0 && (token > s_highest || s_highest - token > 100000)) {
    s_highest = token;
    g_frames_public.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_frames % TRACE_EVERY_FRAMES == 60) {
    g_trace_left = TRACE_FRAMES;
    Line("--- every call for the next %u frames, starting at frame %llu ---", TRACE_FRAMES, g_frames);
  }
  g_tracing = g_trace_left != 0;
  if (g_trace_left != 0) --g_trace_left;
}

void AddDriveSummary(char* text, size_t size, int& used);

void Summary() {
  const ULONGLONG now = GetTickCount64();
  if (now < g_next_summary) return;
  g_next_summary = now + 2000;
  char text[1900];
  int used = std::snprintf(text, sizeof(text), "summary: frames %llu token %u constants +%llu resets %llu failed %llu |", g_frames, g_last_token, g_constants_period, g_resets, g_calls_failed);
  g_constants_period = 0;
  AddDriveSummary(text, sizeof(text), used);
  for (TagSeen& t : g_tags) {
    if (!t.used || t.count_period == 0) continue;
    if (used > 0 && static_cast<size_t>(used) < sizeof(text))
      used += std::snprintf(text + used, sizeof(text) - used, " %s/vp%u/%s +%llu (%u textures)", t.kind == KIND_SET_TAG ? "tag" : t.kind == KIND_TAG_FOR_FRAME ? "frametag" : "evaltag", t.viewport,
                            BufferName(t.type), t.count_period, t.native_count);
    t.count_period = 0;
  }
  for (FeatureSeen& f : g_features) {
    if (!f.used || f.count_period == 0) continue;
    if (used > 0 && static_cast<size_t>(used) < sizeof(text)) used += std::snprintf(text + used, sizeof(text) - used, " eval/%s +%llu", FeatureName(f.feature), f.count_period);
    f.count_period = 0;
  }
  Line("%s", text);
}

void LogGuid(const char* what, const sl::StructType& t, size_t version) {
  Line("%s struct %08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x version %zu", what, t.data1, t.data2, t.data3, t.data4[0], t.data4[1], t.data4[2], t.data4[3], t.data4[4], t.data4[5], t.data4[6],
       t.data4[7], version);
}

void SeeTag(Kind kind, uint32_t viewport, const sl::ResourceTag* tag_address, bool has_cmd) {
  alignas(sl::ResourceTag) unsigned char tag_bytes[sizeof(sl::ResourceTag)];
  if (!SafeCopy(tag_bytes, tag_address, sizeof(tag_bytes))) {
    Line("%s: tag could not be read at %p", KIND_NAMES[kind], tag_address);
    return;
  }
  const sl::ResourceTag& tag = *reinterpret_cast<const sl::ResourceTag*>(tag_bytes);
  if (tag.structType != sl::ResourceTag::s_structType) {
    LogGuid("tag is not a ResourceTag:", tag.structType, tag.structVersion);
    return;
  }
  TagSeen* seen = nullptr;
  for (TagSeen& t : g_tags) {
    if (t.used && t.kind == kind && t.viewport == viewport && t.type == tag.type) {
      seen = &t;
      break;
    }
    if (!t.used) {
      t = {};
      t.used = true;
      t.kind = kind;
      t.viewport = viewport;
      t.type = tag.type;
      t.lifecycle = UINT32_MAX;
      seen = &t;
      break;
    }
  }
  if (seen == nullptr) return;
  ++seen->count;
  ++seen->count_period;

  alignas(sl::Resource) unsigned char resource_bytes[sizeof(sl::Resource)];
  const bool have_resource = tag.resource != nullptr && SafeCopy(resource_bytes, tag.resource, sizeof(resource_bytes));
  if (!have_resource) {
    ++seen->null_count;
    if (seen->null_count <= 5 || g_tracing)
      Line("%s vp%u %s(%u): no resource%s lifecycle %s", KIND_NAMES[kind], viewport, BufferName(tag.type), tag.type, tag.resource == nullptr ? "" : " (unreadable)", LifecycleName(tag.lifecycle));
    return;
  }
  const sl::Resource& res = *reinterpret_cast<const sl::Resource*>(resource_bytes);
  D3D12_RESOURCE_DESC desc{};
  const bool have_desc = res.native != nullptr && SafeDesc(res.native, &desc);
  const uint32_t width = have_desc ? static_cast<uint32_t>(desc.Width) : res.width;
  const uint32_t height = have_desc ? desc.Height : res.height;
  const uint32_t format = have_desc ? static_cast<uint32_t>(desc.Format) : res.nativeFormat;

  bool new_texture = true;
  for (uint32_t i = 0; i < seen->native_count; ++i)
    if (seen->natives[i] == res.native) new_texture = false;
  if (new_texture && seen->native_count < std::size(seen->natives)) seen->natives[seen->native_count++] = res.native;

  const bool changed = width != seen->width || height != seen->height || format != seen->format || res.state != seen->state || static_cast<uint32_t>(tag.lifecycle) != seen->lifecycle ||
                       tag.extent.top != seen->extent_top || tag.extent.left != seen->extent_left || tag.extent.width != seen->extent_width || tag.extent.height != seen->extent_height ||
                       has_cmd != seen->has_cmd;
  if ((changed && seen->lines < MAX_TAG_LINES) || g_tracing) {
    if (changed) ++seen->lines;
    Line("%s vp%u %s(%u): %ux%u dxgi %u flags 0x%x mips %u | given %ux%u dxgi %u state 0x%x type %u | %s extent %u,%u %ux%u | native %p view %p cmd %d | structs tag v%zu resource v%zu%s", KIND_NAMES[kind],
         viewport, BufferName(tag.type), tag.type, width, height, format, have_desc ? static_cast<uint32_t>(desc.Flags) : 0u, have_desc ? desc.MipLevels : 0u, res.width, res.height, res.nativeFormat, res.state,
         static_cast<uint32_t>(res.type), LifecycleName(tag.lifecycle), tag.extent.left, tag.extent.top, tag.extent.width, tag.extent.height, res.native, res.view, has_cmd ? 1 : 0, tag.structVersion,
         res.structVersion, have_desc ? "" : " (no desc)");
  }
  seen->width = width;
  seen->height = height;
  seen->format = format;
  seen->state = res.state;
  seen->lifecycle = tag.lifecycle;
  seen->extent_top = tag.extent.top;
  seen->extent_left = tag.extent.left;
  seen->extent_width = tag.extent.width;
  seen->extent_height = tag.extent.height;
  seen->has_cmd = has_cmd;
}

void LogMatrix(const char* name, const sl::float4x4& m) {
  Line("  %-18s %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g", name, m.row[0].x, m.row[0].y, m.row[0].z, m.row[0].w, m.row[1].x, m.row[1].y, m.row[1].z, m.row[1].w, m.row[2].x,
       m.row[2].y, m.row[2].z, m.row[2].w, m.row[3].x, m.row[3].y, m.row[3].z, m.row[3].w);
}

// The game's struct may be an older version than the SDK's, so only fields of the version it says it is get read.
void SeeConstants(const sl::Constants* address, uint32_t viewport, uint32_t token) {
  alignas(sl::Constants) unsigned char bytes[sizeof(sl::Constants)];
  if (!SafeCopy(bytes, address, sizeof(bytes))) {
    Line("slSetConstants: could not be read at %p", address);
    return;
  }
  const sl::Constants& c = *reinterpret_cast<const sl::Constants*>(bytes);
  ++g_constants;
  ++g_constants_period;
  const bool reset = c.reset == sl::Boolean::eTrue;
  if (reset) ++g_resets;
  const bool full = g_constants <= 5 || g_constants % 600 == 0 || (reset && g_resets <= 30);
  if (!full && !g_tracing) return;
  if (!full) {
    Line("slSetConstants vp%u token %u jitter %.4f %.4f mvecScale %.5f %.5f reset %d", viewport, token, c.jitterOffset.x, c.jitterOffset.y, c.mvecScale.x, c.mvecScale.y, static_cast<int>(c.reset));
    return;
  }
  Line("slSetConstants #%llu vp%u token %u struct v%zu%s", g_constants, viewport, token, c.structVersion, c.next != nullptr ? " (has a chained struct)" : "");
  if (c.structType != sl::Constants::s_structType) LogGuid("  NOT the SDK's Constants:", c.structType, c.structVersion);
  LogMatrix("cameraViewToClip", c.cameraViewToClip);
  LogMatrix("clipToCameraView", c.clipToCameraView);
  LogMatrix("clipToLensClip", c.clipToLensClip);
  LogMatrix("clipToPrevClip", c.clipToPrevClip);
  LogMatrix("prevClipToClip", c.prevClipToClip);
  Line("  jitter %.5f %.5f mvecScale %.6f %.6f pinhole %.6g %.6g", c.jitterOffset.x, c.jitterOffset.y, c.mvecScale.x, c.mvecScale.y, c.cameraPinholeOffset.x, c.cameraPinholeOffset.y);
  Line("  pos %.6g %.6g %.6g up %.6g %.6g %.6g right %.6g %.6g %.6g fwd %.6g %.6g %.6g", c.cameraPos.x, c.cameraPos.y, c.cameraPos.z, c.cameraUp.x, c.cameraUp.y, c.cameraUp.z, c.cameraRight.x, c.cameraRight.y,
       c.cameraRight.z, c.cameraFwd.x, c.cameraFwd.y, c.cameraFwd.z);
  Line("  near %.6g far %.6g fov %.6g aspect %.6g mvecInvalid %.6g", c.cameraNear, c.cameraFar, c.cameraFOV, c.cameraAspectRatio, c.motionVectorsInvalidValue);
  Line("  (0 false, 1 true, 2 invalid) depthInverted %d cameraMotionIncluded %d motionVectors3D %d reset %d orthographic %d dilated %d jittered %d", static_cast<int>(c.depthInverted), static_cast<int>(c.cameraMotionIncluded),
       static_cast<int>(c.motionVectors3D), static_cast<int>(c.reset), static_cast<int>(c.orthographicProjection), static_cast<int>(c.motionVectorsDilated), static_cast<int>(c.motionVectorsJittered));
  if (c.structVersion >= sl::kStructVersion2) Line("  minRelativeLinearDepthObjectSeparation %.6g", c.minRelativeLinearDepthObjectSeparation);
}

// ---- Hooks ----

std::atomic<int> g_in_flight{0};
struct InFlight {
  InFlight() { g_in_flight.fetch_add(1, std::memory_order_acquire); }
  ~InFlight() { g_in_flight.fetch_sub(1, std::memory_order_release); }
};

struct Lock {
  Lock() { AcquireSRWLockExclusive(&g_lock); }
  ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using SetTagForFrameFn = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using SetConstantsFn = sl::Result (*)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
using EvaluateFn = sl::Result (*)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);
using GetFunctionFn = sl::Result (*)(sl::Feature, const char*, void*&);
using GetTokenFn = sl::Result (*)(sl::FrameToken*&, const uint32_t*);
using SetDlssOptionsFn = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSOptions&);
using SetDlssgOptionsFn = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);
using GetDlssgStateFn = sl::Result (*)(const sl::ViewportHandle&, sl::DLSSGState&, const sl::DLSSGOptions*);
using SetReflexOptionsFn = sl::Result (*)(const sl::ReflexOptions&);
using SetFeatureLoadedFn = sl::Result (*)(sl::Feature, bool);
using IsFeatureLoadedFn = sl::Result (*)(sl::Feature, bool&);

SetTagFn g_real_set_tag = nullptr;
SetTagForFrameFn g_real_tag_for_frame = nullptr;
SetConstantsFn g_real_constants = nullptr;
EvaluateFn g_real_evaluate = nullptr;
GetFunctionFn g_real_get_function = nullptr;
GetTokenFn g_real_get_token = nullptr;
SetDlssOptionsFn g_real_dlss_options = nullptr;
SetDlssgOptionsFn g_real_dlssg_options = nullptr;
SetReflexOptionsFn g_real_reflex_options = nullptr;
SetFeatureLoadedFn g_real_set_loaded = nullptr;
IsFeatureLoadedFn g_is_loaded = nullptr;

// ---- Driving DLSS-G ----

std::atomic<bool> g_drive{false};
std::atomic<bool> g_allowed{false};
std::atomic<bool> g_focus_hold{false};  // the game's window is not in front, or only just came back: generation off (SetFocusHold)
std::atomic<uint32_t> g_want_frames{1};
std::atomic<uint32_t> g_viewport{0};
std::atomic<ULONGLONG> g_last_evaluate{0};
std::atomic<uint32_t> g_evaluate_streak{0};
std::atomic<uint32_t> g_mv_width{0}, g_mv_height{0};
std::atomic<bool> g_dynamic{false};
std::atomic<bool> g_camera_rebuild{false};
std::atomic<bool> g_cut_check{true}, g_quick_off{false}, g_score_guard{false};
std::atomic<bool> g_vectors_doubted{false};
std::atomic<VectorsHook> g_vectors_hook{nullptr};
std::atomic<float> g_mvec_scale_x{0.0f}, g_mvec_scale_y{0.0f};
// Optical motion: wanted, when TagOptical last ran, and what the constants then get.
std::atomic<bool> g_optical{false};
std::atomic<ULONGLONG> g_optical_last{0};
std::atomic<float> g_optical_scale_x{0.0f}, g_optical_scale_y{0.0f};
std::atomic<bool> g_optical_dilated{false};
constexpr ULONGLONG OPTICAL_GONE_MS = 300;
bool OpticalActive() { return g_optical.load(std::memory_order_relaxed) && GetTickCount64() - g_optical_last.load(std::memory_order_relaxed) <= OPTICAL_GONE_MS; }
std::atomic<uint32_t> g_cap_fps{0};
std::atomic<bool> g_reflex_boost{false};
std::atomic<float> g_evaluate_gap_ms{0.0f};  // the usual time between evaluates
std::atomic<uint64_t> g_cuts_by_hsf{0}, g_picture_cuts{0}, g_picture_cuts_no_reset{0};
uint64_t g_camera_rebuilt_period = 0, g_camera_refused = 0;  // under g_lock
float g_camera_move_rebuilt = 0.0f, g_camera_move_game = 0.0f;  // under g_lock: largest move of the screen centre this period, in clip space, by each matrix
std::atomic<uint32_t> g_output_width{0}, g_output_height{0};  // DLSS's output, from the game's slDLSSSetOptions: 0 until it sends a real one
std::atomic<uint64_t> g_tagged{0}, g_tagged_period{0}, g_tag_failures{0};
std::atomic<uint32_t> g_presented_period{0}, g_status{0}, g_max_frames{0}, g_presented_last{0};
std::atomic<bool> g_generating{false};
std::atomic<uint32_t> g_frames_sent{0};
std::atomic<bool> g_have_options{false};
std::atomic<bool> g_tag_without_frame{false};
std::atomic<int> g_feature_loaded{-1};  // is DLSS-G's plugin loaded: -1 not known yet
// The latest frame an evaluate came with, which is the one the HUD is about to go on. A game that tags DLSS-G itself and runs no evaluates gives it with its tags and constants.
std::atomic<const sl::FrameToken*> g_last_frame{nullptr};
// When the game last tagged its own vectors for DLSS-G (slSetTag or slSetTagForFrame), and how many came in a row. Unreal's Streamline plugin does this with no evaluates at all.
std::atomic<ULONGLONG> g_last_game_vectors{0};
std::atomic<uint32_t> g_game_vectors_streak{0};

// Generation needs vectors and depth, which come with evaluate calls, or with the game's own tags for DLSS-G. Menus have none, so generation waits for a run of them and stops when they stop.
constexpr ULONGLONG EVALUATE_GONE_MS = 250;
constexpr ULONGLONG ON_SETTLE_MS = 1000;  // after generation goes on, quick off waits this long (DriveFrame)
constexpr uint32_t EVALUATE_STREAK_TO_START = 10;
constexpr float QUICK_OFF_GAPS = 3.0f, QUICK_OFF_LEAST_MS = 50.0f;

// The score over this many pairs, about 2 s. Below DOUBT_GAIN the vectors do clearly worse than no motion; above TRUST_GAIN they are trusted again.
constexpr uint32_t SCORE_WINDOW = 8;
constexpr float DOUBT_GAIN = -0.1f, TRUST_GAIN = 0.0f, SCORE_MIN_MOVING = 0.1f;
struct ScorePair {
  float warp, none, better, worse, moving;
};
ScorePair g_score_pairs[SCORE_WINDOW];  // under g_lock, as are the counts here
uint32_t g_score_written = 0, g_score_still = 0, g_score_cuts = 0;
std::atomic<float> g_score_gain{0.0f};

// A camera that moved this far between two frames has cut. A turn of 60 degrees in a frame is 2400 degrees a second at 40 FPS. Distance is in the game's units, so it also has to be far beyond its recent speed.
constexpr float CUT_TURN_COS = 0.5f, CUT_DISTANCE = 10.0f, CUT_SPEED_TIMES = 20.0f;
uint32_t g_cut_token = UINT32_MAX;  // under g_lock
bool g_cut_answer = false;
float g_camera_speed = 0.0f;
uint64_t g_last_reset_frame = 0;
constexpr uint64_t PICTURE_CUT_FRAMES = 8;  // the picture's answer comes a few frames after the reset it should match

// The game's own DLSS-G options, kept so that what HSF sends has the game's struct version and the game's other fields. Streamline's option calls are not thread safe, and the game
// and the present thread can both reach them, so this lock goes round every one. Never take g_lock while holding it.
SRWLOCK g_options_lock = SRWLOCK_INIT;
constexpr size_t OPTIONS_BYTES = 512;  // more than any version so far, so a newer struct than the SDK's stays whole
static_assert(sizeof(sl::DLSSGOptions) <= OPTIONS_BYTES);
alignas(16) unsigned char g_game_options[OPTIONS_BYTES];
bool g_have_game_options = false;
SetDlssgOptionsFn g_direct_dlssg_options = nullptr;  // from asking Streamline, for when the hook was refused
GetDlssgStateFn g_get_dlssg_state = nullptr;

struct OptionsLock {
  OptionsLock() { AcquireSRWLockExclusive(&g_options_lock); }
  ~OptionsLock() { ReleaseSRWLockExclusive(&g_options_lock); }
};

// HSF's choices over a copy of the game's options. Never changes the struct's version.
void ApplyChoices(sl::DLSSGOptions& options, bool on, uint32_t frames) {
  options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  options.numFramesToGenerate = frames;
  // Menus turn generation off and on: keeping DLSS-G's resources avoids a stall each time.
  options.flags = options.flags | sl::DLSSGFlags::eRetainResourcesWhenOff;
  if (g_dynamic.load(std::memory_order_relaxed)) {
    options.flags = options.flags | sl::DLSSGFlags::eDynamicResolutionEnabled;
    // Streamline's guide wants a middle value inside the range, fixed unless settings change, and gives 3/4 of output as its example. 0 would mean half of output, which in Shadows
    // is below the smallest size seen (1976x828 of 3440x1440). A size the game chose itself stands.
    if (options.dynamicResWidth == 0 || options.dynamicResHeight == 0) {
      options.dynamicResWidth = g_output_width.load(std::memory_order_relaxed) * 3 / 4;
      options.dynamicResHeight = g_output_height.load(std::memory_order_relaxed) * 3 / 4;
    }
  }
}

void AddDriveSummary(char* text, size_t size, int& used) {
  if (!g_drive.load(std::memory_order_relaxed) || used <= 0 || static_cast<size_t>(used) >= size) return;
  used += std::snprintf(text + used, size - used, " generating %d frames %u status 0x%x max %u presented +%u tagged +%llu tag failures %llu dynamic %d | camera %s +%llu refused %llu largest move rebuilt %.3f game's %.3f | cuts by HSF %llu picture cuts %llu with no reset %llu |", g_generating.load() ? 1 : 0, g_frames_sent.load(), g_status.load(),
                        g_max_frames.load(), g_presented_period.exchange(0), g_tagged_period.exchange(0), g_tag_failures.load(), g_dynamic.load() ? 1 : 0, g_camera_rebuild.load() ? "rebuilt" : "game's", g_camera_rebuilt_period, g_camera_refused, g_camera_move_rebuilt, g_camera_move_game,
                        g_cuts_by_hsf.load(), g_picture_cuts.load(), g_picture_cuts_no_reset.load());
  g_camera_rebuilt_period = 0;
  g_camera_move_rebuilt = g_camera_move_game = 0.0f;
  if (used > 0 && static_cast<size_t>(used) < size && g_score_written != 0) {
    const uint32_t pairs = std::min(g_score_written, SCORE_WINDOW);
    float warp = 0.0f, none = 0.0f, better = 0.0f, worse = 0.0f, moving = 0.0f;
    for (uint32_t i = 0; i < pairs; ++i) {
      warp += g_score_pairs[i].warp;
      none += g_score_pairs[i].none;
      better += g_score_pairs[i].better;
      worse += g_score_pairs[i].worse;
      moving += g_score_pairs[i].moving;
    }
    used += std::snprintf(text + used, size - used, " score gain %.2f error %.2f, %.2f without, better %.2f worse %.2f moving %.2f over %u, still %u cuts %u%s |", g_score_gain.load(), warp / pairs * 255.0f, none / pairs * 255.0f, better / pairs, worse / pairs, moving / pairs, pairs,
                          g_score_still, g_score_cuts, g_vectors_doubted.load() ? " DOUBTED" : "");
  }
}

void NoteResult(const char* name, sl::Result result) {
  if (result == sl::Result::eOk) return;
  ++g_calls_failed;
  if (g_calls_failed <= 40) Line("%s returned %d", name, static_cast<int>(result));
}

// The game tags its own vectors for DLSS-G. With no evaluates coming, those tags say the game's data is flowing, and HSF's own tags go to the same frame and viewport.
// Call with g_lock held. frame is null for slSetTag.
void NoteGameVectors(const sl::FrameToken* frame, uint32_t viewport, const sl::ResourceTag* tags, uint32_t count) {
  if (!g_drive.load(std::memory_order_relaxed) || tags == nullptr) return;
  bool vectors = false;
  for (uint32_t i = 0; i < count; ++i) {
    sl::BufferType type = 0;
    if (SafeCopy(&type, &tags[i].type, sizeof(type)) && type == sl::kBufferTypeMotionVectors) vectors = true;
  }
  if (!vectors) return;
  const ULONGLONG now = GetTickCount64();
  // Evaluates, where they come, already set all of this.
  if (now - g_last_evaluate.load(std::memory_order_relaxed) <= EVALUATE_GONE_MS) return;
  if (frame != nullptr) g_last_frame.store(frame, std::memory_order_relaxed);
  if (viewport != UINT32_MAX) g_viewport.store(viewport, std::memory_order_relaxed);
  const ULONGLONG last = g_last_game_vectors.exchange(now, std::memory_order_relaxed);
  if (now - last <= EVALUATE_GONE_MS) {
    const float gap = g_evaluate_gap_ms.load(std::memory_order_relaxed);
    g_evaluate_gap_ms.store(gap + (static_cast<float>(now - last) - gap) * 0.1f, std::memory_order_relaxed);
    g_game_vectors_streak.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_game_vectors_streak.store(1, std::memory_order_relaxed);
    static uint32_t s_lines = 0;
    if (s_lines++ < 20) Line("the game tags its own vectors for DLSS-G (viewport %u, %s): they count as the game's data", viewport, frame != nullptr ? "by frame" : "slSetTag");
  }
}

// The game's own data, from its evaluates or from its own tags, came recently and in a run.
bool GameDataFlowing(ULONGLONG now, ULONGLONG gone_ms) {
  return (now - g_last_evaluate.load(std::memory_order_relaxed) <= gone_ms && g_evaluate_streak.load(std::memory_order_relaxed) >= EVALUATE_STREAK_TO_START) ||
         (now - g_last_game_vectors.load(std::memory_order_relaxed) <= gone_ms && g_game_vectors_streak.load(std::memory_order_relaxed) >= EVALUATE_STREAK_TO_START);
}

ULONGLONG LastGameData() { return std::max(g_last_evaluate.load(std::memory_order_relaxed), g_last_game_vectors.load(std::memory_order_relaxed)); }

sl::Result HookSetTag(const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* cmd) {
  InFlight in_flight;
  {
    Lock lock;
    const uint32_t vp = SafeViewport(&viewport);
    for (uint32_t i = 0; i < count && tags != nullptr; ++i) SeeTag(KIND_SET_TAG, vp, &tags[i], cmd != nullptr);
    NoteGameVectors(nullptr, vp, tags, count);
    Summary();
  }
  const sl::Result result = g_real_set_tag(viewport, tags, count, cmd);
  Lock lock;
  NoteResult("slSetTag", result);
  return result;
}

sl::Result HookTagForFrame(const sl::FrameToken& frame, const sl::ViewportHandle& viewport, const sl::ResourceTag* tags, uint32_t count, sl::CommandBuffer* cmd) {
  InFlight in_flight;
  {
    Lock lock;
    SeeToken(SafeToken(&frame));
    const uint32_t vp = SafeViewport(&viewport);
    for (uint32_t i = 0; i < count && tags != nullptr; ++i) SeeTag(KIND_TAG_FOR_FRAME, vp, &tags[i], cmd != nullptr);
    NoteGameVectors(&frame, vp, tags, count);
    Summary();
  }
  const sl::Result result = g_real_tag_for_frame(frame, viewport, tags, count, cmd);
  Lock lock;
  NoteResult("slSetTagForFrame", result);
  return result;
}

// Shadows' clipToPrevClip is not a move from the last frame: it tracks the camera's heading in play. So rebuild both matrices from what is sound, its projection and camera.
struct CameraFrame {
  CameraPose pose;
  float view_to_clip[4][4], clip_to_view[4][4];
  uint32_t token, viewport;
};
CameraFrame g_camera_now{}, g_camera_before{};  // under g_lock
bool g_have_camera_now = false, g_have_camera_before = false;

// The move of a point in the screen's centre, 100 units in front of the camera: what a far-off world does on screen. Close points only show the camera's own travel.
float CentreMove(const float m[4][4], const float view_to_clip[4][4]) {
  constexpr float DEPTH = 100.0f;
  const float clip_z = -DEPTH * view_to_clip[2][2] + view_to_clip[3][2], clip_w = -DEPTH * view_to_clip[2][3] + view_to_clip[3][3];
  if (clip_w == 0.0f || !std::isfinite(clip_w)) return 1000.0f;
  const float z = clip_z / clip_w;
  const float x = z * m[2][0] + m[3][0], y = z * m[2][1] + m[3][1], w = z * m[2][3] + m[3][3];
  if (w == 0.0f || !std::isfinite(w)) return 1000.0f;
  return std::min(std::sqrt((x / w) * (x / w) + (y / w) * (y / w)), 1000.0f);
}

// Call with g_lock held, before RebuildCamera moves its "now" on. True if HSF has just set this frame's reset. A frame's constants sent twice get the same answer.
bool CheckCut(sl::Constants& fixed, uint32_t token, uint32_t viewport) {
  if (fixed.reset == sl::Boolean::eTrue) {
    g_last_reset_frame = g_frames;
    g_cut_token = token;
    g_cut_answer = false;
    return false;
  }
  if (!g_cut_check.load(std::memory_order_relaxed)) return false;
  if (token != g_cut_token) {
    g_cut_token = token;
    g_cut_answer = false;
    if (g_have_camera_now && g_camera_now.viewport == viewport && g_camera_now.token != token) {
      const CameraPose& last = g_camera_now.pose;
      const float dx = fixed.cameraPos.x - last.pos[0], dy = fixed.cameraPos.y - last.pos[1], dz = fixed.cameraPos.z - last.pos[2];
      const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
      const float facing = fixed.cameraFwd.x * last.fwd[0] + fixed.cameraFwd.y * last.fwd[1] + fixed.cameraFwd.z * last.fwd[2];
      const bool sound = std::isfinite(distance) && std::isfinite(facing);
      if (sound && (facing < CUT_TURN_COS || distance > std::max(CUT_DISTANCE, g_camera_speed * CUT_SPEED_TIMES))) {
        g_cut_answer = true;
        const uint64_t cuts = g_cuts_by_hsf.fetch_add(1, std::memory_order_relaxed) + 1;
        if (cuts <= 300) Line("camera jumped at token %u with no reset from the game, moved %.2f, recent speed %.3f, facing %.3f: HSF sets the reset", token, distance, g_camera_speed, facing);
      } else if (sound) {
        g_camera_speed += (distance - g_camera_speed) * 0.1f;
      }
    }
  }
  if (!g_cut_answer) return false;
  fixed.reset = sl::Boolean::eTrue;
  g_last_reset_frame = g_frames;
  return true;
}

// Call with g_lock held. fixed is a copy of the game's constants: true if its two matrices are now HSF's.
bool RebuildCamera(sl::Constants& fixed, uint32_t token, uint32_t viewport) {
  static_assert(sizeof(sl::float4x4) == sizeof(float[4][4]));
  // The game can send a frame's constants more than once. "Before" only moves on with a new frame.
  if (!g_have_camera_now || g_camera_now.token != token || g_camera_now.viewport != viewport) {
    const bool same_view = g_have_camera_now && g_camera_now.viewport == viewport;
    g_camera_before = g_camera_now;
    g_have_camera_before = same_view;
    g_have_camera_now = true;
  }
  CameraFrame& now = g_camera_now;
  now.token = token;
  now.viewport = viewport;
  std::memcpy(now.pose.right, &fixed.cameraRight, sizeof(now.pose.right));
  std::memcpy(now.pose.up, &fixed.cameraUp, sizeof(now.pose.up));
  std::memcpy(now.pose.fwd, &fixed.cameraFwd, sizeof(now.pose.fwd));
  std::memcpy(now.pose.pos, &fixed.cameraPos, sizeof(now.pose.pos));
  std::memcpy(now.view_to_clip, &fixed.cameraViewToClip, sizeof(now.view_to_clip));
  std::memcpy(now.clip_to_view, &fixed.clipToCameraView, sizeof(now.clip_to_view));

  float to_prev[4][4], from_prev[4][4];
  const CameraFrame& before = g_have_camera_before && fixed.reset != sl::Boolean::eTrue ? g_camera_before : now;  // after a cut nothing moved from anywhere: identity
  if (!RebuildClipToPrev(now.clip_to_view, now.pose, before.view_to_clip, before.pose, to_prev) || !RebuildClipToPrev(before.clip_to_view, before.pose, now.view_to_clip, now.pose, from_prev)) {
    if (++g_camera_refused <= 20) Line("camera: not sound at token %u, the game's matrices go on", token);
    return false;
  }
  float game_matrix[4][4];
  std::memcpy(game_matrix, &fixed.clipToPrevClip, sizeof(game_matrix));
  g_camera_move_rebuilt = std::max(g_camera_move_rebuilt, CentreMove(to_prev, now.view_to_clip));
  g_camera_move_game = std::max(g_camera_move_game, CentreMove(game_matrix, now.view_to_clip));
  ++g_camera_rebuilt_period;
  if (!g_camera_rebuild.load(std::memory_order_relaxed)) return false;
  std::memcpy(&fixed.clipToPrevClip, to_prev, sizeof(to_prev));
  std::memcpy(&fixed.prevClipToClip, from_prev, sizeof(from_prev));
  return true;
}

sl::Result HookConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) {
  InFlight in_flight;
  // Larger than the SDK's struct, in case the game's is newer, so that a newer struct stays whole. If that runs off the end of readable memory, the SDK's size.
  constexpr size_t CONSTANTS_BYTES = 1024;
  static_assert(sizeof(sl::Constants) <= CONSTANTS_BYTES);
  alignas(16) unsigned char fixed_bytes[CONSTANTS_BYTES] = {};
  bool use_fixed = false;
  {
    Lock lock;
    const uint32_t token = SafeToken(&frame);
    SeeToken(token);
    SeeConstants(&values, SafeViewport(&viewport), token);
    if (g_drive.load(std::memory_order_relaxed) && (SafeCopy(fixed_bytes, &values, CONSTANTS_BYTES) || SafeCopy(fixed_bytes, &values, sizeof(sl::Constants)))) {
      sl::Constants& fixed = *reinterpret_cast<sl::Constants*>(fixed_bytes);
      if (fixed.structType == sl::Constants::s_structType) {
        g_mvec_scale_x.store(fixed.mvecScale.x, std::memory_order_relaxed);
        g_mvec_scale_y.store(fixed.mvecScale.y, std::memory_order_relaxed);
        const bool cut = CheckCut(fixed, token, SafeViewport(&viewport));
        const bool rebuilt = RebuildCamera(fixed, token, SafeViewport(&viewport));
        use_fixed = cut || rebuilt;
        // Optical motion is the picture's own, so it has its scale, no jitter, and is dilated when the compose pass has run.
        if (OpticalActive()) {
          fixed.mvecScale = {g_optical_scale_x.load(std::memory_order_relaxed), g_optical_scale_y.load(std::memory_order_relaxed)};
          fixed.jitterOffset = {0.0f, 0.0f};
          fixed.motionVectorsJittered = sl::Boolean::eFalse;
          fixed.motionVectorsDilated = g_optical_dilated.load(std::memory_order_relaxed) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
          use_fixed = true;
        }
      }
    }
    Summary();
  }
  const sl::Result result = g_real_constants(use_fixed ? *reinterpret_cast<const sl::Constants*>(fixed_bytes) : values, frame, viewport);
  Lock lock;
  NoteResult("slSetConstants", result);
  return result;
}

// The evaluate inputs carry the game's vectors and depth, on an open command list. DLSS-G needs them at present, so hand them to Streamline for this frame. They are tagged as only valid
// now, which has Streamline copy them on this command list, because the game's "valid until present" was only ever put to the test by DLSS, which reads them here and now.
// One tag per call: the game's Streamline indexes an array by its own size of the struct, which may not be the SDK's.
void TagForGeneration(const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t count, sl::CommandBuffer* cmd) {
  if (cmd == nullptr || inputs == nullptr) return;
  sl::ViewportHandle viewport(g_viewport.load(std::memory_order_relaxed));
  for (uint32_t i = 0; i < count; ++i) {
    const sl::BaseStructure* input = nullptr;
    if (!SafeCopy(&input, &inputs[i], sizeof(input)) || input == nullptr) continue;
    alignas(sl::ResourceTag) unsigned char bytes[sizeof(sl::ResourceTag)];
    if (!SafeCopy(bytes, input, sizeof(bytes))) continue;
    sl::ResourceTag& tag = *reinterpret_cast<sl::ResourceTag*>(bytes);
    if (tag.structType == sl::ViewportHandle::s_structType) {
      const uint32_t id = SafeViewport(static_cast<const sl::ViewportHandle*>(input));
      viewport = sl::ViewportHandle(id);
      g_viewport.store(id, std::memory_order_relaxed);
      continue;
    }
    if (tag.structType != sl::ResourceTag::s_structType || tag.resource == nullptr) continue;
    if (tag.type != sl::kBufferTypeDepth && tag.type != sl::kBufferTypeMotionVectors) continue;
    alignas(sl::Resource) unsigned char resource_bytes[sizeof(sl::Resource)];
    if (!SafeCopy(resource_bytes, tag.resource, sizeof(resource_bytes))) continue;
    void* const native = reinterpret_cast<const sl::Resource*>(resource_bytes)->native;
    D3D12_RESOURCE_DESC desc{};
    if (native == nullptr || !SafeDesc(native, &desc)) continue;
    const uint32_t width = static_cast<uint32_t>(desc.Width), height = desc.Height;
    // The game's textures change size with its dynamic resolution, and its extent stays empty. Streamline's guide wants an extent with dynamic resolution.
    if (tag.extent.width == 0 || tag.extent.height == 0) {
      tag.extent.left = tag.extent.top = 0;
      tag.extent.width = width;
      tag.extent.height = height;
    }
    if (tag.type == sl::kBufferTypeMotionVectors) {
      const uint32_t old_width = g_mv_width.exchange(width, std::memory_order_relaxed), old_height = g_mv_height.exchange(height, std::memory_order_relaxed);
      if (old_width != 0 && (old_width != width || old_height != height)) {
        g_dynamic.store(true, std::memory_order_relaxed);
        static std::atomic<uint32_t> s_lines{0};
        if (s_lines.fetch_add(1, std::memory_order_relaxed) < 4000) {
          Lock lock;
          Line("vectors now %ux%u, were %ux%u, generating %d", width, height, old_width, old_height, g_generating.load(std::memory_order_relaxed) ? 1 : 0);
        }
      }
    }
    if (tag.type == sl::kBufferTypeMotionVectors) {
      if (const VectorsHook hook = g_vectors_hook.load(std::memory_order_acquire)) {
        const uint32_t extent[4] = {tag.extent.left, tag.extent.top, tag.extent.width, tag.extent.height};
        const float scale[2] = {g_mvec_scale_x.load(std::memory_order_relaxed), g_mvec_scale_y.load(std::memory_order_relaxed)};
        hook(cmd, native, reinterpret_cast<const sl::Resource*>(resource_bytes)->state, extent, scale);
      }
    }
    // Optical motion: HSF tags its own depth and flow at present instead.
    if (OpticalActive()) continue;
    tag.lifecycle = sl::eOnlyValidNow;
    // slSetTagForFrame answers eErrorInvalidIntegration unless the game set eUseFrameBasedResourceTagging at slInit, which is not ours to change, so such a game gets slSetTag, as it uses itself.
    sl::Result result = sl::Result::eErrorInvalidIntegration;
    if (!g_tag_without_frame.load(std::memory_order_relaxed) && g_real_tag_for_frame != nullptr) result = g_real_tag_for_frame(frame, viewport, &tag, 1, cmd);
    if (result == sl::Result::eErrorInvalidIntegration && g_real_set_tag != nullptr) {
      if (!g_tag_without_frame.exchange(true, std::memory_order_relaxed)) {
        Lock lock;
        Line("this game does not tag by frame: HSF uses slSetTag");
      }
      result = g_real_set_tag(viewport, &tag, 1, cmd);
    }
    if (result == sl::Result::eOk) {
      g_tagged.fetch_add(1, std::memory_order_relaxed);
      g_tagged_period.fetch_add(1, std::memory_order_relaxed);
    } else if (g_tag_failures.fetch_add(1, std::memory_order_relaxed) < 20) {
      Lock lock;
      Line("HSF's tag of %s returned %d", BufferName(tag.type), static_cast<int>(result));
    }
  }
}

sl::Result HookEvaluate(sl::Feature feature, const sl::FrameToken& frame, const sl::BaseStructure** inputs, uint32_t count, sl::CommandBuffer* cmd) {
  InFlight in_flight;
  g_last_frame.store(&frame, std::memory_order_relaxed);
  {
    Lock lock;
    const uint32_t token = SafeToken(&frame);
    SeeToken(token);
    FeatureSeen* seen = nullptr;
    for (FeatureSeen& f : g_features) {
      if (f.used && f.feature == feature) {
        seen = &f;
        break;
      }
      if (!f.used) {
        f = {};
        f.used = true;
        f.feature = feature;
        seen = &f;
        break;
      }
    }
    bool log_call = g_tracing;
    if (seen != nullptr) {
      ++seen->count;
      ++seen->count_period;
      if (seen->lines < 10) {
        ++seen->lines;
        log_call = true;
      }
    }
    if (log_call) Line("slEvaluateFeature %s(%u) token %u inputs %u cmd %d", FeatureName(feature), feature, token, count, cmd != nullptr ? 1 : 0);
    // Tags can also come in with the evaluate call instead of through slSetTag.
    uint32_t vp = UINT32_MAX;
    for (uint32_t i = 0; i < count && inputs != nullptr; ++i) {
      const sl::BaseStructure* input = nullptr;
      if (!SafeCopy(&input, &inputs[i], sizeof(input)) || input == nullptr) continue;
      alignas(sl::BaseStructure) unsigned char base_bytes[sizeof(sl::BaseStructure)];
      if (!SafeCopy(base_bytes, input, sizeof(base_bytes))) continue;
      const sl::BaseStructure& base = *reinterpret_cast<const sl::BaseStructure*>(base_bytes);
      if (base.structType == sl::ViewportHandle::s_structType) {
        vp = SafeViewport(static_cast<const sl::ViewportHandle*>(input));
        if (log_call) Line("  viewport %u", vp);
      } else if (base.structType == sl::ResourceTag::s_structType) {
        SeeTag(KIND_EVALUATE, vp, static_cast<const sl::ResourceTag*>(input), cmd != nullptr);
      } else if (log_call) {
        LogGuid("  input", base.structType, base.structVersion);
      }
    }
    Summary();
  }
  if (g_drive.load(std::memory_order_relaxed)) {
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last = g_last_evaluate.exchange(now, std::memory_order_relaxed);
    if (now - last <= EVALUATE_GONE_MS) {
      const float gap = g_evaluate_gap_ms.load(std::memory_order_relaxed);
      g_evaluate_gap_ms.store(gap + (static_cast<float>(now - last) - gap) * 0.1f, std::memory_order_relaxed);
    } else {
      g_evaluate_streak.store(0, std::memory_order_relaxed);
      static std::atomic<uint32_t> s_lines{0};
      if (last != 0 && s_lines.fetch_add(1, std::memory_order_relaxed) < 500) {
        Lock lock;
        Line("evaluates back after %llu ms without any", now - last);
      }
    }
    g_evaluate_streak.fetch_add(1, std::memory_order_relaxed);
    TagForGeneration(frame, inputs, count, cmd);
  }
  const sl::Result result = g_real_evaluate(feature, frame, inputs, count, cmd);
  Lock lock;
  NoteResult("slEvaluateFeature", result);
  return result;
}

sl::Result HookGetToken(sl::FrameToken*& token, const uint32_t* frame_index) {
  InFlight in_flight;
  const sl::Result result = g_real_get_token(token, frame_index);
  Lock lock;
  if (result == sl::Result::eOk && token != nullptr) SeeToken(SafeToken(token));
  if (g_tracing) Line("slGetNewFrameToken -> %u%s", token != nullptr ? SafeToken(token) : UINT32_MAX, frame_index != nullptr ? " (game gave the index)" : "");
  NoteResult("slGetNewFrameToken", result);
  Summary();
  return result;
}

uint32_t g_dlss_lines = 0, g_dlssg_lines = 0;
sl::DLSSMode g_last_dlss_mode = sl::DLSSMode::eCount;
sl::DLSSGMode g_last_dlssg_mode = sl::DLSSGMode::eCount;

sl::Result HookDlssOptions(const sl::ViewportHandle& viewport, const sl::DLSSOptions& options) {
  InFlight in_flight;
  {
    Lock lock;
    alignas(sl::DLSSOptions) unsigned char bytes[sizeof(sl::DLSSOptions)];
    if (SafeCopy(bytes, &options, sizeof(bytes))) {
      const sl::DLSSOptions& o = *reinterpret_cast<const sl::DLSSOptions*>(bytes);
      // Shadows also sends mode 0 with an output of 4294967295x4294967295, which is no size.
      if (o.outputWidth != 0 && o.outputHeight != 0 && o.outputWidth <= 16384 && o.outputHeight <= 16384) {
        g_output_width.store(o.outputWidth, std::memory_order_relaxed);
        g_output_height.store(o.outputHeight, std::memory_order_relaxed);
      }
      if ((o.mode != g_last_dlss_mode || g_dlss_lines < 3) && g_dlss_lines < 60) {
        ++g_dlss_lines;
        g_last_dlss_mode = o.mode;
        Line("slDLSSSetOptions vp%u mode %u (1 perf, 2 balanced, 3 quality, 4 ultra perf, 5 ultra quality, 6 DLAA) output %ux%u hdr %d struct v%zu", SafeViewport(&viewport), static_cast<uint32_t>(o.mode), o.outputWidth,
             o.outputHeight, static_cast<int>(o.colorBuffersHDR), o.structVersion);
      }
    }
  }
  return g_real_dlss_options(viewport, options);
}

sl::Result HookDlssgOptions(const sl::ViewportHandle& viewport, const sl::DLSSGOptions& options) {
  InFlight in_flight;
  alignas(16) unsigned char bytes[OPTIONS_BYTES] = {};
  // Much more than the struct, in case it is newer than the SDK's. If that runs off the end of the stack, the SDK's size.
  const bool have = SafeCopy(bytes, &options, OPTIONS_BYTES) || SafeCopy(bytes, &options, sizeof(sl::DLSSGOptions));
  {
    Lock lock;
    if (have) {
      const sl::DLSSGOptions& o = *reinterpret_cast<const sl::DLSSGOptions*>(bytes);
      if ((o.mode != g_last_dlssg_mode || g_dlssg_lines < 3) && g_dlssg_lines < 60) {
        ++g_dlssg_lines;
        g_last_dlssg_mode = o.mode;
        Line("game's slDLSSGSetOptions vp%u mode %u (0 off, 1 on, 2 auto, 3 dynamic) frames %u flags 0x%x dynamic %ux%u backbuffers %u mvec %ux%u color %ux%u formats %u %u %u %u %u struct v%zu", SafeViewport(&viewport),
             static_cast<uint32_t>(o.mode), o.numFramesToGenerate, static_cast<uint32_t>(o.flags), o.dynamicResWidth, o.dynamicResHeight, o.numBackBuffers, o.mvecDepthWidth, o.mvecDepthHeight, o.colorWidth, o.colorHeight,
             o.colorBufferFormat, o.mvecBufferFormat, o.depthBufferFormat, o.hudLessBufferFormat, o.uiBufferFormat, o.structVersion);
      }
    }
  }
  if (!g_drive.load(std::memory_order_relaxed) || !have) return g_real_dlssg_options(viewport, options);
  // The game's call never changes what HSF has chosen: the game may believe FG is off.
  OptionsLock options_lock;
  std::memcpy(g_game_options, bytes, OPTIONS_BYTES);
  g_have_game_options = true;
  g_viewport.store(SafeViewport(&viewport), std::memory_order_relaxed);
  sl::DLSSGOptions& ours = *reinterpret_cast<sl::DLSSGOptions*>(bytes);
  ApplyChoices(ours, g_generating.load(std::memory_order_relaxed), std::max(g_frames_sent.load(std::memory_order_relaxed), 1u));
  return g_real_dlssg_options(viewport, ours);
}

// DLSS-G refuses to run without Reflex, so a game with Reflex set to off gets low latency while HSF drives.
// The game's last Reflex options as it sent them, so HSF can send them again with its own cap and boost when those change. A game may send them only once, at
// start, before HSF has read its settings (every Shadows log shows one call), and then HSF's cap and boost never reached Reflex.
alignas(sl::ReflexOptions) unsigned char g_game_reflex[sizeof(sl::ReflexOptions)] = {};
bool g_have_game_reflex = false;  // under g_lock
// What the last call to Reflex carried, from HSF's side: the cap, boost and frames shown per real frame. Poll re-sends when one changes.
std::atomic<uint32_t> g_reflex_sent_cap{UINT32_MAX}, g_reflex_sent_shown{0};
std::atomic<int> g_reflex_sent_boost{-1};

sl::Result HookReflexOptions(const sl::ReflexOptions& options) {
  InFlight in_flight;
  alignas(sl::ReflexOptions) unsigned char bytes[sizeof(sl::ReflexOptions)];
  if (!SafeCopy(bytes, &options, sizeof(bytes))) return g_real_reflex_options(options);
  {
    // ResendReflex passes a copy of these same bytes back in, so storing them again changes nothing.
    Lock lock;
    std::memcpy(g_game_reflex, bytes, sizeof(bytes));
    g_have_game_reflex = true;
  }
  sl::ReflexOptions& o = *reinterpret_cast<sl::ReflexOptions*>(bytes);
  const bool drive = g_drive.load(std::memory_order_relaxed);
  const uint32_t cap = g_cap_fps.load(std::memory_order_relaxed);
  const bool boost = g_reflex_boost.load(std::memory_order_relaxed);
  const uint32_t shown_now = g_generating.load(std::memory_order_relaxed) ? g_frames_sent.load(std::memory_order_relaxed) + 1 : 1;
  g_reflex_sent_cap.store(cap, std::memory_order_relaxed);
  g_reflex_sent_boost.store(boost ? 1 : 0, std::memory_order_relaxed);
  g_reflex_sent_shown.store(shown_now, std::memory_order_relaxed);
  const bool force = drive && (o.mode == sl::ReflexMode::eOff || cap != 0 || (boost && o.mode != sl::ReflexMode::eLowLatencyWithBoost));
  {
    // The game sends this every frame.
    static uint32_t s_last_mode = UINT32_MAX, s_last_limit = UINT32_MAX;
    Lock lock;
    if (static_cast<uint32_t>(o.mode) != s_last_mode || o.frameLimitUs != s_last_limit) {
      s_last_mode = static_cast<uint32_t>(o.mode);
      s_last_limit = o.frameLimitUs;
      Line("game's slReflexSetOptions mode %u (0 off, 1 low latency, 2 boost) frame limit %u us struct v%zu%s", s_last_mode, s_last_limit, o.structVersion, force ? ", HSF sends low latency instead" : "");
    }
  }
  if (!force) return g_real_reflex_options(options);
  if (boost) o.mode = sl::ReflexMode::eLowLatencyWithBoost;
  else if (o.mode == sl::ReflexMode::eOff) o.mode = sl::ReflexMode::eLowLatency;
  // The cap is on screen, and with DLSS-G on Reflex's limit already counts the frames shown, so it goes as it is. Multiplying it by the frames shown per real frame
  // (as HSF did) limited twice: Shadows at a 155 cap and 6x made 4.5 real frames a second, 26 on screen.
  if (cap != 0) o.frameLimitUs = static_cast<uint32_t>(1000000ull / cap);
  return g_real_reflex_options(o);
}

// With its FG setting off, a game can unload DLSS-G's plugin before making its swap chain, and then that swap chain is not Streamline's and DLSS-G can never present. So while HSF drives, the
// plugin stays loaded, and the game's mode off is all that "off" means.
sl::Result HookSetFeatureLoaded(sl::Feature feature, bool loaded) {
  InFlight in_flight;
  const bool keep = g_drive.load(std::memory_order_relaxed) && feature == sl::kFeatureDLSS_G && !loaded;
  {
    Lock lock;
    Line("game's slSetFeatureLoaded %s(%u) %s%s", FeatureName(feature), feature, loaded ? "true" : "false", keep ? ", HSF keeps it loaded" : "");
  }
  return g_real_set_loaded(feature, keep ? true : loaded);
}

// ---- Patching ----

struct Hook {
  const char* name;
  void* hook;
  void** real;
  uint8_t* target;
  uint8_t saved[8];
  uint8_t patch[8];
  bool installed;
  uint8_t* relay;       // in a code page that outlives HSF: an absolute jump to the hook
  uint8_t* trampoline;  // the moved instructions, then a jump back into the target
  bool adopted;         // took over the relay from an earlier load of HSF, with someone else's jump on top
};

sl::Result HookGetFunction(sl::Feature feature, const char* name, void*& function);

Hook g_hooks[] = {
    {"slSetTag", reinterpret_cast<void*>(&HookSetTag), reinterpret_cast<void**>(&g_real_set_tag)},
    {"slSetTagForFrame", reinterpret_cast<void*>(&HookTagForFrame), reinterpret_cast<void**>(&g_real_tag_for_frame)},
    {"slSetConstants", reinterpret_cast<void*>(&HookConstants), reinterpret_cast<void**>(&g_real_constants)},
    {"slEvaluateFeature", reinterpret_cast<void*>(&HookEvaluate), reinterpret_cast<void**>(&g_real_evaluate)},
    {"slGetNewFrameToken", reinterpret_cast<void*>(&HookGetToken), reinterpret_cast<void**>(&g_real_get_token)},
    {"slGetFeatureFunction", reinterpret_cast<void*>(&HookGetFunction), reinterpret_cast<void**>(&g_real_get_function)},
    {"slSetFeatureLoaded", reinterpret_cast<void*>(&HookSetFeatureLoaded), reinterpret_cast<void**>(&g_real_set_loaded)},
};
// Found through slGetFeatureFunction, in other modules.
Hook g_feature_hooks[] = {
    {"slDLSSSetOptions", reinterpret_cast<void*>(&HookDlssOptions), reinterpret_cast<void**>(&g_real_dlss_options)},
    {"slDLSSGSetOptions", reinterpret_cast<void*>(&HookDlssgOptions), reinterpret_cast<void**>(&g_real_dlssg_options)},
    {"slReflexSetOptions", reinterpret_cast<void*>(&HookReflexOptions), reinterpret_cast<void**>(&g_real_reflex_options)},
};

// One relay and one trampoline for each hook, in a page within 2 GB of the patched code, as the patch is a 5-byte relative jump.
constexpr size_t SLOT_BYTES = 64;
struct CodePage {
  uint8_t* base;
  size_t used;
};
CodePage g_pages[8];

uint8_t* AllocateNear(const uint8_t* near_address) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const uintptr_t step = info.dwAllocationGranularity;
  const uintptr_t origin = reinterpret_cast<uintptr_t>(near_address) & ~(step - 1);
  constexpr uintptr_t RANGE = 0x70000000;
  for (uintptr_t distance = step; distance < RANGE; distance += step) {
    for (const uintptr_t address : {origin + distance, origin > distance ? origin - distance : uintptr_t{0}}) {
      if (address == 0) continue;
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_FREE) continue;
      void* const page = VirtualAlloc(reinterpret_cast<void*>(address), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (page != nullptr) return static_cast<uint8_t*>(page);
    }
  }
  return nullptr;
}

// A slot in a page near target. Pages stay for good: a thread can still be inside a trampoline when hooks come off, and add-on reloads are few.
uint8_t* NearSlot(const uint8_t* target) {
  for (CodePage& page : g_pages) {
    if (page.base == nullptr) {
      page.base = AllocateNear(target);
      if (page.base == nullptr) return nullptr;
    }
    const intptr_t distance = page.base - target;
    if (distance > -0x7FF00000LL && distance < 0x7FF00000LL && page.used + SLOT_BYTES <= 4096) {
      uint8_t* const slot = page.base + page.used;
      page.used += SLOT_BYTES;
      return slot;
    }
  }
  return nullptr;
}

void WriteAbsoluteJump(uint8_t* at, const void* to) {
  at[0] = 0xFF;
  at[1] = 0x25;
  std::memset(at + 2, 0, 4);
  std::memcpy(at + 6, &to, sizeof(to));
}

// Code still mapped at this address. A module that has been unloaded leaves free memory behind.
bool CodeStillThere(const void* address) {
  MEMORY_BASIC_INFORMATION mbi;
  return VirtualQuery(address, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// All 8 bytes in one write, so a thread entering the function sees the old start or the new one, never half of each.
bool WriteFirstBytes(uint8_t* target, const uint8_t bytes[8]) {
  DWORD old = 0;
  if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
  LONG64 value;
  std::memcpy(&value, bytes, 8);
  _InterlockedExchange64(reinterpret_cast<volatile LONG64*>(target), value);
  VirtualProtect(target, 8, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, 8);
  return true;
}

// ReShade loads its add-ons, unloads them and loads them again. Another add-on that hooks the same function after HSF (RenoDX DLSS does, in Witcher 3) copies HSF's jump into its own
// trampoline, so after the unload its calls still go through HSF's relay, into wherever HSF's code was. The relay and trampoline sit in a code page that stays, and the process environment
// survives the reload, so each hook leaves there where its relay is. Remove points the relay at the trampoline; the next load finds it and points it at the new hook again.
void RelayName(const Hook& hook, wchar_t* name, size_t size) { std::swprintf(name, size, L"HSF_HOOK_%hs", hook.name); }

void PublishRelay(const Hook& hook) {
  wchar_t name[64], value[96];
  RelayName(hook, name, 64);
  std::swprintf(value, 96, L"%p %p %p", static_cast<void*>(hook.target), static_cast<void*>(hook.relay), static_cast<void*>(hook.trampoline));
  SetEnvironmentVariableW(name, value);
}

// The 8-byte address in the relay's absolute jump, written in one go.
void PointRelay(uint8_t* relay, const void* to) {
  LONG64 value;
  std::memcpy(&value, &to, 8);
  _InterlockedExchange64(reinterpret_cast<volatile LONG64*>(relay + 6), value);
  FlushInstructionCache(GetCurrentProcess(), relay, 16);
}

// target starts with someone else's jump. If an earlier load of HSF hooked it and left its relay, take that relay back: their jump still leads through it.
bool Adopt(Hook& hook, uint8_t* target) {
  wchar_t name[64], value[96] = L"";
  RelayName(hook, name, 64);
  if (GetEnvironmentVariableW(name, value, 96) == 0) return false;
  void *old_target = nullptr, *relay = nullptr, *trampoline = nullptr;
  if (swscanf_s(value, L"%p %p %p", &old_target, &relay, &trampoline) != 3 || old_target != target) return false;
  uint8_t head[6];
  if (!CodeStillThere(relay) || !CodeStillThere(trampoline) || !SafeCopy(head, relay, sizeof(head))) return false;
  if (head[0] != 0xFF || head[1] != 0x25 || head[2] != 0 || head[3] != 0 || head[4] != 0 || head[5] != 0) return false;
  hook.target = target;
  hook.relay = static_cast<uint8_t*>(relay);
  hook.trampoline = static_cast<uint8_t*>(trampoline);
  *hook.real = trampoline;  // ready before any call can arrive
  std::atomic_thread_fence(std::memory_order_seq_cst);
  PointRelay(hook.relay, hook.hook);
  hook.installed = true;
  hook.adopted = true;
  Line("hook %s taken back at %p: someone else's jump is on top, and it leads through HSF's relay from the last load", hook.name, target);
  return true;
}

// Call with the lock held. A function already hooked by someone else starts with a jump. If that jump leads through HSF's own relay (an earlier load of HSF, under someone
// else's hook), the relay is taken back. Otherwise HSF hooks on top: the jump is moved into the trampoline, so HSF runs first and then their hook (RenoDX DLSS on NGX in Jusant).
bool Install(Hook& hook, uint8_t* target) {
  if (hook.installed && hook.target == target) return true;
  char bytes_text[64] = "";
  uint8_t first[32] = {};
  if (!SafeCopy(first, target, sizeof(first))) return false;
  if (first[0] == 0xE9 && Adopt(hook, target)) return true;
  for (int i = 0; i < 12; ++i) std::snprintf(bytes_text + i * 3, 4, "%02x ", first[i]);
  size_t rip_at[MAX_RIP_LOADS];
  size_t rip_count = 0;
  const size_t length = PrologueLength(first, 5, rip_at, &rip_count);
  if (length == 0 || length > 16 || (reinterpret_cast<uintptr_t>(target) & 7) != 0) {
    if (Adopt(hook, target)) return true;
    Line("hook %s NOT installed at %p: starts %s(left alone)", hook.name, target, bytes_text);
    return false;
  }
  uint8_t* const slot = NearSlot(target);
  if (slot == nullptr) {
    Line("hook %s NOT installed: no memory near %p", hook.name, target);
    return false;
  }
  // [relay: jump to the hook][trampoline: the moved instructions, jump back]
  uint8_t* const trampoline = slot + 16;
  WriteAbsoluteJump(slot, hook.hook);
  std::memcpy(trampoline, first, length);
  // A moved load relative to rip must still reach the same address. The page is near the target and what it loads usually is too, but check.
  for (size_t i = 0; i < rip_count; ++i) {
    const bool is_jump = first[rip_at[i]] == 0xE9;
    const size_t DISP_AT = is_jump ? 1 : 3, INSTRUCTION_BYTES = is_jump ? 5 : 7;
    int32_t displacement;
    std::memcpy(&displacement, first + rip_at[i] + DISP_AT, sizeof(displacement));
    const uint8_t* const address = target + rip_at[i] + INSTRUCTION_BYTES + displacement;
    const intptr_t moved = address - (trampoline + rip_at[i] + INSTRUCTION_BYTES);
    if (moved > INT32_MAX || moved < INT32_MIN) {
      Line("hook %s NOT installed at %p: its load cannot reach from the trampoline. Starts %s", hook.name, target, bytes_text);
      return false;
    }
    const int32_t new_displacement = static_cast<int32_t>(moved);
    std::memcpy(trampoline + rip_at[i] + DISP_AT, &new_displacement, sizeof(new_displacement));
  }
  WriteAbsoluteJump(trampoline + length, target + length);
  FlushInstructionCache(GetCurrentProcess(), slot, SLOT_BYTES);

  std::memcpy(hook.saved, first, 8);
  std::memcpy(hook.patch, hook.saved, 8);
  hook.patch[0] = 0xE9;
  const int32_t relative = static_cast<int32_t>(slot - (target + 5));
  std::memcpy(hook.patch + 1, &relative, 4);
  *hook.real = trampoline;  // ready before any call can arrive
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!WriteFirstBytes(target, hook.patch)) {
    Line("hook %s NOT installed: could not write at %p", hook.name, target);
    return false;
  }
  hook.target = target;
  hook.relay = slot;
  hook.trampoline = trampoline;
  hook.adopted = false;
  hook.installed = true;
  PublishRelay(hook);
  Line("hook %s installed at %p, moved %zu bytes, %zu loads relative to rip: %s", hook.name, target, length, rip_count, bytes_text);
  return true;
}

void Remove(Hook& hook) {
  if (!hook.installed) return;
  hook.installed = false;
  if (!CodeStillThere(hook.target)) return;
  uint8_t now[8];
  if (!SafeCopy(now, hook.target, 8)) return;
  // Someone patched on top of us (RenoDX DLSS, for one): leave their jump in place rather than cut it out, and point HSF's relay straight at the trampoline, so their calls reach the real
  // function and never code of HSF's that is about to go. The next load of HSF takes the relay back (Adopt).
  if (hook.adopted || std::memcmp(now, hook.patch, 5) != 0) {
    if (hook.relay != nullptr && hook.trampoline != nullptr && CodeStillThere(hook.relay)) PointRelay(hook.relay, hook.trampoline);
    Line("hook %s changed by someone else, not restored; its calls now skip HSF", hook.name);
    return;
  }
  WriteFirstBytes(hook.target, hook.saved);
  // Nothing leads through the relay now, so the next load must not take it back.
  wchar_t name[64];
  RelayName(hook, name, 64);
  SetEnvironmentVariableW(name, nullptr);
}

void InstallFeatureHook(const char* name, void* function) {
  for (Hook& hook : g_feature_hooks)
    if (std::strcmp(hook.name, name) == 0 && function != nullptr) Install(hook, static_cast<uint8_t*>(function));
}

sl::Result HookGetFunction(sl::Feature feature, const char* name, void*& function) {
  InFlight in_flight;
  const sl::Result result = g_real_get_function(feature, name, function);
  Lock lock;
  Line("slGetFeatureFunction %s(%u) %s -> %p result %d", FeatureName(feature), feature, name != nullptr ? name : "(null)", result == sl::Result::eOk ? function : nullptr, static_cast<int>(result));
  if (result == sl::Result::eOk && name != nullptr) InstallFeatureHook(name, function);
  return result;
}

// ---- Keeping up with the interposer ----

HMODULE g_interposer = nullptr;
std::atomic<bool> g_old_streamline{false};  // the game's Streamline is 1.x: HSF does nothing with it
bool g_started = false;
bool g_asked_features = false;
ULONGLONG g_next_poll = 0;

void HookInterposer(HMODULE module) {
  wchar_t path[MAX_PATH] = L"?";
  GetModuleFileNameW(module, path, MAX_PATH);
  Line("sl.interposer.dll at %p: %ls", module, path);
  // Streamline 1.x (Returnal, 2023) has no slGetFeatureFunction, and its slSetTag, slSetConstants and slEvaluateFeature take different arguments from 2.x. HSF's hooks read
  // them as 2.x and its own tags are sent as 2.x, which crashed Returnal at start. So HSF leaves a 1.x Streamline alone: no hooks, no tags, the game's own FG as it was.
  if (GetProcAddress(module, "slGetFeatureFunction") == nullptr) {
    g_old_streamline.store(true, std::memory_order_relaxed);
    Line("Streamline 1.x (no slGetFeatureFunction): HSF leaves the game's Streamline alone, no hooks");
    return;
  }
  g_is_loaded = reinterpret_cast<IsFeatureLoadedFn>(GetProcAddress(module, "slIsFeatureLoaded"));
  for (Hook& hook : g_hooks) {
    void* const function = reinterpret_cast<void*>(GetProcAddress(module, hook.name));
    if (function == nullptr) {
      Line("export %s not found", hook.name);
      continue;
    }
    Install(hook, static_cast<uint8_t*>(function));
  }
}

template <typename F>
void ForEachHook(F&& f) {
  for (Hook& hook : g_hooks) f(hook);
  for (Hook& hook : g_feature_hooks) f(hook);
}

}  // namespace

namespace {
Hook g_other_hooks[8];
}  // namespace

bool HookFunction(const char* name, void* target, void* hook_function, void** real, void (*log)(const char* text)) {
  Lock lock;
  for (Hook& hook : g_other_hooks) {
    if (hook.name != nullptr) continue;
    hook.name = name;
    hook.hook = hook_function;
    hook.real = real;
    g_line_sink = log != nullptr ? log : [](const char*) {};
    const bool installed = Install(hook, static_cast<uint8_t*>(target));
    g_line_sink = nullptr;
    if (!installed) hook = Hook{};
    return installed;
  }
  return false;
}

void UnhookFunctions() {
  Lock lock;
  g_line_sink = [](const char*) {};
  for (Hook& hook : g_other_hooks) {
    Remove(hook);
    hook = Hook{};
  }
  g_line_sink = nullptr;
}

size_t PrologueLength(const uint8_t* code, size_t min_bytes, size_t* rip_at, size_t* rip_count) {
  size_t at = 0;
  size_t rips = 0;
  while (at < min_bytes) {
    const uint8_t* const p = code + at;
    size_t length = 0;
    if (p[0] >= 0x50 && p[0] <= 0x57) {
      length = 1;  // push r
    } else if ((p[0] == 0x40 || p[0] == 0x41) && p[1] >= 0x50 && p[1] <= 0x57) {
      length = 2;  // push r, with a prefix
    } else if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x44 && p[3] == 0x24) {
      length = 5;  // mov [rsp+disp8], r
    } else if ((p[0] == 0x48 || p[0] == 0x4C || p[0] == 0x49 || p[0] == 0x4D) && (p[1] == 0x8B || p[1] == 0x89) && p[2] >= 0xC0) {
      length = 3;  // mov r, r
    } else if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) {
      length = 4;  // sub rsp, imm8
    } else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) {
      length = 7;  // sub rsp, imm32
    } else if ((p[0] == 0x48 || p[0] == 0x4C) && (p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC7) == 0x05) {
      if (rips == MAX_RIP_LOADS) return 0;
      if (rip_at != nullptr) rip_at[rips] = at;
      ++rips;
      length = 7;  // mov r, [rip+disp32] or lea r, [rip+disp32]
    } else if (p[0] == 0x33 && p[1] >= 0xC0) {
      length = 2;  // xor r, r
    } else if (p[0] == 0xE9) {
      // jmp rel32, as a small forwarding function ends: slReflexSetOptions is xor edx, edx then this. At the very start it is someone else's hook, and HSF goes on top of it.
      if (rips == MAX_RIP_LOADS) return 0;
      if (rip_at != nullptr) rip_at[rips] = at;
      ++rips;
      at += 5;
      break;
    } else {
      return 0;
    }
    at += length;
  }
  if (at < min_bytes) return 0;  // ended by a jump before there was room for the patch
  if (rip_count != nullptr) *rip_count = rips;
  return at;
}

int HooksInstalled() {
  Lock lock;
  int count = 0;
  ForEachHook([&](Hook& hook) { count += hook.installed ? 1 : 0; });
  return count;
}

void SetCutCheck(bool on) {
  if (g_cut_check.exchange(on, std::memory_order_relaxed) == on) return;
  Lock lock;
  Line("HSF's cut check from the camera %s", on ? "on" : "off");
}

void SetQuickOff(bool on) {
  if (g_quick_off.exchange(on, std::memory_order_relaxed) == on) return;
  Lock lock;
  Line("quick off when vectors stop: %s", on ? "on" : "off");
}

void SetScoreGuard(bool on) {
  if (g_score_guard.exchange(on, std::memory_order_relaxed) == on) return;
  Lock lock;
  Line("score guard: %s", on ? "on, generation stops while the vectors are doubted" : "off, the score only logs");
}

void SetVectorsHook(VectorsHook hook) { g_vectors_hook.store(hook, std::memory_order_release); }

void NoteScore(float warp, float none, float better, float worse, float moving, int kind) {
  Lock lock;
  if (kind == 1) {
    ++g_score_still;
    return;
  }
  if (kind == 2) {
    ++g_score_cuts;
    return;
  }
  g_score_pairs[g_score_written++ % SCORE_WINDOW] = {warp, none, better, worse, moving};
  const uint32_t pairs = std::min(g_score_written, SCORE_WINDOW);
  float sum_warp = 0.0f, sum_none = 0.0f, sum_moving = 0.0f;
  for (uint32_t i = 0; i < pairs; ++i) {
    sum_warp += g_score_pairs[i].warp;
    sum_none += g_score_pairs[i].none;
    sum_moving += g_score_pairs[i].moving;
  }
  const float gain = sum_none > 0.0f ? 1.0f - sum_warp / sum_none : 0.0f;
  g_score_gain.store(gain);
  // A ratio of two tiny errors means little: only a full window with plenty moving can change the answer.
  if (pairs < SCORE_WINDOW || sum_moving / pairs < SCORE_MIN_MOVING) return;
  const bool doubted = g_vectors_doubted.load();
  if (!doubted && gain < DOUBT_GAIN) {
    g_vectors_doubted.store(true);
    Line("score: the game's vectors do worse than no motion, gain %.2f over %u pairs. %s", gain, pairs, g_score_guard.load() ? "Generation stops." : "Guard is off, so only logged.");
  } else if (doubted && gain > TRUST_GAIN) {
    g_vectors_doubted.store(false);
    Line("score: the game's vectors help again, gain %.2f", gain);
  }
}

void NotePictureCut(float changed, float shape) {
  Lock lock;
  const bool near_reset = g_last_reset_frame != 0 && g_frames - g_last_reset_frame <= PICTURE_CUT_FRAMES;
  g_picture_cuts.fetch_add(1);
  if (!near_reset) g_picture_cuts_no_reset.fetch_add(1);
  if (g_picture_cuts.load() <= 300) Line("picture cut at frame %llu, %.2f changed, shape %.2f, %s", g_frames, changed, shape, near_reset ? "with a reset" : "with NO reset near it");
}

void NoteChecksProblem(const char* problem) {
  Lock lock;
  Line("checks on the game's device stopped: %s", problem);
}

void SetCameraRebuild(bool on) {
  if (g_camera_rebuild.exchange(on, std::memory_order_relaxed) == on) return;
  Lock lock;
  Line("DLSS-G now gets %s camera matrices", on ? "HSF's rebuilt" : "the game's");
}

bool RebuildClipToPrev(const float clip_to_view_cur[4][4], const CameraPose& cur, const float view_to_clip_prev[4][4], const CameraPose& prev, float out[4][4]) {
  // Doubles: world positions are thousands and the moves small.
  double axes[2][3][3];
  double position[2][3];
  for (int which = 0; which < 2; ++which) {
    const CameraPose& pose = which == 0 ? cur : prev;
    for (int i = 0; i < 3; ++i) {
      axes[which][0][i] = pose.right[i];
      axes[which][1][i] = pose.up[i];
      axes[which][2][i] = -static_cast<double>(pose.fwd[i]);
      position[which][i] = pose.pos[i];
    }
    double (*const a)[3] = axes[which];
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) {
        const double dot = a[i][0] * a[j][0] + a[i][1] * a[j][1] + a[i][2] * a[j][2];
        if (!(std::abs(dot - (i == j ? 1.0 : 0.0)) < 0.01)) return false;
      }
    const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    if (!(det > 0.9)) return false;
    for (int i = 0; i < 3; ++i)
      if (!std::isfinite(position[which][i])) return false;
  }
  // viewToViewPrev, row vectors: view to world with the current axes and position, then world to view with the previous ones.
  double move[4][4] = {};
  move[3][3] = 1.0;
  for (int j = 0; j < 3; ++j) {
    for (int i = 0; i < 3; ++i)
      for (int k = 0; k < 3; ++k) move[i][j] += axes[0][i][k] * axes[1][j][k];
    for (int k = 0; k < 3; ++k) move[3][j] += (position[0][k] - position[1][k]) * axes[1][j][k];
  }
  double step[4][4] = {};
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      for (int k = 0; k < 4; ++k) step[i][j] += static_cast<double>(clip_to_view_cur[i][k]) * move[k][j];
  float result[4][4];
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) {
      double value = 0.0;
      for (int k = 0; k < 4; ++k) value += step[i][k] * static_cast<double>(view_to_clip_prev[k][j]);
      if (!std::isfinite(value)) return false;
      result[i][j] = static_cast<float>(value);
    }
  std::memcpy(out, result, sizeof(result));
  return true;
}

void Start(const wchar_t* log_path, bool drive, bool dynamic_resolution_known) {
  Lock lock;
  if (g_started) return;
  g_started = true;
  g_asked_features = false;
  g_generating.store(false);
  g_have_options.store(false);
  g_evaluate_streak.store(0);
  g_last_evaluate.store(0);
  g_last_game_vectors.store(0);
  g_game_vectors_streak.store(0);
  g_last_frame.store(nullptr);
  g_tag_without_frame.store(false);
  g_dynamic.store(dynamic_resolution_known);
  g_have_camera_now = g_have_camera_before = false;
  g_camera_rebuilt_period = g_camera_refused = 0;
  g_camera_move_rebuilt = g_camera_move_game = 0.0f;
  g_cut_token = UINT32_MAX;
  g_cut_answer = false;
  g_camera_speed = 0.0f;
  g_last_reset_frame = 0;
  g_score_written = g_score_still = g_score_cuts = 0;
  g_score_gain.store(0.0f);
  g_vectors_doubted.store(false);
  g_cuts_by_hsf.store(0);
  g_picture_cuts.store(0);
  g_picture_cuts_no_reset.store(0);
  g_evaluate_gap_ms.store(0.0f);
  g_vectors_hook.store(nullptr);
  g_mv_width.store(0);
  g_mv_height.store(0);
  g_output_width.store(0);
  g_output_height.store(0);
  g_drive.store(drive);
  {
    OptionsLock options_lock;
    g_have_game_options = false;
    g_direct_dlssg_options = nullptr;
    g_get_dlssg_state = nullptr;
  }
  // Appends: ReShade reloads add-ons during startup, and each load must not wipe the one before.
  _wfopen_s(&g_file, log_path, L"ab");
  if (g_file != nullptr) std::setvbuf(g_file, nullptr, _IOFBF, 8192);
  wchar_t exe[MAX_PATH] = L"?";
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  Line("=== HSF started in %ls. %s ===", exe, drive ? "Driving DLSS-G inside the game." : "Only logging: every call goes on unchanged.");
  g_interposer = GetModuleHandleW(L"sl.interposer.dll");
  if (g_interposer != nullptr) HookInterposer(g_interposer);
  else Line("sl.interposer.dll not loaded yet");
}

// The lock is not reentrant, so it comes off around the call into Streamline. Call with it held.
static void* AskFor(sl::Feature feature, const char* name) {
  void* function = nullptr;
  ReleaseSRWLockExclusive(&g_lock);
  const sl::Result result = g_real_get_function(feature, name, function);
  AcquireSRWLockExclusive(&g_lock);
  if (result != sl::Result::eOk) function = nullptr;
  if (function != nullptr) InstallFeatureHook(name, function);
  Line("asked Streamline for %s: %p result %d", name, function, static_cast<int>(result));
  return function;
}

static const Hook& FeatureHook(const char* name) {
  for (const Hook& hook : g_feature_hooks)
    if (std::strcmp(hook.name, name) == 0) return hook;
  return g_feature_hooks[0];
}

// Sends HSF's mode to DLSS-G when it has to change. On the present thread, without g_lock.
static void DriveFrame() {
  const ULONGLONG now = GetTickCount64();
  ULONGLONG gone_ms = EVALUATE_GONE_MS;
  if (g_quick_off.load(std::memory_order_relaxed)) gone_ms = std::min<ULONGLONG>(gone_ms, static_cast<ULONGLONG>(std::max(g_evaluate_gap_ms.load(std::memory_order_relaxed) * QUICK_OFF_GAPS, QUICK_OFF_LEAST_MS)));
  // DLSS-G starting up stalls the game's frames for a moment, which quick off took for missing vectors: on, off and on again within 90 ms, after which Crimson Desert lost its
  // device. The stall there reached 281 ms, so for the first second after HSF turns generation on no gap turns it off.
  static ULONGLONG s_on_at = 0;
  if (g_generating.load(std::memory_order_relaxed) && now - s_on_at < ON_SETTLE_MS) gone_ms = ON_SETTLE_MS;
  // With optical motion DLSS-G gets HSF's flow and depth, so the game need not run DLSS at all: with TAA (AC Shadows) there are no evaluates, and
  // waiting for them held generation off although the game sent its constants and HSF tagged every frame.
  const bool data_flowing = OpticalActive() || GameDataFlowing(now, gone_ms);
  const bool allowed = g_allowed.load(std::memory_order_relaxed);
  // The score judges the game's vectors, which optical motion does not use.
  const bool doubted = g_score_guard.load(std::memory_order_relaxed) && g_vectors_doubted.load(std::memory_order_relaxed) && !OpticalActive();
  const bool focus_hold = g_focus_hold.load(std::memory_order_relaxed);
  bool on = allowed && !focus_hold && data_flowing && !doubted;
  uint32_t frames = std::max(g_want_frames.load(std::memory_order_relaxed), 1u);
  const uint32_t max_frames = g_max_frames.load(std::memory_order_relaxed);
  if (max_frames != 0) frames = std::min(frames, max_frames);
  static bool s_dynamic_sent = false;
  static uint32_t s_output_sent = 0;
  const bool dynamic = g_dynamic.load(std::memory_order_relaxed);
  const uint32_t output = g_output_width.load(std::memory_order_relaxed);
  if (on == g_generating.load(std::memory_order_relaxed) && (!on || frames == g_frames_sent.load(std::memory_order_relaxed)) && (!on || (dynamic == s_dynamic_sent && (!dynamic || output == s_output_sent)))) return;
  char why[64] = "";
  if (!allowed) std::snprintf(why, sizeof(why), ", frame generation is off in HSF");
  else if (focus_hold) std::snprintf(why, sizeof(why), ", the game's window is not in front");
  else if (!data_flowing) {
    const ULONGLONG gone = now - LastGameData();
    if (gone > gone_ms) std::snprintf(why, sizeof(why), ", no vectors for %llu ms", gone);
    else std::snprintf(why, sizeof(why), ", vectors just came back");
  } else if (doubted) std::snprintf(why, sizeof(why), ", the score doubts the vectors");
  // A new frame count while DLSS-G generates has left it on and generating nothing (seen in the test harness). So off for one frame, then on with the new count from the next call.
  if (on && g_generating.load(std::memory_order_relaxed) && frames != g_frames_sent.load(std::memory_order_relaxed)) {
    on = false;
    frames = g_frames_sent.load(std::memory_order_relaxed);
    std::snprintf(why, sizeof(why), ", for a new frame count");
  }

  sl::Result result = sl::Result::eOk;
  uint32_t optimal_width = 0, optimal_height = 0;
  {
    OptionsLock options_lock;
    const SetDlssgOptionsFn send = FeatureHook("slDLSSGSetOptions").installed ? g_real_dlssg_options : g_direct_dlssg_options;
    if (send == nullptr) return;
    alignas(16) unsigned char bytes[OPTIONS_BYTES] = {};
    if (g_have_game_options) std::memcpy(bytes, g_game_options, OPTIONS_BYTES);
    else new (bytes) sl::DLSSGOptions{};
    sl::DLSSGOptions& options = *reinterpret_cast<sl::DLSSGOptions*>(bytes);
    ApplyChoices(options, on, frames);
    const sl::ViewportHandle viewport(g_viewport.load(std::memory_order_relaxed));
    if (on && !g_generating.load(std::memory_order_relaxed)) s_on_at = now;
    g_generating.store(on, std::memory_order_relaxed);
    g_frames_sent.store(frames, std::memory_order_relaxed);
    s_dynamic_sent = dynamic;
    s_output_sent = output;
    optimal_width = options.dynamicResWidth;
    optimal_height = options.dynamicResHeight;
    result = send(viewport, options);
  }
  Lock lock;
  Line("HSF sets DLSS-G %s, %u generated per real frame, dynamic resolution %d, optimal %ux%u%s: result %d", on ? "ON" : "off", frames, dynamic ? 1 : 0, optimal_width, optimal_height, why, static_cast<int>(result));
}

// HSF's cap, boost or the frames shown per real frame changed since Reflex last heard: send the game's own options again, through the hook, which adds HSF's.
void ResendReflex() {
  const uint32_t cap = g_cap_fps.load(std::memory_order_relaxed);
  const int boost = g_reflex_boost.load(std::memory_order_relaxed) ? 1 : 0;
  const uint32_t shown = g_generating.load(std::memory_order_relaxed) ? g_frames_sent.load(std::memory_order_relaxed) + 1 : 1;
  if (cap == g_reflex_sent_cap.load(std::memory_order_relaxed) && boost == g_reflex_sent_boost.load(std::memory_order_relaxed)) return;
  alignas(sl::ReflexOptions) unsigned char copy[sizeof(sl::ReflexOptions)];
  {
    Lock lock;
    if (!g_have_game_reflex || g_real_reflex_options == nullptr || !FeatureHook("slReflexSetOptions").installed) return;
    std::memcpy(copy, g_game_reflex, sizeof(copy));
  }
  const sl::Result result = HookReflexOptions(*reinterpret_cast<const sl::ReflexOptions*>(copy));
  static uint32_t s_lines = 0;
  if (s_lines < 40) {
    ++s_lines;
    Line("HSF sent Reflex again: cap %u on screen, boost %d, %u shown per real frame, result %d", cap, boost, shown, static_cast<int>(result));
  }
}

void Poll() {
  if (g_drive.load(std::memory_order_relaxed) && !g_old_streamline.load(std::memory_order_relaxed)) {
    DriveFrame();
    ResendReflex();
  }
  const ULONGLONG now = GetTickCount64();
  // Only this thread's frame loop calls Poll, so the rate limit needs no lock.
  if (now < g_next_poll) return;
  g_next_poll = now + 1000;
  {
    Lock lock;
    if (!g_started) return;
    Summary();
    const HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
    if (module != g_interposer) {
      Line("sl.interposer.dll changed: %p to %p", g_interposer, module);
      ForEachHook([](Hook& hook) { hook.installed = false; });
      g_interposer = module;
      g_asked_features = false;
      g_have_options.store(false);
      {
        OptionsLock options_lock;
        g_direct_dlssg_options = nullptr;
        g_get_dlssg_state = nullptr;
      }
      if (module != nullptr) HookInterposer(module);
    }
    // HSF may have arrived after the game asked for these, so ask too. Only reads a pointer. Waits until frames run, as Streamline must be up, and tries again now and then until DLSS-G answers.
    static ULONGLONG s_next_ask = 0;
    if (!g_asked_features && g_frames > 30 && g_real_get_function != nullptr && now >= s_next_ask) {
      s_next_ask = now + 10000;
      void* const options = AskFor(sl::kFeatureDLSS_G, "slDLSSGSetOptions");
      void* const state = AskFor(sl::kFeatureDLSS_G, "slDLSSGGetState");
      AskFor(sl::kFeatureDLSS, "slDLSSSetOptions");
      AskFor(sl::kFeatureReflex, "slReflexSetOptions");
      g_asked_features = options != nullptr;
      g_have_options.store(options != nullptr);
      OptionsLock options_lock;
      g_direct_dlssg_options = reinterpret_cast<SetDlssgOptionsFn>(options);
      g_get_dlssg_state = reinterpret_cast<GetDlssgStateFn>(state);
    }
  }
  if (g_is_loaded != nullptr && g_frames > 30) {
    bool loaded = false;
    const sl::Result is_loaded = g_is_loaded(sl::kFeatureDLSS_G, loaded);
    const int now_loaded = is_loaded == sl::Result::eOk ? (loaded ? 1 : 0) : -1;
    if (g_feature_loaded.exchange(now_loaded) != now_loaded) {
      Lock lock;
      Line("DLSS-G's plugin loaded: %d, result %d. Not loaded when the swap chain was made means no generation, whatever the mode says.", now_loaded, static_cast<int>(is_loaded));
    }
  }
  if (!g_drive.load(std::memory_order_relaxed)) return;
  // Once a second: how DLSS-G says it is doing. Its guide puts this call on the present thread, which this is.
  sl::DLSSGState state{};
  sl::Result result = sl::Result::eOk;
  {
    OptionsLock options_lock;
    if (g_get_dlssg_state == nullptr) return;
    result = g_get_dlssg_state(sl::ViewportHandle(g_viewport.load(std::memory_order_relaxed)), state, nullptr);
  }
  constexpr uint32_t NOT_YET = 0xFFFFFFFE;
  static uint32_t s_last_status = UINT32_MAX;
  Lock lock;
  if (result != sl::Result::eOk) {
    if (s_last_status != NOT_YET) Line("slDLSSGGetState returned %d", static_cast<int>(result));
    s_last_status = NOT_YET;
    return;
  }
  const uint32_t status = static_cast<uint32_t>(state.status);
  if (status != s_last_status || state.numFramesToGenerateMax != g_max_frames.load()) {
    Line("DLSS-G says status 0x%x (0 fine, 1 resolution too low, 2 no Reflex, 4 HDR format, 8 constants invalid, 0x10 back buffer index not asked) max frames %u min size %u dynamic MFG %d", status, state.numFramesToGenerateMax,
         state.minWidthOrHeight, state.bIsDynamicMFGSupported == sl::Boolean::eTrue ? 1 : 0);
    s_last_status = status;
  }
  g_status.store(status);
  g_max_frames.store(state.numFramesToGenerateMax);
  // Real frames count for this as well, so 3 means 3x.
  if (g_presented_last.exchange(state.numFramesActuallyPresented) != state.numFramesActuallyPresented) {
    static uint32_t s_lines = 0;
    if (s_lines < 200) {
      ++s_lines;
      Line("DLSS-G now shows %u per real frame; HSF last asked for %u generated", state.numFramesActuallyPresented, g_frames_sent.load());
    }
  }
  g_presented_period.fetch_add(state.numFramesActuallyPresented);
}

void SetGeneration(bool allowed, uint32_t frames) {
  g_allowed.store(allowed, std::memory_order_relaxed);
  g_want_frames.store(frames, std::memory_order_relaxed);
}

void SetFocusHold(bool hold) { g_focus_hold.store(hold, std::memory_order_relaxed); }

namespace {
// As TagForGeneration: by frame where the game allows it, or else as the game tags.
sl::Result SendHudlessTag(sl::ResourceTag& tag, sl::CommandBuffer* cmd) {
  const sl::ViewportHandle viewport(g_viewport.load(std::memory_order_relaxed));
  const sl::FrameToken* const frame = g_last_frame.load(std::memory_order_relaxed);
  sl::Result result = sl::Result::eErrorInvalidIntegration;
  if (!g_tag_without_frame.load(std::memory_order_relaxed) && g_real_tag_for_frame != nullptr && frame != nullptr) result = g_real_tag_for_frame(*frame, viewport, &tag, 1, cmd);
  if (result == sl::Result::eErrorInvalidIntegration && g_real_set_tag != nullptr) result = g_real_set_tag(viewport, &tag, 1, cmd);
  return result;
}
void NoteHudlessResult(sl::Result result) {
  if (result == sl::Result::eOk) return;
  static std::atomic<uint32_t> s_lines{0};
  if (s_lines.fetch_add(1, std::memory_order_relaxed) < 20) {
    Lock lock;
    Line("HSF's tag of HUDLessColor returned %d", static_cast<int>(result));
  }
}
}  // namespace

void SetOptical(bool on) {
  if (g_optical.exchange(on, std::memory_order_relaxed) != on) {
    Lock lock;
    Line("motion for DLSS-G: %s", on ? "optical (HSF's flow and depth)" : "the game's vectors and depth");
  }
}

void SetReflex(uint32_t fps_cap, bool boost) {
  g_cap_fps.store(fps_cap, std::memory_order_relaxed);
  g_reflex_boost.store(boost, std::memory_order_relaxed);
}

bool TagOptical(void* command_list, void* depth, uint32_t depth_state, void* motion, uint32_t motion_state, const uint32_t extent[4], const float scale[2], bool dilated) {
  InFlight in_flight;
  if (command_list == nullptr || depth == nullptr || motion == nullptr || !g_optical.load(std::memory_order_relaxed) || g_old_streamline.load(std::memory_order_relaxed)) return false;
  g_optical_scale_x.store(scale[0], std::memory_order_relaxed);
  g_optical_scale_y.store(scale[1], std::memory_order_relaxed);
  g_optical_dilated.store(dilated, std::memory_order_relaxed);
  if (g_optical_last.exchange(GetTickCount64(), std::memory_order_relaxed) == 0) {
    Lock lock;
    Line("optical motion: first tags, %ux%u", extent[2], extent[3]);
  }
  sl::Extent ext{};
  ext.left = extent[0];
  ext.top = extent[1];
  ext.width = extent[2];
  ext.height = extent[3];
  sl::Resource depth_resource{};
  depth_resource.type = sl::ResourceType::eTex2d;
  depth_resource.native = depth;
  depth_resource.state = depth_state;
  sl::Resource motion_resource{};
  motion_resource.type = sl::ResourceType::eTex2d;
  motion_resource.native = motion;
  motion_resource.state = motion_state;
  sl::ResourceTag tags[] = {sl::ResourceTag(&depth_resource, sl::kBufferTypeDepth, sl::eOnlyValidNow, &ext), sl::ResourceTag(&motion_resource, sl::kBufferTypeMotionVectors, sl::eOnlyValidNow, &ext)};
  bool ok = true;
  // One tag per call, as in TagForGeneration.
  for (sl::ResourceTag& tag : tags) {
    const sl::Result result = SendHudlessTag(tag, static_cast<sl::CommandBuffer*>(command_list));
    if (result == sl::Result::eOk) {
      g_tagged.fetch_add(1, std::memory_order_relaxed);
      g_tagged_period.fetch_add(1, std::memory_order_relaxed);
    } else {
      ok = false;
      if (g_tag_failures.fetch_add(1, std::memory_order_relaxed) < 20) {
        Lock lock;
        Line("HSF's optical tag of %s returned %d", BufferName(tag.type), static_cast<int>(result));
      }
    }
  }
  return ok;
}

bool TagHudless(void* command_list, void* back_buffer, uint32_t width, uint32_t height) {
  InFlight in_flight;
  if (command_list == nullptr || back_buffer == nullptr || !g_generating.load(std::memory_order_relaxed)) return false;
  sl::Resource resource{};
  resource.type = sl::ResourceType::eTex2d;
  resource.native = back_buffer;
  resource.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  sl::Extent extent{};
  extent.width = width;
  extent.height = height;
  sl::ResourceTag tag(&resource, sl::kBufferTypeHUDLessColor, sl::eOnlyValidNow, &extent);
  const sl::Result result = SendHudlessTag(tag, static_cast<sl::CommandBuffer*>(command_list));
  NoteHudlessResult(result);
  return result == sl::Result::eOk;
}

void ClearHudless() {
  InFlight in_flight;
  sl::ResourceTag tag(nullptr, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent);
  NoteHudlessResult(SendHudlessTag(tag, nullptr));
}

void LogLine(const char* text) {
  Lock lock;
  Line("%s", text);
}

Stats GetStats() {
  Stats stats{};
  stats.generating = g_generating.load();
  stats.have_options = g_have_options.load();
  stats.frames = g_frames_sent.load();
  stats.max_frames = g_max_frames.load();
  stats.status = g_status.load();
  stats.presented = g_presented_last.load();
  stats.tagged = g_tagged.load();
  stats.dynamic_resolution = g_dynamic.load();
  stats.plugin_loaded = g_feature_loaded.load();
  stats.data_flowing = GameDataFlowing(GetTickCount64(), EVALUATE_GONE_MS);
  stats.cuts_by_hsf = g_cuts_by_hsf.load();
  stats.picture_cuts = g_picture_cuts.load();
  stats.picture_cuts_no_reset = g_picture_cuts_no_reset.load();
  stats.score_gain = g_score_gain.load();
  {
    Lock lock;
    stats.score_pairs = std::min(g_score_written, SCORE_WINDOW);
  }
  stats.vectors_doubted = g_vectors_doubted.load();
  stats.old_streamline = g_old_streamline.load();
  return stats;
}

uint64_t GameFrames() { return g_frames_public.load(std::memory_order_relaxed); }

bool HookFeatureFunctionForTest(const char* name, void* function) {
  Lock lock;
  InstallFeatureHook(name, function);
  return FeatureHook(name).installed;
}

void Stop(bool process_exiting) {
  // Generation goes off with the hooks, as nothing would tag vectors and depth any more. Only when an unload happens in a running game; under the loader lock, but this call only stores options.
  if (!process_exiting && g_drive.load() && g_generating.load()) {
    OptionsLock options_lock;
    const Hook& hook = FeatureHook("slDLSSGSetOptions");
    if (hook.installed && CodeStillThere(hook.target)) {
      alignas(16) unsigned char bytes[OPTIONS_BYTES] = {};
      if (g_have_game_options) std::memcpy(bytes, g_game_options, OPTIONS_BYTES);
      else new (bytes) sl::DLSSGOptions{};
      sl::DLSSGOptions& options = *reinterpret_cast<sl::DLSSGOptions*>(bytes);
      ApplyChoices(options, false, std::max(g_frames_sent.load(), 1u));
      g_real_dlssg_options(sl::ViewportHandle(g_viewport.load()), options);
    }
    g_generating.store(false);
  }
  g_drive.store(false);
  g_vectors_hook.store(nullptr, std::memory_order_release);
  {
    Lock lock;
    if (!g_started) return;
    if (process_exiting) {
      Line("=== process exiting ===");
    } else {
      ForEachHook([](Hook& hook) { Remove(hook); });
      Line("=== stopped: add-on unloading, game's bytes put back ===");
    }
    g_started = false;
  }
  if (!process_exiting) {
    Sleep(5);  // a thread that took the old jump just before the bytes went back reaches its counter by now
    for (int i = 0; i < 200 && g_in_flight.load(std::memory_order_acquire) != 0; ++i) Sleep(1);
  }
  Lock lock;
  if (g_file != nullptr) {
    std::fclose(g_file);
    g_file = nullptr;
  }
}

}  // namespace hsf::slgame
