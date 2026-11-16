// See sl_game.h. Hooks are written here, in HSF's own code: a 5-byte jump at the start of each function goes to a relay in a page HSF allocates near the module, which
// jumps to the hook; the hook calls a trampoline holding the moved prologue, which jumps back. Anything unexpected at a function's start means no hook there, only a log line.
#include "sl_game.h"

#include "hook_patch.h"
#include "safe_try.h"

#include <windows.h>
#include <psapi.h>
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
#include <cwctype>
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

void Line(const char* format, ...) {
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
// The game's DLSS data through NGX goes the optical way too, with its own jitter and reset (SetOpticalJitter). All zero for flow.
std::atomic<float> g_optical_jitter_x{0.0f}, g_optical_jitter_y{0.0f};
std::atomic<bool> g_optical_jittered{false}, g_optical_reset{false};
constexpr ULONGLONG OPTICAL_GONE_MS = 300;
std::atomic<bool> g_flow_fill{false};
constexpr ULONGLONG FLOW_FILL_MS = 3000;  // with flow fill on, generation stops only when neither the game's data nor HSF's flow came for this long
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
// When the game last sent its own slSetConstants. With its DLSS off a game may send none at all (PBZ, Unreal), and DLSS-G has no frame without them: optical motion then sends HSF's own.
std::atomic<ULONGLONG> g_last_game_constants{0};
// The game's own slDLSSGSetOptions asks for frame generation. While it does not, the game's Reflex frame limit is meant for its real frames.
std::atomic<bool> g_game_fg_on{false};
// The newest frame token each thread asked for. Unreal asks on its game thread for the frame it simulates and on its render thread for the frame it renders, one apart
// (PBZ: 1143 render, 1144 game, 1143 render again). With no evaluates and no tags of the game's, the frame being presented is the render thread's, the lowest recent one.
struct TokenAsked {
  DWORD thread;
  const sl::FrameToken* token;
  uint32_t index;
  ULONGLONG at;
};
constexpr size_t TOKEN_THREADS = 4;
TokenAsked g_tokens_asked[TOKEN_THREADS] = {};  // under g_lock
// The lowest is the first guess. If DLSS-G shows nothing with it, the state poll switches to the highest, and back (an RHI thread a frame behind would make the lowest wrong).
std::atomic<bool> g_token_pick_highest{false};
// When HSF last sent its own constants and Streamline took them.
std::atomic<ULONGLONG> g_own_constants_last{0};

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
  g_last_game_constants.store(GetTickCount64(), std::memory_order_relaxed);
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
        // Optical motion is the picture's own, so it has its scale, no jitter, and is dilated when the compose pass has run. The game's DLSS data through NGX
        // goes the same way and brings its own jitter and reset.
        if (OpticalActive()) {
          fixed.mvecScale = {g_optical_scale_x.load(std::memory_order_relaxed), g_optical_scale_y.load(std::memory_order_relaxed)};
          fixed.jitterOffset = {g_optical_jitter_x.load(std::memory_order_relaxed), g_optical_jitter_y.load(std::memory_order_relaxed)};
          fixed.motionVectorsJittered = g_optical_jittered.load(std::memory_order_relaxed) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
          if (g_optical_reset.exchange(false, std::memory_order_relaxed)) fixed.reset = sl::Boolean::eTrue;
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

// A game that tags by frame (eUseFrameBasedResourceTagging) answers slSetTag with eErrorInvalidIntegration, so HSF's own tags need a frame token. With the game's
// DLSS off it neither evaluates nor tags (PBZ), and the tokens it asks for here are the only ones there are. Evaluates and the game's own tags still come first.
// Call with g_lock held.
void NoteTokenAsked(const sl::FrameToken* token, uint32_t index) {
  if (index == UINT32_MAX) return;
  const ULONGLONG now = GetTickCount64();
  const DWORD thread = GetCurrentThreadId();
  TokenAsked* slot = nullptr;
  TokenAsked* oldest = &g_tokens_asked[0];
  for (TokenAsked& t : g_tokens_asked) {
    if (t.thread == thread) {
      slot = &t;
      break;
    }
    if (t.at < oldest->at) oldest = &t;
  }
  if (slot == nullptr) slot = oldest;
  *slot = {thread, token, index, now};
  if (now - g_last_evaluate.load(std::memory_order_relaxed) <= EVALUATE_GONE_MS || now - g_last_game_vectors.load(std::memory_order_relaxed) <= EVALUATE_GONE_MS) return;
  const bool highest = g_token_pick_highest.load(std::memory_order_relaxed);
  const TokenAsked* pick = nullptr;
  for (const TokenAsked& t : g_tokens_asked)
    if (t.token != nullptr && now - t.at <= EVALUATE_GONE_MS && (pick == nullptr || (highest ? t.index > pick->index : t.index < pick->index))) pick = &t;
  if (pick == nullptr) return;
  if (g_last_frame.exchange(pick->token, std::memory_order_relaxed) == nullptr) {
    static uint32_t s_lines = 0;
    if (s_lines++ < 10) Line("no evaluates and no tags from the game: HSF's tags go to the %s frame it asked for, %u (thread %lu)", highest ? "highest" : "lowest", pick->index, pick->thread);
  }
}

sl::Result HookGetToken(sl::FrameToken*& token, const uint32_t* frame_index) {
  InFlight in_flight;
  const sl::Result result = g_real_get_token(token, frame_index);
  Lock lock;
  if (result == sl::Result::eOk && token != nullptr) {
    const uint32_t index = SafeToken(token);
    SeeToken(index);
    NoteTokenAsked(token, index);
  }
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
      g_game_fg_on.store(o.mode != sl::DLSSGMode::eOff, std::memory_order_relaxed);
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
  // The game's own limit with its FG off is for real frames, but with DLSS-G on Reflex counts it on screen: PBZ's 60 fps limit held the screen at 60, so 2x gave
  // 30 real frames and 5x gave 12. Divided by the frames shown, the game keeps its own real rate. HSF's cap, when set, wins as before.
  const bool scale_game_limit = drive && cap == 0 && o.frameLimitUs != 0 && shown_now > 1 && !g_game_fg_on.load(std::memory_order_relaxed);
  const bool force = drive && (o.mode == sl::ReflexMode::eOff || cap != 0 || scale_game_limit || (boost && o.mode != sl::ReflexMode::eLowLatencyWithBoost));
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
  else if (scale_game_limit) {
    const uint32_t game_limit = o.frameLimitUs;
    o.frameLimitUs = game_limit / shown_now;
    static uint32_t s_last_game = 0, s_last_shown = 0, s_lines = 0;
    if ((game_limit != s_last_game || shown_now != s_last_shown) && s_lines < 40) {
      s_last_game = game_limit;
      s_last_shown = shown_now;
      ++s_lines;
      Lock lock;
      Line("the game's frame limit %u us is for its real frames: Reflex gets %u us, for %u shown per real frame", game_limit, o.frameLimitUs, shown_now);
    }
  }
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
// The patcher itself is in hook_patch.cpp. sl_game's own hooks are installed and removed under g_lock and log to this file.

using hookpatch::CodeStillThere;
using hookpatch::Hook;
using hookpatch::NearSlot;
using hookpatch::PointRelay;
using hookpatch::WriteAbsoluteJump;

void HookLog(const char* text) { Line("%s", text); }
bool Install(Hook& hook, uint8_t* target) { return hookpatch::Install(hook, target, &HookLog); }
void Remove(Hook& hook) { hookpatch::Remove(hook, &HookLog); }

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

// H1: what the plugin hook hands out in place of slDLSSGSetOptions, a jump to HookDlssgOptions in a code page that outlives HSF (see HandOutOptions). Under g_lock.
uint8_t* g_options_stub = nullptr;
std::atomic<bool> g_plugin_route{false};  // the game's slDLSSGSetOptions calls come through HSF's stub, with no patch on DLSS-G's code

// The Steam overlay hooks DLSS-G's functions itself. Handed HSF's stub, it would patch HSF's code, which goes away when ReShade unloads HSF.
bool FromSteamOverlay(const void* address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module)) return false;
  wchar_t path[MAX_PATH] = L"";
  GetModuleFileNameW(module, path, MAX_PATH);
  const wchar_t* const slash = std::wcsrchr(path, L'\\');
  return _wcsicmp(slash != nullptr ? slash + 1 : path, L"gameoverlayrenderer64.dll") == 0;
}

void InstallFeatureHook(const char* name, void* function) {
  if (function != nullptr && function == g_options_stub) return;  // HSF's own, from the plugin hook: nothing to patch
  // Names the file DLSS-G's options call lives in, once per address: the plugin hook looks for sl.dlss_g.dll, and a game that loads DLSS-G under another name shows here.
  static void* s_named_options = nullptr;
  if (function != nullptr && function != s_named_options && std::strcmp(name, "slDLSSGSetOptions") == 0) {
    s_named_options = function;
    HMODULE owner = nullptr;
    wchar_t path[MAX_PATH] = L"(no module)";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(function), &owner)) GetModuleFileNameW(owner, path, MAX_PATH);
    Line("slDLSSGSetOptions %p lives in %p: %ls", function, static_cast<void*>(owner), path);
  }
  for (Hook& hook : g_feature_hooks)
    if (std::strcmp(hook.name, name) == 0 && function != nullptr) Install(hook, static_cast<uint8_t*>(function));
}

sl::Result HookGetFunction(sl::Feature feature, const char* name, void*& function) {
  InFlight in_flight;
  const sl::Result result = g_real_get_function(feature, name, function);
  Lock lock;
  if (result == sl::Result::eOk && function != nullptr && function == g_options_stub && g_real_dlssg_options != nullptr && FromSteamOverlay(_ReturnAddress())) {
    function = reinterpret_cast<void*>(g_real_dlssg_options);
    Line("the Steam overlay asked for slDLSSGSetOptions: it gets DLSS-G's own, not HSF's");
  }
  Line("slGetFeatureFunction %s(%u) %s -> %p result %d%s", FeatureName(feature), feature, name != nullptr ? name : "(null)", result == sl::Result::eOk ? function : nullptr, static_cast<int>(result),
       result == sl::Result::eOk && function != nullptr && function == g_options_stub ? " (HSF's, from the plugin hook)" : "");
  if (result == sl::Result::eOk && name != nullptr) InstallFeatureHook(name, function);
  return result;
}

// ---- Keeping up with the interposer ----

HMODULE g_interposer = nullptr;
std::atomic<bool> g_old_streamline{false};  // the game's Streamline is 1.x: HSF does nothing with it
bool g_started = false;
bool g_asked_features = false;
ULONGLONG g_next_poll = 0;
ULONGLONG g_next_ask = 0;  // when to ask Streamline for the feature functions again

// The game's own sl.interposer.dll. HSF's own copy, from its HSF_streamline folder (the bring route), is not the game's: hooking it sent HSF's own tags back through these
// hooks and failed with eErrorInvalidIntegration thousands of times (Townfall, Rise of the Tomb Raider). Both can be loaded at once, so every loaded module is checked.
bool IsHsfStreamline(HMODULE module) {
  wchar_t path[MAX_PATH] = L"";
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0) return false;
  for (wchar_t* c = path; *c != 0; ++c) *c = static_cast<wchar_t>(towlower(*c));
  return std::wcsstr(path, L"\\hsf_streamline\\") != nullptr;
}

bool g_only_hsf_streamline = false;  // the only sl.interposer.dll loaded is HSF's own
HMODULE g_picked = nullptr;           // a copy picked in the menu, used while it stays loaded

// Every sl.interposer.dll loaded, HSF's own too.
int AllInterposers(HMODULE* out, int max) {
  HMODULE modules[1024];
  DWORD bytes = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return 0;
  int count = 0;
  for (DWORD i = 0; i < std::min<DWORD>(bytes / sizeof(HMODULE), 1024) && count < max; ++i) {
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleFileNameW(modules[i], path, MAX_PATH) == 0) continue;
    const wchar_t* const slash = std::wcsrchr(path, L'\\');
    if (_wcsicmp(slash != nullptr ? slash + 1 : path, L"sl.interposer.dll") == 0) out[count++] = modules[i];
  }
  return count;
}

HMODULE FindGameInterposer() {
  HMODULE all[8];
  const int count = AllInterposers(all, 8);
  g_only_hsf_streamline = false;
  if (g_picked != nullptr) {
    for (int i = 0; i < count; ++i)
      if (all[i] == g_picked) return g_picked;
    g_picked = nullptr;
  }
  const HMODULE first = GetModuleHandleW(L"sl.interposer.dll");
  if (first == nullptr || !IsHsfStreamline(first)) return first;
  for (int i = 0; i < count; ++i)
    if (!IsHsfStreamline(all[i])) return all[i];
  g_only_hsf_streamline = true;
  static bool s_said = false;
  if (!s_said) {
    s_said = true;
    Line("sl.interposer.dll loaded is HSF's own (HSF_streamline), not the game's: left to the bring route, no hooks here");
  }
  return nullptr;
}

// The file version from the module's own version resource. Not through version.dll: in a D3D12 game version.dll may be another tool's stand-in (MFG-Unlocker loads as one),
// and this also runs under the loader lock.
bool ModuleVersion(HMODULE module, uint32_t out[4]) {
  const HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(16));  // RT_VERSION
  if (found == nullptr) return false;
  const HGLOBAL loaded = LoadResource(module, found);
  const DWORD size = SizeofResource(module, found);
  const auto* const bytes = static_cast<const uint8_t*>(loaded != nullptr ? LockResource(loaded) : nullptr);
  if (bytes == nullptr) return false;
  for (DWORD at = 0; at + sizeof(VS_FIXEDFILEINFO) <= size; at += 4) {
    VS_FIXEDFILEINFO info;
    std::memcpy(&info, bytes + at, sizeof(info));
    if (info.dwSignature != 0xFEEF04BD) continue;
    out[0] = HIWORD(info.dwFileVersionMS);
    out[1] = LOWORD(info.dwFileVersionMS);
    out[2] = HIWORD(info.dwFileVersionLS);
    out[3] = LOWORD(info.dwFileVersionLS);
    return true;
  }
  return false;
}

void HookInterposer(HMODULE module) {
  wchar_t path[MAX_PATH] = L"?";
  GetModuleFileNameW(module, path, MAX_PATH);
  uint32_t version[4] = {};
  const bool have_version = ModuleVersion(module, version);
  if (have_version) Line("sl.interposer.dll at %p, version %u.%u.%u.%u: %ls", module, version[0], version[1], version[2], version[3], path);
  else Line("sl.interposer.dll at %p, no version in the file: %ls", module, path);
  // Streamline 1.x (Returnal, 2023) has no slGetFeatureFunction, and its slSetTag, slSetConstants and slEvaluateFeature take different arguments from 2.x. HSF's hooks read
  // them as 2.x and its own tags are sent as 2.x, which crashed Returnal at start. So HSF leaves a 1.x Streamline alone: no hooks, no tags, the game's own FG as it was.
  // The file's version says so first; a missing slGetFeatureFunction says so too.
  const bool old_by_version = have_version && version[0] == 1;
  if (old_by_version || GetProcAddress(module, "slGetFeatureFunction") == nullptr) {
    g_old_streamline.store(true, std::memory_order_relaxed);
    Line("Streamline 1.x (%s): HSF leaves the game's Streamline alone, no hooks", old_by_version ? "by its file version" : "no slGetFeatureFunction");
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

// ---- DLSS-G at its plugin (H1, H4, H5) ----
// Streamline's interposer gets DLSS-G's functions from sl.dlss_g.dll's export slGetPluginFunction. HSF hooks that export (one hook per loaded copy) and, asked for
// slDLSSGSetOptions, hands back its own, so DLSS-G's code is never patched: that is where MFG-Unlocker and others put their jumps (PBZ: a far jump there blocked HSF's patch).
// If the game's slDLSSGSetOptions still comes back as DLSS-G's own (the interposer asked before HSF's hook was in), HSF patches it as before (InstallFeatureHook).
// What is handed out is a jump in a code page that outlives HSF: on unload it is pointed at DLSS-G's own function, and the next load takes it back (as with the relays).
using PluginFunctionFn = void* (*)(const char* name);
constexpr int PLUGIN_COPIES = 4;
PluginFunctionFn g_plugin_real[PLUGIN_COPIES] = {};
HMODULE g_plugin_module[PLUGIN_COPIES] = {};  // under g_lock
constexpr wchar_t STUB_VARIABLE[] = L"HSF_DLSSG_STUB";

void* PluginFunction(int copy, const char* name);
template <int N>
void* HookPluginFunction(const char* name) {
  return PluginFunction(N, name);
}
Hook g_plugin_hooks[PLUGIN_COPIES] = {
    {"slGetPluginFunction_dlssg0", reinterpret_cast<void*>(&HookPluginFunction<0>), reinterpret_cast<void**>(&g_plugin_real[0])},
    {"slGetPluginFunction_dlssg1", reinterpret_cast<void*>(&HookPluginFunction<1>), reinterpret_cast<void**>(&g_plugin_real[1])},
    {"slGetPluginFunction_dlssg2", reinterpret_cast<void*>(&HookPluginFunction<2>), reinterpret_cast<void**>(&g_plugin_real[2])},
    {"slGetPluginFunction_dlssg3", reinterpret_cast<void*>(&HookPluginFunction<3>), reinterpret_cast<void**>(&g_plugin_real[3])},
};

bool OptionsPatched() {
  for (const Hook& hook : g_feature_hooks)
    if (std::strcmp(hook.name, "slDLSSGSetOptions") == 0) return hook.installed;
  return false;
}

// Under g_lock. real is DLSS-G's slDLSSGSetOptions from one copy of the plugin.
void* HandOutOptions(void* real) {
  if (g_plugin_route.load() && g_options_stub != nullptr && reinterpret_cast<void*>(g_real_dlssg_options) == real) return g_options_stub;
  // One at a time: a second copy, or DLSS-G's code is already patched, keeps its own.
  if (g_plugin_route.load() || OptionsPatched()) return real;
  uint8_t* const stub = g_options_stub != nullptr ? g_options_stub : NearSlot(static_cast<uint8_t*>(real));
  if (stub == nullptr) return real;
  g_real_dlssg_options = reinterpret_cast<SetDlssgOptionsFn>(real);  // ready before any call can arrive
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (g_options_stub == nullptr) {
    WriteAbsoluteJump(stub, reinterpret_cast<const void*>(&HookDlssgOptions));
    FlushInstructionCache(GetCurrentProcess(), stub, 16);
  } else {
    PointRelay(stub, reinterpret_cast<const void*>(&HookDlssgOptions));
  }
  g_options_stub = stub;
  g_plugin_route.store(true);
  wchar_t value[64];
  std::swprintf(value, 64, L"%p %p", static_cast<void*>(stub), real);
  SetEnvironmentVariableW(STUB_VARIABLE, value);
  Line("DLSS-G's slDLSSGSetOptions (%p) handed to Streamline as HSF's (%p) through the plugin hook: DLSS-G's code is not patched", real, static_cast<void*>(stub));
  return stub;
}

void* PluginFunction(int copy, const char* name) {
  InFlight in_flight;
  const PluginFunctionFn real = g_plugin_real[copy];
  void* const function = real != nullptr ? real(name) : nullptr;
  if (function == nullptr || name == nullptr || std::strcmp(name, "slDLSSGSetOptions") != 0) return function;
  Lock lock;
  return HandOutOptions(function);
}

// An earlier load of HSF handed out the stub: Streamline still calls it, so point it at this load's hook.
void AdoptOptionsStub() {
  wchar_t value[64] = L"";
  if (GetEnvironmentVariableW(STUB_VARIABLE, value, 64) == 0) return;
  void *stub = nullptr, *real = nullptr;
  uint8_t head[6];
  if (swscanf_s(value, L"%p %p", &stub, &real) != 2 || !CodeStillThere(stub) || !CodeStillThere(real) || !SafeCopy(head, stub, sizeof(head))) return;
  if (head[0] != 0xFF || head[1] != 0x25 || head[2] != 0 || head[3] != 0 || head[4] != 0 || head[5] != 0) return;
  g_real_dlssg_options = reinterpret_cast<SetDlssgOptionsFn>(real);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  g_options_stub = static_cast<uint8_t*>(stub);
  PointRelay(g_options_stub, reinterpret_cast<const void*>(&HookDlssgOptions));
  g_plugin_route.store(true);
  Line("DLSS-G's slDLSSGSetOptions: HSF's stub from the last load (%p) taken back", stub);
}

// Unload: Streamline keeps calling the stub, which now goes straight to DLSS-G.
void ReleaseOptionsStub() {
  if (g_options_stub != nullptr && g_real_dlssg_options != nullptr && CodeStillThere(g_options_stub)) PointRelay(g_options_stub, reinterpret_cast<const void*>(g_real_dlssg_options));
  g_plugin_route.store(false);
}

bool IsHsfPath(const wchar_t* path) {
  wchar_t lower[MAX_PATH];
  wcsncpy_s(lower, path, _TRUNCATE);
  for (wchar_t* c = lower; *c != 0; ++c) *c = static_cast<wchar_t>(towlower(*c));
  return std::wcsstr(lower, L"\\hsf_streamline\\") != nullptr;
}

// DLSS-G's plugin: sl.dlss_g.dll, or a copy NVIDIA puts under its NGX models folder (the NVIDIA App's override, sl_dlss_g_override_0, and NVIDIA's own updates,
// sl_dlss_g_0), which Streamline loads under a numbered name such as 1B0_E658700.dll (Witcher 3).
bool IsNgxPluginPath(const wchar_t* path) {
  wchar_t lower[MAX_PATH];
  wcsncpy_s(lower, path, _TRUNCATE);
  for (wchar_t* c = lower; *c != 0; ++c) *c = static_cast<wchar_t>(towlower(*c));
  return std::wcsstr(lower, L"\\ngx\\models\\sl_dlss_g") != nullptr;
}

// HSF runs its own Streamline in this process (a game without one): an NGX copy of the plugin may then be HSF's own, so only the game's sl.dlss_g.dll is hooked.
bool HsfStreamlineLoaded() {
  HMODULE modules[1024];
  DWORD bytes = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return false;
  const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), 1024);
  for (DWORD m = 0; m < count; ++m) {
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleFileNameW(modules[m], path, MAX_PATH) == 0) continue;
    const wchar_t* const slash = std::wcsrchr(path, L'\\');
    if (_wcsicmp(slash != nullptr ? slash + 1 : path, L"sl.interposer.dll") == 0 && IsHsfPath(path)) return true;
  }
  return false;
}

// One loaded file is DLSS-G's plugin and the game's: sl.dlss_g.dll outside HSF's folder, or an NGX copy while HSF has no Streamline of its own here.
bool IsGamePlugin(const wchar_t* path) {
  const wchar_t* const slash = std::wcsrchr(path, L'\\');
  if (_wcsicmp(slash != nullptr ? slash + 1 : path, L"sl.dlss_g.dll") == 0) return !IsHsfPath(path);
  return IsNgxPluginPath(path) && !HsfStreamlineLoaded();
}

// Under g_lock. One copy of DLSS-G's plugin, not HSF's own.
void HookPluginCopy(HMODULE module) {
  if (!safetry::Allowed("H1_plugin")) return;
  int free_slot = -1;
  for (int i = 0; i < PLUGIN_COPIES; ++i) {
    if (g_plugin_module[i] == module) return;
    if (g_plugin_module[i] == nullptr && free_slot < 0) free_slot = i;
  }
  void* const function = reinterpret_cast<void*>(GetProcAddress(module, "slGetPluginFunction"));
  if (free_slot < 0 || function == nullptr) {
    static HMODULE s_said = nullptr;
    if (s_said == module) return;
    s_said = module;
    Line("DLSS-G's plugin at %p not hooked: %s", static_cast<void*>(module), free_slot < 0 ? "no free slot (4 copies already)" : "it has no slGetPluginFunction export");
    return;
  }
  g_plugin_module[free_slot] = module;
  safetry::Begin("H1_plugin");
  if (Install(g_plugin_hooks[free_slot], static_cast<uint8_t*>(function))) Line("DLSS-G's plugin at %p: its slGetPluginFunction is hooked (copy %d)", static_cast<void*>(module), free_slot);
}

// Under g_lock: every sl.dlss_g.dll loaded is hooked, and copies that went away are dropped (OTA updates can swap one).
void ScanPlugins() {
  HMODULE modules[1024];
  DWORD bytes = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return;
  const DWORD count = std::min<DWORD>(bytes / sizeof(HMODULE), 1024);
  for (int i = 0; i < PLUGIN_COPIES; ++i) {
    if (g_plugin_module[i] == nullptr) continue;
    bool still = false;
    for (DWORD m = 0; m < count && !still; ++m) still = modules[m] == g_plugin_module[i];
    if (still) continue;
    Line("DLSS-G's plugin copy %d (%p) was unloaded: dropped", i, static_cast<void*>(g_plugin_module[i]));
    if (g_plugin_route.load() && g_real_dlssg_options != nullptr && !CodeStillThere(reinterpret_cast<const void*>(g_real_dlssg_options))) g_plugin_route.store(false);
    g_plugin_module[i] = nullptr;
    g_plugin_real[i] = nullptr;
    g_plugin_hooks[i].installed = false;
    g_plugin_hooks[i].target = nullptr;
  }
  for (DWORD m = 0; m < count; ++m) {
    wchar_t path[MAX_PATH] = L"";
    if (GetModuleFileNameW(modules[m], path, MAX_PATH) == 0) continue;
    const wchar_t* const slash = std::wcsrchr(path, L'\\');
    const bool named_plugin = _wcsicmp(slash != nullptr ? slash + 1 : path, L"sl.dlss_g.dll") == 0;
    if (!named_plugin && !IsNgxPluginPath(path)) continue;
    // Each copy named once in the log.
    static HMODULE s_named[8] = {};
    bool known = false;
    for (HMODULE& named : s_named) {
      if (named == modules[m]) known = true;
      else if (named == nullptr && !known) {
        named = modules[m];
        break;
      }
    }
    const bool game_plugin = IsGamePlugin(path);
    if (!known) Line("DLSS-G's plugin loaded at %p: %ls%s", static_cast<void*>(modules[m]), path, game_plugin ? "" : " (may be HSF's own, left alone)");
    if (game_plugin) HookPluginCopy(modules[m]);
  }
}

// ---- Hooking at load time (H2) ----
// Windows tells HSF when a DLL loads (LdrRegisterDllNotification, no patching), so the game's Streamline and DLSS-G's plugin are hooked before their first call.
// The once-a-second look in Poll stays as the backup.
struct LdrString {
  USHORT length, maximum;
  PWSTR buffer;
};
struct LdrLoaded {
  ULONG flags;
  const LdrString* full_name;
  const LdrString* base_name;
  PVOID base;
  ULONG size;
};
using LdrCallback = VOID(CALLBACK*)(ULONG reason, const LdrLoaded* data, PVOID context);
using LdrRegisterFn = LONG(NTAPI*)(ULONG flags, LdrCallback callback, PVOID context, PVOID* cookie);
using LdrUnregisterFn = LONG(NTAPI*)(PVOID cookie);
PVOID g_load_cookie = nullptr;
std::atomic<bool> g_load_seen{false};  // a load the callback could not handle at once: the next Poll looks without waiting

bool NameIs(const LdrString* name, const wchar_t* want) {
  const size_t chars = std::wcslen(want);
  return name != nullptr && name->buffer != nullptr && name->length == chars * sizeof(wchar_t) && _wcsnicmp(name->buffer, want, chars) == 0;
}

VOID CALLBACK OnDllLoad(ULONG reason, const LdrLoaded* data, PVOID) {
  if (reason != 1 || data == nullptr) return;  // 1: loaded
  const bool interposer = NameIs(data->base_name, L"sl.interposer.dll");
  wchar_t path[MAX_PATH] = L"";
  if (data->full_name != nullptr && data->full_name->buffer != nullptr)
    wcsncpy_s(path, data->full_name->buffer, std::min<size_t>(data->full_name->length / sizeof(wchar_t), MAX_PATH - 1));
  const bool plugin = NameIs(data->base_name, L"sl.dlss_g.dll") || IsNgxPluginPath(path);
  if (!interposer && !plugin) return;
  // Under the loader lock. A thread holding g_lock may be waiting for the loader lock (GetModuleFileNameW takes it), so only try: if busy, the next Poll does it.
  if (!TryAcquireSRWLockExclusive(&g_lock)) {
    g_load_seen.store(true);
    return;
  }
  if (g_started && !IsHsfPath(path)) {
    const HMODULE module = static_cast<HMODULE>(data->base);
    if (interposer && g_interposer == nullptr && g_picked == nullptr) {
      Line("sl.interposer.dll is loading: hooked now (%ls)", path);
      g_interposer = module;
      HookInterposer(module);
    }
    if (plugin && IsGamePlugin(path)) HookPluginCopy(module);
  }
  ReleaseSRWLockExclusive(&g_lock);
}

// Under g_lock, from Start (DllMain): registering is allowed under the loader lock.
void WatchLoads() {
  if (g_load_cookie != nullptr || !safetry::Allowed("H2_load_notify")) return;
  const auto reg = reinterpret_cast<LdrRegisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification"));
  if (reg == nullptr) return;
  safetry::Begin("H2_load_notify");
  const LONG status = reg(0, &OnDllLoad, nullptr, &g_load_cookie);
  if (status != 0) g_load_cookie = nullptr;
  Line("DLL load notification: %s", status == 0 ? "on, Streamline is hooked as it loads" : "refused, the once-a-second look finds it");
}

void UnwatchLoads() {
  if (g_load_cookie == nullptr) return;
  const auto unreg = reinterpret_cast<LdrUnregisterFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrUnregisterDllNotification"));
  if (unreg != nullptr) unreg(g_load_cookie);
  g_load_cookie = nullptr;
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
  // Already hooked (a rescan asks again): nothing to do.
  for (const Hook& hook : g_other_hooks)
    if (hook.name != nullptr && hook.installed && hook.target == target && std::strcmp(hook.name, name) == 0) return true;
  for (Hook& hook : g_other_hooks) {
    if (hook.name != nullptr) continue;
    hook.name = name;
    hook.hook = hook_function;
    hook.real = real;
    const bool installed = hookpatch::Install(hook, static_cast<uint8_t*>(target), log != nullptr ? log : [](const char*) {});
    if (!installed) hook = Hook{};
    return installed;
  }
  return false;
}

void UnhookFunctions() {
  Lock lock;
  for (Hook& hook : g_other_hooks) {
    hookpatch::Remove(hook, [](const char*) {});
    hook = Hook{};
  }
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
  g_last_game_constants.store(0);
  g_game_fg_on.store(false);
  for (TokenAsked& t : g_tokens_asked) t = {};
  g_token_pick_highest.store(false);
  g_own_constants_last.store(0);
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
  g_interposer = FindGameInterposer();
  if (g_interposer != nullptr) HookInterposer(g_interposer);
  else Line("sl.interposer.dll not loaded yet");
  AdoptOptionsStub();
  ScanPlugins();
  WatchLoads();
}

// The lock is not reentrant, so it comes off around the call into Streamline. Call with it held.
static void* AskFor(sl::Feature feature, const char* name) {
  void* function = nullptr;
  ReleaseSRWLockExclusive(&g_lock);
  const sl::Result result = g_real_get_function(feature, name, function);
  AcquireSRWLockExclusive(&g_lock);
  if (result != sl::Result::eOk) function = nullptr;
  if (function != nullptr && function == g_options_stub) {
    Line("asked Streamline for %s: HSF's own through the plugin hook, DLSS-G's is %p", name, reinterpret_cast<void*>(g_real_dlssg_options));
    return reinterpret_cast<void*>(g_real_dlssg_options);
  }
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
  bool data_flowing = OpticalActive() || GameDataFlowing(now, gone_ms);
  // Flow fill: the game's vectors stop in menus, and HSF's flow takes over a moment later (SetOptical). Generation stays on across that moment instead of going off
  // and on, so it never stops while either source comes.
  if (!data_flowing && g_flow_fill.load(std::memory_order_relaxed) && g_generating.load(std::memory_order_relaxed) &&
      now - std::max(LastGameData(), g_optical_last.load(std::memory_order_relaxed)) <= FLOW_FILL_MS)
    data_flowing = true;
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
    const SetDlssgOptionsFn send = FeatureHook("slDLSSGSetOptions").installed || g_plugin_route.load(std::memory_order_relaxed) ? g_real_dlssg_options : g_direct_dlssg_options;
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
  // The frames shown matter only while the game's own limit is scaled (HookReflexOptions); HSF's own cap goes as it is.
  const bool shown_matters = cap == 0 && !g_game_fg_on.load(std::memory_order_relaxed);
  if (cap == g_reflex_sent_cap.load(std::memory_order_relaxed) && boost == g_reflex_sent_boost.load(std::memory_order_relaxed) &&
      (!shown_matters || shown == g_reflex_sent_shown.load(std::memory_order_relaxed)))
    return;
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

int ListInterposers(Interposer* out, int max) {
  HMODULE all[8];
  const int count = AllInterposers(all, std::min(max, 8));
  Lock lock;
  for (int i = 0; i < count; ++i) {
    out[i] = Interposer{};
    out[i].module = all[i];
    GetModuleFileNameW(all[i], out[i].path, MAX_PATH);
    out[i].hsf_own = IsHsfStreamline(all[i]);
    out[i].in_use = all[i] == g_interposer;
  }
  return count;
}

void PickInterposer(void* module) {
  Lock lock;
  g_picked = static_cast<HMODULE>(module);
  Line("sl.interposer.dll %p picked in the menu", module);
  g_next_poll = 0;  // the next Poll sees the change and hooks it
}

void Rescan() {
  Lock lock;
  Line("=== rescan: hooks that did not go in are tried again, and DLSS-G's functions asked for again ===");
  if (g_interposer != nullptr && !g_old_streamline.load(std::memory_order_relaxed)) {
    for (Hook& hook : g_hooks) {
      if (hook.installed) continue;
      void* const function = reinterpret_cast<void*>(GetProcAddress(g_interposer, hook.name));
      if (function != nullptr) Install(hook, static_cast<uint8_t*>(function));
    }
  }
  ScanPlugins();
  g_asked_features = false;
  g_next_ask = 0;
  g_next_poll = 0;
}

void Poll() {
  if (g_drive.load(std::memory_order_relaxed) && !g_old_streamline.load(std::memory_order_relaxed)) {
    DriveFrame();
    ResendReflex();
  }
  safetry::Tick();
  const ULONGLONG now = GetTickCount64();
  if (g_load_seen.exchange(false)) g_next_poll = 0;  // a load the notification could not handle
  // Only this thread's frame loop calls Poll, so the rate limit needs no lock.
  if (now < g_next_poll) return;
  g_next_poll = now + 1000;
  {
    Lock lock;
    if (!g_started) return;
    Summary();
    ScanPlugins();
    const HMODULE module = FindGameInterposer();
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
    if (!g_asked_features && g_frames > 30 && g_real_get_function != nullptr && now >= g_next_ask) {
      g_next_ask = now + 10000;
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
  // HSF picks the frame itself (no evaluates, no game tags, its own constants). If its tags land and DLSS-G still shows nothing for a few seconds, the frame
  // it picked is not the one being presented: try the other end of the recent tokens. No setting for anyone to change; the log says which one works.
  const ULONGLONG t = GetTickCount64();
  const bool own_frame = OpticalActive() && t - g_own_constants_last.load(std::memory_order_relaxed) <= 1000;
  static uint32_t s_idle_polls = 0;
  static bool s_told_working = false;
  if (own_frame && g_generating.load(std::memory_order_relaxed) && state.numFramesActuallyPresented <= 1) {
    if (++s_idle_polls >= 3) {
      s_idle_polls = 0;
      s_told_working = false;
      const bool highest = !g_token_pick_highest.load(std::memory_order_relaxed);
      g_token_pick_highest.store(highest, std::memory_order_relaxed);
      static uint32_t s_flips = 0;
      if (s_flips++ < 20) Line("HSF's tags land but DLSS-G shows nothing: HSF's tags now go to the %s frame the game asked for", highest ? "highest" : "lowest");
    }
  } else {
    s_idle_polls = 0;
    if (own_frame && state.numFramesActuallyPresented > 1 && !s_told_working) {
      s_told_working = true;
      Line("DLSS-G generates on HSF's own frame pick (the %s frame the game asked for)", g_token_pick_highest.load(std::memory_order_relaxed) ? "highest" : "lowest");
    }
  }
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

void SetOpticalJitter(const float jitter[2], bool jittered, bool reset) {
  g_optical_jitter_x.store(jitter[0], std::memory_order_relaxed);
  g_optical_jitter_y.store(jitter[1], std::memory_order_relaxed);
  g_optical_jittered.store(jittered, std::memory_order_relaxed);
  if (reset) g_optical_reset.store(true, std::memory_order_relaxed);
}

void SetFlowFill(bool on) {
  if (g_flow_fill.exchange(on, std::memory_order_relaxed) == on) return;
  Lock lock;
  Line("flow fills gaps in the game's vectors: %s", on ? "on" : "off");
}

void SetReflex(uint32_t fps_cap, bool boost) {
  g_cap_fps.store(fps_cap, std::memory_order_relaxed);
  g_reflex_boost.store(boost, std::memory_order_relaxed);
}

namespace {
// The game sends no constants of its own (its DLSS is off: PBZ), so DLSS-G would have none for this frame. HSF's flow is the picture's own motion, camera
// included, so the camera itself is left as identity, as in slbring's own constants. Never while the game sends its own: two sets in one frame is a Streamline error.
void SendOwnConstants(const uint32_t extent[4], const float scale[2], bool dilated, bool depth_inverted) {
  const ULONGLONG now = GetTickCount64();
  if (now - g_last_game_constants.load(std::memory_order_relaxed) <= EVALUATE_GONE_MS || g_real_constants == nullptr) return;
  const sl::FrameToken* const frame = g_last_frame.load(std::memory_order_relaxed);
  if (frame == nullptr) return;
  // One set per frame: a second for the same frame is eErrorDuplicatedConstants (27, PBZ while loading, two presents on one token).
  static std::atomic<uint32_t> s_last_index{UINT32_MAX};
  const uint32_t index = SafeToken(frame);
  if (index == UINT32_MAX || s_last_index.exchange(index, std::memory_order_relaxed) == index) return;
  static std::atomic<ULONGLONG> s_last_sent{0};
  // After a gap nothing moved from anywhere. The game's DLSS data through NGX also brings its own reset.
  const bool gap = now - s_last_sent.exchange(now, std::memory_order_relaxed) > EVALUATE_GONE_MS;
  const bool reset = g_optical_reset.exchange(false, std::memory_order_relaxed) || gap;
  sl::float4x4 identity{};
  identity.setRow(0, {1, 0, 0, 0});
  identity.setRow(1, {0, 1, 0, 0});
  identity.setRow(2, {0, 0, 1, 0});
  identity.setRow(3, {0, 0, 0, 1});
  sl::Constants c{};
  c.cameraViewToClip = c.clipToCameraView = c.clipToLensClip = c.clipToPrevClip = c.prevClipToClip = identity;
  c.jitterOffset = {g_optical_jitter_x.load(std::memory_order_relaxed), g_optical_jitter_y.load(std::memory_order_relaxed)};
  c.mvecScale = {scale[0], scale[1]};
  c.cameraPinholeOffset = {0, 0};
  c.cameraPos = {0, 0, 0};
  c.cameraUp = {0, 1, 0};
  c.cameraRight = {1, 0, 0};
  c.cameraFwd = {0, 0, 1};
  c.cameraNear = 0.1f;
  c.cameraFar = 100.f;
  c.cameraFOV = 1.f;
  c.cameraAspectRatio = extent[3] != 0 ? static_cast<float>(extent[2]) / static_cast<float>(extent[3]) : 1.f;
  c.depthInverted = depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.motionVectorsDilated = dilated ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.motionVectorsJittered = g_optical_jittered.load(std::memory_order_relaxed) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  const sl::ViewportHandle viewport(g_viewport.load(std::memory_order_relaxed));
  // Straight to Streamline, not through HookConstants, so these never count as the game's.
  const sl::Result result = g_real_constants(c, *frame, viewport);
  if (result == sl::Result::eOk) g_own_constants_last.store(now, std::memory_order_relaxed);
  static std::atomic<uint32_t> s_lines{0};
  if ((reset || result != sl::Result::eOk) && s_lines.fetch_add(1, std::memory_order_relaxed) < 20) {
    Lock lock;
    Line("the game sends no constants: HSF sends its own for frame %u, viewport %u%s: result %d", SafeToken(frame), static_cast<uint32_t>(viewport), reset ? " (reset)" : "", static_cast<int>(result));
  }
}
}  // namespace

bool TagOptical(void* command_list, void* depth, uint32_t depth_state, void* motion, uint32_t motion_state, const uint32_t extent[4], const float scale[2], bool dilated, bool depth_inverted) {
  InFlight in_flight;
  if (command_list == nullptr || depth == nullptr || motion == nullptr || !g_optical.load(std::memory_order_relaxed) || g_old_streamline.load(std::memory_order_relaxed)) return false;
  g_optical_scale_x.store(scale[0], std::memory_order_relaxed);
  g_optical_scale_y.store(scale[1], std::memory_order_relaxed);
  g_optical_dilated.store(dilated, std::memory_order_relaxed);
  SendOwnConstants(extent, scale, dilated, depth_inverted);
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
  // Optical motion counts as running only once its tags land: failed tags left generation on with nothing to generate from (PBZ, every tag 19).
  if (ok && g_optical_last.exchange(GetTickCount64(), std::memory_order_relaxed) == 0) {
    Lock lock;
    Line("optical motion: first tags, %ux%u", extent[2], extent[3]);
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
  stats.tag_failures = g_tag_failures.load(std::memory_order_relaxed);
  {
    Lock lock;
    stats.game_frames = g_frames;
    stats.interposer_found = g_interposer != nullptr;
    stats.only_hsf_streamline = g_only_hsf_streamline;
    ForEachHook([&](Hook& hook) { stats.hooks_installed += hook.installed ? 1u : 0u; });
  }
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
    if ((hook.installed && CodeStillThere(hook.target)) || (g_plugin_route.load() && g_real_dlssg_options != nullptr && CodeStillThere(reinterpret_cast<const void*>(g_real_dlssg_options)))) {
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
  UnwatchLoads();
  {
    Lock lock;
    if (!g_started) return;
    if (process_exiting) {
      Line("=== process exiting ===");
    } else {
      ForEachHook([](Hook& hook) { Remove(hook); });
      for (int i = 0; i < PLUGIN_COPIES; ++i) {
        Remove(g_plugin_hooks[i]);
        g_plugin_module[i] = nullptr;
      }
      ReleaseOptionsStub();
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
