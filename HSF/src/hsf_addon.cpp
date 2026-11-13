// HSF (Hunter's Sloppy Frames) by Capt-Hunter. DLSS Frame Generation for ReShade.
// DLSS Frame Generation from inside the game: D3D12 on the game's own swap chain, D3D11 and DX9 through HSF's helper. Written against
// reshade::api where it can be, so one code path can serve Direct3D 9/10/11/12, OpenGL and Vulkan in 32-bit and 64-bit games.
// How it all fits together: ../../docs/architecture.md.
//
// Direct3D 11 and DX9: the game's swap chain keeps presenting as before; in its native Present HSF hands the finished frame to DLSS-G in
// hsf-dlssg-helper64.exe (dlssg.h, dlssg_link.h), which shows real and generated frames on a child window over the game's.
// Settings live in HSF.ini next to the add-on and are re-read when it changes, so they can be switched without the menu.

#include <imgui.h>
#include <reshade.hpp>

#include "camera_record.h"
#include "dlssg.h"
#include "dlssg_link.h"
#include "dx9_bridge.h"
#include "vk_bridge.h"
#include "vk_hook.h"
#include "depth.h"
#include "gpu_reserve.h"
#include "gpu_spans.h"
#include "hud.h"
#include "launchpad_tune.h"
#include "score.h"
#include "motion_convention.h"
#include "ui.h"
#ifdef HSF_SL_GAME
#include <d3d12.h>
#include "sl_bring.h"
#include "ngx_input.h"
#include "frame_checks.h"
#include "sl_game.h"
#endif

#include <d3d11.h>
#include <dxgi1_5.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <functional>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" __declspec(dllexport) const char* NAME = "HSF - Hunter's Sloppy Frames";
extern "C" __declspec(dllexport) const char* DESCRIPTION =
    "by Capt-Hunter. Frame generation for (almost) every game, from the motion vectors of LaunchPad or Lumenite. Runs inside the game.";

namespace {

using namespace reshade::api;
using Microsoft::WRL::ComPtr;

constexpr const char* BITNESS = sizeof(void*) == 8 ? "64-bit" : "32-bit";

// What the game gives us, refreshed every frame.
struct Seen {
  device_api api{};
  bool have_api = false;
  uint32_t width = 0, height = 0;
  format back_format = format::unknown;
  bool have_motion = false;
  int source = -1;  // SOURCES index of the motion vectors in use, -1 = none
  uint32_t mv_width = 0, mv_height = 0;
  format mv_format = format::unknown;
} g_seen;

// Per-second counters, shown in the panel and logged every 10 s.
struct Stats {
  float game_fps = 0;
} g_stats;

// HSF.ini Motion=: what the motion vectors' values mean (motion_convention.h, conventions 1-6; auto = what the source is known to give).
const char* const MOTION_NAMES[] = {"auto", "uv", "-uv", "mvpx", "-mvpx", "px", "-px"};
constexpr int MOTION_COUNT = 7;

// ---- Motion sources ----
// Where the motion vectors come from. HSF reads each source's own texture (looked up by name among the loaded effects), not a copy made by another
// add-on. `convention` is what the source's values mean (motion_convention.h), so Motion=auto needs nothing more.
struct Source {
  const char* key;       // HSF.ini Source=
  const char* label;
  const char* names[2];  // texture variable names, tried in order
  int convention;
};
const Source SOURCES[] = {
    // Delta UV from this frame to the previous one, full size. LaunchPad computes it only when an effect asks (HSF.fx does, see EnableRequest).
    {"launchpad", "iMMERSE LaunchPad optical flow", {"Deferred::MotionVectorsTex", "MotionVectorsTex"}, 1},
    // Delta UV, this frame to the previous one, at 1/8 size (LumeniteFX's own effects fetch the previous frame at uv + flow).
    {"lumenite", "LumeniteFX Kernel optical flow", {"Kernel::tFlow", "tFlow"}, 1},
};
constexpr int SOURCE_COUNT = 2;
// HSF.ini Source= and the panel: 0 = auto (the first of SOURCES that is there), i + 1 = SOURCES[i].
const char* const SOURCE_NAMES[] = {"auto", "launchpad", "lumenite"};
int g_source_setting = 0;
effect_texture_variable g_source_variables[SOURCE_COUNT] = {};
bool g_sources_listed = false;  // the by-name fallback (ListSources) ran since the last effect reload
ULONGLONG g_next_source_search = 0;
uint32_t g_effect_reloads = 0;
int g_overlay_mode = 1;  // HSF.ini Overlay: 0 off, 1 simple, 2 advanced
// How each overlay looks, set in the "HSF Overlay" tab; [0] simple, [1] advanced. Saved as HSF.ini OverlaySimple / OverlayAdvanced.
struct OverlayLook {
  int size;     // percent, 50-200
  int opacity;  // the panel behind the text, percent, 0-100
  int corner;   // 0 top left, 1 top right, 2 bottom left, 3 bottom right
  int x, y;     // in from that corner, in pixels at 1440 lines (scaled with the frame)
};
constexpr OverlayLook OVERLAY_DEFAULT[2] = {{100, 59, 0, 28, 28}, {100, 96, 0, 28, 28}};
OverlayLook g_overlay_look[2] = {OVERLAY_DEFAULT[0], OVERLAY_DEFAULT[1]};
const char* const CORNER_NAMES[] = {"TOP LEFT", "TOP RIGHT", "BOTTOM LEFT", "BOTTOM RIGHT"};
const wchar_t* const OVERLAY_LOOK_KEYS[2] = {L"OverlaySimple", L"OverlayAdvanced"};
const char* const OVERLAY_NAMES[] = {"OFF", "SIMPLE", "ADVANCED"};
// This frame's motion vectors as a Direct3D 11 texture (set at finish_effects, handed to the presenter in the native Present right after).
ID3D11Texture2D* g_motion_texture = nullptr;
// The same on Direct3D 9: this frame's flow (LaunchPad) and HSF.fx's depth copy, copied across by dx9_bridge.h in the native Present.
IDirect3DTexture9* g_motion9 = nullptr;
IDirect3DTexture9* g_depth9 = nullptr;
// The same in a Vulkan game: ReShade's resources (vk_bridge.h copies them across), {0} while not there.
resource g_motion_vk = {0};
resource g_depth_vk = {0};
// What one motion vector texel is in UV, for DLSS-G, from this frame towards the previous one (Streamline's mvecScale; 0 = unknown).
float g_dlssg_motion_scale[2] = {};
uint32_t g_dlssg_frames = 1;  // HSF.ini DlssgFrames: generated per real frame (1 = 2x)
// Direct3D 12: where DLSS-G's motion comes from. 0 auto: the game's own DLSS vectors and depth whenever they come, else optical; 1 always optical, HSF's flow and depth;
// 2 the game's DLSS data only. HSF.ini MotionFrom=auto|optical|game, and HSF's panel, which changes it in play. The menu shows them as AUTO / DLSS / FLOW.
int g_motion_from = 0;
bool g_dlss_seen = false;   // this run has seen the game's own DLSS data: the menu's DLSS choice is no longer crossed out
// D3D11: this frame's depth from a DLSS mod's own call (Luma), in place of ReShade's pick; and a reset for DLSS-G at the next Present.
ID3D11ShaderResourceView* g_dlss11_depth = nullptr;
bool g_dlss11_reset = false;
// D3D11: the flow's own view (ReShade's handle is the native view), for scaling it up to the size of a DLSS mod's data (Dlss11Motion).
ID3D11ShaderResourceView* g_motion_srv11 = nullptr;
// D3D11: this frame's motion was put into DLSS-G's units by Dlss11Motion (a DLSS mod's vectors, or the flow scaled up to their size).
bool g_motion_fitted = false;
// HSF.ini FlowPause: while a DLSS mod's data feeds DLSS-G, the flow effect is switched off (it costs GPU time for nothing) and back on when the data stops.
bool g_flow_pause = true;
bool g_using_dlss = false;  // DLSS-G got the game's own DLSS data this frame, not flow (D3D12 routes)
uint32_t g_mfg_key = VK_F7;   // HSF.ini MfgKey: virtual-key code that cycles DlssgFrames in game (0 = none)
// HSF.ini FpsCap: frames on screen per second (real + generated; 0 = no cap). With DLSS-G it is Reflex's FPS cap, applied where the game
// thread sleeps on Reflex; otherwise HSF's own limiter holds the game thread after its Present.
uint32_t g_fps_cap = 0;
// The FPS cap's range when one is set: below 30 on screen frame generation has too little to work with; 1000 is the most HSF.ini ever held.
constexpr uint32_t FPS_CAP_MIN = 30;
constexpr uint32_t FPS_CAP_MAX = 1000;
bool g_cap_wheel_used = false;  // the menu's mouse wheel went to the FPS cap this frame, not to scrolling the menu
// Every way of setting the cap (menu bar, typed number, mouse wheel, HSF.ini) goes through this: 0 or less is no cap, anything else is kept in range.
uint32_t CapRule(long long value) {
  return value <= 0 ? 0u : static_cast<uint32_t>(std::clamp<long long>(value, FPS_CAP_MIN, FPS_CAP_MAX));
}
// HSF.ini GpuReserve=0|1 (menu ADVANCED > GPU RESERVE, on by default): when the card is full and the game's real frames stutter, the FPS cap sent
// through Reflex is lowered a step at a time until the stutter stops (gpu_reserve.h). A full card with even frames costs nothing.
bool g_gpu_reserve = true;
hsf::reserve::Governor g_reserve;
hsf::reserve::LoadReader* const g_load_reader = new hsf::reserve::LoadReader;  // kept for the whole run: its thread uses it
// The FPS cap sent through Reflex: the player's, or the reserve's while it is lower.
uint32_t CapInForce() { return g_reserve.InForce(g_fps_cap); }
bool g_reflex_boost = false;  // HSF.ini ReflexBoost: Reflex low latency + boost (keeps GPU clocks up)
bool g_vk_overlay_window = true;  // HSF.ini VkOverlayWindow (default 1; 0 = the child window as before): on the Vulkan route the helper presents from a click-through window of its own (dlssg_link.h, version 13)
bool g_queue_one = true;      // HSF.ini GameQueue1: with DLSS-G, the game's device queues at most 1 frame (SetMaximumFrameLatency)
bool g_reflex_sleep = true;   // HSF.ini ReflexSleep: 0 = the game thread does not sleep on Reflex (markers only), for an A/B latency test
// The motion quality score and its tools (score.h).
static_assert(SOURCE_COUNT <= hsf::score::MAX_INPUTS);
bool g_score_enabled = true;   // HSF.ini Score: rates every motion source found, a few times a second
uint32_t g_view_key = VK_F8;   // HSF.ini ViewKey: cycles the debug view on screen (0 = none)
uint32_t g_input_key = VK_F9;  // HSF.ini InputKey: A/B of DLSS-G's motion input: each source found, then zero motion (0 = none)
hsf::score::View g_score_view = hsf::score::View::off;  // not saved: a debug view should not come back by itself
// The score runs when HSF.ini Score is on, and also while one of its debug views is on screen, so picking a view always shows something.
bool ScoreRuns() {
  return g_score_enabled || (g_score_view != hsf::score::View::off && g_score_view != hsf::score::View::depth);
}
bool g_input_zero = false;     // A/B: DLSS-G gets zero motion. Not saved, so a test never sticks to the next session.
// Real depth (depth.h).
bool g_depth_enabled = true;     // HSF.ini Depth: sends the game's depth to DLSS-G (0 = constant depth, as before)
// HSF.ini FlowEngine=shader|nvof: where the flow's contents come from. shader: the ReShade effect picked in FLOW FROM. nvof: NVIDIA Optical Flow, the GPU's own engine
// (nvof_flow.h), on HSF's Direct3D 12 work; the effect must still be loaded, as its texture gives the size, format and scale. The menu switches it in play, to compare.
// HSF.ini NvofSize: the height of the grey image the engine gets (360 by default; 0 = the picture's own).
int g_flow_engine = 0;
int g_nvof_size = 360;
// The motion source in force: NVOF makes motion from the picture, so with it picked the game's DLSS data is never used and MOTION counts as FLOW.
// The saved MotionFrom stays as the player left it and comes back when they switch to SHADER.
int MotionFrom() { return g_flow_engine == 1 ? 1 : g_motion_from; }
// NVOF as a backup: on SHADER with no flow effect loaded (LaunchPad, Lumenite), NVOF makes the flow on its own after this long with none.
// The wait lets effects that compile slowly at start come up first. Inspect keeps the clock.
constexpr ULONGLONG NVOF_BACKUP_AFTER_MS = 10000;
ULONGLONG g_flow_effect_seen_ms = 0;  // the last time a flow effect was loaded, or when HSF first looked
bool NvofBackup() { return g_flow_engine == 0 && g_flow_effect_seen_ms != 0 && GetTickCount64() - g_flow_effect_seen_ms > NVOF_BACKUP_AFTER_MS; }
// NVOF in force this frame: picked in FLOW ENGINE, or as the backup.
bool NvofOn() { return g_flow_engine == 1 || NvofBackup(); }
bool g_compose_enabled = true;   // HSF.ini Compose: cleans up the motion with the depth before DLSS-G gets it (compose.h)
bool g_checks_enabled = true;    // HSF.ini FrameChecks: scene cuts, still pictures and the HUD (frame_checks.h)
bool g_dx9_device_ex = false;  // the D3D9 device ReShade made is 9Ex (asked by HSF or the game)
bool g_dx9_ex = true;    // HSF.ini Dx9Ex: a DX9 game's device is made Direct3D 9Ex (through ReShade's create_device), so the bridge copies on the GPU
bool g_dx9_half = true;   // HSF.ini Dx9HalfInputs: DX9 flow and depth go to DLSS-G at half width and height
bool g_keep_overlays_out = true;  // HSF.ini KeepOverlaysOut: DLSS-G gets the frame from before the overlays were drawn, so it does not generate them
bool g_camera_enabled = true;     // HSF.ini CameraMotion: the still world's motion from the camera (camera.h)
bool g_camera_fit = false;        // HSF.ini CameraFit: with no matrix, find it from the flow and depth. Off: it made the camera shake in Tomb Raider
std::atomic<bool> g_camera_listening{false};  // the buffer write events below look at what the game writes
hsf::camera::Scanner g_scanner;
hsf::camera::Candidate g_camera_candidates[hsf::camera::CANDIDATES];
int64_t g_camera_locked = 0;  // what the helper said last
int64_t g_camera_saved = 0;   // what the game's profile holds
// A recording to study offline (camera_record.h). RecordKey or the panel's button starts it, and stops it early.
constexpr ULONGLONG RECORD_MS = 30000;
uint32_t g_record_key = VK_F6;  // HSF.ini RecordKey (0 = none)
bool g_record_request = false;
ULONGLONG g_record_until = 0;
float g_record_aspect = 0;
hsf::camera::record::MatricesWriter g_record_file;
hsf::camera::Candidate g_record_found[hsf::camera::Scanner::SEEN];
hsf::camera::Candidate g_record_parts[hsf::camera::Scanner::PARTS];
bool g_camera_hint_read = false;
ULONGLONG g_camera_log = 0;
ID3D11Texture2D* g_clean_frame = nullptr;  // that frame, copied at reshade_finish_effects
bool g_clean_ready = false;                // and it is this frame's
// The game's own HUD out of generation (hud.h). The draw events run on the game's threads, so what they read while no HUD is wanted is only these atomics.
bool g_hud_enabled = true;  // HSF.ini GameHud
std::atomic<bool> g_hud_listening{false};
std::atomic<bool> g_hud_on_back{false};  // the game's immediate context is drawing on the back buffer
bool g_hud_depth_bound = false;
uint64_t g_hud_back = 0;  // the back buffer's handle, only compared until a bound view has shown it is alive
hsf::hud::Watch g_hud_watch;
hsf::hud::Guard g_hud_guard;
hsf::hud::Remover g_hud_remover;
// A HUD on another target, which the game's last pass draws to the back buffer.
std::atomic<bool> g_hud_on_source{false};  // the immediate context is drawing on a target like the one that pass read last frame
ID3D11Texture2D* g_hud_target = nullptr;   // that target, while it is bound
UINT g_hud_source_width = 0, g_hud_source_height = 0;
format g_hud_source_format = format::unknown;
D3D11_SHADER_RESOURCE_VIEW_DESC g_hud_source_view{};
hsf::hud::Watch g_hud_source_watch;
ID3D11Texture2D* g_hud_copied_from = nullptr;  // only compared
bool g_hud_replayed = false;                   // this frame's back buffer without the HUD is ready
uint32_t g_hud_replays = 0, g_hud_not_read = 0;  // since the last log line
bool g_hud_pass_fresh = false, g_hud_in_pass = false;  // a pass with depth on such a target: bound and not drawn on yet, and under way
std::string g_hud_detail;
int g_hud_detail_count = 0;
bool g_hud_at_effects = false;           // the copy at the start of ReShade's effects is this frame's
ID3D11Texture2D* g_hud_frame = nullptr;  // this frame's HUD-less image, or nullptr: hand over the clean frame
float g_hud_share = 0;                   // of the picture, the last that arrived
uint32_t g_hud_frames = 0, g_hud_found = 0, g_hud_copies = 0;  // since the last log line
ULONGLONG g_hud_next_log = 0;
bool g_hud_log_due = false;
// HSF's HUD work on the game's GPU (copies, the last pass drawn again, the HUD-less image), for the log.
hsf::gpu::Spans g_hud_spans;
// While the guard keeps the HUD out of use (it covers too much of the picture), all that work is thrown away: after HUD_LOOK_FRAMES such frames
// it rests for HUD_REST_MS, then looks again.
constexpr uint32_t HUD_LOOK_FRAMES = 60;
constexpr ULONGLONG HUD_REST_MS = 5000;
uint32_t g_hud_unused_frames = 0, g_hud_rests = 0;  // rests since the last log line
ULONGLONG g_hud_rest_until = 0;
// Every 10 s one frame's render targets go to the log, so a game whose HUD is not found can be read from it.
struct HudPass {
  UINT width, height;
  DXGI_FORMAT format;
  bool back, depth;
  uint32_t draws, hud;
};
constexpr int HUD_PASSES = 32;
HudPass g_hud_passes[HUD_PASSES];
uint32_t g_hud_pass_count = 0;  // ever this frame; the last HUD_PASSES are kept
std::atomic<bool> g_hud_logging{false};
int g_depth_inverted_setting = -1;  // HSF.ini DepthInverted: -1 = found from the depth itself, 0 = normal, 1 = reversed (far = 0)
int g_depth_upside_down_setting = -1;  // HSF.ini DepthUpsideDown: -1 = found from the picture, 0 = as the game has it, 1 = upside down
uint32_t g_depth_key = VK_F10;   // HSF.ini DepthKey: A/B of DLSS-G's depth input, real or constant (0 = none)
// ReShade.ini [DEPTH] DepthCopyBeforeClears, for a depth buffer that only ever reads flat. Generic Depth reads it when the device is made, so a change needs a game restart.
bool g_flat_depth_noticed = false;  // the first time depth::Status::flat_for_long was seen this run
bool g_copy_before_clears = false;  // the setting this run started with, read when flat depth is first noticed
bool g_copy_before_clears_set = false;  // the panel's button was pressed this run
// Generic Depth turns away a depth buffer of another shape than the screen, which is what an emulator drawing a 16:9 picture on a wider screen has;
// ReShade.ini [DEPTH] UseAspectRatioHeuristics=0 lets it through (panel button).
bool g_aspect_check_off_set = false;  // the panel's button was pressed this run
// PinDepthSize changed ReShade's depth size this run (takes effect at the next game start): the menu says so. 0 = no, 1 = saved, 2 = cleared.
int g_depth_pin_note = 0;
uint32_t g_depth_pin_size[2] = {};
bool g_depth_ab_off = false;     // A/B: DLSS-G gets the constant. Not saved, so a test never sticks to the next session.
effect_texture_variable g_depth_variable = {};
ULONGLONG g_next_depth_search = 0;
ID3D11Texture2D* g_depth_texture = nullptr;  // this frame's depth for DLSS-G (set at finish_effects), nullptr = the constant
bool g_depth_inverted = false;
hsf::dlssg::Stats g_dlssg_stats;
// Streamline's DLLs: HSF_streamline next to the add-on.
wchar_t g_sl_folder[MAX_PATH] = L"";
bool g_logged = false;
bool g_menu_open = false;
bool g_borderless = true;  // HSF.ini Borderless: DX9 games asking for exclusive fullscreen get a borderless window instead (OnCreateSwapchain)
bool g_game_fg_ok = false;  // HSF.ini GameFgWarningOk: the player pressed OK on the "turn off the game's frame generation" box, so it stays gone in this game
bool g_force_borderless = false;  // HSF.ini ForceBorderless (menu, ADVANCED > WINDOW): a game that chose windowed mode is made borderless fullscreen, live, in every API
// The borderless crash rule. Some games crash when their window changes size (Dead Space 2, windowed 2560x1440 on a 3440x1440 monitor). With
// ForceBorderless saved, every launch resized the window and crashed again, and the game could not be started. A crash in the game's exe within
// BORDERLESS_CRASH_MS of HSF's last window change turns ForceBorderless off for the next launch and sets HSF.ini BorderlessCrash=1, which the menu shows.
std::atomic<ULONGLONG> g_borderless_moved_at{0};  // GetTickCount64 of HSF's last window change, 0 = none
constexpr ULONGLONG BORDERLESS_CRASH_MS = 15000;
bool g_borderless_crash = false;  // HSF.ini BorderlessCrash: the rule above turned borderless off
int g_launchpad_mode = 3;  // HSF.ini LaunchPadQuality (menu, LAUNCHPAD row): auto (default), low, medium, high, or off (-1, the player's own); see launchpad_tune.h
bool g_extra_enabled = true;
int g_motion_convention = 0;
char g_note[160] = "waiting for the first frame";

void Note(const char* text) { std::snprintf(g_note, sizeof(g_note), "%s", text); }

// The main monitor's refresh rate in Hz, 0 if Windows does not say. Only the panel asks, and it looks again once a second so a changed mode shows up.
uint32_t MainMonitorHz() {
  static uint32_t hz = 0;
  static ULONGLONG next_look = 0;
  const ULONGLONG now = GetTickCount64();
  if (now >= next_look) {
    next_look = now + 1000;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    // 0 and 1 mean "the hardware's default", not a rate.
    hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
  }
  return hz;
}

// ---- HSF.ini ----
// [HSF] Extra=0|1 (frame generation on), Motion=auto|uv|-uv|mvpx|-mvpx|px|-px,
// Source=auto|launchpad|lumenite (an old "uplift" reads as auto), Overlay=0|1, DlssgFrames=1-5, MfgKey=<virtual-key code> (cycles DlssgFrames,
// 118 = F7, 0 = none), FpsCap=<fps on screen, 0 = none>, GpuReserve=0|1 (default 1), ReflexBoost=0|1, GameQueue1=0|1, ReflexSleep=0|1, Score=0|1, ViewKey=<code> (119 = F8),
// InputKey=<code> (120 = F9), Depth=0|1, Compose=0|1, DepthInverted=-1|0|1, DepthUpsideDown=-1|0|1, DepthKey=<code> (121 = F10). Re-read whenever the file's time stamp changes;
// the panel writes it. With InGameDlssg=1: DlssgRebuildCamera=0|1, DlssgCameraKey=<code> (122 = F11, 0 = none), DlssgDynamicRes=0|1, DlssgCutCheck=0|1, DlssgQuickOff=0|1, DlssgScoreGuard=0|1, DlssgChecks=0|1, DlssgHud=0|1, DlssgHudWatch=0|1.

// Frames this run has finished on each API, counted in OnFinishEffects. A D3D11 game can show a few D3D12 frames from something else in the process (an overlay,
// a launcher window), and a D3D12 game can open on a D3D11 splash, so the D3D12 routes count only while D3D12 has shown more frames than D3D11.
std::atomic<uint32_t> g_d3d11_frames{0};
std::atomic<uint32_t> g_d3d12_frames{0};
bool D3d12Run() { return g_d3d12_frames.load(std::memory_order_relaxed) > g_d3d11_frames.load(std::memory_order_relaxed); }

#ifdef HSF_SL_GAME
bool g_sl_bring = false;  // SlBringIn=1: a D3D12 game with no Streamline gets it from HSF
bool g_ngx_input = true;  // NgxInput=0: flow even when the game's DLSS could give its vectors
bool g_sl_game = false;   // StreamlineProbe=1 or InGameDlssg=1: hooks on the game's own Streamline
bool g_sl_drive = false;  // InGameDlssg=1: DLSS-G runs inside the game, through the game's Streamline
bool g_sl_camera_rebuild = true;  // HSF.ini DlssgRebuildCamera: DLSS-G gets camera matrices rebuilt by HSF, not the game's
uint32_t g_sl_camera_key = VK_F11;  // HSF.ini DlssgCameraKey: virtual-key code that flips DlssgRebuildCamera in game (0 = none)
bool g_sl_dynamic_saved = false;  // DlssgDynamicRes=1: this game's vectors have been seen changing size, in this run or an earlier one
bool g_sl_cut_check = true;     // HSF.ini DlssgCutCheck: a camera that jumps with no reset from the game gets a reset from HSF
bool g_sl_quick_off = false;    // HSF.ini DlssgQuickOff: generation stops after about 3 frames with no vectors, not 250 ms
bool g_sl_score_guard = false;  // HSF.ini DlssgScoreGuard: generation stops while the score doubts the game's vectors
bool g_sl_hud = false;          // HSF.ini DlssgHud: DLSS-G gets the picture from before the game's HUD
bool g_sl_hud_watch = false;    // HSF.ini DlssgHudWatch: the draws are watched and logged even with DlssgHud off
bool g_sl_checks = true;        // HSF.ini DlssgChecks: the picture check and the score run on the game's device
hsf::frame_checks::GameChecks* g_sl_game_checks = nullptr;  // kept behind g_sl_checks_lock: the game's thread can be inside it when the swap chain goes
SRWLOCK g_sl_checks_lock = SRWLOCK_INIT;

void SlhMarkVectors();
void SlVectors(void* command_list, void* motion, uint32_t state, const uint32_t extent[4], const float scale[2]) {
  SlhMarkVectors();
  AcquireSRWLockShared(&g_sl_checks_lock);
  if (g_sl_game_checks != nullptr) g_sl_game_checks->NoteVectors(static_cast<ID3D12GraphicsCommandList*>(command_list), static_cast<ID3D12Resource*>(motion), state, extent, scale);
  ReleaseSRWLockShared(&g_sl_checks_lock);
}

void DropSlChecks() {
  AcquireSRWLockExclusive(&g_sl_checks_lock);
  delete g_sl_game_checks;
  g_sl_game_checks = nullptr;
  ReleaseSRWLockExclusive(&g_sl_checks_lock);
}
#endif
wchar_t g_ini_path[MAX_PATH] = L"";
FILETIME g_ini_time{};

void FindIni(HMODULE module) {
  GetModuleFileNameW(module, g_ini_path, MAX_PATH);
  wcscpy_s(g_sl_folder, g_ini_path);
  if (wchar_t* const slash = wcsrchr(g_ini_path, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_ini_path), L"HSF.ini");
  if (wchar_t* const slash = wcsrchr(g_sl_folder, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_sl_folder), L"HSF_streamline");
}

// HSF.ini LaunchPadQuality's words for the LAUNCHPAD row's modes (-1 off, 0 to 2 that Flow Quality, 3 auto).
const wchar_t* LaunchPadModeName(int mode) {
  static const wchar_t* const NAMES[] = {L"off", L"low", L"medium", L"high", L"auto"};
  return NAMES[std::clamp(mode, -1, 3) + 1];
}

void SaveIni() {
  // Keys of HSF's old in-house presenter: DLSS-G is the only generator now, so they are taken out of older HSF.ini files.
  for (const wchar_t* old : {L"Presenter", L"Generated", L"Pacing"}) WritePrivateProfileStringW(L"HSF", old, nullptr, g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"Extra", g_extra_enabled ? L"1" : L"0", g_ini_path);
  wchar_t motion[16];
  swprintf(motion, std::size(motion), L"%hs", MOTION_NAMES[g_motion_convention]);
  WritePrivateProfileStringW(L"HSF", L"Motion", motion, g_ini_path);
  wchar_t source[16];
  swprintf(source, std::size(source), L"%hs", SOURCE_NAMES[g_source_setting]);
  WritePrivateProfileStringW(L"HSF", L"Source", source, g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"Overlay", g_overlay_mode == 2 ? L"2" : g_overlay_mode == 1 ? L"1" : L"0", g_ini_path);
  for (int i = 0; i < 2; ++i) {
    // "size,opacity,corner,x,y"
    wchar_t look[64];
    const OverlayLook& l = g_overlay_look[i];
    swprintf(look, std::size(look), L"%d,%d,%d,%d,%d", l.size, l.opacity, l.corner, l.x, l.y);
    WritePrivateProfileStringW(L"HSF", OVERLAY_LOOK_KEYS[i], look, g_ini_path);
  }
  wchar_t frames[8];
  swprintf(frames, std::size(frames), L"%u", g_dlssg_frames);
  WritePrivateProfileStringW(L"HSF", L"DlssgFrames", frames, g_ini_path);
  wchar_t key[8];
  swprintf(key, std::size(key), L"%u", g_mfg_key);
  WritePrivateProfileStringW(L"HSF", L"MfgKey", key, g_ini_path);
#ifdef HSF_SL_GAME
  if (g_sl_drive) {
    WritePrivateProfileStringW(L"HSF", L"DlssgRebuildCamera", g_sl_camera_rebuild ? L"1" : L"0", g_ini_path);
    swprintf(key, std::size(key), L"%u", g_sl_camera_key);
    WritePrivateProfileStringW(L"HSF", L"DlssgCameraKey", key, g_ini_path);
    WritePrivateProfileStringW(L"HSF", L"DlssgCutCheck", g_sl_cut_check ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"HSF", L"DlssgQuickOff", g_sl_quick_off ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"HSF", L"DlssgScoreGuard", g_sl_score_guard ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"HSF", L"DlssgHud", g_sl_hud ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"HSF", L"DlssgChecks", g_sl_checks ? L"1" : L"0", g_ini_path);
  }
  if (g_sl_drive || g_sl_bring || g_dlss_seen) WritePrivateProfileStringW(L"HSF", L"MotionFrom", g_motion_from == 1 ? L"optical" : g_motion_from == 2 ? L"game" : L"auto", g_ini_path);
  if (g_dlss_seen) WritePrivateProfileStringW(L"HSF", L"FlowPause", g_flow_pause ? L"1" : L"0", g_ini_path);
#endif
  wchar_t cap[12];
  swprintf(cap, std::size(cap), L"%u", g_fps_cap);
  WritePrivateProfileStringW(L"HSF", L"FpsCap", cap, g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"GpuReserve", g_gpu_reserve ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"ReflexBoost", g_reflex_boost ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"GameQueue1", g_queue_one ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"ReflexSleep", g_reflex_sleep ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"Score", g_score_enabled ? L"1" : L"0", g_ini_path);
  swprintf(key, std::size(key), L"%u", g_view_key);
  WritePrivateProfileStringW(L"HSF", L"ViewKey", key, g_ini_path);
  swprintf(key, std::size(key), L"%u", g_input_key);
  WritePrivateProfileStringW(L"HSF", L"InputKey", key, g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"Depth", g_depth_enabled ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"Compose", g_compose_enabled ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"FlowEngine", g_flow_engine == 1 ? L"nvof" : L"shader", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"FrameChecks", g_checks_enabled ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"KeepOverlaysOut", g_keep_overlays_out ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"CameraMotion", g_camera_enabled ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"CameraFit", g_camera_fit ? L"1" : L"0", g_ini_path);
  WritePrivateProfileStringW(L"HSF", L"GameHud", g_hud_enabled ? L"1" : L"0", g_ini_path);
  swprintf(key, std::size(key), L"%d", g_depth_inverted_setting);
  WritePrivateProfileStringW(L"HSF", L"DepthInverted", key, g_ini_path);
  swprintf(key, std::size(key), L"%d", g_depth_upside_down_setting);
  WritePrivateProfileStringW(L"HSF", L"DepthUpsideDown", key, g_ini_path);
  swprintf(key, std::size(key), L"%u", g_depth_key);
  WritePrivateProfileStringW(L"HSF", L"DepthKey", key, g_ini_path);
  swprintf(key, std::size(key), L"%u", g_record_key);
  WritePrivateProfileStringW(L"HSF", L"RecordKey", key, g_ini_path);
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (GetFileAttributesExW(g_ini_path, GetFileExInfoStandard, &data)) g_ini_time = data.ftLastWriteTime;
}

void PollIni() {
  static ULONGLONG next_check = 0;
  const ULONGLONG now = GetTickCount64();
  if (now < next_check) return;
  next_check = now + 500;
  WIN32_FILE_ATTRIBUTE_DATA data{};
  if (!GetFileAttributesExW(g_ini_path, GetFileExInfoStandard, &data)) { SaveIni(); return; }  // first run: write the defaults
  if (CompareFileTime(&data.ftLastWriteTime, &g_ini_time) == 0) return;
  g_ini_time = data.ftLastWriteTime;
  g_borderless = GetPrivateProfileIntW(L"HSF", L"Borderless", 1, g_ini_path) != 0;
  g_force_borderless = GetPrivateProfileIntW(L"HSF", L"ForceBorderless", 0, g_ini_path) != 0;
  g_borderless_crash = GetPrivateProfileIntW(L"HSF", L"BorderlessCrash", 0, g_ini_path) != 0;
  {
    wchar_t mode[16];
    GetPrivateProfileStringW(L"HSF", L"LaunchPadQuality", L"auto", mode, static_cast<DWORD>(std::size(mode)), g_ini_path);
    g_launchpad_mode = hsf::launchpad::MODE_AUTO;
    for (int i = -1; i <= 3; ++i)
      if (_wcsicmp(mode, LaunchPadModeName(i)) == 0) g_launchpad_mode = i;
  }
  g_game_fg_ok = GetPrivateProfileIntW(L"HSF", L"GameFgWarningOk", 0, g_ini_path) != 0;
  g_extra_enabled = GetPrivateProfileIntW(L"HSF", L"Extra", 1, g_ini_path) != 0;
  wchar_t motion[16];
  GetPrivateProfileStringW(L"HSF", L"Motion", L"auto", motion, static_cast<DWORD>(std::size(motion)), g_ini_path);
  g_motion_convention = 0;
  for (int i = 0; i < MOTION_COUNT; ++i) {
    wchar_t name[16];
    swprintf(name, std::size(name), L"%hs", MOTION_NAMES[i]);
    if (_wcsicmp(motion, name) == 0) g_motion_convention = i;
  }
  wchar_t source[16];
  GetPrivateProfileStringW(L"HSF", L"Source", L"auto", source, static_cast<DWORD>(std::size(source)), g_ini_path);
  g_source_setting = 0;
  for (int i = 0; i <= SOURCE_COUNT; ++i) {
    wchar_t name[16];
    swprintf(name, std::size(name), L"%hs", SOURCE_NAMES[i]);
    if (_wcsicmp(source, name) == 0) g_source_setting = i;
  }
  g_overlay_mode = std::clamp<int>(static_cast<int>(GetPrivateProfileIntW(L"HSF", L"Overlay", 1, g_ini_path)), 0, 2);
  for (int i = 0; i < 2; ++i) {
    wchar_t look[64] = {};
    GetPrivateProfileStringW(L"HSF", OVERLAY_LOOK_KEYS[i], L"", look, static_cast<DWORD>(std::size(look)), g_ini_path);
    OverlayLook l = OVERLAY_DEFAULT[i];
    if (swscanf_s(look, L"%d,%d,%d,%d,%d", &l.size, &l.opacity, &l.corner, &l.x, &l.y) != 5) l = OVERLAY_DEFAULT[i];
    l.size = std::clamp(l.size, 50, 200);
    l.opacity = std::clamp(l.opacity, 0, 100);
    l.corner = std::clamp(l.corner, 0, 3);
    l.x = std::clamp(l.x, 0, 2000);
    l.y = std::clamp(l.y, 0, 2000);
    g_overlay_look[i] = l;
  }
  g_dlssg_frames = std::clamp<UINT>(GetPrivateProfileIntW(L"HSF", L"DlssgFrames", 1, g_ini_path), 1, 5);
  g_mfg_key = std::min<UINT>(GetPrivateProfileIntW(L"HSF", L"MfgKey", VK_F7, g_ini_path), 255);
#ifdef HSF_SL_GAME
  g_sl_camera_rebuild = GetPrivateProfileIntW(L"HSF", L"DlssgRebuildCamera", 1, g_ini_path) != 0;
  g_sl_camera_key = std::min<UINT>(GetPrivateProfileIntW(L"HSF", L"DlssgCameraKey", VK_F11, g_ini_path), 255);
  g_sl_cut_check = GetPrivateProfileIntW(L"HSF", L"DlssgCutCheck", 1, g_ini_path) != 0;
  g_sl_quick_off = GetPrivateProfileIntW(L"HSF", L"DlssgQuickOff", 0, g_ini_path) != 0;
  g_sl_score_guard = GetPrivateProfileIntW(L"HSF", L"DlssgScoreGuard", 0, g_ini_path) != 0;
  g_sl_hud = GetPrivateProfileIntW(L"HSF", L"DlssgHud", 0, g_ini_path) != 0;
  g_sl_hud_watch = GetPrivateProfileIntW(L"HSF", L"DlssgHudWatch", 0, g_ini_path) != 0;
  g_sl_checks = GetPrivateProfileIntW(L"HSF", L"DlssgChecks", 1, g_ini_path) != 0;
  {
    wchar_t from[16];
    GetPrivateProfileStringW(L"HSF", L"MotionFrom", L"auto", from, static_cast<DWORD>(std::size(from)), g_ini_path);
    g_motion_from = _wcsicmp(from, L"optical") == 0 ? 1 : _wcsicmp(from, L"game") == 0 ? 2 : 0;
  }
  g_flow_pause = GetPrivateProfileIntW(L"HSF", L"FlowPause", 1, g_ini_path) != 0;
#endif
  g_fps_cap = CapRule(static_cast<int>(GetPrivateProfileIntW(L"HSF", L"FpsCap", 0, g_ini_path)));
  g_gpu_reserve = GetPrivateProfileIntW(L"HSF", L"GpuReserve", 1, g_ini_path) != 0;
  g_reflex_boost = GetPrivateProfileIntW(L"HSF", L"ReflexBoost", 0, g_ini_path) != 0;
  g_vk_overlay_window = GetPrivateProfileIntW(L"HSF", L"VkOverlayWindow", 1, g_ini_path) != 0;
  g_queue_one = GetPrivateProfileIntW(L"HSF", L"GameQueue1", 1, g_ini_path) != 0;
  g_reflex_sleep = GetPrivateProfileIntW(L"HSF", L"ReflexSleep", 1, g_ini_path) != 0;
  g_score_enabled = GetPrivateProfileIntW(L"HSF", L"Score", 1, g_ini_path) != 0;
  g_view_key = std::min<UINT>(GetPrivateProfileIntW(L"HSF", L"ViewKey", VK_F8, g_ini_path), 255);
  g_input_key = std::min<UINT>(GetPrivateProfileIntW(L"HSF", L"InputKey", VK_F9, g_ini_path), 255);
  g_depth_enabled = GetPrivateProfileIntW(L"HSF", L"Depth", 1, g_ini_path) != 0;
  g_compose_enabled = GetPrivateProfileIntW(L"HSF", L"Compose", 1, g_ini_path) != 0;
  {
    wchar_t engine[16] = L"";
    GetPrivateProfileStringW(L"HSF", L"FlowEngine", L"shader", engine, static_cast<DWORD>(std::size(engine)), g_ini_path);
    g_flow_engine = _wcsicmp(engine, L"nvof") == 0 ? 1 : 0;
    g_nvof_size = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"HSF", L"NvofSize", 360, g_ini_path)), 0, 2160);
  }
  g_checks_enabled = GetPrivateProfileIntW(L"HSF", L"FrameChecks", 1, g_ini_path) != 0;
  g_keep_overlays_out = GetPrivateProfileIntW(L"HSF", L"KeepOverlaysOut", 1, g_ini_path) != 0;
  g_dx9_ex = GetPrivateProfileIntW(L"HSF", L"Dx9Ex", 1, g_ini_path) != 0;
  g_dx9_half = GetPrivateProfileIntW(L"HSF", L"Dx9HalfInputs", 1, g_ini_path) != 0;
  hsf::dx9::SetHalfInputs(g_dx9_half);
  hsf::dx9::SetGpuFence(GetPrivateProfileIntW(L"HSF", L"Dx9GpuFence", 1, g_ini_path) != 0);
  g_camera_enabled = GetPrivateProfileIntW(L"HSF", L"CameraMotion", 1, g_ini_path) != 0;
  g_camera_fit = GetPrivateProfileIntW(L"HSF", L"CameraFit", 0, g_ini_path) != 0;
  g_hud_enabled = GetPrivateProfileIntW(L"HSF", L"GameHud", 1, g_ini_path) != 0;
  g_depth_inverted_setting = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"HSF", L"DepthInverted", -1, g_ini_path)), -1, 1);
  g_depth_upside_down_setting = std::clamp(static_cast<int>(GetPrivateProfileIntW(L"HSF", L"DepthUpsideDown", -1, g_ini_path)), -1, 1);
  g_depth_key = std::min<UINT>(GetPrivateProfileIntW(L"HSF", L"DepthKey", VK_F10, g_ini_path), 255);
  g_record_key = std::min<UINT>(GetPrivateProfileIntW(L"HSF", L"RecordKey", VK_F6, g_ini_path), 255);
  char text[400];
  std::snprintf(text, sizeof(text), "HSF.ini: frame generation %s, motion %s, source %s, overlay %s, dlssg frames %u, "
                "fps cap %u, gpu reserve %s, reflex boost %s, game queue 1 %s, reflex sleep %s, score %s",
                g_extra_enabled ? "on" : "off", MOTION_NAMES[g_motion_convention], SOURCE_NAMES[g_source_setting], OVERLAY_NAMES[g_overlay_mode],
                g_dlssg_frames, g_fps_cap, g_gpu_reserve ? "on" : "off", g_reflex_boost ? "on" : "off", g_queue_one ? "on" : "off", g_reflex_sleep ? "on" : "off", g_score_enabled ? "on" : "off");
  reshade::log_message(reshade::log_level::info, text);
}

const char* ApiName(device_api api) {
  switch (api) {
    case device_api::d3d9: return "Direct3D 9";
    case device_api::d3d10: return "Direct3D 10";
    case device_api::d3d11: return "Direct3D 11";
    case device_api::d3d12: return "Direct3D 12";
    case device_api::opengl: return "OpenGL";
    case device_api::vulkan: return "Vulkan";
  }
  return "unknown";
}

void Log(const char* text) {
  char line[400];
  std::snprintf(line, sizeof(line), "HSF: %s", text);
  reshade::log_message(reshade::log_level::info, line);
}

double Seconds(LONGLONG ticks) {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  return static_cast<double>(ticks) / static_cast<double>(freq.QuadPart);
}

LONGLONG Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

// ---- The native swap chain's Present ----
// The swap chain under ReShade's wrapper. ReShade's own Present (the one the game calls) runs Uplift and the effects and then forwards to
// this one, after ReShade has drawn its menu. Its vtable slot is patched so HSF can copy the finished frame out to its presenter right before it.
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
constexpr size_t PRESENT_SLOT = 8;
void** g_patched_vtable = nullptr;
PresentFn g_real_present = nullptr;
IDXGISwapChain* g_native_swap = nullptr;
bool g_native_ok = false;  // the slot really is DXGI's and not ReShade's wrapper
UINT g_game_sync = 1, g_game_flags = 0;
uint32_t g_game_presents = 0;
// The game's frame times for the overlay's graph: from one return of its Present to the next (the whole frame, Reflex's sleep or HSF's
// limiter included). Game thread only.
constexpr size_t FRAME_HISTORY = 300;
float g_frame_ms[FRAME_HISTORY] = {};
size_t g_frame_write = 0;
LONGLONG g_last_frame = 0;

// HSF's own FPS cap, for when Reflex is not there (no DLSS-G presenter): holds the game thread after its Present until the next frame's time.
// A high-resolution waitable timer for most of the wait, then a short spin. A frame later than one whole period restarts the timeline.
void CapWait(uint32_t fps) {
  static LONGLONG next = 0;
  static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  if (fps == 0) {
    next = 0;
    return;
  }
  const LONGLONG period = freq.QuadPart / fps;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (next == 0 || now.QuadPart - next > period) next = now.QuadPart;  // first frame or far behind: start again from now
  const LONGLONG spin = freq.QuadPart / 1000;                         // the last ~1 ms is spun
  const LONGLONG left = next - now.QuadPart;
  if (left > spin && timer != nullptr) {
    LARGE_INTEGER due;
    due.QuadPart = -((left - spin) * 10000000 / freq.QuadPart);  // relative, 100 ns units
    if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, 1000);
  }
  do QueryPerformanceCounter(&now);
  while (now.QuadPart < next);
  next += period;
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* self, UINT sync_interval, UINT flags) {
  if (self == g_native_swap && (flags & DXGI_PRESENT_TEST) == 0) {
    g_game_sync = sync_interval;
    g_game_flags = flags;
    ++g_game_presents;
    if (hsf::dlssg::Feeding()) {
      ID3D11Texture2D* back = nullptr;
      if (SUCCEEDED(self->GetBuffer(0, IID_PPV_ARGS(&back)))) {
        {
          hsf::dlssg::FrameInputs inputs;
          inputs.motion = g_motion_texture;
          inputs.motion_scale[0] = g_dlssg_motion_scale[0];
          inputs.motion_scale[1] = g_dlssg_motion_scale[1];
          inputs.depth = g_depth_texture;
          inputs.depth_inverted = g_depth_inverted;
          inputs.reset = g_dlss11_reset;
          g_dlss11_reset = false;
          inputs.compose = g_compose_enabled;
          inputs.checks = g_checks_enabled;
          inputs.nvof = NvofOn();
          inputs.nvof_size = static_cast<uint32_t>(g_nvof_size);
          inputs.hudless = !g_clean_ready ? nullptr : g_hud_frame != nullptr ? g_hud_frame : g_clean_frame;
          g_clean_ready = false;
          g_hud_frame = nullptr;
          if (g_camera_listening.load(std::memory_order_relaxed)) {
            D3D11_TEXTURE2D_DESC back_desc{};
            back->GetDesc(&back_desc);
            if (back_desc.Height != 0) {
              g_record_aspect = static_cast<float>(back_desc.Width) / static_cast<float>(back_desc.Height);
              g_scanner.SetAspect(g_record_aspect);
            }
            inputs.camera_motion = true;
            inputs.camera_fit = g_camera_fit;
            inputs.camera = g_camera_candidates;
            const int found = g_record_file.IsOpen() ? g_scanner.CopyFound(g_record_found) : 0;
            const int parts = g_record_file.IsOpen() ? g_scanner.CopyParts(g_record_parts) : 0;
            inputs.camera_count = g_scanner.EndFrame(g_camera_candidates, inputs.camera_frame);
            if (g_record_file.IsOpen()) {
              g_record_file.Frame(inputs.camera_frame, g_record_found, found, g_record_parts, parts);
              inputs.camera_record = true;
            }
          }
          hsf::dlssg::Submit(back, inputs);
        }
        back->Release();
      }
    }
    const HRESULT hr = g_real_present(self, sync_interval, flags);
    // The game's next frame starts once its Present returns: Reflex sleeps the game thread here (low latency + FPS cap) with DLSS-G, HSF's
    // own limiter without it.
    if (hsf::dlssg::Feeding()) {
      CapWait(0);
      hsf::dlssg::FrameStart();
    } else {
      CapWait(g_fps_cap);
    }
    const LONGLONG now = Now();
    if (g_last_frame != 0) {
      const double ms = Seconds(now - g_last_frame) * 1000.0;
      g_frame_ms[g_frame_write++ % FRAME_HISTORY] = static_cast<float>(std::min(ms, 1000.0));
    }
    g_last_frame = now;
    return hr;
  }
  return g_real_present(self, sync_interval, flags);
}

void PatchSlot(void** vtable, size_t slot, void* with) {
  DWORD old = 0;
  if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return;
  vtable[slot] = with;
  VirtualProtect(&vtable[slot], sizeof(void*), old, &old);
}

HMODULE ModuleOf(const void* address) {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module);
  return module;
}

void HookNativePresent(effect_runtime* runtime) {
  IDXGISwapChain* const native = reinterpret_cast<IDXGISwapChain*>(runtime->get_native());
  if (native == nullptr || native == g_native_swap) return;
  g_native_swap = native;
  if (g_patched_vtable != nullptr) return;  // a new swap chain of the same class shares the patched vtable
  void** const vtable = *reinterpret_cast<void***>(native);
  g_real_present = reinterpret_cast<PresentFn>(vtable[PRESENT_SLOT]);
  // ReShade's own objects live in ReShade's module: if the native Present is in there too, this is not the swap chain under the wrapper.
  const HMODULE present_module = ModuleOf(reinterpret_cast<const void*>(g_real_present));
  const HMODULE reshade_module = ModuleOf(*reinterpret_cast<void**>(runtime));
  wchar_t path[MAX_PATH] = L"?";
  GetModuleFileNameW(present_module, path, MAX_PATH);
  g_native_ok = present_module != nullptr && present_module != reshade_module;
  char text[MAX_PATH + 120];
  std::snprintf(text, sizeof(text), "native Present is in %ls (%s)", path, g_native_ok ? "DXGI, using it" : "ReShade's wrapper: presenter disabled");
  Log(text);
  if (!g_native_ok) return;
  PatchSlot(vtable, PRESENT_SLOT, reinterpret_cast<void*>(&HookedPresent));
  g_patched_vtable = vtable;
}

void UnhookNativePresent() {
  if (g_patched_vtable == nullptr) return;
  PatchSlot(g_patched_vtable, PRESENT_SLOT, reinterpret_cast<void*>(g_real_present));
  g_patched_vtable = nullptr;
}

// Whether DLSS-G gets real depth: the D3D11/D3D12 copy, or in a DX9 game the HSF.fx depth the bridge carries.
bool DepthSent() { return g_depth_texture != nullptr || ((g_depth9 != nullptr || g_depth_vk.handle != 0) && hsf::dlssg::Running()); }

bool Dx9DepthLine(char* text, size_t size, const char* sent);

// What DLSS-G's depth input is now, for the log and the panel.
void DepthLine(char* text, size_t size) {
  const hsf::depth::Status s = hsf::depth::Current();
  const char* const sent = DepthSent() ? "real" : g_depth_ab_off ? "constant (A/B)" : !g_depth_enabled ? "constant (Depth=0)" : "constant";
  if (Dx9DepthLine(text, size, sent)) return;
  if (hsf::depth::Problem()[0] != '\0')
    std::snprintf(text, size, "depth: %s; %s", sent, hsf::depth::Problem());
  else if (!s.source)
    std::snprintf(text, size, "depth: %s; ReShade has no depth buffer bound%s", sent,
                  g_seen.api == device_api::vulkan ? " (the game binds none; path-traced games have none)"
                  : g_depth_variable.handle == 0   ? " (HSF.fx is not loaded)"
                                                   : "");
  else if (!s.measured)
    std::snprintf(text, size, "depth: %s; %ux%u format %u, %u sample(s), not measured yet", sent, s.width, s.height, static_cast<unsigned>(s.format), s.samples);
  else
    std::snprintf(text, size, "depth: %s; %ux%u format %u, %u sample(s), values %.4f / %.4f / %.4f%s, %s%s, %s%s (edges %.5f, mirrored %.5f)", sent, s.width, s.height, static_cast<unsigned>(s.format), s.samples,
                  s.lowest, s.median, s.highest, s.flat ? ", flat" : "", g_depth_inverted ? "reversed" : "normal", g_depth_inverted_setting >= 0 ? " (HSF.ini)" : " (found)",
                  s.upside_down ? "upside down" : "upright", g_depth_upside_down_setting >= 0 ? " (HSF.ini)" : " (found)", s.edges_same, s.edges_mirrored);
}

// One line per source that was measured in the last 10 s, for reading a test back from the log.
void LogScore() {
  for (int i = 0; i < SOURCE_COUNT; ++i) {
    const hsf::score::Result r = hsf::score::Recent(i, 10000);
    if (r.pairs + r.still + r.cuts == 0) continue;
    char text[300];
    std::snprintf(text, sizeof(text), "motion score %s%s: gain %.1f%%, error %.2f with motion, %.2f without (levels of 255), better %.0f%%, worse %.0f%% of the %.0f%% that changed; %u pairs, "
                  "%u still, %u cuts%s%s", SOURCES[i].key, i == g_seen.source ? " (in use)" : "", r.gain * 100.f, r.error_warp, r.error_none, r.better * 100.f, r.worse * 100.f,
                  r.moving * 100.f, r.pairs, r.still, r.cuts, i == g_seen.source && g_input_zero ? "; DLSS-G gets ZERO motion (A/B)" : "", r.thin ? "; too little to rate" : "");
    Log(text);
  }
  if (hsf::score::Problem()[0] != '\0') {
    char text[220];
    std::snprintf(text, sizeof(text), "motion score not running: %s", hsf::score::Problem());
    Log(text);
  }
  if (hsf::dlssg::Running()) {
    char text[300];
    DepthLine(text, sizeof(text));
    Log(text);
  }
}

void UpdateStats() {
  static LONGLONG window_start = 0, last_log = 0;
  static uint32_t game_start = 0;
  const LONGLONG now = Now();
  if (window_start == 0) { window_start = last_log = now; return; }
  const double seconds = Seconds(now - window_start);
  if (seconds < 1.0) return;
  g_stats.game_fps = static_cast<float>((g_game_presents - game_start) / seconds);
  g_dlssg_stats = hsf::dlssg::TakeStats();
  game_start = g_game_presents;
  window_start = now;
  static LONGLONG last_score_log = now;
  if (g_score_enabled && Seconds(now - last_score_log) >= 10.0) {
    last_score_log = now;
    LogScore();
  }
  if (Seconds(now - last_log) >= 10.0 && hsf::dlssg::Running()) {
    last_log = now;
    const hsf::dlssg::Stats& d = g_dlssg_stats;
    char text[700];
    std::snprintf(text, sizeof(text), "game presents %.1f/s; DLSS-G presenter: real %.1f/s, shown %.1f/s (x%.2f, %u generated per real set), "
                  "generating %s, skipped %u, paced %u (%.1f ms each), waited at the frame start %u (%.1f ms each), copies %.2f ms on the GPU, status 0x%x; Reflex %s, game thread slept %.2f ms/frame, FPS cap %u; latency: HSF timer %.1f ms "
                  "(worst %.1f), Reflex report %.1f ms over %u frames%s%s", g_stats.game_fps, d.real_fps,
                  d.shown_fps, d.real_fps > 0 ? d.shown_fps / d.real_fps : 0.f, g_dlssg_frames, d.generating ? "yes" : "no", d.skipped, d.paced, d.paced_ms, d.early, d.early_ms, d.copy_ms, d.status,
                  d.reflex_game_paced ? "on the game's frame" : "not pacing", d.reflex_sleep_ms, g_fps_cap, d.latency_ms, d.latency_max_ms,
                  d.report_total_ms, d.report_frames, d.note[0] != '\0' ? "; " : "", d.note);
    Log(text);
  }
  // D3D11 and DX9 only: the D3D12 routes log to HSF_sl_probe.log and HSF_sl_bring.log.
  if (Seconds(now - last_log) >= 10.0 && g_seen.api != device_api::d3d12) {
    last_log = now;
    char text[320];
    std::snprintf(text, sizeof(text), "game presents %.1f/s (sync interval %u, flags 0x%x); DLSS-G not running: %s", g_stats.game_fps, g_game_sync,
                  g_game_flags, g_note);
    Log(text);
  }
}

// The by-name fallback, once per effect reload: every texture variable's name, for sources whose names find_texture_variable did not match (the
// part after a namespace counts too). The handles are collected first and named after the enumeration: no runtime calls inside ReShade's
// enumeration.
void ListSources(effect_runtime* runtime) {
  g_sources_listed = true;
  std::vector<effect_texture_variable> all;
  runtime->enumerate_texture_variables(nullptr, [&all](effect_runtime*, effect_texture_variable variable) { all.push_back(variable); });
  for (const effect_texture_variable variable : all) {
    char name[160] = "";
    runtime->get_texture_variable_name(variable, name);
    const char* const colons = std::strrchr(name, ':');
    const char* const short_name = colons != nullptr ? colons + 1 : name;
    for (int i = 0; i < SOURCE_COUNT; ++i) {
      if (g_source_variables[i].handle != 0) continue;
      for (const char* wanted : SOURCES[i].names) {
        if (wanted == nullptr) continue;
        const char* const wanted_colons = std::strrchr(wanted, ':');
        if (std::strcmp(name, wanted) == 0 || std::strcmp(short_name, wanted_colons != nullptr ? wanted_colons + 1 : wanted) == 0) {
          g_source_variables[i] = variable;
          char text[240];
          std::snprintf(text, sizeof(text), "motion source %s: texture variable \"%s\" (found by its name)", SOURCES[i].key, name);
          Log(text);
          break;
        }
      }
    }
  }
}

// Looks for the sources that are not found yet (once a second, so effects compiled later are seen).
void FindSources(effect_runtime* runtime) {
  const ULONGLONG now = GetTickCount64();
  if (now < g_next_source_search) return;
  g_next_source_search = now + 1000;
  bool missing = false;
  for (int i = 0; i < SOURCE_COUNT; ++i) {
    for (const char* name : SOURCES[i].names) {
      if (g_source_variables[i].handle != 0 || name == nullptr) continue;
      g_source_variables[i] = runtime->find_texture_variable(nullptr, name);
      if (g_source_variables[i].handle != 0) {
        char text[200];
        std::snprintf(text, sizeof(text), "motion source %s: texture variable \"%s\"", SOURCES[i].key, name);
        Log(text);
      }
    }
    missing |= g_source_variables[i].handle == 0;
  }
  if (missing && !g_sources_listed) ListSources(runtime);
}

// The source in use: the chosen one, or with auto the first that is there. -1 if none.
int PickSource() {
  if (g_source_setting > 0) return g_source_variables[g_source_setting - 1].handle != 0 ? g_source_setting - 1 : -1;
  for (int i = 0; i < SOURCE_COUNT; ++i)
    if (g_source_variables[i].handle != 0) return i;
  return -1;
}

// The motion convention in use: HSF.ini's, or with auto the source's own (0 = not known: no motion goes to DLSS-G).
int Convention() {
  if (g_motion_convention != 0) return g_motion_convention;
  return g_seen.source >= 0 ? SOURCES[g_seen.source].convention : 0;
}

// LaunchPad computes optical flow only for frames an effect asked it for. HSF.fx's one technique does nothing but ask, so with LaunchPad as the
// source it is switched on here (it costs nothing otherwise, but is left as the user set it).
void EnableRequest(effect_runtime* runtime) {
  static bool warned = false;
  if (g_seen.source != 0) return;
  const effect_technique request = runtime->find_technique("HSF.fx", "HSF_Motion");
  if (request.handle == 0) {
    if (!warned) Log("HSF.fx (technique HSF_Motion) is not loaded: LaunchPad computes its optical flow only while another effect asks for it");
    warned = true;
    return;
  }
  if (!runtime->get_technique_state(request)) {
    runtime->set_technique_state(request, true);
    Log("switched on HSF_Motion (HSF.fx), which asks LaunchPad for its optical flow");
  }
}

// Remembers what the game gives us; logged on the first frame and again whenever the motion vectors appear, go away or change source.
void Inspect(effect_runtime* runtime, device* dev, const resource_desc& back) {
  g_seen.api = dev->get_api();
  g_seen.have_api = true;
  g_seen.width = back.texture.width;
  g_seen.height = back.texture.height;
  g_seen.back_format = back.texture.format;
  FindSources(runtime);
  g_seen.source = PickSource();
  if (g_seen.source >= 0 || g_flow_effect_seen_ms == 0) g_flow_effect_seen_ms = GetTickCount64();
  g_seen.have_motion = false;
  g_motion_texture = nullptr;
  g_motion_srv11 = nullptr;
  g_motion9 = nullptr;
  g_motion_vk = {0};
  if (g_seen.source >= 0) {
    EnableRequest(runtime);
    resource_view srv{}, srv_srgb{};
    runtime->get_texture_binding(g_source_variables[g_seen.source], &srv, &srv_srgb);
    if (srv.handle != 0) {
      const resource motion = dev->get_resource_from_view(srv);
      const resource_desc mv = dev->get_resource_desc(motion);
      // On Direct3D 11 a ReShade resource handle is the native resource.
      if (g_seen.api == device_api::d3d11 && mv.type == resource_type::texture_2d) {
        g_motion_texture = reinterpret_cast<ID3D11Texture2D*>(motion.handle);
        g_motion_srv11 = reinterpret_cast<ID3D11ShaderResourceView*>(srv.handle);
      }
      // On Direct3D 9 it is the IDirect3DResource9, here a texture's (dx9_bridge.h copies it across).
      if (g_seen.api == device_api::d3d9 && mv.type == resource_type::texture_2d) g_motion9 = reinterpret_cast<IDirect3DTexture9*>(motion.handle);
      if (g_seen.api == device_api::vulkan && mv.type == resource_type::texture_2d) g_motion_vk = motion;
      g_seen.have_motion = true;
      g_seen.mv_width = mv.texture.width;
      g_seen.mv_height = mv.texture.height;
      g_seen.mv_format = mv.texture.format;
    }
  }
  static bool logged_motion = false;
  static int logged_source = -2;
  if (!g_logged || logged_motion != g_seen.have_motion || logged_source != g_seen.source) {
    g_logged = true;
    logged_motion = g_seen.have_motion;
    logged_source = g_seen.source;
    char text[320];
    if (g_seen.have_motion)
      std::snprintf(text, sizeof(text), "%s game, %s, back buffer %ux%u format %u, motion vectors from %s (%ux%u format %u)", BITNESS, ApiName(g_seen.api),
                    g_seen.width, g_seen.height, static_cast<unsigned>(g_seen.back_format), SOURCES[g_seen.source].label, g_seen.mv_width,
                    g_seen.mv_height, static_cast<unsigned>(g_seen.mv_format));
    else
      std::snprintf(text, sizeof(text), "%s game, %s, back buffer %ux%u format %u, motion vectors: %s %s", BITNESS, ApiName(g_seen.api), g_seen.width,
                    g_seen.height, static_cast<unsigned>(g_seen.back_format), SOURCE_NAMES[g_source_setting],
                    g_seen.source >= 0 ? "found but not bound yet" : "not found (yet)");
    Log(text);
  }
}

// Starts, restarts (new window, size or format) or stops the DLSS-G presenter (dlssg.h) to match the game's swap chain.
bool AnyRunning() { return hsf::dlssg::Running(); }
const char* StopAny(DWORD wait_ms = 250) { return hsf::dlssg::Stop(wait_ms); }

// Frame gen OFF keeps the helper running, so ON comes back at once (no start-up) for a quick on/off comparison. The helper's window covers
// the game's, though, so while it runs with generation off it shows only what it manages to pass through, and on a card that struggles with
// it that can be far below the game's rate. The menu then shows a red CLOSE HELPER box beside OFF (g_helper_closed): the helper stops and the game presents by
// itself until frame gen is turned ON again. OFF never starts a helper that is not running. When the helper's GPU guard trips (DLSS-G stopped
// because the GPU could not keep up), frame gen is switched OFF and the helper closed, so the game is back at its own rate at once.
// Frame gen ON with no motion to give DLSS-G (no flow effect bound, and NVOF not making the flow) is treated like a closed helper: DLSS-G
// generates nothing and the helper would only pass frames through. The helper does not start then, and a running one stops once the motion
// has been gone for NO_MOTION_STOP_MS (an effect reload unbinds the texture for a moment, and a restart costs the helper's start-up). The
// NVOF backup counts as motion, so with no flow effect at all the helper starts when the backup takes over.
// Returns true while nothing should start.
constexpr ULONGLONG NO_MOTION_STOP_MS = 3000;
ULONGLONG g_no_motion_since = 0;  // when the motion went away while frame gen was on, 0 while there is motion
bool g_helper_closed = false;     // CLOSE HELPER pressed (or the GPU guard tripped) while frame gen is off; cleared by ON
bool StopHelperWhileOff() {
  if (g_extra_enabled && AnyRunning() && g_dlssg_stats.gpu_hang) {
    g_extra_enabled = false;
    g_helper_closed = true;
    Log("frame gen turned OFF and the helper closed: the GPU could not keep up with DLSS-G; the game shows its own frames again. Turn FRAME GEN on to try again");
  }
  if (g_extra_enabled) g_helper_closed = false;
  const bool no_motion = g_extra_enabled && !g_seen.have_motion && !NvofOn();
  const ULONGLONG now = GetTickCount64();
  if (!no_motion) g_no_motion_since = 0;
  else if (g_no_motion_since == 0) g_no_motion_since = now;
  if (no_motion) {
    if (AnyRunning()) {
      if (now - g_no_motion_since < NO_MOTION_STOP_MS) return false;
      const char* const late = StopAny();
      Log(late != nullptr ? late : "presenter stopped: no motion vectors, the game shows its own frames until a flow effect gives them");
      g_dlssg_stats = {};
    }
    Note("no motion vectors (is the flow effect on?): the game shows its own frames");
    return true;
  }
  if (g_extra_enabled) return false;
  if (AnyRunning() && !g_helper_closed) return false;  // OFF, helper kept for a quick ON
  if (AnyRunning()) {
    const char* const late = StopAny();
    Log(late != nullptr ? late : "presenter stopped: helper closed with frame gen off, the game shows its own frames");
    g_dlssg_stats = {};
  }
  Note("frame gen off: the game shows its own frames");
  return true;
}

// The game swap chain's colour space as DXGI names it, for the helper's swap chain (dlssg.h, SetColorSpace). ReShade keeps it from the game's
// own SetColorSpace1 calls, a mod's (RenoDX's) included; it is read at each present, as the effect runtime in HSF's headers has no getter.
// The mapping is RenoDX's (src/utils/swapchain.hpp, ChangeColorSpace).
std::atomic<uint32_t> g_game_color_space{DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709};

void OnPresentColorSpace(command_queue*, swapchain* swap, const rect*, const rect*, uint32_t, const rect*) {
  if (swap == nullptr) return;
  uint32_t space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  switch (swap->get_color_space()) {
    case reshade::api::color_space::hdr10_st2084: space = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020; break;
    case reshade::api::color_space::extended_srgb_linear: space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709; break;
    case reshade::api::color_space::hdr10_hlg: space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020; break;
    default: break;
  }
  g_game_color_space.store(space, std::memory_order_relaxed);
}

uint32_t GameColorSpace() { return g_game_color_space.load(std::memory_order_relaxed); }

// The colour space straight from the game's DXGI swap chain: the private data that Special K, ReShade and RenoDX all write at SetColorSpace1
// (ReShade's d3d11_impl_swapchain.cpp reads it the same way). RenoDX sets it on the swap chain under ReShade's, so this holds with any ReShade
// version; without the data, ReShade's value.
uint32_t GameColorSpace(IDXGISwapChain* swap) {
  static constexpr GUID SKID_SwapChainColorSpace = {0x18b57e4, 0x1493, 0x4953, {0xad, 0xf2, 0xde, 0x6d, 0x99, 0xcc, 0x5, 0xe5}};
  DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  UINT size = sizeof(space);
  if (swap != nullptr && SUCCEEDED(swap->GetPrivateData(SKID_SwapChainColorSpace, &size, &space)) && size == sizeof(space) &&
      space != DXGI_COLOR_SPACE_CUSTOM)
    return static_cast<uint32_t>(space);
  return GameColorSpace();
}

void UpdatePresenter(device* dev) {
  static HWND running_window = nullptr;
  static D3D11_TEXTURE2D_DESC running{}, refused{};
  static uint32_t running_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  static int running_source = -1;
  if (hsf::dlssg::Reap()) Log("presenter stopped (finished late)");
  if (StopHelperWhileOff()) {
    refused = {};
    return;
  }
  if (AnyRunning() && running_source != g_seen.source) {
    // The motion vector slots are made for one source's size and format: a new source needs a new presenter.
    const char* const late = StopAny();
    Log(late != nullptr ? late : "presenter stopped: new motion source");
    refused = {};
    return;
  }
  // The same for a new motion size (a DLSS mod's data at the back buffer's size after the flow's, Dlss11Motion): the slots keep the first size they got.
  UINT slot_width = 0, slot_height = 0;
  if (AnyRunning() && g_seen.have_motion && hsf::dlssg::MotionSize(&slot_width, &slot_height) &&
      (slot_width != g_seen.mv_width || slot_height != g_seen.mv_height)) {
    const char* const late = StopAny();
    char text[160];
    std::snprintf(text, sizeof(text), "presenter stopped: motion size now %ux%u (the helper's slots are %ux%u)", g_seen.mv_width, g_seen.mv_height, slot_width,
                  slot_height);
    Log(late != nullptr ? late : text);
    refused = {};
    return;
  }
  if (hsf::dlssg::Stopping()) {
    Note("presenter stopping: waiting for its thread");
    return;
  }
  if (!g_native_ok || g_native_swap == nullptr) {
    if (AnyRunning()) {
      const char* const late = StopAny();
      Log(late != nullptr ? late : "presenter stopped");
    }
    refused = {};
    return;
  }
  DXGI_SWAP_CHAIN_DESC swap_desc{};
  BOOL fullscreen = FALSE;
  ID3D11Texture2D* back = nullptr;
  if (FAILED(g_native_swap->GetDesc(&swap_desc)) || FAILED(g_native_swap->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
  D3D11_TEXTURE2D_DESC desc{};
  back->GetDesc(&desc);
  back->Release();
  g_native_swap->GetFullscreenState(&fullscreen, nullptr);
  const auto same = [&](const D3D11_TEXTURE2D_DESC& other) { return other.Width == desc.Width && other.Height == desc.Height && other.Format == desc.Format; };
  const uint32_t space = GameColorSpace(g_native_swap);
  if (AnyRunning() && running_window == swap_desc.OutputWindow && same(running) && space == running_space) return;
  if (fullscreen) {
    if (AnyRunning()) {
      const char* const late = StopAny();
      if (late != nullptr) Log(late);
    }
    Note("exclusive fullscreen: the presenter needs windowed or borderless");
    return;
  }
  if (same(refused)) return;  // failed for this size and format already; a new size or format tries again
  if (AnyRunning()) {
    // New window, size or format: stop first, start on a later frame.
    const char* const late = StopAny();
    Log(late != nullptr ? late : "presenter stopped: the game's swap chain changed");
    return;
  }
  char error[320];
  char text[480];
  ID3D11Device* const native_device = reinterpret_cast<ID3D11Device*>(dev->get_native());
  hsf::dlssg::SetColorSpace(space);
  const bool started = hsf::dlssg::Start(native_device, swap_desc.OutputWindow, desc.Width, desc.Height, desc.Format, g_sl_folder, error, sizeof(error));
  if (started) {
    running_window = swap_desc.OutputWindow;
    running = desc;
    running_space = space;
    running_source = g_seen.source;
    std::snprintf(text, sizeof(text), "DLSS-G presenter started: %ux%u format %u%s%s", desc.Width, desc.Height, static_cast<unsigned>(desc.Format),
                  error[0] != '\0' ? "; " : "", error);
    Note("presenter on: DLSS Frame Generation (log: HSF_streamline\\hsf_dlssg.log)");
  } else {
    refused = desc;
    std::snprintf(text, sizeof(text), "presenter could not start: %s", error);
    Note(text);
  }
  Log(text);
}

// HSF.ini GameQueue1: while DLSS-G runs, the game's device queues at most 1 frame ahead of its GPU (IDXGIDevice1::SetMaximumFrameLatency),
// restored when DLSS-G stops. A game with no limit of its own (AC3: 3 by default) otherwise adds up to 2 frames of latency before Reflex.
// Games with a waitable swap chain (Castlevania) set their own limit on the swap chain; this one does not change theirs.
void UpdateGameQueue(device* dev) {
  static ID3D11Device* applied = nullptr;  // not referenced: only compared, so a gone device is never touched again
  static UINT old = 0;
  ID3D11Device* const game = reinterpret_cast<ID3D11Device*>(dev->get_native());
  const bool want = g_queue_one && hsf::dlssg::Running();
  if (applied != nullptr && (!want || applied != game)) {
    IDXGIDevice1* dxgi = nullptr;
    if (applied == game && SUCCEEDED(game->QueryInterface(IID_PPV_ARGS(&dxgi)))) {
      dxgi->SetMaximumFrameLatency(old);
      dxgi->Release();
      char text[96];
      std::snprintf(text, sizeof(text), "game's frame queue back to %u", old);
      Log(text);
    }
    applied = nullptr;
  }
  if (want && applied == nullptr) {
    IDXGIDevice1* dxgi = nullptr;
    if (FAILED(game->QueryInterface(IID_PPV_ARGS(&dxgi)))) return;
    old = 0;
    dxgi->GetMaximumFrameLatency(&old);
    const HRESULT hr = dxgi->SetMaximumFrameLatency(1);
    dxgi->Release();
    applied = game;
    char text[120];
    std::snprintf(text, sizeof(text), "game's frame queue: at most 1 frame (was %u): 0x%08lx", old, hr);
    Log(text);
  }
}

// Hands the score this finished frame and every motion source found, each with what its values mean. The frame is taken here, before ReShade's
// overlay is drawn, so HSF's own numbers are never rated. A source that nothing asks to run (a switched-off effect) scores as stale motion.
void UpdateScore(effect_runtime* runtime, device* dev, resource target) {
  if (!ScoreRuns()) return;
  hsf::score::Input inputs[hsf::score::MAX_INPUTS];
  for (int i = 0; i < SOURCE_COUNT; ++i) {
    if (g_source_variables[i].handle == 0) continue;
    resource_view srv{}, srv_srgb{};
    runtime->get_texture_binding(g_source_variables[i], &srv, &srv_srgb);
    if (srv.handle == 0) continue;
    const resource motion = dev->get_resource_from_view(srv);
    const resource_desc desc = dev->get_resource_desc(motion);
    const int convention = g_motion_convention != 0 ? g_motion_convention : SOURCES[i].convention;
    if (desc.type != resource_type::texture_2d || convention == 0) continue;
    inputs[i].motion = reinterpret_cast<ID3D11Texture2D*>(motion.handle);
    hsf::motion::ConventionScale(convention, static_cast<float>(g_seen.width), static_cast<float>(g_seen.height), static_cast<float>(desc.texture.width),
                                  static_cast<float>(desc.texture.height), inputs[i].to_pixels);
  }
  hsf::score::Frame(reinterpret_cast<ID3D11Device*>(dev->get_native()), reinterpret_cast<ID3D11Texture2D*>(target.handle), inputs);
}

// ---- What the game writes to its constant buffers (camera.h) ----
// The buffer's size, cached, as these events can come thousands of times a frame. A resource's handle can come back as another buffer once the old one is gone, so a destroyed
// resource empties the caches: a wrong size here would read past the mapped memory.
std::atomic<uint32_t> g_buffer_generation{1};

uint32_t ConstantBufferWidth(device* dev, resource buffer) {
  struct Entry {
    uint64_t handle;
    uint32_t width;
  };
  static thread_local Entry cache[256];
  static thread_local uint32_t generation = 0;
  const uint32_t now = g_buffer_generation.load(std::memory_order_acquire);
  if (generation != now) {
    std::memset(cache, 0, sizeof(cache));
    generation = now;
  }
  Entry& e = cache[(buffer.handle >> 4) & 255];
  if (e.handle == buffer.handle) return e.width;
  const resource_desc desc = dev->get_resource_desc(buffer);
  e.handle = buffer.handle;
  e.width = desc.type == resource_type::buffer && (desc.usage & resource_usage::constant_buffer) != 0 && desc.buffer.size <= hsf::camera::Scanner::MAX_WIDTH ? static_cast<uint32_t>(desc.buffer.size) : 0;
  return e.width;
}

void OnDestroyResource(device*, resource) { g_buffer_generation.fetch_add(1, std::memory_order_acq_rel); }

// Maps open now. Few at a time; a map is read when it closes, once the game has written it.
struct OpenMap {
  uint64_t handle;
  const void* data;
  uint32_t width;
};
thread_local OpenMap t_open_maps[8];

void OnMapBuffer(device* dev, resource buffer, uint64_t offset, uint64_t, map_access access, void** data) {
  if (!g_camera_listening.load(std::memory_order_relaxed) || access == map_access::read_only || offset != 0 || data == nullptr || *data == nullptr) return;
  const uint32_t width = ConstantBufferWidth(dev, buffer);
  if (!g_scanner.Wants(width)) return;
  for (OpenMap& m : t_open_maps)
    if (m.handle == 0 || m.handle == buffer.handle) {
      m = {buffer.handle, *data, width};
      return;
    }
}

void OnUnmapBuffer(device*, resource buffer) {
  if (!g_camera_listening.load(std::memory_order_relaxed)) {
    for (OpenMap& m : t_open_maps) m.handle = 0;
    return;
  }
  for (OpenMap& m : t_open_maps)
    if (m.handle == buffer.handle) {
      g_scanner.Write(m.width, m.data, 0, m.width);
      m.handle = 0;
      return;
    }
}

bool OnUpdateBuffer(device* dev, const void* data, resource buffer, uint64_t offset, uint64_t size) {
  if (!g_camera_listening.load(std::memory_order_relaxed) || data == nullptr || size < 64) return false;
  const uint32_t width = ConstantBufferWidth(dev, buffer);
  if (!g_scanner.Wants(width)) return false;
  const uint64_t bytes = size == UINT64_MAX ? width : size;
  if (offset < width && bytes <= width - offset) g_scanner.Write(width, data, static_cast<uint32_t>(offset), static_cast<uint32_t>(bytes));
  return false;
}

// The game's profile: <folder of HSF.ini>\HSF_profiles\<exe>.ini, the matrix that won before, so the next run finds it sooner. It still has to pass the same test.
std::wstring CameraProfilePath() {
  std::wstring folder = g_ini_path;
  const size_t cut = folder.find_last_of(L"\\/");
  folder = cut == std::wstring::npos ? L"" : folder.substr(0, cut + 1);
  folder += L"HSF_profiles";
  CreateDirectoryW(folder.c_str(), nullptr);
  wchar_t exe[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const wchar_t* const name = std::max(wcsrchr(exe, L'\\'), wcsrchr(exe, L'/'));
  return folder + L"\\" + (name != nullptr ? name + 1 : exe) + L".ini";
}

void StopRecording() {
  if (!g_record_file.IsOpen()) return;
  g_scanner.SetRecording(false);
  char line[96];
  std::snprintf(line, sizeof(line), "camera record: done, %u frames", g_record_file.Frames());
  Log(line);
  g_record_file.Close();
  MessageBeep(MB_OK);
}

// Game thread: starts and stops a recording.
void UpdateRecording(bool can_record) {
  const bool toggle = g_record_request;
  g_record_request = false;
  if (g_record_file.IsOpen()) {
    if (toggle || !can_record || GetTickCount64() >= g_record_until) StopRecording();
    return;
  }
  if (!toggle) return;
  if (!can_record) {
    Log("camera record: needs DLSS-G running and exact camera motion on");
    return;
  }
  std::wstring folder = g_ini_path;
  const size_t cut = folder.find_last_of(L"\\/");
  folder = cut == std::wstring::npos ? L"" : folder.substr(0, cut + 1);
  folder += L"HSF_recordings";
  CreateDirectoryW(folder.c_str(), nullptr);
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t name[64];
  swprintf(name, std::size(name), L"\\%04u%02u%02u-%02u%02u%02u-matrices.bin", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
  char line[128];
  if (g_record_file.Open((folder + name).c_str(), g_record_aspect)) {
    g_scanner.SetRecording(true);
    g_record_until = GetTickCount64() + RECORD_MS;
    std::snprintf(line, sizeof(line), "camera record: started, %02u:%02u:%02u, for %u s", t.wHour, t.wMinute, t.wSecond, static_cast<unsigned>(RECORD_MS / 1000));
    MessageBeep(MB_OK);
  } else {
    std::snprintf(line, sizeof(line), "camera record: could not open a file in HSF_recordings");
  }
  Log(line);
}

// Game thread, once per frame: follows the helper's lock.
void UpdateCamera() {
  const bool want = g_camera_enabled && hsf::dlssg::Feeding();
  g_camera_listening.store(want, std::memory_order_relaxed);
  UpdateRecording(want);
  if (!want) return;
  if (!g_camera_hint_read) {
    g_camera_hint_read = true;
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"Camera", L"Matrix", L"0", text, 32, CameraProfilePath().c_str());
    g_camera_saved = _wtoi64(text);
    hsf::dlssg::SetCameraHint(g_camera_saved);
    if (g_camera_saved != 0)
{
      char line[128];
      std::snprintf(line, sizeof(line), "camera: this game's profile has a matrix in a constant buffer of %u bytes at offset %u", hsf::camera::KeyWidth(g_camera_saved), hsf::camera::KeyOffset(g_camera_saved));
      Log(line);
    }
  }
  const int64_t locked = g_dlssg_stats.camera_locked;
  if (locked != g_camera_locked) {
    g_camera_locked = locked;
    g_scanner.Lock(locked);
    Log(locked != 0 ? "camera: matrix locked, the scan stops" : "camera: matrix let go, scanning again");
    if (locked != 0 && locked != g_camera_saved) {
      g_camera_saved = locked;
      wchar_t text[32];
      swprintf(text, std::size(text), L"%lld", static_cast<long long>(locked));
      WritePrivateProfileStringW(L"Camera", L"Matrix", text, CameraProfilePath().c_str());
    }
  }
  // What the scan sees, for a game where nothing locks.
  const ULONGLONG now = GetTickCount64();
  if (now - g_camera_log >= 10000) {
    g_camera_log = now;
    float aspect = 0;
    const uint32_t wrong = g_scanner.TakeWrongAspect(aspect);
    const hsf::camera::ScanStats scan = g_scanner.TakeStats();
    char text[1024];
    std::snprintf(text, sizeof(text), "camera: scan read %.1f MB in 10 s over %u frames, %.2f ms a frame; %u writes, %u unread for their size's budget, %u for the frame's; up to %d matrices a frame, %d last frame, %u left out for their size's limit, %u for the frame's; %u camera-shaped ones of a very different aspect (last %.3f)",
                  static_cast<double>(scan.bytes) / 1048576.0, scan.frames, scan.frames != 0 ? scan.ms / scan.frames : 0.0, scan.writes, scan.unread_for_size, scan.unread_for_total, scan.most, g_scanner.LastCount(), scan.left_for_size, scan.left_for_total, wrong, aspect);
    Log(text);
    if (scan.writes != 0) {
      int used = std::snprintf(text, sizeof(text), "camera: %d buffer sizes in the last scanned frame, least written first, bytes x writes (matrices):", scan.widths);
      for (int i = 0; i < scan.widths && used > 0 && used < static_cast<int>(sizeof(text)); ++i)
        used += std::snprintf(text + used, sizeof(text) - used, " %ux%u(%u)", scan.all[i].width, scan.all[i].writes, scan.all[i].matrices);
      Log(text);
    }
  }
}

// This frame's depth for DLSS-G. Works only while something uses it: DLSS-G's input or its debug view. HSF.fx's technique makes ReShade's effect use the
// DEPTH texture; what that texture is bound to is the depth buffer Generic Depth picked.
// A square depth buffer under a picture that is not square is a shadow map, not the game's view: ReShade picks one now and then when its depth
// shape check is off (emulators). DLSS-G then gets a constant depth instead of a wrong one.
bool ShadowMapBound(device* dev, resource_view srv) {
  if (srv.handle == 0 || g_seen.width == g_seen.height) return false;
  const resource_desc d = dev->get_resource_desc(dev->get_resource_from_view(srv));
  if (d.type != resource_type::texture_2d || d.texture.width != d.texture.height) return false;
  static ULONGLONG next_log = 0;
  const ULONGLONG now = GetTickCount64();
  if (now >= next_log) {
    next_log = now + 60000;
    char text[160];
    std::snprintf(text, sizeof(text), "depth: ReShade bound a square %ux%u depth buffer (a shadow map); DLSS-G gets a constant depth for those frames",
                  d.texture.width, d.texture.height);
    Log(text);
  }
  return true;
}

void UpdateDepth(effect_runtime* runtime, device* dev, resource target) {
  g_depth_texture = nullptr;
  const bool to_dlssg = g_depth_enabled && !g_depth_ab_off && hsf::dlssg::Feeding();
  if (!to_dlssg && g_score_view != hsf::score::View::depth) return;
  if (g_depth_variable.handle == 0) {
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_depth_search) return;
    g_next_depth_search = now + 1000;
    g_depth_variable = runtime->find_texture_variable("HSF.fx", "HSFDepth");
    if (g_depth_variable.handle == 0) return;
    Log("depth: texture variable HSFDepth found");
  }
  const effect_technique technique = runtime->find_technique("HSF.fx", "HSF_Depth");
  if (technique.handle != 0 && !runtime->get_technique_state(technique)) {
    runtime->set_technique_state(technique, true);
    Log("switched on HSF_Depth (HSF.fx), which uses ReShade's depth buffer");
  }
  resource_view srv{}, srv_srgb{};
  runtime->get_texture_binding(g_depth_variable, &srv, &srv_srgb);
  if (ShadowMapBound(dev, srv)) srv = {0};
  if (g_dlss11_depth != nullptr) srv = {reinterpret_cast<uint64_t>(g_dlss11_depth)};  // the game's own DLSS depth (Dlss11Motion)
  // On Direct3D 11 a ReShade view handle is the native view. DLSS-G has one size for motion and depth, so depth takes the motion vectors'.
  hsf::depth::SetUpsideDown(g_depth_upside_down_setting);
  // A depth buffer of another shape than the picture (an emulator's 16:9 picture centred on a wider screen) goes where the picture is.
  hsf::depth::SetFitPicture(true);
  ID3D11Texture2D* const copy = hsf::depth::Frame(reinterpret_cast<ID3D11Device*>(dev->get_native()), reinterpret_cast<ID3D11ShaderResourceView*>(srv.handle),
                                                  g_seen.have_motion ? g_seen.mv_width : g_seen.width, g_seen.have_motion ? g_seen.mv_height : g_seen.height,
                                                  reinterpret_cast<ID3D11Texture2D*>(target.handle));
  g_depth_inverted = g_depth_inverted_setting >= 0 ? g_depth_inverted_setting != 0 : hsf::depth::Current().inverted;
  if (to_dlssg && g_seen.have_motion) g_depth_texture = copy;
  static int fitted_logged = -1;
  const hsf::depth::Status status = hsf::depth::Current();
  if (status.source && fitted_logged != (status.fitted ? 1 : 0)) {
    fitted_logged = status.fitted ? 1 : 0;
    char text[160];
    std::snprintf(text, sizeof(text), status.fitted ? "depth: %ux%u has another shape than the picture; put into the picture's middle" : "depth: %ux%u has the picture's shape",
                  status.width, status.height);
    Log(text);
  }
  if (hsf::depth::Current().flat_for_long && !g_flat_depth_noticed) {
    g_flat_depth_noticed = true;
    reshade::get_config_value(nullptr, "DEPTH", "DepthCopyBeforeClears", g_copy_before_clears);
    Log(g_copy_before_clears ? "depth: a buffer is bound but has only ever been flat, even with DepthCopyBeforeClears=1; no usable depth here so far"
                             : "depth: a buffer is bound but has only ever been flat; if this is gameplay, the game may clear it before the frame ends: try DepthCopyBeforeClears (HSF panel)");
  }
}

// ReShade draws its overlays after this event and before the native Present, so this copy is the frame without them.
void UpdateCleanFrame(device* dev, resource target) {
  g_clean_ready = false;
  if (!g_keep_overlays_out || !hsf::dlssg::Feeding()) return;
  auto* const frame = reinterpret_cast<ID3D11Texture2D*>(target.handle);
  D3D11_TEXTURE2D_DESC d{};
  frame->GetDesc(&d);
  if (d.SampleDesc.Count != 1) return;
  if (g_clean_frame != nullptr) {
    D3D11_TEXTURE2D_DESC have{};
    g_clean_frame->GetDesc(&have);
    if (have.Width != d.Width || have.Height != d.Height || have.Format != d.Format) {
      g_clean_frame->Release();
      g_clean_frame = nullptr;
    }
  }
  auto* const device11 = reinterpret_cast<ID3D11Device*>(dev->get_native());
  if (g_clean_frame == nullptr) {
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = 0;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    const HRESULT hr = device11->CreateTexture2D(&d, nullptr, &g_clean_frame);
    if (FAILED(hr)) {
      static bool logged = false;
      if (!logged) Log("the copy of the frame without overlays could not be made");
      logged = true;
      return;
    }
  }
  ID3D11DeviceContext* context = nullptr;
  device11->GetImmediateContext(&context);
  context->CopyResource(g_clean_frame, frame);
  context->Release();
  g_clean_ready = true;
}

ID3D11DeviceContext* ImmediateContext(command_list* cmd_list) {
  if (cmd_list->get_device()->get_api() != device_api::d3d11) return nullptr;
  auto* const context = reinterpret_cast<ID3D11DeviceContext*>(cmd_list->get_native());
  return context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE ? context : nullptr;
}

#ifdef HSF_SL_GAME
// The game's HUD in a native D3D12 game. The same rule as hud.h's Watch, per back buffer, as frames overlap: the first blended draw with no depth test on the
// back buffer since the last pass that was not HUD. There Streamline copies the back buffer on the game's own list, and that copy is DLSS-G's HUD-free picture.
// Lists are recorded on many threads, so what is bound is kept per thread. Blend and depth come from the pipeline, known from when the game made it.
// A game may put its HUD on a texture of its own and pass that on to the back buffer, as Shadows does: one blended batch of thousands of vertices, with no depth, on a texture of the
// back buffer's size and format. Such a batch counts as the HUD's start too, once per frame of evaluates. reshade_present runs for generated frames as well, so evaluates mark the frames.
// If that texture holds only the HUD, or a last pass changes its colours, this picture is wrong: that shows at once by eye.
// Every 10 s one frame's passes go to the log, so a game whose HUD is not found can be read from it.
constexpr int SLH_BUFFERS = 8;
constexpr uint8_t SLH_BLENDED = 1, SLH_DEPTH = 2;
struct SlhPass {
  uint32_t width, height, format, thread, draws, hud, unknown, first_vertices;
  int buffer;
  bool depth, after_vectors, full;
};
constexpr uint32_t SLH_PASSES = 256;
constexpr uint32_t SLH_NO_PASS = UINT32_MAX;
struct SlhThread {
  command_list* list = nullptr;
  uint64_t pipeline = 0;
  int buffer = -1;
  bool depth = false;
  bool full = false;         // a texture of the back buffer's size and format
  uint64_t target = 0;
  bool render_pass = false;  // inside a D3D12 render pass, where nothing may be copied
  uint32_t pass = SLH_NO_PASS;
};
thread_local SlhThread t_slh;
std::atomic<bool> g_slh_on{false};
std::atomic<bool> g_slh_tagging{false};
std::atomic<bool> g_slh_sampling{false};
std::atomic<bool> g_slh_vectors_seen{false};  // in the frame being sampled
std::atomic<uint64_t> g_slh_back[SLH_BUFFERS];
std::atomic<uint32_t> g_slh_format{0};
std::atomic<bool> g_slh_had_tag{false};       // DLSS-G holds a picture from us
std::atomic<bool> g_slh_tagged_frame{false};  // ... from this frame
std::atomic<uint64_t> g_slh_evaluates{0}, g_slh_found_at{0}, g_slh_batch_vertices{0}, g_slh_sample_from{0};
std::atomic<uint32_t> g_slh_width{0}, g_slh_height{0};
std::atomic<uint64_t> g_slh_copies_onto_back{0}, g_slh_uav_back{0}, g_slh_in_render_pass{0};
std::atomic<uint64_t> g_slh_tags{0}, g_slh_frames{0}, g_slh_found{0}, g_slh_unknown{0};
SRWLOCK g_slh_lock = SRWLOCK_INIT;  // the watches and the sampled passes
hsf::hud::Watch g_slh_watch[SLH_BUFFERS];
uint32_t g_slh_age[SLH_BUFFERS] = {};
SlhPass g_slh_passes[SLH_PASSES];
uint32_t g_slh_pass_count = 0;
SRWLOCK g_slh_pipe_lock = SRWLOCK_INIT;
std::unordered_map<uint64_t, uint8_t> g_slh_pipelines;

int SlhBufferOf(uint64_t handle) {
  if (handle == 0) return -1;
  for (int i = 0; i < SLH_BUFFERS; ++i)
    if (g_slh_back[i].load(std::memory_order_relaxed) == handle) return i;
  return -1;
}

void SlhTarget(command_list* cmd_list, resource_view rtv, bool depth) {
  SlhThread& t = t_slh;
  t.list = cmd_list;
  t.depth = depth;
  t.render_pass = false;
  t.pass = SLH_NO_PASS;
  device* const dev = cmd_list->get_device();
  const resource target = rtv.handle != 0 ? dev->get_resource_from_view(rtv) : resource{0};
  t.buffer = SlhBufferOf(target.handle);
  t.target = target.handle;
  t.full = false;
  resource_desc desc{};
  if (target.handle != 0) {
    desc = dev->get_resource_desc(target);
    t.full = t.buffer < 0 && desc.texture.width == g_slh_width.load(std::memory_order_relaxed) && desc.texture.height == g_slh_height.load(std::memory_order_relaxed) &&
             static_cast<uint32_t>(desc.texture.format) == g_slh_format.load(std::memory_order_relaxed);
  }
  if (g_slh_sampling.load(std::memory_order_relaxed)) {
    SlhPass pass{};
    pass.thread = GetCurrentThreadId();
    pass.buffer = t.buffer;
    pass.depth = depth;
    pass.after_vectors = g_slh_vectors_seen.load(std::memory_order_relaxed);
    pass.full = t.full;
    if (target.handle != 0) {
      pass.width = desc.texture.width;
      pass.height = desc.texture.height;
      pass.format = static_cast<uint32_t>(desc.texture.format);
    }
    AcquireSRWLockExclusive(&g_slh_lock);
    t.pass = g_slh_pass_count++;
    g_slh_passes[t.pass % SLH_PASSES] = pass;
    ReleaseSRWLockExclusive(&g_slh_lock);
  }
}

void SlhDraw(command_list* cmd_list, uint32_t vertices) {
  SlhThread& t = t_slh;
  if (t.list != cmd_list || (t.buffer < 0 && !t.full && t.pass == SLH_NO_PASS)) return;
  bool known = false, blended = false, depth_test = t.depth;
  AcquireSRWLockShared(&g_slh_pipe_lock);
  const auto found = g_slh_pipelines.find(t.pipeline);
  if (found != g_slh_pipelines.end()) {
    known = true;
    blended = (found->second & SLH_BLENDED) != 0;
    depth_test = t.depth && (found->second & SLH_DEPTH) != 0;
  }
  ReleaseSRWLockShared(&g_slh_pipe_lock);
  bool copy = false;
  AcquireSRWLockExclusive(&g_slh_lock);
  if (t.pass != SLH_NO_PASS && t.pass < g_slh_pass_count && g_slh_pass_count - t.pass <= SLH_PASSES) {
    SlhPass& pass = g_slh_passes[t.pass % SLH_PASSES];
    if (pass.draws == 0) pass.first_vertices = vertices;
    ++pass.draws;
    if (blended && !depth_test) ++pass.hud;
    if (!known) ++pass.unknown;
  }
  if (t.buffer >= 0) copy = g_slh_watch[t.buffer].Draw(blended, depth_test, vertices);
  ReleaseSRWLockExclusive(&g_slh_lock);
  if (!known && t.buffer >= 0) g_slh_unknown.fetch_add(1, std::memory_order_relaxed);
  if (!copy && t.full && blended && !depth_test && vertices > hsf::hud::Watch::POST_VERTICES) {
    g_slh_batch_vertices.store(vertices, std::memory_order_relaxed);
    const uint64_t frame = g_slh_evaluates.load(std::memory_order_relaxed);
    copy = g_slh_found_at.exchange(frame, std::memory_order_relaxed) != frame;
  }
  if (!copy) return;
  g_slh_found.fetch_add(1, std::memory_order_relaxed);
  if (t.render_pass) {
    g_slh_in_render_pass.fetch_add(1, std::memory_order_relaxed);
  } else if (g_slh_tagging.load(std::memory_order_relaxed) && hsf::slgame::TagHudless(reinterpret_cast<void*>(cmd_list->get_native()), reinterpret_cast<void*>(t.target), g_slh_width.load(std::memory_order_relaxed), g_slh_height.load(std::memory_order_relaxed))) {
    g_slh_tags.fetch_add(1, std::memory_order_relaxed);
    g_slh_tagged_frame.store(true, std::memory_order_relaxed);
    g_slh_had_tag.store(true, std::memory_order_relaxed);
  }
}

void OnSlhInitPipeline(device*, pipeline_layout, uint32_t count, const pipeline_subobject* subobjects, pipeline made) {
  uint8_t flags = SLH_DEPTH;  // no depth state means the default, which tests
  for (uint32_t i = 0; i < count; ++i) {
    if (subobjects[i].type == pipeline_subobject_type::blend_state && subobjects[i].data != nullptr) {
      if (static_cast<const blend_desc*>(subobjects[i].data)->blend_enable[0]) flags |= SLH_BLENDED;
    } else if (subobjects[i].type == pipeline_subobject_type::depth_stencil_state && subobjects[i].data != nullptr) {
      if (!static_cast<const depth_stencil_desc*>(subobjects[i].data)->depth_enable) flags = static_cast<uint8_t>(flags & ~SLH_DEPTH);
    }
  }
  AcquireSRWLockExclusive(&g_slh_pipe_lock);
  g_slh_pipelines[made.handle] = flags;
  ReleaseSRWLockExclusive(&g_slh_pipe_lock);
}
void OnSlhDestroyPipeline(device*, pipeline gone) {
  AcquireSRWLockExclusive(&g_slh_pipe_lock);
  g_slh_pipelines.erase(gone.handle);
  ReleaseSRWLockExclusive(&g_slh_pipe_lock);
}
void OnSlhBindPipeline(command_list* cmd_list, pipeline_stage, pipeline bound) {
  if (!g_slh_on.load(std::memory_order_relaxed)) return;
  SlhThread& t = t_slh;
  if (t.list != cmd_list) {
    t = SlhThread{};
    t.list = cmd_list;
  }
  t.pipeline = bound.handle;
}
void OnSlhRenderPass(command_list* cmd_list, uint32_t count, const render_pass_render_target_desc* rts, const render_pass_depth_stencil_desc* ds) {
  if (!g_slh_on.load(std::memory_order_relaxed)) return;
  const uint64_t pipeline = t_slh.list == cmd_list ? t_slh.pipeline : 0;
  SlhTarget(cmd_list, count != 0 && rts != nullptr ? rts[0].view : resource_view{0}, ds != nullptr && ds->view.handle != 0);
  t_slh.pipeline = pipeline;
  t_slh.render_pass = true;
}
// How else the back buffer may be written, for the log.
void SlhCopy(resource dest) {
  if (g_slh_on.load(std::memory_order_relaxed) && SlhBufferOf(dest.handle) >= 0) g_slh_copies_onto_back.fetch_add(1, std::memory_order_relaxed);
}
bool OnSlhCopyResource(command_list*, resource, resource dest) {
  SlhCopy(dest);
  return false;
}
bool OnSlhCopyRegion(command_list*, resource, uint32_t, const subresource_box*, resource dest, uint32_t, const subresource_box*, filter_mode) {
  SlhCopy(dest);
  return false;
}
void OnSlhBarrier(command_list*, uint32_t count, const resource* resources, const resource_usage*, const resource_usage* new_states) {
  if (!g_slh_on.load(std::memory_order_relaxed)) return;
  for (uint32_t i = 0; i < count; ++i)
    if (new_states[i] == resource_usage::unordered_access && SlhBufferOf(resources[i].handle) >= 0) g_slh_uav_back.fetch_add(1, std::memory_order_relaxed);
}

void SlhLog() {
  char line[360];
  std::snprintf(line, sizeof(line), "HUD: found on %llu of %llu frames, tagged %llu, in use %d; back buffer %ux%u format %u; last batch %llu vertices; draws on it with an unknown pipeline %llu, copies onto it %llu, made writable by compute %llu, found inside a render pass and left %llu",
                g_slh_found.load(), g_slh_evaluates.load(), g_slh_tags.load(), g_slh_tagging.load() ? 1 : 0, g_slh_width.load(), g_slh_height.load(), g_slh_format.load(), g_slh_batch_vertices.load(), g_slh_unknown.load(), g_slh_copies_onto_back.load(), g_slh_uav_back.load(), g_slh_in_render_pass.load());
  hsf::slgame::LogLine(line);
  AcquireSRWLockShared(&g_slh_lock);
  const uint32_t count = g_slh_pass_count;
  std::string text;
  for (uint32_t i = count > SLH_PASSES ? count - SLH_PASSES : 0; i < count; ++i) {
    const SlhPass& p = g_slh_passes[i % SLH_PASSES];
    char one[140];
    std::snprintf(one, sizeof(one), " [%s%ux%u fmt %u, t%u, %u draws, first %u vertices, %u HUD-like, %u unknown%s%s]", p.buffer >= 0 ? "BACK " : p.full ? "FULL " : "", p.width, p.height, p.format, p.thread, p.draws, p.first_vertices, p.hud, p.unknown, p.depth ? ", depth" : "", p.after_vectors ? ", after vectors" : "");
    if (text.size() + std::strlen(one) > 240) {
      hsf::slgame::LogLine(("HUD: passes:" + text).c_str());
      text.clear();
    }
    text += one;
  }
  ReleaseSRWLockShared(&g_slh_lock);
  hsf::slgame::LogLine(("HUD: passes, of " + std::to_string(count) + " bound:" + text).c_str());
}

// With each frame's vectors. A frame gone by with no picture of its own: DLSS-G must not be left with an old one.
void SlhMarkVectors() {
  g_slh_vectors_seen.store(true, std::memory_order_relaxed);
  g_slh_evaluates.fetch_add(1, std::memory_order_relaxed);
  if (!g_slh_tagged_frame.exchange(false, std::memory_order_relaxed) && g_slh_had_tag.exchange(false, std::memory_order_relaxed)) hsf::slgame::ClearHudless();
}

// On the thread that presents, for the frame being presented.
void SlhAtPresent(effect_runtime* runtime) {
  static ULONGLONG s_next_log = 0;
  const bool want = (g_sl_hud_watch || g_sl_hud) && runtime->get_device()->get_api() == device_api::d3d12;
  if (!want) {
    g_slh_on.store(false, std::memory_order_relaxed);
    g_slh_tagging.store(false, std::memory_order_relaxed);
    if (g_slh_had_tag.exchange(false, std::memory_order_relaxed)) hsf::slgame::ClearHudless();
    return;
  }
  const uint32_t buffers = std::min<uint32_t>(runtime->get_back_buffer_count(), SLH_BUFFERS);
  for (uint32_t i = 0; i < SLH_BUFFERS; ++i) g_slh_back[i].store(i < buffers ? runtime->get_back_buffer(i).handle : 0, std::memory_order_relaxed);
  const uint32_t index = runtime->get_current_back_buffer_index();
  if (buffers != 0 && index < buffers) {
    const resource_desc desc = runtime->get_device()->get_resource_desc(runtime->get_back_buffer(index));
    g_slh_format.store(static_cast<uint32_t>(desc.texture.format), std::memory_order_relaxed);
    g_slh_width.store(desc.texture.width, std::memory_order_relaxed);
    g_slh_height.store(desc.texture.height, std::memory_order_relaxed);
  }
  if (index < SLH_BUFFERS) {
    AcquireSRWLockExclusive(&g_slh_lock);
    g_slh_watch[index].BeginFrame();
    // A buffer never named as current is still cleared in time, so it cannot hold on to a frame that is over.
    for (uint32_t i = 0; i < SLH_BUFFERS; ++i) {
      if (i == index) {
        g_slh_age[i] = 0;
      } else if (++g_slh_age[i] > 2 * SLH_BUFFERS) {
        g_slh_age[i] = 0;
        g_slh_watch[i].BeginFrame();
      }
    }
    ReleaseSRWLockExclusive(&g_slh_lock);
  }
  g_slh_frames.fetch_add(1, std::memory_order_relaxed);
  g_slh_on.store(true, std::memory_order_relaxed);
  g_slh_tagging.store(g_sl_hud, std::memory_order_relaxed);
  if (!g_sl_hud && g_slh_had_tag.exchange(false, std::memory_order_relaxed)) hsf::slgame::ClearHudless();
  // Every ten seconds the passes of two real frames are sampled: presents come for generated frames too, so evaluates count.
  const ULONGLONG now = GetTickCount64();
  if (g_slh_sampling.load(std::memory_order_relaxed) && g_slh_evaluates.load(std::memory_order_relaxed) >= g_slh_sample_from.load(std::memory_order_relaxed) + 2) {
    g_slh_sampling.store(false, std::memory_order_relaxed);
    SlhLog();
  }
  if (now >= s_next_log) {
    s_next_log = now + 10000;
    AcquireSRWLockExclusive(&g_slh_lock);
    g_slh_pass_count = 0;
    ReleaseSRWLockExclusive(&g_slh_lock);
    g_slh_vectors_seen.store(false, std::memory_order_relaxed);
    g_slh_sample_from.store(g_slh_evaluates.load(std::memory_order_relaxed), std::memory_order_relaxed);
    g_slh_sampling.store(true, std::memory_order_relaxed);
  }
}
#endif

void OnBindTargets(command_list* cmd_list, uint32_t count, const resource_view* rtvs, resource_view dsv) {
#ifdef HSF_SL_GAME
  if (g_slh_on.load(std::memory_order_relaxed)) {
    const uint64_t pipeline = t_slh.list == cmd_list ? t_slh.pipeline : 0;
    SlhTarget(cmd_list, count != 0 && rtvs != nullptr ? rtvs[0] : resource_view{0}, dsv.handle != 0);
    t_slh.pipeline = pipeline;
    return;
  }
#endif
  if (!g_hud_listening.load(std::memory_order_relaxed) || ImmediateContext(cmd_list) == nullptr) return;
  device* const dev = cmd_list->get_device();
  const bool has_target = count != 0 && rtvs != nullptr && rtvs[0].handle != 0;
  const resource target = has_target ? dev->get_resource_from_view(rtvs[0]) : resource{0};
  const bool back = has_target && target.handle == g_hud_back;
  g_hud_on_back.store(back, std::memory_order_relaxed);
  g_hud_depth_bound = dsv.handle != 0;
  // A target like the one the game's last pass read last frame. Pools of targets swap about, so the texture itself may differ.
  g_hud_target = nullptr;
  bool source = false;
  if (has_target && !back && g_hud_source_width != 0) {
    const resource_desc desc = dev->get_resource_desc(target);
    if (desc.type == resource_type::texture_2d && desc.texture.width == g_hud_source_width && desc.texture.height == g_hud_source_height && desc.texture.format == g_hud_source_format) {
      source = true;
      g_hud_target = reinterpret_cast<ID3D11Texture2D*>(target.handle);
    }
  }
  g_hud_on_source.store(source, std::memory_order_relaxed);
  g_hud_pass_fresh = source && g_hud_depth_bound;
  g_hud_in_pass = false;
  if (g_hud_logging.load(std::memory_order_relaxed)) {
    HudPass pass{};
    pass.back = back;
    pass.depth = g_hud_depth_bound;
    if (has_target) {
      D3D11_TEXTURE2D_DESC d{};
      ComPtr<ID3D11Texture2D> texture;
      if (SUCCEEDED(reinterpret_cast<ID3D11Resource*>(target.handle)->QueryInterface(IID_PPV_ARGS(&texture)))) {
        texture->GetDesc(&d);
        pass.width = d.Width;
        pass.height = d.Height;
        pass.format = d.Format;
      }
    }
    g_hud_passes[g_hud_pass_count++ % HUD_PASSES] = pass;
  }
}

// The biggest 2D texture this draw's pixel shader reads, and its slot. On the last pass that is the picture.
int FindPicture(ID3D11DeviceContext* context, ComPtr<ID3D11ShaderResourceView>& view, ComPtr<ID3D11Texture2D>& texture) {
  ID3D11ShaderResourceView* views[8] = {};
  context->PSGetShaderResources(0, 8, views);
  int slot = -1;
  UINT64 biggest = 0;
  for (int i = 0; i < 8; ++i) {
    if (views[i] == nullptr) continue;
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> t;
    views[i]->GetResource(&resource);
    if (resource != nullptr && SUCCEEDED(resource.As(&t))) {
      D3D11_TEXTURE2D_DESC d{};
      t->GetDesc(&d);
      const UINT64 size = static_cast<UINT64>(d.Width) * d.Height;
      if (d.SampleDesc.Count == 1 && d.ArraySize == 1 && size > biggest) {
        biggest = size;
        slot = i;
        view = views[i];
        texture = t;
      }
    }
    views[i]->Release();
  }
  return slot;
}

// The game's last pass, about to draw `from_slot`'s picture to the back buffer: the same pass again, from the copy without the HUD, into HSF's own texture.
void ReplayWithoutHud(ID3D11DeviceContext* context, int slot, const std::function<void()>& draw) {
  ID3D11RenderTargetView* targets[8] = {};
  ID3D11DepthStencilView* depth = nullptr;
  context->OMGetRenderTargets(8, targets, &depth);
  D3D11_RENDER_TARGET_VIEW_DESC game{};
  D3D11_RENDER_TARGET_VIEW_DESC ours{};
  if (targets[0] != nullptr) targets[0]->GetDesc(&game);
  g_hud_remover.BeforeHudTarget()->GetDesc(&ours);
  if (targets[0] != nullptr && game.Format == ours.Format) {
    ID3D11ShaderResourceView* picture = nullptr;
    context->PSGetShaderResources(slot, 1, &picture);
    g_hud_spans.Begin(context);
    context->CopyResource(g_hud_remover.BeforeHud(), reinterpret_cast<ID3D11Texture2D*>(g_hud_back));  // what the pass does not touch stays the same
    ID3D11RenderTargetView* const target = g_hud_remover.BeforeHudTarget();
    ID3D11ShaderResourceView* const copy = g_hud_remover.SourceCopy();
    context->OMSetRenderTargets(1, &target, nullptr);
    context->PSSetShaderResources(slot, 1, &copy);
    draw();
    g_hud_spans.End(context);
    context->PSSetShaderResources(slot, 1, &picture);
    context->OMSetRenderTargets(8, targets, depth);
    if (picture != nullptr) picture->Release();
    g_hud_replayed = true;
    ++g_hud_replays;
  }
  for (auto* view : targets)
    if (view != nullptr) view->Release();
  if (depth != nullptr) depth->Release();
}

void OnGameDraw(command_list* cmd_list, uint32_t vertices, const std::function<void(ID3D11DeviceContext*)>& draw) {
  if (!g_hud_listening.load(std::memory_order_relaxed)) return;
  const bool back = g_hud_on_back.load(std::memory_order_relaxed);
  const bool on_source = g_hud_on_source.load(std::memory_order_relaxed);
  const bool logging = g_hud_logging.load(std::memory_order_relaxed);
  if (!back && !on_source && !logging) return;
  ID3D11DeviceContext* const context = ImmediateContext(cmd_list);
  if (context == nullptr) return;
  ComPtr<ID3D11BlendState> blend;
  ComPtr<ID3D11DepthStencilState> depth;
  UINT mask = 0, stencil = 0;
  FLOAT factor[4];
  context->OMGetBlendState(&blend, factor, &mask);
  context->OMGetDepthStencilState(&depth, &stencil);
  bool blended = false;
  if (blend != nullptr) {
    D3D11_BLEND_DESC d{};
    blend->GetDesc(&d);
    const D3D11_RENDER_TARGET_BLEND_DESC& t = d.RenderTarget[0];
    blended = t.BlendEnable && !(t.SrcBlend == D3D11_BLEND_ONE && t.DestBlend == D3D11_BLEND_ZERO);
  }
  bool depth_test = g_hud_depth_bound;  // no state set means the default, which tests
  if (depth_test && depth != nullptr) {
    D3D11_DEPTH_STENCIL_DESC d{};
    depth->GetDesc(&d);
    depth_test = d.DepthEnable != FALSE;
  }
  if (logging && g_hud_pass_count != 0) {
    HudPass& pass = g_hud_passes[(g_hud_pass_count - 1) % HUD_PASSES];
    ++pass.draws;
    if (blended && !depth_test) ++pass.hud;
  }
  if (on_source) {
    // AC3 draws its HUD with a depth buffer bound, and none of its draws looks like HUD by its state. So a pass with depth on this kind of target also counts as
    // a start. Earlier ones are undone by the full-screen passes after them, and if it was the world, the guard refuses what covers most of the picture.
    bool copy = false;
    if (g_hud_pass_fresh) {
      g_hud_pass_fresh = false;
      g_hud_in_pass = true;
      if (!g_hud_source_watch.HaveCopy() || g_hud_copied_from != g_hud_target) copy = g_hud_source_watch.StartPass();
    } else if (g_hud_in_pass) {
      g_hud_source_watch.CountPassDraw();
    } else {
      copy = g_hud_source_watch.Draw(blended, depth_test, vertices);
    }
    if (copy) {
      // The view is last frame's: the last pass reads this kind of target by it.
      ComPtr<ID3D11Device> device;
      context->GetDevice(&device);
      g_hud_spans.Begin(context);
      g_hud_copied_from = g_hud_remover.CopySource(device.Get(), context, g_hud_target, g_hud_source_view) ? g_hud_target : nullptr;
      g_hud_spans.End(context);
    }
    // For the log: the state of the first draws of a pass with depth, which is what a rule by state would have to read.
    if (logging && g_hud_in_pass && g_hud_detail_count < 6) {
      D3D11_BLEND_DESC b{};
      D3D11_DEPTH_STENCIL_DESC s{};
      if (blend != nullptr) blend->GetDesc(&b);
      if (depth != nullptr) depth->GetDesc(&s);
      char one[80];
      std::snprintf(one, sizeof(one), " [blend %d src %d dest %d depth %d func %d write %d stencil %d, %u vertices]", b.RenderTarget[0].BlendEnable, static_cast<int>(b.RenderTarget[0].SrcBlend), static_cast<int>(b.RenderTarget[0].DestBlend),
                    s.DepthEnable, static_cast<int>(s.DepthFunc), static_cast<int>(s.DepthWriteMask), s.StencilEnable, vertices);
      g_hud_detail += one;
      ++g_hud_detail_count;
    }
  }
  if (!back) return;
  if (!blended && !depth_test && vertices <= hsf::hud::Watch::POST_VERTICES) {
    // A pass that may be the game's last: learn what it reads, and if that holds a HUD, draw it again without.
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11Texture2D> texture;
    const int slot = FindPicture(context, view, texture);
    if (slot >= 0) {
      D3D11_TEXTURE2D_DESC d{};
      texture->GetDesc(&d);
      resource_desc desc = cmd_list->get_device()->get_resource_desc(resource{reinterpret_cast<uint64_t>(texture.Get())});
      g_hud_source_width = d.Width;
      g_hud_source_height = d.Height;
      g_hud_source_format = desc.texture.format;
      view->GetDesc(&g_hud_source_view);
      if (g_hud_source_watch.HaveCopy() && g_hud_copied_from == texture.Get() && g_hud_remover.SourceCopy() != nullptr)
        ReplayWithoutHud(context, slot, [&] { draw(context); });
      else if (g_hud_source_watch.HaveCopy())
        ++g_hud_not_read;
    }
  }
  if (!g_hud_replayed && g_hud_watch.Draw(blended, depth_test, vertices)) {
    g_hud_spans.Begin(context);
    const bool copied = g_hud_remover.CopyBeforeHud(context, reinterpret_cast<ID3D11Texture2D*>(g_hud_back));
    g_hud_spans.End(context);
    if (!copied) g_hud_watch.Close();
  }
}

bool OnDraw(command_list* cmd_list, uint32_t vertices, uint32_t instances, uint32_t first_vertex, uint32_t first_instance) {
#ifdef HSF_SL_GAME
  if (g_slh_on.load(std::memory_order_relaxed)) {
    SlhDraw(cmd_list, vertices);
    return false;
  }
#endif
  OnGameDraw(cmd_list, vertices, [=](ID3D11DeviceContext* context) {
    if (instances > 1 || first_instance != 0)
      context->DrawInstanced(vertices, instances, first_vertex, first_instance);
    else
      context->Draw(vertices, first_vertex);
  });
  return false;
}

bool OnDrawIndexed(command_list* cmd_list, uint32_t indices, uint32_t instances, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
#ifdef HSF_SL_GAME
  if (g_slh_on.load(std::memory_order_relaxed)) {
    SlhDraw(cmd_list, indices);
    return false;
  }
#endif
  OnGameDraw(cmd_list, indices, [=](ID3D11DeviceContext* context) {
    if (instances > 1 || first_instance != 0)
      context->DrawIndexedInstanced(indices, instances, first_index, vertex_offset, first_instance);
    else
      context->DrawIndexed(indices, first_index, vertex_offset);
  });
  return false;
}

// Before any of ReShade's effects: the frame with the HUD, and the end of the game's draws.
// The size of the game's depth buffer ReShade binds to HSF.fx's DEPTH texture; false when it binds none or a placeholder (no view, or a
// texture under half the picture).
bool BoundDepthSize(effect_runtime* runtime, uint32_t& width, uint32_t& height) {
  static effect_texture_variable semantic = {};
  static uint32_t reload_seen = ~0u;
  if (reload_seen != g_effect_reloads || semantic.handle == 0) {
    reload_seen = g_effect_reloads;
    semantic = runtime->find_texture_variable("HSF.fx", "HSFDepth");
  }
  if (semantic.handle == 0) return false;
  resource_view srv{}, srv_srgb{};
  runtime->get_texture_binding(semantic, &srv, &srv_srgb);
  if (srv.handle == 0) return false;
  device* const dev = runtime->get_device();
  const resource_desc d = dev->get_resource_desc(dev->get_resource_from_view(srv));
  if (d.type != resource_type::texture_2d || d.texture.width * 2 < g_seen.width || d.texture.height * 2 < g_seen.height) return false;
  width = d.texture.width;
  height = d.texture.height;
  return true;
}

// Vulkan and DX9 read HSF.fx's depth copy, made at the picture's size. A depth buffer of another shape than the picture (an emulator drawing a
// 16:9 picture centred on a wider screen) goes into the picture's middle there (HSFDepthFit), as the depth module does it for D3D11.
void FitDepthCopy(effect_runtime* runtime) {
  const device_api api = runtime->get_device()->get_api();
  if ((api != device_api::vulkan && api != device_api::d3d9) || g_seen.width == 0 || g_seen.height == 0) return;
  static effect_uniform_variable fit = {};
  static uint32_t reload_seen = ~0u;
  static float sent[4] = {0.f, 0.f, 1.f, 1.f};
  if (reload_seen != g_effect_reloads) {
    reload_seen = g_effect_reloads;
    fit = runtime->find_uniform_variable("HSF.fx", "HSFDepthFit");
    sent[0] = sent[1] = 0.f;
    sent[2] = sent[3] = 1.f;
  }
  if (fit.handle == 0) return;
  float want[4] = {0.f, 0.f, 1.f, 1.f};
  uint32_t width = 0, height = 0;
  if (BoundDepthSize(runtime, width, height)) {
    // Shapes closer than 3% are the same shape: a depth buffer a few rows taller than the picture (1088 for 1080) is stretched, as before.
    const float shape = (static_cast<float>(width) / height) / (static_cast<float>(g_seen.width) / g_seen.height);
    if (shape < 0.97f)
      want[2] = shape;  // bars at the sides
    else if (shape > 1.03f)
      want[3] = 1.f / shape;  // bars above and below
    want[0] = (1.f - want[2]) * 0.5f;
    want[1] = (1.f - want[3]) * 0.5f;
  }
  if (std::equal(want, want + 4, sent)) return;
  runtime->set_uniform_value_float(fit, want, 4);
  std::copy(want, want + 4, sent);
  char text[200];
  if (want[2] < 1.f || want[3] < 1.f)
    std::snprintf(text, sizeof(text), "depth: %ux%u has another shape than the picture %ux%u; HSF.fx puts it into the picture's middle", width, height,
                  g_seen.width, g_seen.height);
  else
    std::snprintf(text, sizeof(text), "depth: HSF.fx stretches the depth over the whole picture");
  Log(text);
}

// With ReShade's depth shape check off, ReShade jumps between the game's depth and other buffers (an emulator's shadow maps). Once the depth of
// another shape has stayed bound for 5 s, HSF has ReShade take only depth of that exact size from the next game start
// (UseAspectRatioHeuristics=4 with FilterResolutionWidth/Height). When a game finds no depth for 60 s with a size kept, the check goes back to
// off, so the next start finds the new size (another game, another resolution).
void PinDepthSize(effect_runtime* runtime) {
  const device_api api = runtime->get_device()->get_api();
  if ((api != device_api::vulkan && api != device_api::d3d11 && api != device_api::d3d9) || g_seen.width == 0 || g_seen.height == 0) return;
  static uint32_t reload_seen = ~0u;
  static int check = 1;
  static uint32_t seen_width = 0, seen_height = 0;
  static ULONGLONG seen_since = 0, missing_since = 0;
  static bool written = false;
  if (reload_seen != g_effect_reloads) {
    reload_seen = g_effect_reloads;
    check = 1;
    reshade::get_config_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", check);
    seen_width = seen_height = 0;
    missing_since = 0;
    written = false;
    g_depth_pin_note = 0;  // a new game start (an emulator booting again) has taken the change
  }
  if (written || (check != 0 && check != 4)) return;
  if (!hsf::dlssg::Feeding()) {
    missing_since = 0;
    return;
  }
  const ULONGLONG now = GetTickCount64();
  uint32_t width = 0, height = 0;
  const bool bound = BoundDepthSize(runtime, width, height);
  if (check == 4) {
    if (bound) {
      missing_since = 0;
    } else if (missing_since == 0) {
      missing_since = now;
    } else if (now - missing_since >= 60000) {
      reshade::set_config_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", "0");
      written = true;
      g_depth_pin_note = 2;
      Log("depth: none of the kept size for 60 s; ReShade's depth shape check is off again from the next game start, to find the new size");
    }
    return;
  }
  const float shape = bound && height != 0 ? (static_cast<float>(width) / height) / (static_cast<float>(g_seen.width) / g_seen.height) : 1.f;
  if (!bound || width == height || (shape >= 0.97f && shape <= 1.03f)) {
    seen_width = seen_height = 0;
    return;
  }
  if (width != seen_width || height != seen_height) {
    seen_width = width;
    seen_height = height;
    seen_since = now;
    return;
  }
  if (now - seen_since < 5000) return;
  reshade::set_config_value(nullptr, "DEPTH", "FilterResolutionWidth", std::to_string(width).c_str());
  reshade::set_config_value(nullptr, "DEPTH", "FilterResolutionHeight", std::to_string(height).c_str());
  reshade::set_config_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", "4");
  written = true;
  g_depth_pin_note = 1;
  g_depth_pin_size[0] = width;
  g_depth_pin_size[1] = height;
  char text[200];
  std::snprintf(text, sizeof(text), "depth: %ux%u stayed bound for 5 s; ReShade takes only depth of that size from the next game start (no more jumps to other buffers)",
                width, height);
  Log(text);
}

void OnBeginEffects(effect_runtime* runtime, command_list*, resource_view rtv, resource_view) {
  FitDepthCopy(runtime);
  PinDepthSize(runtime);
#ifdef HSF_SL_GAME
  if (runtime->get_device()->get_api() == device_api::d3d11) {
    static bool ngx11_started = false;
    if (!ngx11_started) {
      ngx11_started = true;
      hsf::ngx::Start11(&Log);
    }
    hsf::ngx::Poll11();
  }
#endif
  g_hud_at_effects = false;
  if (!g_hud_listening.load(std::memory_order_relaxed)) return;
  g_hud_watch.Close();
  g_hud_source_watch.Close();
  g_hud_logging.store(false, std::memory_order_relaxed);
  if ((!g_hud_watch.HaveCopy() && !g_hud_replayed) || rtv.handle == 0) return;
  device* const dev = runtime->get_device();
  if (dev->get_api() != device_api::d3d11) return;
  auto* const device11 = reinterpret_cast<ID3D11Device*>(dev->get_native());
  ComPtr<ID3D11DeviceContext> context;
  device11->GetImmediateContext(&context);
  g_hud_spans.Begin(context.Get());
  g_hud_at_effects = g_hud_remover.CopyAtEffects(context.Get(), reinterpret_cast<ID3D11Texture2D*>(dev->get_resource_from_view(rtv).handle));
  g_hud_spans.End(context.Get());
}

void LogHud() {
  char line[1800];
  double gpu_ms = 0;
  unsigned timed = 0;
  if (g_hud_spans.Take(gpu_ms, timed) || g_hud_rests != 0) {
    std::snprintf(line, sizeof(line), "HUD: HSF's HUD work on the GPU %.2f ms a frame (%u frames timed); rested %u times (%llu s each) as the HUD was not used", gpu_ms, timed,
                  g_hud_rests, HUD_REST_MS / 1000);
    Log(line);
  }
  g_hud_rests = 0;
  std::snprintf(line, sizeof(line), "HUD: found on %u of %u frames, %.1f copies a frame; last frame %u draws on the back buffer, %u of them HUD; it covers %.1f%% of the picture; %s", g_hud_found, g_hud_frames,
                g_hud_frames != 0 ? static_cast<double>(g_hud_copies) / g_hud_frames : 0.0, g_hud_watch.OnBackDraws(), g_hud_watch.HudDraws(), g_hud_share * 100.f,
                g_hud_found == 0 ? "not found: the mask alone protects it" : g_hud_guard.InUse() ? "taken out of generation" : "too much of the picture, not used");
  Log(line);
  std::snprintf(line, sizeof(line), "HUD: the last pass reads a %ux%u target of format %u; on such targets last frame %u draws, %u of them HUD; drawn again without it on %u frames, not read from the copied target on %u", g_hud_source_width, g_hud_source_height,
                static_cast<unsigned>(g_hud_source_format), g_hud_source_watch.OnBackDraws(), g_hud_source_watch.HudDraws(), g_hud_replays, g_hud_not_read);
  Log(line);
  std::snprintf(line, sizeof(line), "HUD: first draws of passes with depth on such targets:%s", g_hud_detail.c_str());
  Log(line);
  std::string text;
  const uint32_t kept = std::min<uint32_t>(g_hud_pass_count, HUD_PASSES);
  for (uint32_t i = g_hud_pass_count - kept; i < g_hud_pass_count; ++i) {
    const HudPass& p = g_hud_passes[i % HUD_PASSES];
    if (p.draws == 0) continue;
    char one[80];
    std::snprintf(one, sizeof(one), " %ux%u/%u%s%s:%u(%u)", p.width, p.height, static_cast<unsigned>(p.format), p.back ? "/BACK" : "", p.depth ? "/depth" : "", p.draws, p.hud);
    text += one;
    // Log() holds under 400 characters.
    if (text.size() > 300) {
      std::snprintf(line, sizeof(line), "HUD: targets:%s", text.c_str());
      Log(line);
      text.clear();
    }
  }
  std::snprintf(line, sizeof(line), "HUD: targets, of %u bound, size/format, draws (blended with no depth test):%s", g_hud_pass_count, text.c_str());
  Log(line);
}

// After the clean frame, as without one there is nothing to hand over.
void UpdateHud(device* dev, resource target) {
  g_hud_frame = nullptr;
  const bool at_effects = g_hud_at_effects;
  g_hud_at_effects = false;
  const ULONGLONG tick = GetTickCount64();
  if (!g_hud_enabled || !g_clean_ready || tick < g_hud_rest_until) {
    g_hud_listening.store(false, std::memory_order_relaxed);
    g_hud_logging.store(false, std::memory_order_relaxed);
    g_hud_on_back.store(false, std::memory_order_relaxed);
    if (g_hud_spans.Open() && dev->get_api() == device_api::d3d11) {
      ComPtr<ID3D11DeviceContext> context;
      reinterpret_cast<ID3D11Device*>(dev->get_native())->GetImmediateContext(&context);
      g_hud_spans.EndFrame(context.Get());
    }
    return;
  }
  auto* const frame = reinterpret_cast<ID3D11Texture2D*>(target.handle);
  D3D11_TEXTURE2D_DESC d{};
  frame->GetDesc(&d);
  auto* const device11 = reinterpret_cast<ID3D11Device*>(dev->get_native());
  const char* why = nullptr;
  if (!g_hud_remover.Prepare(device11, d, why)) {
    if (why != nullptr) {
      char line[160];
      std::snprintf(line, sizeof(line), "HUD: not taken out of generation: %s", why);
      Log(line);
    }
    g_hud_listening.store(false, std::memory_order_relaxed);
    return;
  }
  g_hud_back = target.handle;
  ComPtr<ID3D11DeviceContext> context;
  device11->GetImmediateContext(&context);
  ++g_hud_frames;
  g_hud_copies += static_cast<uint32_t>(g_hud_watch.Copies());
  if (at_effects && g_hud_listening.load(std::memory_order_relaxed)) {
    ++g_hud_found;
    g_hud_spans.Begin(context.Get());
    ID3D11Texture2D* const made = g_hud_remover.Make(context.Get(), frame);
    g_hud_spans.End(context.Get());
    float share = 0;
    if (g_hud_remover.ReadShare(context.Get(), share)) {
      g_hud_share = share;
      g_hud_guard.Feed(share);
    }
    if (made != nullptr && g_hud_guard.InUse()) g_hud_frame = made;
  }
  g_hud_spans.EndFrame(context.Get());
  if (g_hud_guard.InUse()) {
    g_hud_unused_frames = 0;
  } else if (++g_hud_unused_frames >= HUD_LOOK_FRAMES) {
    g_hud_unused_frames = 0;
    g_hud_rest_until = tick + HUD_REST_MS;
    ++g_hud_rests;
    g_hud_listening.store(false, std::memory_order_relaxed);
    g_hud_logging.store(false, std::memory_order_relaxed);
    g_hud_on_back.store(false, std::memory_order_relaxed);
    return;
  }
  const ULONGLONG now = GetTickCount64();
  if (g_hud_next_log == 0) g_hud_next_log = now + 10000;
  // Every 10 s: record one frame's targets, and log them when it is over.
  if (g_hud_log_due) {
    LogHud();
    g_hud_log_due = false;
    g_hud_next_log = now + 10000;
    g_hud_frames = g_hud_found = g_hud_copies = g_hud_replays = g_hud_not_read = 0;
  } else if (now >= g_hud_next_log) {
    g_hud_log_due = true;
    g_hud_pass_count = 0;
    g_hud_detail.clear();
    g_hud_detail_count = 0;
    g_hud_logging.store(true, std::memory_order_relaxed);
  }
  g_hud_watch.BeginFrame();
  g_hud_source_watch.BeginFrame();
  g_hud_replayed = false;
  g_hud_copied_from = nullptr;
  g_hud_spans.BeginFrame(device11, context.Get());
  g_hud_listening.store(true, std::memory_order_relaxed);
}

// InputKey: the next motion source found, after the last one zero motion, then back to the first. A new source restarts the presenter (a short
// flash); zero motion does not. Written nowhere, so a test never sticks to the next session.
void CycleInput() {
  int found[SOURCE_COUNT];
  int count = 0;
  for (int i = 0; i < SOURCE_COUNT; ++i)
    if (g_source_variables[i].handle != 0) found[count++] = i;
  if (count == 0) return;
  int next = -1;
  if (g_input_zero) {
    next = found[0];
  } else {
    for (int i = 0; i < count; ++i)
      if (found[i] > g_seen.source) {
        next = found[i];
        break;
      }
  }
  g_input_zero = next < 0;
  if (next >= 0 && next != g_seen.source) g_source_setting = next + 1;
  char text[120];
  std::snprintf(text, sizeof(text), "A/B: DLSS-G's motion input is now %s", g_input_zero ? "ZERO" : SOURCES[next].key);
  Log(text);
}

// After ReShade's effects, and so after GITC Uplift's NR, have finished the frame (docs/architecture.md has the order).
void UpdateDlssgMotionScale() {
  const int convention = Convention();
  g_dlssg_motion_scale[0] = g_dlssg_motion_scale[1] = 0.f;
  if (g_seen.have_motion && convention != 0 && g_seen.width != 0 && g_seen.height != 0) {
    float to_pixels[2];
    hsf::motion::ConventionScale(convention, static_cast<float>(g_seen.width), static_cast<float>(g_seen.height), static_cast<float>(g_seen.mv_width),
                                  static_cast<float>(g_seen.mv_height), to_pixels);
    g_dlssg_motion_scale[0] = to_pixels[0] / static_cast<float>(g_seen.width);
    g_dlssg_motion_scale[1] = to_pixels[1] / static_cast<float>(g_seen.height);
    // A/B: as good as no motion. Not exactly 0, which means "no motion vectors" and stops DLSS-G generating at all.
    if (g_input_zero) g_dlssg_motion_scale[0] = g_dlssg_motion_scale[1] = 1e-7f;
  }
  // D3D11 motion fitted by Dlss11Motion (a DLSS mod's vectors, or the flow scaled up to their size) is already in DLSS-G's units.
  if (g_seen.api == device_api::d3d11 && g_motion_fitted) g_dlssg_motion_scale[0] = g_dlssg_motion_scale[1] = 1.f;
}

// D3D11: a mod that adds DLSS to the game (Luma) hands DLSS the game's own vectors and depth every frame (ngx_input.h). While those keep coming and the
// menu's MOTION is not FLOW, DLSS-G gets them in place of the optical flow, at the back buffer's size: the vectors put into the picture's middle (the
// game's 16:9 picture on a wider screen), the depth through the depth module. From the first frame of such data on, the flow goes out at that size too,
// scaled up, so the two take turns (the mod pauses its DLSS in cutscenes and menus) in one size: the helper's shared slots keep one size per start, so it
// restarts once when the size changes (UpdatePresenter). With FlowPause the flow effect is off while the data comes. After Inspect and the score, which
// keep reading the flow.
void PauseFlow(effect_runtime* runtime, bool pause) {
  static bool paused = false;
  static uint32_t reload_seen = 0;
  if (reload_seen != g_effect_reloads) {  // a reload brings the techniques back as the preset has them
    reload_seen = g_effect_reloads;
    paused = false;
  }
  if (pause == paused || runtime == nullptr) return;
  paused = pause;
  for (const auto& [effect, name] : {std::pair{"MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad"}, std::pair{"lumenite_Kernel.fx", "Lumenite_Kernel"}}) {
    const effect_technique technique = runtime->find_technique(effect, name);
    if (technique.handle == 0 || runtime->get_technique_state(technique) == !pause) continue;
    runtime->set_technique_state(technique, !pause);
    char text[160];
    std::snprintf(text, sizeof(text), pause ? "flow paused: %s (%s) off while the game's DLSS data feeds DLSS-G" : "flow back: %s (%s) on", name, effect);
    Log(text);
  }
}

// Luma's add-on (named "Luma-<game>.addon" or ".addon64") is loaded in this process: the menu then names the DLSS data Luma's and says what Luma needs.
// Looked for at most every 600 calls until found, as the module list is not free.
bool LumaLoaded() {
  static bool found = false;
  static int wait = 0;
  if (found || wait-- > 0) return found;
  wait = 600;
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE) return false;
  MODULEENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  for (BOOL more = Module32FirstW(snap, &entry); more && !found; more = Module32NextW(snap, &entry)) {
    const wchar_t* const dot = std::wcsrchr(entry.szModule, L'.');
    found = _wcsnicmp(entry.szModule, L"Luma-", 5) == 0 && dot != nullptr && _wcsnicmp(dot, L".addon", 6) == 0;
  }
  CloseHandle(snap);
  if (found) Log("Luma's add-on is loaded: the menu names the DLSS data Luma's");
  return found;
}

void Dlss11Motion(effect_runtime* runtime, device* dev) {
  g_dlss11_depth = nullptr;
  g_motion_fitted = false;
#ifdef HSF_SL_GAME
  static uint64_t last_serial = 0;
  static int quiet = 0;
  static bool used = false, full = false;
  hsf::ngx::Frame11 f;
  bool use = false;
  if (hsf::ngx::Latest11(&f)) {
    quiet = f.serial != last_serial ? 0 : std::min(quiet + 1, 1000);
    last_serial = f.serial;
    if (quiet < 3) g_dlss_seen = true;
    use = quiet < 3 && MotionFrom() != 1;
  }
  const uint32_t out_width = g_seen.width, out_height = g_seen.height;
  ID3D11Device* const native = reinterpret_cast<ID3D11Device*>(dev->get_native());
  ID3D11Texture2D* fitted = nullptr;
  if (use) {
    const float to_pixels[2] = {f.scale[0], f.scale[1]};
    fitted = hsf::depth::FitMotion(native, f.motion, f.x, f.y, f.width, f.height, to_pixels, out_width, out_height);
    use = fitted != nullptr;
  }
  if (use && !full) {
    full = true;
    Log("the game's DLSS data goes to DLSS-G at the back buffer's size from now on, the flow scaled up to it while the data pauses");
  }
  g_using_dlss = use;
  PauseFlow(runtime, use && g_flow_pause);
  if (use != used) {
    used = use;
    g_dlss11_reset = true;
    char text[200];
    std::snprintf(text, sizeof(text), use ? "motion from the game's own DLSS data (%ux%u in use), put into the picture's middle at %ux%u, with its depth"
                                          : "motion from flow (no DLSS data from the game now, or MOTION is FLOW)",
                  f.width, f.height, out_width, out_height);
    Log(text);
  }
  if (use) {
    g_dlss11_reset = g_dlss11_reset || f.reset;
    g_dlss11_depth = f.depth;
  } else if (full && g_seen.have_motion && g_motion_srv11 != nullptr && Convention() != 0 && (g_seen.mv_width != out_width || g_seen.mv_height != out_height)) {
    // The flow, scaled up to the size the helper was started at. Its whole texture covers the whole picture.
    float to_pixels[2];
    hsf::motion::ConventionScale(Convention(), static_cast<float>(g_seen.width), static_cast<float>(g_seen.height), static_cast<float>(g_seen.mv_width),
                                  static_cast<float>(g_seen.mv_height), to_pixels);
    to_pixels[0] *= static_cast<float>(g_seen.mv_width) / static_cast<float>(g_seen.width);
    to_pixels[1] *= static_cast<float>(g_seen.mv_height) / static_cast<float>(g_seen.height);
    fitted = hsf::depth::FitMotion(native, g_motion_srv11, 0, 0, g_seen.mv_width, g_seen.mv_height, to_pixels, out_width, out_height, true);
    if (fitted == nullptr) return;
  } else {
    return;
  }
  g_motion_fitted = true;
  g_motion_texture = fitted;
  g_seen.have_motion = true;
  g_seen.mv_width = out_width;
  g_seen.mv_height = out_height;
  g_seen.mv_format = format::r16g16_float;
#else
  (void)runtime;
  (void)dev;
#endif
}

constexpr uint32_t ROUTE_DECIDE_FRAMES = 300;
bool g_route_decided = false;  // PickD3d12Route or LeaveAutoD3d12Route has acted this run; only one of them ever does

#ifdef HSF_SL_GAME
// Whether the sl.interposer.dll loaded in this process was started by the game (slInit called). A copy left in a game folder that never uses it (RDR2 with a stray
// copy: every tag answered eErrorNotInitialized) is loaded but never started, and the in-game route can do nothing with it. Asks slIsFeatureLoaded, which only
// reads: before slInit it answers eErrorNotInitialized or eErrorInitNotCalled. True when there is no such export, so an unknown Streamline is trusted as before.
bool GameStreamlineStarted() {
  const HMODULE module = GetModuleHandleW(L"sl.interposer.dll");
  if (module == nullptr) return false;
  using IsFeatureLoadedFn = int (*)(uint32_t, bool&);
  const auto is_loaded = reinterpret_cast<IsFeatureLoadedFn>(GetProcAddress(module, "slIsFeatureLoaded"));
  if (is_loaded == nullptr) return true;
  constexpr uint32_t FEATURE_DLSS_G = 1000;      // sl::kFeatureDLSS_G
  constexpr int NOT_INITIALIZED = 21, INIT_NOT_CALLED = 23;  // sl::Result::eErrorNotInitialized, eErrorInitNotCalled
  bool loaded = false;
  const int result = is_loaded(FEATURE_DLSS_G, loaded);
  return result != NOT_INITIALIZED && result != INIT_NOT_CALLED;
}
#endif

// A D3D12 game with neither D3D12 route on in HSF.ini. Both routes must start when HSF loads, before the game makes its device, so HSF picks one now, writes it
// to HSF.ini and asks for one restart (without it, FF7R in DX12 showed "only Direct3D 11 and DX9" with no way on). A game with its own Streamline gets
// InGameDlssg=2, one without gets SlBringIn=2 (as 1, but written by HSF: LeaveAutoD3d12Route takes either back if the game later runs D3D11).
// A route set to 0 by hand stays off. Waits until D3D12 has shown more frames than D3D11: a D3D11 game that briefly shows a D3D12 window (Batman: Arkham Knight)
// got InGameDlssg=1 here, and the next launch put up the "own frame generation" box and the D3D12 route's readouts in a D3D11 game.
void PickD3d12Route() {
#ifdef HSF_SL_GAME
  if (g_route_decided) return;
  const uint32_t d3d12 = g_d3d12_frames.load(std::memory_order_relaxed);
  if (d3d12 < ROUTE_DECIDE_FRAMES || d3d12 <= g_d3d11_frames.load(std::memory_order_relaxed)) return;
  g_route_decided = true;
  const int bring = GetPrivateProfileIntW(L"HSF", L"SlBringIn", -1, g_ini_path);
  const int drive = GetPrivateProfileIntW(L"HSF", L"InGameDlssg", -1, g_ini_path);
  if (bring == 0 || drive == 0) {
    Note("D3D12: HSF's D3D12 routes are off in HSF.ini (SlBringIn=0 or InGameDlssg=0)");
    return;
  }
  if (GameStreamlineStarted()) {
    WritePrivateProfileStringW(L"HSF", L"InGameDlssg", L"2", g_ini_path);
    Log("route: a D3D12 game with its own Streamline: InGameDlssg=2 written, it starts at the next launch");
  } else {
    if (GetModuleHandleW(L"sl.interposer.dll") != nullptr)
      Log("route: sl.interposer.dll is loaded but the game never started it (a stray copy in the game folder?), so HSF brings its own Streamline");
    WritePrivateProfileStringW(L"HSF", L"SlBringIn", L"2", g_ini_path);
    Log("route: a D3D12 game with no Streamline: SlBringIn=2 written (HSF brings Streamline), it starts at the next launch");
  }
  Note("D3D12: HSF HAS SET UP ITS D3D12 ROUTE. RESTART THE GAME ONCE");
#else
  Note("D3D12: only in 64-bit games");
#endif
}

#ifdef HSF_SL_GAME
// The in-game route in a game whose sl.interposer.dll was never started (written by PickD3d12Route before it checked for that, or set by hand): nothing can
// ever be driven, and the menu waits forever for "the game's DLSS-G". After ROUTE_DECIDE_FRAMES D3D12 frames with the game's Streamline still not started,
// HSF switches to bringing its own Streamline (SlBringIn=2) from the next launch.
bool g_drive_left = false;  // LeaveUnstartedStreamline switched this game to SlBringIn; the menu asks for a restart
void LeaveUnstartedStreamline() {
  static bool s_done = false;
  if (s_done || !D3d12Run() || g_d3d12_frames.load(std::memory_order_relaxed) < ROUTE_DECIDE_FRAMES) return;
  s_done = true;
  if (GameStreamlineStarted() || hsf::slgame::GameFrames() != 0 || hsf::slgame::GetStats().have_options) return;
  WritePrivateProfileStringW(L"HSF", L"InGameDlssg", nullptr, g_ini_path);
  if (GetPrivateProfileIntW(L"HSF", L"SlBringIn", -1, g_ini_path) != 0) WritePrivateProfileStringW(L"HSF", L"SlBringIn", L"2", g_ini_path);
  Log("route: the game never started its sl.interposer.dll (a stray copy in the game folder?), so the in-game route can do nothing; SlBringIn=2 written, HSF brings its own Streamline from the next launch");
  g_drive_left = true;
}
#endif

// A D3D11 run in a game where HSF set up a D3D12 route (FF7R runs both, -dx11 picks D3D11): the D3D11 route needs neither, so it goes back off. SlBringIn=2 and
// InGameDlssg=2 are HSF's own; InGameDlssg=1 is cleared too, as builds before 0.9.6 wrote 1 for the auto route and a D3D11 run never uses it.
void LeaveAutoD3d12Route() {
#ifdef HSF_SL_GAME
  if (g_route_decided) return;
  const uint32_t d3d11 = g_d3d11_frames.load(std::memory_order_relaxed);
  if (d3d11 < ROUTE_DECIDE_FRAMES || d3d11 <= g_d3d12_frames.load(std::memory_order_relaxed)) return;
  g_route_decided = true;
  if (GetPrivateProfileIntW(L"HSF", L"SlBringIn", 0, g_ini_path) == 2) {
    WritePrivateProfileStringW(L"HSF", L"SlBringIn", nullptr, g_ini_path);
    Log("route: this run is D3D11, so the D3D12 route HSF set up (SlBringIn=2) is off again from the next launch");
  }
  const int drive = GetPrivateProfileIntW(L"HSF", L"InGameDlssg", 0, g_ini_path);
  if (drive == 1 || drive == 2) {
    WritePrivateProfileStringW(L"HSF", L"InGameDlssg", nullptr, g_ini_path);
    Log("route: this run is D3D11, so the game-Streamline D3D12 route (InGameDlssg) is off again from the next launch");
  }
#endif
}

#ifdef HSF_SL_GAME
// A Direct3D 12 game HSF brought Streamline into: this frame's flow and depth go to DLSS-G, in the same place in the frame as on Direct3D 11. Effects have just run, and Streamline's Present is next.
// What the picture shows, from HSF's panel: 0 the game, 1 depth, 2 motion, as DLSS-G gets them. Not saved: a look, not a setting.
int g_bring_view = 0;
bool g_bring_using_game = false;  // BringFrame sent the game's own DLSS data this frame, not flow

// This frame's flow (LaunchPad) and depth copy (HSF.fx) as native textures, switching their techniques on as needed. False, with a line in the log once, while either is missing.
bool FindFlowAndDepth(effect_runtime* runtime, device* dev, ID3D12Resource** motion_out, ID3D12Resource** depth_out) {
  static ULONGLONG next_search = 0;
  static effect_texture_variable depth_variable = {};
  static uint32_t reload_seen = 0;
  if (reload_seen != g_effect_reloads) {
    reload_seen = g_effect_reloads;
    depth_variable = {};
    next_search = 0;
  }
  const ULONGLONG now = GetTickCount64();
  if (now >= next_search) {
    next_search = now + 1000;
    if (depth_variable.handle == 0) depth_variable = runtime->find_texture_variable("HSF.fx", "HSFDepthCopy");
    const auto switch_on = [runtime](const char* effect, const char* name) {
      const effect_technique technique = runtime->find_technique(effect, name);
      if (technique.handle != 0 && !runtime->get_technique_state(technique)) {
        runtime->set_technique_state(technique, true);
        char text[160];
        std::snprintf(text, sizeof(text), "switched on %s (%s)", name, effect);
        Log(text);
      }
    };
    switch_on("HSF.fx", "HSF_DepthCopy");
    switch_on("HSF.fx", "HSF_Depth");
    switch_on("MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad");
    switch_on("lumenite_Kernel.fx", "Lumenite_Kernel");  // the other flow HSF reads (FF7R had it and no LaunchPad)
  }
  ID3D12Resource* motion = nullptr;
  if (g_seen.have_motion && g_seen.source >= 0) {
    resource_view srv{}, srv_srgb{};
    runtime->get_texture_binding(g_source_variables[g_seen.source], &srv, &srv_srgb);
    if (srv.handle != 0) motion = reinterpret_cast<ID3D12Resource*>(dev->get_resource_from_view(srv).handle);
  }
  ID3D12Resource* depth = nullptr;
  if (depth_variable.handle != 0) {
    resource_view srv{}, srv_srgb{};
    runtime->get_texture_binding(depth_variable, &srv, &srv_srgb);
    if (srv.handle != 0) depth = reinterpret_cast<ID3D12Resource*>(dev->get_resource_from_view(srv).handle);
  }
  static bool logged_ready = false, logged_missing = false;
  // With NVOF on and no flow effect, NVOF makes the flow alone: depth is enough, and the effect's sign convention does not apply (motion stays nullptr).
  const bool nvof_alone = motion == nullptr && NvofOn();
  if (nvof_alone && depth != nullptr) {
    static bool logged_alone = false;
    if (!logged_alone) {
      logged_alone = true;
      Log("Direct3D 12: no flow effect, NVIDIA Optical Flow makes the flow on its own");
    }
    *motion_out = nullptr;
    *depth_out = depth;
    return true;
  }
  if (motion == nullptr || depth == nullptr || Convention() == 0) {
    static const ULONGLONG first_look = now;
    if (!logged_missing && now - first_look > 20000) {
      logged_missing = true;
      char text[200];
      std::snprintf(text, sizeof(text), "no optical motion yet: flow %s, depth copy %s", motion != nullptr ? "found" : "NOT found (LaunchPad?)", depth != nullptr ? "found" : "NOT found (HSF.fx?)");
      Log(text);
    }
    return false;
  }
  if (!logged_ready) {
    logged_ready = true;
    Log("Direct3D 12: flow and depth found");
  }
  *motion_out = motion;
  *depth_out = depth;
  return true;
}

void BringFrame(effect_runtime* runtime, device* dev, resource target) {
  if (g_mfg_key != 0 && !g_menu_open && runtime->is_key_pressed(g_mfg_key)) {
    g_dlssg_frames = g_dlssg_frames >= std::max(hsf::slbring::GetStats().max_frames, 1u) ? 1 : g_dlssg_frames + 1;
    SaveIni();
  }
  hsf::slbring::SetReflex(CapInForce(), g_reflex_boost, g_reflex_sleep);
  UpdateDlssgMotionScale();

  // The game's own DLSS data while it keeps coming; a few real frames without it, such as a menu, and flow takes over. Each change of source starts generation's history again.
  static uint64_t ngx_serial = 0;
  static int ngx_quiet = 1000;
  static bool ngx_used = false;
  hsf::ngx::Frame ngx_frame;
  bool use_ngx = false;
  if (g_ngx_input) {
    hsf::ngx::Poll();
    if (hsf::ngx::Latest(&ngx_frame)) {
      ngx_quiet = ngx_frame.serial != ngx_serial ? 0 : ngx_quiet + 1;
      ngx_serial = ngx_frame.serial;
      if (ngx_quiet < 3) g_dlss_seen = true;
      use_ngx = ngx_quiet < 3 && MotionFrom() != 1;
    }
  }
  g_using_dlss = use_ngx;
  const bool source_changed = use_ngx != ngx_used;
  if (source_changed) {
    char text[120];
    std::snprintf(text, sizeof(text), "motion from %s", use_ngx ? "the game's DLSS" : "flow");
    hsf::slbring::LogLine(text);
  }
  ngx_used = use_ngx;
  g_bring_using_game = use_ngx;
  const auto send_common = [&](hsf::slbring::FrameFeed& feed) {
    // ReShade's own list holds the effects, so it goes first: HSF's list reads what they wrote.
    runtime->get_command_queue()->flush_immediate_command_list();
    feed.device = reinterpret_cast<ID3D12Device*>(dev->get_native());
    feed.queue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
    feed.back_buffer = reinterpret_cast<ID3D12Resource*>(target.handle);
    feed.back_buffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    feed.on = g_extra_enabled;
    feed.frames = g_dlssg_frames;
    feed.view = static_cast<unsigned>(g_bring_view);
    hsf::slbring::Frame(feed);
  };
  if (use_ngx) {
    hsf::slbring::FrameFeed feed;
    feed.motion = ngx_frame.motion;
    feed.depth = ngx_frame.depth;
    feed.scale[0] = ngx_frame.scale[0];
    feed.scale[1] = ngx_frame.scale[1];
    feed.depth_inverted = ngx_frame.depth_inverted;
    feed.reset = ngx_frame.reset || source_changed;
    // The game's vectors are sharp and exact, so flow's clean-up has nothing to do. The HUD mask works at the picture's size, which these are not.
    feed.compose = false;
    feed.mask = false;
    feed.x = ngx_frame.x;
    feed.y = ngx_frame.y;
    feed.width = ngx_frame.width;
    feed.height = ngx_frame.height;
    feed.jitter[0] = ngx_frame.jitter[0];
    feed.jitter[1] = ngx_frame.jitter[1];
    feed.jittered = ngx_frame.jittered;
    feed.dilated = ngx_frame.full_res;
    send_common(feed);
    return;
  }

  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  if (!FindFlowAndDepth(runtime, dev, &motion, &depth)) return;
  hsf::slbring::FrameFeed feed;
  feed.reset = source_changed;
  feed.motion = motion;
  feed.depth = depth;
  // NVOF alone (no effect): its own motion texture holds the offset as a share of the picture, so the scale is 1.
  feed.scale[0] = motion != nullptr ? g_dlssg_motion_scale[0] : 1.f;
  feed.scale[1] = motion != nullptr ? g_dlssg_motion_scale[1] : 1.f;
  // Direct3D 12 games have reversed depth nearly always, and the one thing that would find out on Direct3D 11 reads the texture on the CPU. HSF.ini DepthInverted=0 turns it round.
  feed.depth_inverted = g_depth_inverted_setting != 0;
  feed.compose = g_compose_enabled;
  feed.mask = g_checks_enabled;
  feed.nvof = NvofOn();
  feed.nvof_size = static_cast<uint32_t>(g_nvof_size);
  send_common(feed);
}

// In-game route, every frame at present (effect events can be missing, as in Shadows with no effects loaded). AUTO: the game's DLSS data while it comes (it stops in
// menus and with the game's DLSS off), flow otherwise. DLSS: the game's only. FLOW: flow only.
void DriveMotionChoice() {
  const bool flowing = hsf::slgame::GetStats().data_flowing;
  if (flowing) g_dlss_seen = true;
  const bool optical = MotionFrom() == 1 || (MotionFrom() == 0 && !flowing);
  g_using_dlss = !optical && flowing;
  hsf::slgame::SetOptical(optical);
}

// A Direct3D 12 game with its own Streamline, set to optical motion: this frame's flow and depth go through the game's Streamline, at the same place in the frame as BringFrame.
void SlOpticalFrame(effect_runtime* runtime, device* dev, resource target) {
  // DriveMotionChoice, every frame at present, decided which.
  if (g_using_dlss || MotionFrom() == 2) return;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  if (!FindFlowAndDepth(runtime, dev, &motion, &depth)) return;
  UpdateDlssgMotionScale();
  runtime->get_command_queue()->flush_immediate_command_list();
  hsf::slbring::FrameFeed feed;
  feed.device = reinterpret_cast<ID3D12Device*>(dev->get_native());
  feed.queue = reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native());
  feed.back_buffer = reinterpret_cast<ID3D12Resource*>(target.handle);
  feed.back_buffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  feed.motion = motion;
  feed.depth = depth;
  // NVOF alone (no effect): its own motion texture holds the offset as a share of the picture, so the scale is 1.
  feed.scale[0] = motion != nullptr ? g_dlssg_motion_scale[0] : 1.f;
  feed.scale[1] = motion != nullptr ? g_dlssg_motion_scale[1] : 1.f;
  feed.depth_inverted = g_depth_inverted_setting != 0;
  feed.compose = g_compose_enabled;
  feed.mask = g_checks_enabled;
  feed.nvof = NvofOn();
  feed.nvof_size = static_cast<uint32_t>(g_nvof_size);
  feed.view = static_cast<unsigned>(g_bring_view);
  feed.tag = &hsf::slgame::TagOptical;
  hsf::slbring::Frame(feed);
}
#endif

// ---- Direct3D 9 games (dx9_bridge.h) ----
// The native D3D9 device under ReShade's proxy. Its Present, PresentEx (D3D9Ex) and the implicit swap chain's Present are patched like the DXGI
// one: right before, HSF hands the finished frame (overlays drawn; the HUD-less copy is BridgeEarly9's) to DLSS-G; right after, the game's
// next frame starts (Reflex). The same vtable serves every device of the class, so only the device HSF follows is touched.
using Present9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PresentEx9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9Ex*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
using SwapPresent9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*, DWORD);
constexpr size_t DEVICE9_PRESENT = 17, DEVICE9EX_PRESENTEX = 121, SWAP9_PRESENT = 3;
IDirect3DDevice9* g_native9 = nullptr;         // not referenced: ReShade holds it while its effect runtime is there
IDirect3DSwapChain9* g_native_swap9 = nullptr;  // its implicit swap chain, which lives as long as the device
void** g_vtable9 = nullptr;
void** g_vtable9_swap = nullptr;
bool g_vtable9_ex = false;
Present9Fn g_real_present9 = nullptr;
PresentEx9Fn g_real_present_ex9 = nullptr;
SwapPresent9Fn g_real_swap_present9 = nullptr;
bool g_in_present9 = false;  // game thread: a Present inside another is not counted twice
uint32_t g_present9_calls = 0;  // every call of the three hooks, followed device or not (ReclaimSlots9)

// With HSF.ini KeepOverlaysOut=1 (the default) a second copy of the frame is taken at ReShade's finish_effects, before ReShade's menu, HSF's
// overlay and the other add-ons' overlays are drawn, and goes to DLSS-G as the HUD-less frame: the real frames show the overlays, the
// generated ones are made without them. The frame itself is bridged right before the Present, overlays and all.
// Some games present through an additional swap chain and never through the device's own (The Darkness II). When only another
// swap chain of the followed device presents, HSF follows that one instead (holding a reference, released before it goes away or a Reset).
ComPtr<IDirect3DSwapChain9> g_adopted_swap9;
bool AdoptSwap9(IDirect3DSwapChain9* self) {
  static uint32_t followed_seen = 0;
  static int others = 0;
  if (g_game_presents != followed_seen) {
    followed_seen = g_game_presents;
    others = 0;
    return false;
  }
  if (++others < 3 || g_native9 == nullptr) return false;
  others = 0;
  ComPtr<IDirect3DDevice9> owner;
  if (FAILED(self->GetDevice(&owner)) || owner.Get() != g_native9) return false;
  g_adopted_swap9 = self;
  g_native_swap9 = self;
  char text[160];
  std::snprintf(text, sizeof(text), "DX9: the game presents through an additional swap chain %p, not the device's own: HSF follows that one now", static_cast<void*>(self));
  Log(text);
  return true;
}

// Before an adopted swap chain goes away (ReShade's destroy_swapchain) or a Reset: back to the device's own until another one presents.
void DropAdoptedSwap9() {
  if (g_adopted_swap9 == nullptr) return;
  g_adopted_swap9.Reset();
  g_native_swap9 = nullptr;
  ComPtr<IDirect3DSwapChain9> implicit;
  if (g_native9 != nullptr && SUCCEEDED(g_native9->GetSwapChain(0, &implicit))) g_native_swap9 = implicit.Get();
}

bool FollowedBackBuffer9(ComPtr<IDirect3DSurface9>& back) {
  if (g_native_swap9 != nullptr) return SUCCEEDED(g_native_swap9->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back));
  return g_native9 != nullptr && SUCCEEDED(g_native9->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back));
}

void BridgeEarly9() {
  if (!g_keep_overlays_out || !hsf::dlssg::Feeding() || g_native9 == nullptr) return;
  ComPtr<IDirect3DSurface9> back;
  if (!FollowedBackBuffer9(back)) return;
  hsf::dx9::BridgeHudless(back.Get());
}

void BeforePresent9() {
  ++g_game_presents;
  if (!hsf::dlssg::Feeding()) return;
  ComPtr<IDirect3DSurface9> back;
  if (!FollowedBackBuffer9(back)) return;
  const hsf::dx9::Frame f = hsf::dx9::Bridge(back.Get(), g_motion9, g_depth9);
  if (f.color == nullptr) return;
  hsf::dlssg::FrameInputs inputs;
  inputs.motion = f.motion;
  inputs.motion_scale[0] = g_dlssg_motion_scale[0];
  inputs.motion_scale[1] = g_dlssg_motion_scale[1];
  // Flow copied at half size (Dx9HalfInputs): a convention in flow texels needs the scale for the smaller texture.
  const int convention = Convention();
  if (f.motion != nullptr && f.motion_width != 0 && f.motion_width != g_seen.mv_width && convention != 0 && !g_input_zero && g_seen.width != 0 && g_seen.height != 0) {
    float to_pixels[2];
    hsf::motion::ConventionScale(convention, static_cast<float>(g_seen.width), static_cast<float>(g_seen.height), static_cast<float>(f.motion_width),
                                  static_cast<float>(f.motion_height), to_pixels);
    inputs.motion_scale[0] = to_pixels[0] / static_cast<float>(g_seen.width);
    inputs.motion_scale[1] = to_pixels[1] / static_cast<float>(g_seen.height);
  }
  inputs.depth = f.depth;
  inputs.depth_inverted = g_depth_inverted;  // HSF.ini DepthInverted, or found (UpdateDepth9)
  inputs.compose = g_compose_enabled;
  inputs.checks = g_checks_enabled;
  inputs.nvof = NvofOn();
  inputs.nvof_size = static_cast<uint32_t>(g_nvof_size);
  inputs.hudless = f.hudless;
  // The motion score (and its debug views) on HSF's own D3D11 device: the bridged frame and flow, both from the same frame.
  if (ScoreRuns() && g_seen.source >= 0 && g_seen.source < hsf::score::MAX_INPUTS) {
    hsf::score::Input score_inputs[hsf::score::MAX_INPUTS];
    if (f.motion != nullptr && f.motion_width != 0 && convention != 0 && g_seen.width != 0 && g_seen.height != 0) {
      score_inputs[g_seen.source].motion = f.motion;
      hsf::motion::ConventionScale(convention, static_cast<float>(g_seen.width), static_cast<float>(g_seen.height), static_cast<float>(f.motion_width),
                                    static_cast<float>(f.motion_height), score_inputs[g_seen.source].to_pixels);
    }
    hsf::score::Frame(hsf::dx9::Device(), f.color, score_inputs);
  }
  hsf::dlssg::Submit(f.color, inputs);
}

void AfterPresent9() {
  if (hsf::dlssg::Feeding()) {
    CapWait(0);
    hsf::dlssg::FrameStart();
  } else {
    CapWait(g_fps_cap);
  }
  const LONGLONG now = Now();
  if (g_last_frame != 0) g_frame_ms[g_frame_write++ % FRAME_HISTORY] = static_cast<float>(std::min(Seconds(now - g_last_frame) * 1000.0, 1000.0));
  g_last_frame = now;
}

// The first few calls of each hook, for the log: which object, and whether it is the one HSF follows.
void LogHookCall(const char* which, const void* self, const void* followed) {
  static int logged = 0;
  if (logged >= 8) return;
  ++logged;
  char text[160];
  std::snprintf(text, sizeof(text), "DX9 hook: %s called on %p (HSF follows %p)%s", which, self, followed, g_in_present9 ? ", inside another Present" : "");
  Log(text);
}

HRESULT STDMETHODCALLTYPE HookedPresent9(IDirect3DDevice9* self, const RECT* from, const RECT* to, HWND window, const RGNDATA* dirty) {
  ++g_present9_calls;
  LogHookCall("device Present", self, g_native9);
  if (self != g_native9 || g_in_present9) return g_real_present9(self, from, to, window, dirty);
  g_in_present9 = true;
  BeforePresent9();
  const HRESULT hr = g_real_present9(self, from, to, window, dirty);
  AfterPresent9();
  g_in_present9 = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresentEx9(IDirect3DDevice9Ex* self, const RECT* from, const RECT* to, HWND window, const RGNDATA* dirty, DWORD flags) {
  ++g_present9_calls;
  LogHookCall("device PresentEx", self, g_native9);
  if (static_cast<IDirect3DDevice9*>(self) != g_native9 || g_in_present9 || (flags & D3DPRESENT_DONOTWAIT) != 0)
    return g_real_present_ex9(self, from, to, window, dirty, flags);
  g_in_present9 = true;
  BeforePresent9();
  const HRESULT hr = g_real_present_ex9(self, from, to, window, dirty, flags);
  AfterPresent9();
  g_in_present9 = false;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookedSwapPresent9(IDirect3DSwapChain9* self, const RECT* from, const RECT* to, HWND window, const RGNDATA* dirty, DWORD flags) {
  ++g_present9_calls;
  LogHookCall("swap chain Present", self, g_native_swap9);
  if (self != g_native_swap9 && !g_in_present9) AdoptSwap9(self);
  if (self != g_native_swap9 || g_in_present9 || (flags & D3DPRESENT_DONOTWAIT) != 0) return g_real_swap_present9(self, from, to, window, dirty, flags);
  g_in_present9 = true;
  BeforePresent9();
  const HRESULT hr = g_real_swap_present9(self, from, to, window, dirty, flags);
  AfterPresent9();
  g_in_present9 = false;
  return hr;
}

// The device's Reset, patched too: a DX9 game that resets into exclusive fullscreen is kept windowed here (the same rule as OnCreateSwapchain,
// which covers the device's creation), so this does not depend on ReShade's swap chain event.
using Reset9Fn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
constexpr size_t DEVICE9_RESET = 16;
Reset9Fn g_real_reset9 = nullptr;
bool WantBorderless();
extern HWND g_borderless_window;
HRESULT STDMETHODCALLTYPE HookedReset9(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* params) {
  if (self == g_native9 && params != nullptr && !params->Windowed && WantBorderless()) {
    if (params->hDeviceWindow != nullptr) {
      g_borderless_window = params->hDeviceWindow;
    } else {
      D3DDEVICE_CREATION_PARAMETERS creation{};
      if (SUCCEEDED(self->GetCreationParameters(&creation))) g_borderless_window = creation.hFocusWindow;
    }
    params->Windowed = TRUE;
    params->FullScreen_RefreshRateInHz = 0;
    // Windowed D3D9 takes only these intervals; a fullscreen-only one (TWO to FOUR) would fail the Reset.
    if (params->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE && params->PresentationInterval != D3DPRESENT_INTERVAL_DEFAULT)
      params->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    static int logged = 0;
    if (logged++ < 3) {
      char text[160];
      std::snprintf(text, sizeof(text), "DX9 Reset into exclusive fullscreen (%ux%u): made it windowed, borderless (HSF.ini Borderless=0 turns this off)",
                    params->BackBufferWidth, params->BackBufferHeight);
      Log(text);
    }
  }
  return g_real_reset9(self, params);
}

// Another hook may take a slot back after HSF patched it, keeping the system's function as its original: the game's frames then go past HSF
// (Dragon's Dogma with Uplift: one frame reached HSF, then none, while the game ran on). When no Present has reached any of HSF's
// hooks for a few frames and a slot no longer holds HSF's hook, HSF patches it again and calls that other hook as its original, so both run.
// A hook that chains to HSF's keeps frames coming, so it is never touched (patching over it would loop).
template <typename Fn>
bool Reclaim9(void** vtable, size_t slot, void* mine, Fn& real, const char* name) {
  if (vtable == nullptr) return false;
  void* const now = vtable[slot];
  if (now == mine || now == nullptr) return false;
  wchar_t path[MAX_PATH] = L"?";
  if (const HMODULE owner = ModuleOf(now)) GetModuleFileNameW(owner, path, MAX_PATH);
  real = reinterpret_cast<Fn>(now);
  PatchSlot(vtable, slot, mine);
  static int logged = 0;
  if (logged++ < 6) {
    char text[MAX_PATH + 200];
    std::snprintf(text, sizeof(text), "DX9: no frames reached HSF and the %s slot was taken by %ls: HSF patched it again and calls that hook after its own", name, path);
    Log(text);
  }
  return true;
}

void ReclaimSlots9() {
  static uint32_t calls_seen = 0;
  static int quiet_frames = 0;
  if (g_present9_calls != calls_seen) {
    calls_seen = g_present9_calls;
    quiet_frames = 0;
    return;
  }
  if (++quiet_frames < 3) return;
  quiet_frames = 0;
  bool any = Reclaim9(g_vtable9, DEVICE9_PRESENT, reinterpret_cast<void*>(&HookedPresent9), g_real_present9, "device Present");
  if (g_vtable9_ex) any |= Reclaim9(g_vtable9, DEVICE9EX_PRESENTEX, reinterpret_cast<void*>(&HookedPresentEx9), g_real_present_ex9, "device PresentEx");
  if (g_vtable9_swap != nullptr) any |= Reclaim9(g_vtable9_swap, SWAP9_PRESENT, reinterpret_cast<void*>(&HookedSwapPresent9), g_real_swap_present9, "swap chain Present");
  static bool told = false;
  if (!any && !told) {
    told = true;
    Log("DX9: no frames reached HSF's Present hooks, which are all still in place: the game presents some other way");
  }
}

void UnhookNativePresent9();
void HookNativePresent9(device* dev) {
  IDirect3DDevice9* const native = reinterpret_cast<IDirect3DDevice9*>(dev->get_native());
  if (native == nullptr) return;
  // Hooked at the first frame, not at the device's creation: hooked that early, AC Revelations' Present never reached HSF. If the
  // device turns up with another vtable later, the hooks move to it.
  if (native == g_native9 && (g_vtable9 == nullptr || *reinterpret_cast<void***>(native) == g_vtable9)) {
    if (g_vtable9 != nullptr) ReclaimSlots9();
    return;
  }
  if (native == g_native9 && g_vtable9 != nullptr) {
    Log("DX9: the device's vtable changed: hooks moved to the new one");
    UnhookNativePresent9();
  }
  g_adopted_swap9.Reset();
  g_native9 = native;
  g_native_swap9 = nullptr;
  ComPtr<IDirect3DSwapChain9> swap;
  if (SUCCEEDED(native->GetSwapChain(0, &swap))) g_native_swap9 = swap.Get();
  if (g_vtable9 != nullptr) return;  // patched already: the class's vtable is shared
  void** const vtable = *reinterpret_cast<void***>(native);
  const HMODULE present_module = ModuleOf(vtable[DEVICE9_PRESENT]);
  const HMODULE reshade_module = reshade::internal::get_reshade_module_handle();
  wchar_t path[MAX_PATH] = L"?";
  GetModuleFileNameW(present_module, path, MAX_PATH);
  const bool ok = present_module != nullptr && present_module != reshade_module;
  ComPtr<IDirect3DDevice9Ex> ex;
  const bool is_ex = SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&ex)));
  char text[MAX_PATH + 160];
  std::snprintf(text, sizeof(text), "Direct3D 9%s: native Present is in %ls (%s)", is_ex ? "Ex" : "", path, ok ? "the system's, using it" : "ReShade's proxy: presenter disabled");
  Log(text);
  if (!ok) return;
  g_real_present9 = reinterpret_cast<Present9Fn>(vtable[DEVICE9_PRESENT]);
  PatchSlot(vtable, DEVICE9_PRESENT, reinterpret_cast<void*>(&HookedPresent9));
  g_real_reset9 = reinterpret_cast<Reset9Fn>(vtable[DEVICE9_RESET]);
  PatchSlot(vtable, DEVICE9_RESET, reinterpret_cast<void*>(&HookedReset9));
  std::snprintf(text, sizeof(text), "DX9 hooks: Present slot now %p (HSF's %p), Reset slot now %p", vtable[DEVICE9_PRESENT], reinterpret_cast<void*>(&HookedPresent9), vtable[DEVICE9_RESET]);
  Log(text);
  if (is_ex) {
    g_real_present_ex9 = reinterpret_cast<PresentEx9Fn>(vtable[DEVICE9EX_PRESENTEX]);
    PatchSlot(vtable, DEVICE9EX_PRESENTEX, reinterpret_cast<void*>(&HookedPresentEx9));
    g_vtable9_ex = true;
  }
  if (g_native_swap9 != nullptr) {
    void** const swap_vtable = *reinterpret_cast<void***>(g_native_swap9);
    g_real_swap_present9 = reinterpret_cast<SwapPresent9Fn>(swap_vtable[SWAP9_PRESENT]);
    PatchSlot(swap_vtable, SWAP9_PRESENT, reinterpret_cast<void*>(&HookedSwapPresent9));
    g_vtable9_swap = swap_vtable;
  }
  g_vtable9 = vtable;
}

void UnhookNativePresent9() {
  if (g_vtable9 == nullptr) return;
  PatchSlot(g_vtable9, DEVICE9_PRESENT, reinterpret_cast<void*>(g_real_present9));
  PatchSlot(g_vtable9, DEVICE9_RESET, reinterpret_cast<void*>(g_real_reset9));
  if (g_vtable9_ex) PatchSlot(g_vtable9, DEVICE9EX_PRESENTEX, reinterpret_cast<void*>(g_real_present_ex9));
  if (g_vtable9_swap != nullptr) PatchSlot(g_vtable9_swap, SWAP9_PRESENT, reinterpret_cast<void*>(g_real_swap_present9));
  g_vtable9 = g_vtable9_swap = nullptr;
  g_vtable9_ex = false;
}

// HSF.fx's depth copy and LaunchPad, switched on as needed; the depth copy's D3D9 texture into g_depth9 (nullptr while it is not there).
void FindDepth9(effect_runtime* runtime, device* dev) {
  static ULONGLONG next_search = 0;
  static effect_texture_variable depth_variable = {};
  static uint32_t reload_seen = 0;
  if (reload_seen != g_effect_reloads) {
    reload_seen = g_effect_reloads;
    depth_variable = {};
    next_search = 0;
  }
  const ULONGLONG now = GetTickCount64();
  if (now >= next_search) {
    next_search = now + 1000;
    if (depth_variable.handle == 0) depth_variable = runtime->find_texture_variable("HSF.fx", "HSFDepthCopy");
    for (const auto& [effect, name] : {std::pair{"HSF.fx", "HSF_DepthCopy"}, std::pair{"HSF.fx", "HSF_Depth"}, std::pair{"MartysMods_LAUNCHPAD.fx", "MartysMods_Launchpad"}}) {
      const effect_technique technique = runtime->find_technique(effect, name);
      if (technique.handle != 0 && !runtime->get_technique_state(technique)) {
        runtime->set_technique_state(technique, true);
        char text[160];
        std::snprintf(text, sizeof(text), "switched on %s (%s)", name, effect);
        Log(text);
      }
    }
  }
  g_depth9 = nullptr;
  g_depth_vk = {0};
  if (!g_depth_enabled || g_depth_ab_off || depth_variable.handle == 0) return;
  resource_view srv{}, srv_srgb{};
  runtime->get_texture_binding(depth_variable, &srv, &srv_srgb);
  if (srv.handle == 0 || ShadowMapBound(dev, srv)) return;
  const resource depth = dev->get_resource_from_view(srv);
  if (dev->get_resource_desc(depth).type != resource_type::texture_2d) return;
  if (dev->get_api() == device_api::vulkan) g_depth_vk = depth;
  else g_depth9 = reinterpret_cast<IDirect3DTexture9*>(depth.handle);
}

// Starts, restarts (new window or size) or stops DLSS-G for a D3D9 game.
void UpdatePresenter9() {
  static HWND running_window = nullptr, refused_window = nullptr;
  static UINT running_size[2] = {}, refused_size[2] = {};
  if (hsf::dlssg::Reap()) Log("presenter stopped (finished late)");
  if (StopHelperWhileOff()) {
    refused_window = nullptr;
    return;
  }
  if (hsf::dlssg::Stopping()) {
    Note("presenter stopping: waiting for its thread");
    return;
  }
  const auto stop = [](const char* why) {
    if (!hsf::dlssg::Running()) return;
    const char* const late = hsf::dlssg::Stop();
    Log(late != nullptr ? late : why);
  };
  if (g_vtable9 == nullptr || g_native_swap9 == nullptr) {
    stop("presenter stopped");
    refused_window = nullptr;
    Note("DX9: no native Present to use");
    return;
  }
  D3DPRESENT_PARAMETERS params{};
  ComPtr<IDirect3DSurface9> back;
  D3DSURFACE_DESC desc{};
  if (FAILED(g_native_swap9->GetPresentParameters(&params)) || FAILED(g_native_swap9->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &back)) || FAILED(back->GetDesc(&desc))) return;
  HWND window = params.hDeviceWindow;
  if (window == nullptr) {
    D3DDEVICE_CREATION_PARAMETERS creation{};
    if (SUCCEEDED(g_native9->GetCreationParameters(&creation))) window = creation.hFocusWindow;
  }
  if (!params.Windowed) {
    stop("presenter stopped: exclusive fullscreen");
    Note("exclusive fullscreen: the presenter needs windowed or borderless");
    return;
  }
  const bool same_running = window == running_window && desc.Width == running_size[0] && desc.Height == running_size[1];
  if (hsf::dlssg::Running() && same_running) return;
  if (window == refused_window && desc.Width == refused_size[0] && desc.Height == refused_size[1]) return;  // failed for this window and size already; a new size tries again
  if (hsf::dlssg::Running()) {
    stop("presenter stopped: the game's window or size changed");
    return;
  }
  char error[320] = "";
  char text[480];
  const DXGI_FORMAT format = hsf::dx9::ToDxgi(desc.Format);
  bool started = false;
  if (format == DXGI_FORMAT_UNKNOWN)
    std::snprintf(error, sizeof(error), "DX9 back buffer format %u is not supported yet", static_cast<unsigned>(desc.Format));
  else if (hsf::dx9::Start(g_native9, error, sizeof(error)))
    started = hsf::dlssg::Start(hsf::dx9::Device(), window, desc.Width, desc.Height, format, g_sl_folder, error, sizeof(error));
  if (started) {
    running_window = window;
    running_size[0] = desc.Width;
    running_size[1] = desc.Height;
    std::snprintf(text, sizeof(text), "DLSS-G presenter started for Direct3D 9: %ux%u D3D9 format %u, bridge %s%s%s", desc.Width, desc.Height,
                  static_cast<unsigned>(desc.Format), hsf::dx9::ROUTE_NAMES[static_cast<int>(hsf::dx9::CurrentRoute())], error[0] != '\0' ? "; " : "", error);
    Note("presenter on: DLSS Frame Generation (log: HSF_streamline\\hsf_dlssg.log)");
  } else {
    refused_window = window;
    refused_size[0] = desc.Width;
    refused_size[1] = desc.Height;
    std::snprintf(text, sizeof(text), "presenter could not start: %s", error);
    Note(text);
  }
  Log(text);
}

// ---- DX9 depth: which way it runs, and the debug view ----
// From the bridge's depth grid (dx9::DepthGrid), twice a second. The median alone (D3D11's rule: most of a picture sits near far) fails in
// close third-person scenes, where the player and the walls fill the picture. So the main signal is the floor: the bottom quarter of the
// picture is nearer than the top quarter in almost every game, so if its values are larger, larger is nearer and the depth is reversed.
// A decision needs DEPTH9_VOTES reads in a row the other way to change; with no clear difference between the two, the median decides.
constexpr int DEPTH9_VOTES = 4;
struct Depth9 {
  uint32_t serial = 0;
  bool measured = false, inverted = false, flat = true, by_floor = false;
  int votes = 0;  // > 0: reads said reversed, < 0: normal
  float low = 0, median = 0, high = 0, bottom = 0, top = 0;
  device* dev = nullptr;
  resource texture{};
  resource_view view{};
  std::vector<uint32_t> pixels;
};
Depth9 g_d9;

float Median(std::vector<float>& v) {
  if (v.empty()) return 0.f;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

void ReleaseDepth9View() {
  if (g_d9.dev != nullptr) {
    if (g_d9.view.handle != 0) g_d9.dev->destroy_resource_view(g_d9.view);
    if (g_d9.texture.handle != 0) g_d9.dev->destroy_resource(g_d9.texture);
  }
  g_d9.view = {};
  g_d9.texture = {};
  g_d9.dev = nullptr;
}

void UpdateDepth9(device* dev) {
  const float* values = nullptr;
  const uint32_t serial = hsf::dx9::DepthGrid(&values);
  if (serial == 0 || serial == g_d9.serial || values == nullptr) return;
  g_d9.serial = serial;
  constexpr unsigned W = hsf::dx9::DEPTH_GRID_W, H = hsf::dx9::DEPTH_GRID_H;
  std::vector<float> all(values, values + W * H);
  std::sort(all.begin(), all.end());
  g_d9.low = all[all.size() / 50];
  g_d9.high = all[all.size() - 1 - all.size() / 50];
  g_d9.median = all[all.size() / 2];
  g_d9.flat = g_d9.high - g_d9.low < 1e-5f;
  if (!g_d9.flat) {
    std::vector<float> bottom, top;
    for (unsigned y = 0; y < H / 4; ++y)
      for (unsigned x = W / 8; x < W - W / 8; ++x) {
        top.push_back(values[y * W + x]);
        bottom.push_back(values[(H - 1 - y) * W + x]);
      }
    g_d9.bottom = Median(bottom);
    g_d9.top = Median(top);
    const float gap = g_d9.bottom - g_d9.top;
    g_d9.by_floor = std::fabs(gap) > (g_d9.high - g_d9.low) * 0.05f;
    const bool says_reversed = g_d9.by_floor ? gap > 0.f : g_d9.median < 0.5f;
    g_d9.votes = std::clamp(g_d9.votes + (says_reversed ? 1 : -1), -DEPTH9_VOTES, DEPTH9_VOTES);
    if (!g_d9.measured) g_d9.inverted = says_reversed;
    else if (g_d9.votes >= DEPTH9_VOTES) g_d9.inverted = true;
    else if (g_d9.votes <= -DEPTH9_VOTES) g_d9.inverted = false;
    g_d9.measured = true;
  }
  // The view: white = near, black = far, stretched over the picture's own range, whichever way the depth runs.
  if (g_score_view != hsf::score::View::depth) return;
  const bool inverted = g_depth_inverted_setting >= 0 ? g_depth_inverted_setting != 0 : g_d9.inverted;
  const float range = std::max(g_d9.high - g_d9.low, 1e-6f);
  g_d9.pixels.resize(W * H);
  for (unsigned i = 0; i < W * H; ++i) {
    float t = std::clamp((values[i] - g_d9.low) / range, 0.f, 1.f);
    if (!inverted) t = 1.f - t;
    const uint32_t g = static_cast<uint32_t>(t * 255.f + 0.5f);
    g_d9.pixels[i] = 0xFF000000u | (g << 16) | (g << 8) | g;
  }
  if (g_d9.texture.handle == 0) {
    g_d9.dev = dev;
    const subresource_data first{g_d9.pixels.data(), W * 4, W * H * 4};
    if (!dev->create_resource(resource_desc(W, H, 1, 1, format::b8g8r8a8_unorm, 1, memory_heap::gpu_only, resource_usage::shader_resource), &first,
                              resource_usage::shader_resource, &g_d9.texture) ||
        !dev->create_resource_view(g_d9.texture, resource_usage::shader_resource, resource_view_desc(format::b8g8r8a8_unorm), &g_d9.view)) {
      static bool logged = false;
      if (!logged) Log("DX9 depth view: could not make its texture");
      logged = true;
      ReleaseDepth9View();
    }
    return;
  }
  dev->update_texture_region(subresource_data{g_d9.pixels.data(), W * 4, W * H * 4}, g_d9.texture, 0);
}

// The score's debug views (motion, error, better/worse) for a DX9 game: the score runs on HSF's D3D11 device, which ImGui on D3D9 cannot draw
// from, so the view comes over to the CPU (score::ReadView) and up into a D3D9 texture.
struct ScoreView9 {
  uint32_t serial = 0;
  UINT width = 0, height = 0;
  device* dev = nullptr;
  resource texture{};
  resource_view view{};
  std::vector<uint32_t> pixels;
};
ScoreView9 g_v9;

void ReleaseScoreView9() {
  if (g_v9.dev != nullptr) {
    if (g_v9.view.handle != 0) g_v9.dev->destroy_resource_view(g_v9.view);
    if (g_v9.texture.handle != 0) g_v9.dev->destroy_resource(g_v9.texture);
  }
  g_v9 = ScoreView9{};
}

void UpdateScoreView9(device* dev) {
  if (g_score_view == hsf::score::View::off || g_score_view == hsf::score::View::depth) return;
  const uint32_t* rgba = nullptr;
  UINT w = 0, h = 0;
  const uint32_t serial = hsf::score::ReadView(g_seen.source, g_score_view, &rgba, &w, &h);
  if (serial == 0) {
    g_v9.serial = 0;  // nothing of this view yet: DrawScoreView says so instead of showing another view's picture
    return;
  }
  if (serial == g_v9.serial || rgba == nullptr || w == 0 || h == 0) return;
  g_v9.pixels.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < g_v9.pixels.size(); ++i) {  // RGBA to BGRA, opaque
    const uint32_t c = rgba[i];
    g_v9.pixels[i] = 0xFF000000u | ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
  }
  if (g_v9.texture.handle != 0 && (g_v9.width != w || g_v9.height != h)) {
    std::vector<uint32_t> keep = std::move(g_v9.pixels);
    ReleaseScoreView9();
    g_v9.pixels = std::move(keep);
  }
  if (g_v9.texture.handle == 0) {
    g_v9.dev = dev;
    g_v9.width = w;
    g_v9.height = h;
    const subresource_data first{g_v9.pixels.data(), w * 4, w * h * 4};
    if (!dev->create_resource(resource_desc(w, h, 1, 1, format::b8g8r8a8_unorm, 1, memory_heap::gpu_only, resource_usage::shader_resource), &first,
                              resource_usage::shader_resource, &g_v9.texture) ||
        !dev->create_resource_view(g_v9.texture, resource_usage::shader_resource, resource_view_desc(format::b8g8r8a8_unorm), &g_v9.view)) {
      static bool logged = false;
      if (!logged) Log("DX9 score view: could not make its texture");
      logged = true;
      ReleaseScoreView9();
      return;
    }
  } else {
    dev->update_texture_region(subresource_data{g_v9.pixels.data(), w * 4, w * h * 4}, g_v9.texture, 0);
  }
  g_v9.serial = serial;
}

// DepthLine for a DX9 game; false when this is not one.
bool Dx9DepthLine(char* text, size_t size, const char* sent) {
  if (g_native9 == nullptr) return false;
  if (!g_d9.measured)
    std::snprintf(text, size, "depth: %s; DX9, %s", sent, g_d9.serial == 0 ? "not read yet" : "flat so far");
  else
    std::snprintf(text, size, "depth: %s; DX9, values %.4f / %.4f / %.4f%s, %s %s (bottom %.4f, top %.4f, %s)", sent, g_d9.low, g_d9.median, g_d9.high,
                  g_d9.flat ? ", flat" : "", g_depth_inverted ? "reversed" : "normal", g_depth_inverted_setting >= 0 ? "(HSF.ini)" : "(found)", g_d9.bottom, g_d9.top,
                  g_d9.by_floor ? "by the floor" : "by the median");
  return true;
}

void CheckRefused9();
void ClearDx9Run();
void Dx9Frame(effect_runtime* runtime, device* dev) {
  CheckRefused9();
  HookNativePresent9(dev);
  FindDepth9(runtime, dev);
  UpdateDepth9(dev);
  UpdateScoreView9(dev);
  g_depth_inverted = g_depth_inverted_setting >= 0 ? g_depth_inverted_setting != 0 : g_d9.inverted;
  UpdateDlssgMotionScale();
  if (g_mfg_key != 0 && !g_menu_open && runtime->is_key_pressed(g_mfg_key)) {
    g_dlssg_frames = g_dlssg_frames >= std::max(g_dlssg_stats.max_frames, 1u) ? 1 : g_dlssg_frames + 1;
    SaveIni();
  }
  hsf::dlssg::SetOptions(g_extra_enabled, g_dlssg_frames);
  hsf::dlssg::SetMenuOpen(g_menu_open);
  hsf::dlssg::SetReflex(CapInForce(), g_reflex_boost);
  hsf::dlssg::SetReflexSleep(g_reflex_sleep);
  UpdatePresenter9();
  BridgeEarly9();
  static ULONGLONG next_log = 0;
  const ULONGLONG now = GetTickCount64();
  if (now >= next_log && hsf::dlssg::Running()) {
    next_log = now + 10000;
    const hsf::dx9::Stats s = hsf::dx9::TakeStats();
    char text[640];
    std::snprintf(text, sizeof(text), "DX9 bridge (%s): %.2f ms per frame on the game thread (worst %.2f), %u frames, %u failed; flow %s, depth %s; each: %s%s%s",
                  hsf::dx9::ROUTE_NAMES[static_cast<int>(hsf::dx9::CurrentRoute())], s.bridge_ms, s.worst_ms, s.frames, s.failed,
                  g_motion9 != nullptr ? "found" : "NOT found (LaunchPad?)", g_depth9 != nullptr ? "found" : "NOT found (HSF.fx?)", s.lanes,
                  s.problem[0] != '\0' ? "; last problem: " : "", s.problem);
    Log(text);
    if (s.depth_checks != 0) {
      std::snprintf(text, sizeof(text), "DX9 depth: range %.6f to %.6f over %u checks%s; %s %s (bottom %.4f, top %.4f, median %.4f, %s); %s", s.depth_min, s.depth_max, s.depth_checks,
                    s.depth_max - s.depth_min < 1e-4f ? " (FLAT: no real depth reaches DLSS-G; pick another buffer in ReShade's Add-ons tab, Generic Depth)" : "",
                    g_depth_inverted ? "reversed" : "normal", g_depth_inverted_setting >= 0 ? "(HSF.ini)" : "(found)", g_d9.bottom, g_d9.top, g_d9.median,
                    g_d9.by_floor ? "by the floor" : "by the median",
                    g_keep_overlays_out ? "overlays kept out of generated frames (KeepOverlaysOut=1)" : "overlays generated too (KeepOverlaysOut=0)");
      Log(text);
    }
  }
}

// ---- Vulkan games (vk_bridge.h) ----
// Like DX9: the game's frame, LaunchPad's flow and HSF.fx's depth copy are copied into textures of HSF's own D3D11 device, which go to the
// helper exactly like a D3D11 game's. The copies are queued at ReShade's present (overlays drawn) and handed over one frame later, once the
// game's GPU finished them. Reflex's frame start runs right after the hand-off, still before the game's own present.

// Starts, restarts (new window or size) or stops DLSS-G for a Vulkan game.
void UpdatePresenterVk(effect_runtime* runtime, device* dev) {
  static HWND running_window = nullptr, refused_window = nullptr;
  static UINT running_size[2] = {}, refused_size[2] = {};
  if (StopHelperWhileOff()) {
    refused_window = nullptr;
    return;
  }
  const auto stop = [](const char* why) {
    if (!hsf::dlssg::Running()) return;
    const char* const late = hsf::dlssg::Stop();
    Log(late != nullptr ? late : why);
  };
  const resource back = runtime->get_current_back_buffer();
  if (back.handle == 0) return;
  const resource_desc desc = dev->get_resource_desc(back);
  const HWND window = static_cast<HWND>(runtime->get_hwnd());
  const UINT width = desc.texture.width, height = desc.texture.height;
  if (hsf::dlssg::Running() && window == running_window && width == running_size[0] && height == running_size[1]) return;
  if (window == refused_window && width == refused_size[0] && height == refused_size[1]) return;  // failed for this window and size already
  if (hsf::dlssg::Running()) {
    stop("presenter stopped: the game's window or size changed");
    return;
  }
  char error[320] = "";
  char text[480];
  const DXGI_FORMAT format = hsf::vk::ToDxgi(desc.texture.format);
  bool started = false;
  if (format == DXGI_FORMAT_UNKNOWN)
    std::snprintf(error, sizeof(error), "Vulkan back buffer format %u is not supported yet", static_cast<unsigned>(desc.texture.format));
  else if (hsf::vk::Start(dev, error, sizeof(error))) {
    hsf::dlssg::SetOverlayWindow(g_vk_overlay_window);
    hsf::dlssg::SetColorSpace(GameColorSpace());
    started = hsf::dlssg::Start(hsf::vk::Device(), window, width, height, format, g_sl_folder, error, sizeof(error));
  }
  if (started) {
    running_window = window;
    running_size[0] = width;
    running_size[1] = height;
    std::snprintf(text, sizeof(text), "DLSS-G presenter started for Vulkan: %ux%u format %u, %s", width, height, static_cast<unsigned>(desc.texture.format),
                  hsf::vk::SameFrame() ? "frames handed over in the same frame (shared semaphores on the game's device)"
                                       : "frames handed over in the same frame after a CPU wait for the copies (no shared semaphores on the game's device)");
    Note("presenter on: DLSS Frame Generation (log: HSF_streamline\\hsf_dlssg.log)");
    const char* const hook = hsf::vk::HookReport();
    Log(hook[0] != '\0' ? hook : "Vulkan device hook: did not see the game's device (HSF's Vulkan layer may still have added the extensions)");
  } else {
    refused_window = window;
    refused_size[0] = width;
    refused_size[1] = height;
    std::snprintf(text, sizeof(text), "presenter could not start: %s", error);
    Note(text);
  }
  Log(text);
}

// The F8 depth view in a Vulkan game: hsf::depth draws it on HSF's D3D11 device, which ImGui on Vulkan cannot draw from, so it comes over to
// the CPU (a staging copy, read a frame later without waiting) and up into a texture on the game's device.
ScoreView9 g_vkv;
ComPtr<ID3D11Texture2D> g_vkv_staging;
bool g_vkv_pending = false;

// `destroy` false: the game's device is already out of ReShade's table, so the texture is left to it.
void ReleaseVkView(bool destroy) {
  if (destroy && g_vkv.dev != nullptr) {
    if (g_vkv.view.handle != 0) g_vkv.dev->destroy_resource_view(g_vkv.view);
    if (g_vkv.texture.handle != 0) g_vkv.dev->destroy_resource(g_vkv.texture);
  }
  g_vkv = ScoreView9{};
  g_vkv_staging.Reset();
  g_vkv_pending = false;
}

void UpdateVkView(device* dev) {
  if (g_score_view != hsf::score::View::depth) return;
  ID3D11ShaderResourceView* const srv = hsf::depth::View();
  ID3D11Device* const d11 = hsf::vk::Device();
  if (srv == nullptr || d11 == nullptr) return;
  ComPtr<ID3D11Resource> resource;
  srv->GetResource(&resource);
  ComPtr<ID3D11Texture2D> picture;
  if (resource == nullptr || FAILED(resource.As(&picture))) return;
  D3D11_TEXTURE2D_DESC d{};
  picture->GetDesc(&d);
  ComPtr<ID3D11DeviceContext> context;
  d11->GetImmediateContext(&context);
  if (g_vkv_staging != nullptr) {
    D3D11_TEXTURE2D_DESC have{};
    g_vkv_staging->GetDesc(&have);
    if (have.Width != d.Width || have.Height != d.Height) {
      g_vkv_staging.Reset();
      g_vkv_pending = false;
    }
  }
  if (g_vkv_pending) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_vkv_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return;  // not there yet
    g_vkv.pixels.resize(static_cast<size_t>(d.Width) * d.Height);
    for (UINT y = 0; y < d.Height; ++y) std::memcpy(&g_vkv.pixels[static_cast<size_t>(y) * d.Width], static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch, d.Width * 4);
    context->Unmap(g_vkv_staging.Get(), 0);
    g_vkv_pending = false;
    if (g_vkv.texture.handle != 0 && (g_vkv.width != d.Width || g_vkv.height != d.Height)) {
      std::vector<uint32_t> keep = std::move(g_vkv.pixels);
      ReleaseVkView(true);
      g_vkv.pixels = std::move(keep);
    }
    const subresource_data data{g_vkv.pixels.data(), d.Width * 4, d.Width * d.Height * 4};
    if (g_vkv.texture.handle == 0) {
      g_vkv.dev = dev;
      g_vkv.width = d.Width;
      g_vkv.height = d.Height;
      if (!dev->create_resource(resource_desc(d.Width, d.Height, 1, 1, format::r8g8b8a8_unorm, 1, memory_heap::gpu_only, resource_usage::shader_resource), &data,
                                resource_usage::shader_resource, &g_vkv.texture) ||
          !dev->create_resource_view(g_vkv.texture, resource_usage::shader_resource, resource_view_desc(format::r8g8b8a8_unorm), &g_vkv.view)) {
        static bool logged = false;
        if (!logged) Log("Vulkan depth view: could not make its texture");
        logged = true;
        ReleaseVkView(true);
        return;
      }
    } else {
      dev->update_texture_region(data, g_vkv.texture, 0);
    }
    g_vkv.serial = 1;
  }
  if (g_vkv_staging == nullptr) {
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(d11->CreateTexture2D(&d, nullptr, &g_vkv_staging))) return;
  }
  context->CopyResource(g_vkv_staging.Get(), picture.Get());
  g_vkv_pending = true;
}

// HSF's depth module (depth.h) on the bridged depth copy, on HSF's own D3D11 device: it finds whether the depth is reversed or upside down,
// measures it for the log and the panel, and gives DLSS-G the copy at the flow's size, put right side up, like in a D3D11 game.
struct VkDepthViews {
  ID3D11Texture2D* texture[3] = {};
  ComPtr<ID3D11ShaderResourceView> view[3];
};
VkDepthViews g_vk_depth_views;

void StopVkDepth() {
  hsf::depth::Shutdown();
  g_vk_depth_views = VkDepthViews{};
}

ID3D11Texture2D* VkDepth(const hsf::vk::Frame& f) {
  ID3D11Device* const d11 = hsf::vk::Device();
  if (f.depth == nullptr || d11 == nullptr) return nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  for (int i = 0; i < 3 && srv == nullptr; ++i)
    if (g_vk_depth_views.texture[i] == f.depth) srv = g_vk_depth_views.view[i].Get();
  if (srv == nullptr) {
    static int next = 0;
    next = (next + 1) % 3;
    g_vk_depth_views.view[next].Reset();
    g_vk_depth_views.texture[next] = nullptr;
    if (FAILED(d11->CreateShaderResourceView(f.depth, nullptr, &g_vk_depth_views.view[next]))) return nullptr;
    g_vk_depth_views.texture[next] = f.depth;
    srv = g_vk_depth_views.view[next].Get();
  }
  D3D11_TEXTURE2D_DESC size{};
  (f.motion != nullptr ? f.motion : f.color)->GetDesc(&size);
  hsf::depth::SetUpsideDown(g_depth_upside_down_setting);
  ID3D11Texture2D* const copy = hsf::depth::Frame(d11, srv, size.Width, size.Height, f.hudless != nullptr ? f.hudless : f.color);
  g_depth_inverted = g_depth_inverted_setting >= 0 ? g_depth_inverted_setting != 0 : hsf::depth::Current().inverted;
  return copy;
}

// The motion score and its debug views (motion, error, helped) on HSF's D3D11 device, from the bridged frame and flow, as in a DX9 game.
void VkScore(const hsf::vk::Frame& f) {
  if (!ScoreRuns() || g_seen.source < 0 || g_seen.source >= hsf::score::MAX_INPUTS) return;
  hsf::score::Input score_inputs[hsf::score::MAX_INPUTS];
  const int convention = Convention();
  if (f.motion != nullptr && convention != 0 && g_seen.width != 0 && g_seen.height != 0) {
    D3D11_TEXTURE2D_DESC m{};
    f.motion->GetDesc(&m);
    score_inputs[g_seen.source].motion = f.motion;
    hsf::motion::ConventionScale(convention, static_cast<float>(g_seen.width), static_cast<float>(g_seen.height), static_cast<float>(m.Width),
                                  static_cast<float>(m.Height), score_inputs[g_seen.source].to_pixels);
  }
  hsf::score::Frame(hsf::vk::Device(), f.hudless != nullptr ? f.hudless : f.color, score_inputs);
}

// At ReShade's finish_effects in a Vulkan game.
void VkFrame(effect_runtime* runtime, device* dev, resource target) {
  FindDepth9(runtime, dev);
  // No game depth: ReShade binds a placeholder (no view, or a tiny texture) to HSF.fx's DEPTH texture, and the copy holds nothing. DLSS-G then
  // gets a constant depth, as in a D3D11 game, instead of a copy of nothing.
  {
    static effect_texture_variable depth_semantic = {};
    static uint32_t reload_seen = ~0u;
    if (reload_seen != g_effect_reloads || depth_semantic.handle == 0) {
      reload_seen = g_effect_reloads;
      depth_semantic = runtime->find_texture_variable("HSF.fx", "HSFDepth");
    }
    bool real = false;
    if (depth_semantic.handle != 0) {
      resource_view srv{}, srv_srgb{};
      runtime->get_texture_binding(depth_semantic, &srv, &srv_srgb);
      if (srv.handle != 0) {
        const resource_desc d = dev->get_resource_desc(dev->get_resource_from_view(srv));
        real = d.type == resource_type::texture_2d && d.texture.width * 2 >= g_seen.width && d.texture.height * 2 >= g_seen.height &&
               !ShadowMapBound(dev, srv);
      }
    }
    static int logged = -1;
    if (logged != (real ? 1 : 0)) {
      logged = real ? 1 : 0;
      Log(real ? "Vulkan: the game has a depth buffer, DLSS-G gets real depth" : "Vulkan: no game depth buffer bound, DLSS-G gets a constant depth");
    }
    if (!real) g_depth_vk = {0};
  }
  if (g_view_key != 0 && !g_menu_open && runtime->is_key_pressed(g_view_key))
    g_score_view = static_cast<hsf::score::View>((static_cast<int>(g_score_view) + 1) % hsf::score::VIEW_COUNT);
  UpdateDlssgMotionScale();
  if (g_mfg_key != 0 && !g_menu_open && runtime->is_key_pressed(g_mfg_key)) {
    g_dlssg_frames = g_dlssg_frames >= std::max(g_dlssg_stats.max_frames, 1u) ? 1 : g_dlssg_frames + 1;
    SaveIni();
  }
  hsf::dlssg::SetOptions(g_extra_enabled, g_dlssg_frames);
  hsf::dlssg::SetMenuOpen(g_menu_open);
  hsf::dlssg::SetReflex(CapInForce(), g_reflex_boost);
  hsf::dlssg::SetReflexSleep(g_reflex_sleep);
  UpdatePresenterVk(runtime, dev);
  // The frame before ReShade's menu and the overlays are drawn: DLSS-G makes the generated frames from it (KeepOverlaysOut).
  if (g_keep_overlays_out && hsf::dlssg::Feeding()) hsf::vk::BridgeHudless(runtime->get_command_queue(), target, resource_usage::render_target);
  static ULONGLONG next_log = 0;
  const ULONGLONG now = GetTickCount64();
  if (now >= next_log && hsf::dlssg::Running()) {
    next_log = now + 10000;
    const hsf::vk::Stats s = hsf::vk::TakeStats();
    char text[800];
    char waits[120] = "";
    if (s.waits != 0)
      std::snprintf(waits, sizeof(waits), "; CPU waited %.2f ms per frame for the copies, %u of %u too late (went one frame behind)", s.wait_ms / s.waits, s.late, s.waits);
    std::snprintf(text, sizeof(text), "Vulkan bridge: %u frames copied, %u handed over, %u skipped (no free set), %u failed%s; flow %s, depth %s%s%s", s.frames,
                  s.handed, s.busy, s.failed, waits, g_motion_vk.handle != 0 ? "found" : "NOT found (LaunchPad?)",
                  g_depth_vk.handle != 0 ? "found" : "NOT found (no game depth buffer, or HSF.fx off)", s.problem[0] != '\0' ? "; last problem: " : "", s.problem);
    Log(text);
  }
}

bool g_vk_finish_present = false;  // ReShade calls finish_present for the Vulkan game: the next frame starts there (OnVkFinishPresent)

// ReShade's present in a Vulkan game: the frame is finished. Queues its copies, hands an earlier finished one to DLSS-G, then the next frame starts.
void OnVkPresent(effect_runtime* runtime) {
  if (runtime->get_device()->get_api() != device_api::vulkan) return;
  ++g_game_presents;
  if (hsf::dlssg::Feeding()) {
    const hsf::vk::Frame f =
        hsf::vk::Bridge(runtime->get_command_queue(), runtime->get_current_back_buffer(), resource_usage::present, g_motion_vk, g_depth_vk);
    if (f.color != nullptr) {
      hsf::dlssg::FrameInputs inputs;
      inputs.motion = f.motion;
      inputs.motion_scale[0] = g_dlssg_motion_scale[0];
      inputs.motion_scale[1] = g_dlssg_motion_scale[1];
      inputs.depth = VkDepth(f);
      inputs.depth_inverted = g_depth_inverted;
      inputs.compose = g_compose_enabled;
      inputs.checks = g_checks_enabled;
      inputs.nvof = NvofOn();
      inputs.nvof_size = static_cast<uint32_t>(g_nvof_size);
      inputs.hudless = f.hudless;
      VkScore(f);
      UpdateVkView(runtime->get_device());
      UpdateScoreView9(runtime->get_device());
      hsf::dlssg::Submit(f.color, inputs);
      hsf::vk::Handed(f);
    }
  }
  // reshade_present runs before the game's vkQueuePresentKHR. Sleeping here holds the finished frame back from its present, and the present
  // can then block again, so the next frame starts later than Reflex planned. With finish_present the sleep runs after the present instead.
  if (!g_vk_finish_present) AfterPresent9();
}

// ReShade 6.8's finish_present (event 100): the game's vkQueuePresentKHR has returned, so the next frame starts here, as after Present on
// D3D11 and DX9. HSF's ReShade headers predate the event, so it is registered by number; older ReShade versions refuse the number and never
// call it, and the sleep stays at reshade_present.
void OnVkFinishPresent(command_queue*, swapchain* swap) {
  if (swap == nullptr || swap->get_device()->get_api() != device_api::vulkan) return;
  // The first call: this frame already slept at reshade_present.
  if (!g_vk_finish_present) {
    g_vk_finish_present = true;
    Log("Vulkan: the next frame starts after the game's present (ReShade's finish_present)");
    return;
  }
  AfterPresent9();
}

void RegisterVkFinishPresent() {
  constexpr auto FINISH_PRESENT = static_cast<reshade::addon_event>(100);
  void (*callback)(command_queue*, swapchain*) = OnVkFinishPresent;
  const auto func = reinterpret_cast<void (*)(reshade::addon_event, void*)>(GetProcAddress(reshade::internal::get_reshade_module_handle(), "ReShadeRegisterEvent"));
  if (func != nullptr) func(FINISH_PRESENT, reinterpret_cast<void*>(callback));
}

// ---- Exclusive fullscreen to borderless (DX9 games) ----
// HSF's DLSS-G window sits over the game's window, which exclusive fullscreen does not allow, and many older DX9 games have no windowed mode.
// With HSF.ini Borderless=1 (the default) and DLSS-G on, a DX9 game's request for exclusive fullscreen becomes a windowed swap chain at
// ReShade's create_swapchain, and the game's window is made borderless over its monitor every frame (the game may put its style back).
// swapchain_desc's fullscreen fields came with ReShade 6.2 (after the headers HSF builds with), so they are written through this mirror of
// the 6.2 layout, and only when the ReShade running is 6.2 or newer.
struct SwapchainDesc62 {
  resource_desc back_buffer;
  uint32_t back_buffer_count, present_mode, present_flags;
  bool fullscreen_state;
  float fullscreen_refresh_rate;
};
HWND g_borderless_window = nullptr;

// The running ReShade's file version as major * 100 + minor (6.8 = 608), 0 if unknown; and whether it is loaded as d3d9.dll (a DX9 game).
int ReShadeVersion(bool* as_d3d9) {
  static int version = -1;
  static bool d3d9 = false;
  if (version < 0) {
    version = 0;
    wchar_t path[MAX_PATH] = L"";
    const HMODULE module = reshade::internal::get_reshade_module_handle();
    if (module != nullptr && GetModuleFileNameW(module, path, MAX_PATH) != 0) {
      const wchar_t* const name = std::wcsrchr(path, L'\\');
      d3d9 = _wcsicmp(name != nullptr ? name + 1 : path, L"d3d9.dll") == 0;
      DWORD ignored = 0;
      const DWORD size = GetFileVersionInfoSizeW(path, &ignored);
      std::vector<BYTE> data(size);
      VS_FIXEDFILEINFO* info = nullptr;
      UINT bytes = 0;
      if (size != 0 && GetFileVersionInfoW(path, 0, size, data.data()) && VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &bytes) && info != nullptr)
        version = static_cast<int>(HIWORD(info->dwFileVersionMS) * 100 + LOWORD(info->dwFileVersionMS));
    }
  }
  *as_d3d9 = d3d9;
  return version;
}

bool WantBorderless() {
  bool as_d3d9 = false;
  const int version = ReShadeVersion(&as_d3d9);
  return g_borderless && as_d3d9 && version > 0;
}

bool OnCreateSwapchain(swapchain_desc& desc, void* hwnd) {
  PollIni();  // the first swap chain comes before any frame has read HSF.ini
  bool as_d3d9 = false;
  const int version = ReShadeVersion(&as_d3d9);
  if (!g_borderless || !as_d3d9) {
    static int logged = 0;
    if (logged++ < 3) {
      char text[200];
      std::snprintf(text, sizeof(text), "create_swapchain: left alone (borderless %d, ReShade %d as d3d9.dll %d)", g_borderless ? 1 : 0, version,
                    as_d3d9 ? 1 : 0);
      Log(text);
    }
    return false;
  }
  if (version < 602) {
    static bool warned = false;
    if (!warned) Log("exclusive fullscreen left alone: ReShade older than 6.2 cannot change it");
    warned = true;
    return false;
  }
  SwapchainDesc62& d = reinterpret_cast<SwapchainDesc62&>(desc);
  static int logged = 0;
  if (logged++ < 6) {
    char text[200];
    std::snprintf(text, sizeof(text), "create_swapchain: ReShade %d.%d as d3d9.dll, %ux%u, present mode %u, flags 0x%x, fullscreen %d, refresh %.0f", version / 100,
                  version % 100, desc.back_buffer.texture.width, desc.back_buffer.texture.height, desc.present_mode, desc.present_flags,
                  static_cast<int>(d.fullscreen_state), d.fullscreen_refresh_rate);
    Log(text);
  }
  // A game that chose windowed mode itself keeps its window as it made it: forcing it borderless resized it, and The Darkness II
  // rebuilt its swap chain mid-game and crashed. Only a window HSF took out of exclusive fullscreen is made borderless.
  if (!d.fullscreen_state) return false;
  g_borderless_window = static_cast<HWND>(hwnd);
  d.fullscreen_state = false;
  d.fullscreen_refresh_rate = 0.f;
  char text[160];
  std::snprintf(text, sizeof(text), "DX9 game asked for exclusive fullscreen (%ux%u): made it windowed, borderless (HSF.ini Borderless=0 turns this off)",
                desc.back_buffer.texture.width, desc.back_buffer.texture.height);
  Log(text);
  return true;
}

// A DXGI swap chain's waitable-object and allow-tearing flags (0x40, 0x800) cannot be dropped by ResizeBuffers, which then fails with E_INVALIDARG. Crimson Desert (since its
// 4 November update) made its swap chain with 0x840 and resizes it with 0 when frame generation is switched, the window gets focus back or ReShade's menu opens. With ReShade loaded that
// call reaches DXGI as it is, fails, the game makes a new swap chain on the same window while the old one lives, gets E_ACCESSDENIED and loses its device. So the flags a window's
// swap chain was made with are kept on its later resizes and swap chains. Only ever adds those two flags, and only on a window that already had them.
// With this the game and ReShade seemed to work in one test (the PC then crashed for another reason), so it stays; not yet confirmed.
bool KeepDxgiFlags(swapchain_desc& desc, void* hwnd) {
  constexpr uint32_t KEEP = 0x40 | 0x800;  // DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
  struct Seen {
    void* hwnd;
    uint32_t flags;
  };
  static Seen seen[8] = {};
  static SRWLOCK lock = SRWLOCK_INIT;
  if (hwnd == nullptr) return false;
  AcquireSRWLockExclusive(&lock);
  Seen* slot = nullptr;
  for (Seen& s : seen)
    if (s.hwnd == hwnd) slot = &s;
  bool changed = false;
  uint32_t before = desc.present_flags, kept = 0;
  if (slot == nullptr) {
    for (Seen& s : seen)
      if (s.hwnd == nullptr) {
        slot = &s;
        break;
      }
    if (slot == nullptr) slot = &seen[0];
    slot->hwnd = hwnd;
    slot->flags = desc.present_flags & KEEP;
  } else {
    kept = slot->flags & ~desc.present_flags;
    if (kept != 0) {
      desc.present_flags |= kept;
      changed = true;
    }
    slot->flags |= desc.present_flags & KEEP;
  }
  ReleaseSRWLockExclusive(&lock);
  if (changed) {
    static int logged = 0;
    if (logged++ < 20) {
      char text[160];
      std::snprintf(text, sizeof(text), "swap chain resize asked for flags 0x%x, which drops 0x%x it was made with: kept them (0x%x)", before, kept, desc.present_flags);
      Log(text);
    }
  }
  return changed;
}

// ReShade 6.5 and newer: create_swapchain is event 97, with the API first (6.2 to 6.4: event 7, as in the headers HSF builds with).
bool OnCreateSwapchain65(device_api api, swapchain_desc& desc, void* hwnd) {
  if (api == device_api::d3d10 || api == device_api::d3d11 || api == device_api::d3d12) return KeepDxgiFlags(desc, hwnd);
  if (api != device_api::d3d9) return false;
  return OnCreateSwapchain(desc, hwnd);
}

// ReShade 6.5 and newer, event 96 (not in the headers HSF builds with): right before the game's D3D9 device is made. With HSF.ini Dx9Ex=1 (the
// default) and DLSS-G on, HSF asks for Direct3D 9Ex: ReShade then makes the device with CreateDeviceEx and moves the game's managed-pool
// resources to 9Ex's managed pool itself. On a 9Ex device the bridge shares its copies with D3D11 on the GPU (no CPU copies at all).
// The 9Ex crash rule. Some games crash on a 9Ex device while loading, before their first frame (MGR: a null read in its own
// code; every texture and buffer it asked for was made, so the cause is some other 9Ex difference and cannot be seen ahead). HSF.ini keeps
// two keys HSF writes itself:
//   Dx9ExRun   = ex / plain while a launch has not reached its first frame yet (cleared at the first frame and when the device closes normally)
//   Dx9ExCrash = 1 once a launch on 9Ex ended before its first frame: HSF then makes plain D3D9 devices for this game (slower CPU copies)
// A launch that ends early on plain D3D9 too shows 9Ex was not the cause, so Dx9ExCrash goes back to 0. A crash after the first frame never
// counts, with one exception: an access violation in the game's own code on a 9Ex device HSF asked for, on a thread where a texture or
// buffer the game asked for was not made (OnFault9): a game that does not check then uses the missing object. That sets Dx9ExCrash = 1
// too. (AC Revelations did this 22 s in with a 2x16 DXT5 texture; OnCreateResource9 now makes such textures as whole blocks.) The menu shows the state and can switch either way.
bool g_dx9_run_marked = false;  // this process wrote Dx9ExRun (a second device in the same launch is not a crash)
bool g_dx9_ex_crash = false;     // Dx9ExCrash as read at device creation
bool g_dx9_ex_asked = false;     // this launch's device is 9Ex because HSF asked ReShade for it (not the game's own choice)
bool g_dx9_ex_at_launch = true;  // HSF.ini Dx9Ex as it was at device creation (g_dx9_ex follows the file while the game runs)

void ReadDx9Run(wchar_t* run, DWORD size) {
  GetPrivateProfileStringW(L"HSF", L"Dx9ExRun", L"", run, size, g_ini_path);
}

void ClearDx9Run() {
  if (!g_dx9_run_marked) return;
  g_dx9_run_marked = false;
  WritePrivateProfileStringW(L"HSF", L"Dx9ExRun", nullptr, g_ini_path);
}

bool OnCreateDevice65(device_api api, uint32_t& api_version) {
  if (api != device_api::d3d9) return false;
  PollIni();
  g_dx9_ex_crash = GetPrivateProfileIntW(L"HSF", L"Dx9ExCrash", 0, g_ini_path) != 0;
  g_dx9_ex_at_launch = g_dx9_ex;
  if (g_dx9_ex && api_version < 0x9100) {
    wchar_t run[16] = L"";
    if (!g_dx9_run_marked) ReadDx9Run(run, 16);
    if (std::wcscmp(run, L"ex") == 0 && !g_dx9_ex_crash) {
      g_dx9_ex_crash = true;
      WritePrivateProfileStringW(L"HSF", L"Dx9ExCrash", L"1", g_ini_path);
      Log("9Ex crash rule: the last launch ended before its first frame on Direct3D 9Ex: this launch uses plain D3D9 (slower copies). The menu (ADVANCED, DX9 GPU ROUTE) can try 9Ex again.");
    } else if (std::wcscmp(run, L"plain") == 0 && g_dx9_ex_crash) {
      g_dx9_ex_crash = false;
      WritePrivateProfileStringW(L"HSF", L"Dx9ExCrash", L"0", g_ini_path);
      Log("9Ex crash rule: the last launch ended before its first frame on plain D3D9 too, so 9Ex was not the cause: back on 9Ex");
    }
    if (!g_dx9_run_marked) {
      WritePrivateProfileStringW(L"HSF", L"Dx9ExRun", g_dx9_ex_crash ? L"plain" : L"ex", g_ini_path);
      g_dx9_run_marked = true;
    }
    if (g_dx9_ex_crash) {
      Log("create_device: D3D9 left as it is: this game crashed on 9Ex before (HSF.ini Dx9ExCrash=1)");
      return false;
    }
  }
  if (!g_dx9_ex) {
    Log("create_device: D3D9 left as it is (HSF.ini Dx9Ex=0)");
    return false;
  }
  if (api_version >= 0x9100) {
    Log("create_device: the game makes a Direct3D 9Ex device itself");
    g_dx9_device_ex = true;
    return false;
  }
  api_version = 0x9100;
  g_dx9_device_ex = true;
  g_dx9_ex_asked = true;
  Log("create_device: asked ReShade for Direct3D 9Ex, so DX9 frames reach DLSS-G on the GPU (HSF.ini Dx9Ex=0 turns this off if the game misbehaves)");
  return true;
}

// D3D9Ex check, log only (MGR crashed on 9Ex 1 s in, reading a null object, and runs with Dx9Ex=0). ReShade sends create_resource
// before each texture or buffer the game makes and init_resource only when it was made: a create with no init after it was refused. Some
// games do not check and crash on the missing object, so the last pending one is also logged when the game thread faults. Nothing changes
// yet: the log is to learn whether a refused creation is a reliable sign that a game needs Dx9Ex=0.
thread_local bool t_create9_pending = false;
thread_local resource_desc t_create9_desc = {};
std::atomic<uint32_t> g_dx9_created{0}, g_dx9_refused{0};
void* g_fault9_handler = nullptr;

void DescribeResource9(const resource_desc& d, char* text, size_t size) {
  if (d.type == resource_type::buffer)
    std::snprintf(text, size, "buffer %llu bytes, heap %u, usage 0x%x, flags 0x%x", static_cast<unsigned long long>(d.buffer.size), static_cast<unsigned>(d.heap),
                  static_cast<unsigned>(d.usage), static_cast<unsigned>(d.flags));
  else
    std::snprintf(text, size, "texture type %u %ux%u, %u levels, format %u, heap %u, usage 0x%x, flags 0x%x", static_cast<unsigned>(d.type), d.texture.width,
                  d.texture.height, static_cast<unsigned>(d.texture.levels), static_cast<unsigned>(d.texture.format), static_cast<unsigned>(d.heap),
                  static_cast<unsigned>(d.usage), static_cast<unsigned>(d.flags));
}

void ReportRefused9(const resource_desc& d) {
  const uint32_t n = ++g_dx9_refused;
  if (n > 12) return;
  char what[200], text[300];
  DescribeResource9(d, what, sizeof(what));
  std::snprintf(text, sizeof(text), "DX9 check: the device%s refused a resource the game asked for: %s", g_dx9_device_ex ? " (9Ex)" : "", what);
  Log(text);
}

// A block-compressed (DXT) texture whose width or height is not a multiple of 4, e.g. 2x16. Direct3D 9 refuses those at the top level. AC
// Revelations asks for one mid-game and, on a 9Ex device, uses the missing texture and crashes. On a 9Ex device HSF asked for, the size is
// rounded up to whole blocks: the same bytes (a 2x16 and a 4x16 DXT texture are both one block wide), so the game's upload fits.
thread_local bool t_create9_odd = false;
bool OddBlocks9(const resource_desc& d) {
  return d.type == resource_type::texture_2d && d.texture.format >= format::bc1_typeless && d.texture.format <= format::bc5_snorm &&
         ((d.texture.width & 3) != 0 || (d.texture.height & 3) != 0);
}

bool OnCreateResource9(device* dev, resource_desc& desc, subresource_data*, resource_usage) {
  if (dev->get_api() != device_api::d3d9) return false;
  if (t_create9_pending) ReportRefused9(t_create9_desc);
  bool changed = false;
  t_create9_odd = OddBlocks9(desc);
  if (t_create9_odd) {
    static std::atomic<int> logged{0};
    char what[200], text[320];
    DescribeResource9(desc, what, sizeof(what));
    if (g_dx9_ex_asked) {
      desc.texture.width = (desc.texture.width + 3) & ~3u;
      desc.texture.height = (desc.texture.height + 3) & ~3u;
      changed = true;
      if (logged++ < 6) {
        std::snprintf(text, sizeof(text), "DX9 check (9Ex): the game asks for a DXT texture that is not whole blocks (%s): made as %ux%u instead", what,
                      desc.texture.width, desc.texture.height);
        Log(text);
      }
    } else if (logged++ < 6) {
      std::snprintf(text, sizeof(text), "DX9 check: the game asks for a DXT texture that is not whole blocks (%s)", what);
      Log(text);
    }
  }
  t_create9_pending = true;
  t_create9_desc = desc;
  return changed;
}

void OnInitResource9(device* dev, const resource_desc&, const subresource_data*, resource_usage, resource) {
  if (dev->get_api() != device_api::d3d9) return;
  if (t_create9_pending) ++g_dx9_created;
  if (t_create9_pending && t_create9_odd) {
    static std::atomic<int> logged{0};
    if (logged++ < 6) Log("DX9 check: that DXT texture was made");
  }
  t_create9_pending = false;
  t_create9_odd = false;
}

// Called at each DX9 frame: a creation still pending there was refused; a short count once.
void CheckRefused9() {
  ClearDx9Run();  // the first frame: this launch did not crash while loading
  if (t_create9_pending) {
    ReportRefused9(t_create9_desc);
    t_create9_pending = false;
  }
  static bool told = false;
  if (!told) {
    told = true;
    char text[200];
    std::snprintf(text, sizeof(text), "DX9 check at the first frame: device %s, %u resources made, %u refused", g_dx9_device_ex ? "9Ex" : "plain 9",
                  g_dx9_created.load(), g_dx9_refused.load());
    Log(text);
  }
}

// An access violation in the game's own exe: log where, and the creation still pending on that thread (the likely missing object).
LONG CALLBACK OnFault9(EXCEPTION_POINTERS* info) {
  if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  const void* const at = info->ExceptionRecord->ExceptionAddress;
  if (ModuleOf(at) != GetModuleHandleW(nullptr)) return EXCEPTION_CONTINUE_SEARCH;
  // The borderless crash rule (see g_borderless_moved_at), in every API.
  const ULONGLONG moved_at = g_borderless_moved_at.load();
  static std::atomic<bool> borderless_marked{false};
  if (g_force_borderless && moved_at != 0 && GetTickCount64() - moved_at < BORDERLESS_CRASH_MS && !borderless_marked.exchange(true)) {
    WritePrivateProfileStringW(L"HSF", L"ForceBorderless", L"0", g_ini_path);
    WritePrivateProfileStringW(L"HSF", L"BorderlessCrash", L"1", g_ini_path);
    Log("borderless crash rule: the game faulted right after HSF changed its window's size: the next launch keeps the game's own window (ForceBorderless=0). "
        "The menu (ADVANCED, WINDOW) can try borderless again.");
  }
  static std::atomic<int> logged{0};
  if (logged++ >= 3) return EXCEPTION_CONTINUE_SEARCH;
  char pending[200] = "none";
  if (t_create9_pending) DescribeResource9(t_create9_desc, pending, sizeof(pending));
  char text[400];
  std::snprintf(text, sizeof(text), "DX9 check: access violation in the game's exe at +0x%llx (address 0x%llx); device %s, %u made, %u refused; creation still pending on this thread: %s",
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(at) - reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))),
                static_cast<unsigned long long>(info->ExceptionRecord->NumberParameters > 1 ? info->ExceptionRecord->ExceptionInformation[1] : 0),
                g_dx9_device_ex ? "9Ex" : "plain 9", g_dx9_created.load(), g_dx9_refused.load(), pending);
  Log(text);
  // The 9Ex crash rule, after the first frame: HSF asked for 9Ex, and the game faulted where something it asked the device for was not made.
  static std::atomic<bool> marked{false};
  if (g_dx9_ex_asked && t_create9_pending && !marked.exchange(true)) {
    WritePrivateProfileStringW(L"HSF", L"Dx9ExCrash", L"1", g_ini_path);
    Log("9Ex crash rule: the game faulted on Direct3D 9Ex where a texture or buffer it asked for was not made: the next launch uses plain D3D9 (slower copies). The menu (ADVANCED, DX9 GPU ROUTE) can try 9Ex again.");
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

// Registers whichever create_swapchain the running ReShade has, and create_device where there is one.
void RegisterCreateSwapchain() {
  bool as_d3d9 = false;
  const int version = ReShadeVersion(&as_d3d9);
  if (version >= 605) {
    const auto func = reinterpret_cast<void (*)(reshade::addon_event, void*)>(GetProcAddress(reshade::internal::get_reshade_module_handle(), "ReShadeRegisterEvent"));
    if (func != nullptr) func(static_cast<reshade::addon_event>(97), reinterpret_cast<void*>(&OnCreateSwapchain65));
    if (func != nullptr && as_d3d9) func(static_cast<reshade::addon_event>(96), reinterpret_cast<void*>(&OnCreateDevice65));
  } else if (version >= 602) {
    reshade::register_event<reshade::addon_event::create_swapchain>(OnCreateSwapchain);
  }
}

// The window change itself runs on a short-lived HSF thread, never inside ReShade's frame callback: a game that answers the resize by
// rebuilding its swap chain right away (The Darkness II) makes ReShade free the effect runtime HSF and ReShade are still using,
// and the game crashed a moment later. From another thread the move is posted, and the game handles it in its own message loop, between frames.
struct BorderlessJob {
  HWND window;
  bool restyle;
  LONG_PTR style, ex_style;
  RECT monitor;
  HMODULE self;  // HSF's own reference, so the add-on stays loaded until this thread is done
};
std::atomic<bool> g_borderless_busy{false};

DWORD WINAPI BorderlessThread(void* param) {
  const BorderlessJob job = *static_cast<BorderlessJob*>(param);
  delete static_cast<BorderlessJob*>(param);
  if (job.restyle) {
    SetWindowLongPtrW(job.window, GWL_STYLE, job.style);
    SetWindowLongPtrW(job.window, GWL_EXSTYLE, job.ex_style);
  }
  const RECT& m = job.monitor;
  g_borderless_moved_at.store(GetTickCount64());
  SetWindowPos(job.window, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
               SWP_ASYNCWINDOWPOS | SWP_NOACTIVATE | SWP_NOOWNERZORDER | (job.restyle ? SWP_FRAMECHANGED : 0));
  g_borderless_moved_at.store(GetTickCount64());  // the game handles the move a moment later, in its own message loop
  g_borderless_busy.store(false);
  FreeLibraryAndExitThread(job.self, 0);
}

// Hands the change to BorderlessThread; false when one is still running or the thread could not start.
bool StartBorderlessJob(HWND window, bool restyle, LONG_PTR style, LONG_PTR ex_style, const RECT& monitor) {
  if (g_borderless_busy.exchange(true)) return false;
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&BorderlessThread), &self)) {
    g_borderless_busy.store(false);
    return false;
  }
  auto* const job = new (std::nothrow) BorderlessJob{window, restyle, style, ex_style, monitor, self};
  HANDLE const thread = job != nullptr ? CreateThread(nullptr, 0, &BorderlessThread, job, 0, nullptr) : nullptr;
  if (thread == nullptr) {
    delete job;
    FreeLibrary(self);
    g_borderless_busy.store(false);
    return false;
  }
  CloseHandle(thread);
  return true;
}

// The window of the swap chain HSF follows, for ForceBorderless; nullptr if not known.
HWND FollowedWindow9() {
  if (g_native_swap9 == nullptr) return nullptr;
  D3DPRESENT_PARAMETERS params{};
  if (SUCCEEDED(g_native_swap9->GetPresentParameters(&params)) && params.hDeviceWindow != nullptr) return params.hDeviceWindow;
  D3DDEVICE_CREATION_PARAMETERS creation{};
  return g_native9 != nullptr && SUCCEEDED(g_native9->GetCreationParameters(&creation)) ? creation.hFocusWindow : nullptr;
}

// A windowed game made borderless from the menu: its window as the game made it, so turning the option off puts it back at once.
struct SavedWindow {
  HWND window = nullptr;
  bool restyled = false;
  LONG_PTR style = 0, ex_style = 0;
  RECT rect{};
};
SavedWindow g_saved_window;

// Puts back the window saved by KeepBorderless; true once nothing is left to put back.
bool RestoreWindow() {
  SavedWindow& saved = g_saved_window;
  if (saved.window == nullptr) return true;
  if (!IsWindow(saved.window)) {
    saved = SavedWindow{};
    return true;
  }
  if (!StartBorderlessJob(saved.window, saved.restyled, saved.style, saved.ex_style, saved.rect)) return false;  // a job is still running: next frame
  Log("window: back to the game's own window style and size (ForceBorderless=0)");
  saved = SavedWindow{};
  return true;
}

// Game thread, every presented frame in every API: keeps the window HSF made borderless over its whole monitor, and puts a window made
// borderless from the menu back once the option is turned off. `followed` is the window of the swap chain HSF follows.
void KeepBorderless(HWND followed) {
  HWND window = nullptr;
  if (g_borderless_window != nullptr && g_borderless) window = g_borderless_window;
  else if (g_force_borderless) window = followed;
  if (window == nullptr) {
    if (!g_force_borderless) RestoreWindow();
    return;
  }
  if (!IsWindow(window) || IsIconic(window)) return;
  if (g_saved_window.window != nullptr && g_saved_window.window != window && !RestoreWindow()) return;  // the game moved to another window
  // Many games (Dragon Age: Origins, Tomb Raider, Castlevania) present from a thread that does not own the window. SetWindowLongPtr then
  // waits for the window's own thread, which may itself be waiting on the present thread: called from here, Dragon Age hung. The change runs
  // on BorderlessThread instead, which the present thread never waits on, so such windows are restyled too.
  const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE), ex_style = GetWindowLongPtrW(window, GWL_EXSTYLE);
  if ((style & WS_CHILD) != 0) return;  // a game drawing into a child window: its parent is the one to change, and HSF leaves that alone
  const LONG_PTR frame = WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
  const LONG_PTR ex_frame = WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE;
  MONITORINFO monitor{sizeof(monitor)};
  if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
  const RECT& m = monitor.rcMonitor;
  RECT now{};
  GetWindowRect(window, &now);
  const bool styled = (style & frame) == 0 && (style & WS_POPUP) != 0 && (ex_style & ex_frame) == 0;
  if (styled && EqualRect(&now, &m)) return;
  // The first change to a window the game chose to show windowed: remember it as it is, for turning the option off.
  const bool saving = window != g_borderless_window && g_saved_window.window == nullptr;
  if (!StartBorderlessJob(window, !styled, (style & ~frame) | WS_POPUP, ex_style & ~ex_frame, m)) return;
  if (saving) g_saved_window = SavedWindow{window, !styled, style, ex_style, now};
  static int logged = 0;
  if (logged++ < 3) Log("making the game's window borderless over its monitor (from HSF's own thread, so the game handles it between frames)");
}

// Every present, in every API. A game can show a short window in another API first (a launcher, a splash), so only the swap chain of the
// API the game runs on is followed: DX9 always, D3D12 once it has shown more frames than D3D11, D3D11 otherwise.
void OnWindowFrame(effect_runtime* runtime) {
  const device_api api = runtime->get_device()->get_api();
  HWND followed = nullptr;
  if (api == device_api::d3d9) {
    followed = FollowedWindow9();
    if (followed == nullptr) followed = static_cast<HWND>(runtime->get_hwnd());
  } else if ((api == device_api::d3d12 && D3d12Run()) || (api == device_api::d3d11 && !D3d12Run())) {
    followed = static_cast<HWND>(runtime->get_hwnd());
  } else {
    return;
  }
  KeepBorderless(followed);
}

// Before a Reset of the D3D9 device (ReShade's destroy_swapchain and destroy_effect_runtime): HSF's copies in the default pool must go first,
// or the game's Reset fails. The bridge makes them again on the next frame.
void ReleaseDx9ForReset() {
  DropAdoptedSwap9();
  hsf::dx9::ReleaseGameResources();
  g_motion9 = nullptr;
  g_depth9 = nullptr;
  ReleaseDepth9View();
  ReleaseScoreView9();
}

void OnDestroySwapchain(swapchain* swap) {
  if (swap->get_device()->get_api() == device_api::d3d9) ReleaseDx9ForReset();
}

void OnDestroyDevice(device* dev) {
  if (dev->get_api() == device_api::vulkan) {
    if (!hsf::vk::Follows(dev)) return;  // another device (a game that tries a GPU and drops it): the bridge stays
    const char* const late = hsf::dlssg::Stop(2000);
    if (late != nullptr) Log(late);
    StopVkDepth();
    ReleaseVkView(false);
    g_v9 = ScoreView9{};  // the device is already out of ReShade's table: its texture is left to it
    hsf::score::Shutdown();
    hsf::vk::Stop(false);
    Log("Vulkan exit: the game's device goes away with HSF's bridge still up; HSF let its D3D11 side go and left the images to the device");
    return;
  }
  if (dev->get_api() != device_api::d3d9) return;
  ClearDx9Run();  // closed normally: not a crash, even before the first frame
  IDirect3DDevice9* const native = reinterpret_cast<IDirect3DDevice9*>(dev->get_native());
  char line[200];
  if (native != g_native9) {
    std::snprintf(line, sizeof(line), "DX9 exit: ReShade destroys a D3D9 device %p that HSF does not follow (HSF follows %p)", static_cast<void*>(native),
                  static_cast<void*>(g_native9));
    Log(line);
    return;
  }
  std::snprintf(line, sizeof(line), "DX9 exit: ReShade destroys the game's D3D9 device %p; HSF releases its copies", static_cast<void*>(native));
  Log(line);
  const char* const late = hsf::dlssg::Stop(2000);
  if (late != nullptr) Log(late);
  ReleaseDepth9View();
  ReleaseScoreView9();
  hsf::score::Shutdown();  // it runs on HSF's D3D11 device in a DX9 game, which Stop frees
  hsf::dx9::Stop();
  g_adopted_swap9.Reset();
  g_native9 = nullptr;
  g_native_swap9 = nullptr;
  // Leak check: ReShade releases its last reference right after this event and warns if any other remains. 1 here = only ReShade's
  // is left, so HSF is clean; more = someone else (the game, another add-on) still holds the device.
  native->AddRef();
  const ULONG refs = native->Release();
  std::snprintf(line, sizeof(line), "DX9 exit: HSF released everything; the device now has %lu reference(s) (1 = only ReShade's, so HSF is clean)",
                static_cast<unsigned long>(refs));
  Log(line);
}

void OnFinishEffects(effect_runtime* runtime, command_list*, resource_view rtv, resource_view) {
  device* const dev = runtime->get_device();
  PollIni();
  UpdateStats();
  if (rtv.handle == 0) return;
  const resource target = dev->get_resource_from_view(rtv);
  const resource_desc back = dev->get_resource_desc(target);
  Inspect(runtime, dev, back);
  if (dev->get_api() == device_api::d3d11) g_d3d11_frames.fetch_add(1, std::memory_order_relaxed);
  else if (dev->get_api() == device_api::d3d12) g_d3d12_frames.fetch_add(1, std::memory_order_relaxed);

#ifdef HSF_SL_GAME
  if (g_sl_bring && dev->get_api() == device_api::d3d12) {
    BringFrame(runtime, dev, target);
    return;
  }
  if (g_sl_drive && dev->get_api() == device_api::d3d12) {
    LeaveUnstartedStreamline();
    SlOpticalFrame(runtime, dev, target);
    return;
  }
#endif
  if (dev->get_api() == device_api::d3d9) {
    Dx9Frame(runtime, dev);
    return;
  }
  if (dev->get_api() == device_api::d3d12) {
    PickD3d12Route();
    return;
  }
  if (dev->get_api() == device_api::vulkan) {
    VkFrame(runtime, dev, target);
    return;
  }
  if (dev->get_api() != device_api::d3d11) { Note("presenter: only Direct3D 11, 12, DX9 and Vulkan so far"); return; }
  LeaveAutoD3d12Route();
  HookNativePresent(runtime);
  UpdateScore(runtime, dev, target);
  if (g_view_key != 0 && !g_menu_open && runtime->is_key_pressed(g_view_key))
    g_score_view = static_cast<hsf::score::View>((static_cast<int>(g_score_view) + 1) % hsf::score::VIEW_COUNT);
  if (g_input_key != 0 && !g_menu_open && runtime->is_key_pressed(g_input_key)) CycleInput();
  if (g_record_key != 0 && !g_menu_open && runtime->is_key_pressed(g_record_key)) g_record_request = true;
  if (g_depth_key != 0 && !g_menu_open && runtime->is_key_pressed(g_depth_key)) {
    g_depth_ab_off = !g_depth_ab_off;
    Log(g_depth_ab_off ? "A/B: DLSS-G's depth input is now CONSTANT" : "A/B: DLSS-G's depth input is now real");
  }
  Dlss11Motion(runtime, dev);
  UpdateDepth(runtime, dev, target);
  UpdateCleanFrame(dev, target);
  UpdateHud(dev, target);
  UpdateCamera();
  // DLSS-G wants the offset in UV towards the previous frame: the convention's offset in frame pixels over the frame size.
  UpdateDlssgMotionScale();
  // MfgKey cycles 2x, 3x ... up to what DLSS-G says this machine can do, then back to 2x (saved to HSF.ini like a panel change).
  if (g_mfg_key != 0 && !g_menu_open && runtime->is_key_pressed(g_mfg_key)) {
    g_dlssg_frames = g_dlssg_frames >= std::max(g_dlssg_stats.max_frames, 1u) ? 1 : g_dlssg_frames + 1;
    SaveIni();
  }
  hsf::dlssg::SetOptions(g_extra_enabled, g_dlssg_frames);
  hsf::dlssg::SetMenuOpen(g_menu_open);
  hsf::dlssg::SetReflex(CapInForce(), g_reflex_boost);
  hsf::dlssg::SetReflexSleep(g_reflex_sleep);
  UpdatePresenter(dev);
  UpdateGameQueue(dev);
}

#ifdef HSF_SL_GAME
// The picture check and the score, on the game's own queue. They never wait for the GPU. A failure stops them for this run and leaves generation alone.
void RunSlChecks(effect_runtime* runtime) {
  static bool s_gave_up = false;
  if (!g_sl_checks || s_gave_up || runtime->get_device()->get_api() != device_api::d3d12) return;
  AcquireSRWLockExclusive(&g_sl_checks_lock);
  if (g_sl_game_checks == nullptr) g_sl_game_checks = new (std::nothrow) hsf::frame_checks::GameChecks();
  ReleaseSRWLockExclusive(&g_sl_checks_lock);
  hsf::slgame::SetVectorsHook(&SlVectors);
  AcquireSRWLockShared(&g_sl_checks_lock);
  if (g_sl_game_checks != nullptr) {
    const auto on_grid = [](const hsf::frame_checks::Verdict& verdict) {
      if (verdict.cut) hsf::slgame::NotePictureCut(verdict.changed, verdict.shape);
    };
    const auto on_score = [](const hsf::frame_checks::ScoreSample& s) { hsf::slgame::NoteScore(s.warp, s.none, s.better, s.worse, s.moving, s.kind); };
    if (!g_sl_game_checks->AtPresent(reinterpret_cast<ID3D12CommandQueue*>(runtime->get_command_queue()->get_native()), reinterpret_cast<ID3D12Resource*>(runtime->get_current_back_buffer().handle), on_grid, on_score)) {
      s_gave_up = true;
      hsf::slgame::NoteChecksProblem(g_sl_game_checks->Problem());
    }
  }
  ReleaseSRWLockShared(&g_sl_checks_lock);
}

// Every frame: ReShade's effect events do not fire in a game with no effects loaded, as Shadows showed.
void OnReshadePresent(effect_runtime* runtime) {
  if (g_sl_drive && D3d12Run()) {
    PollIni();
    // MfgKey cycles 2x, 3x ... up to what DLSS-G says this machine can do, then back to 2x (saved to HSF.ini like a panel change).
    if (g_mfg_key != 0 && !g_menu_open && runtime->is_key_pressed(g_mfg_key)) {
      g_dlssg_frames = g_dlssg_frames >= std::max(hsf::slgame::GetStats().max_frames, 1u) ? 1 : g_dlssg_frames + 1;
      SaveIni();
    }
    if (g_sl_camera_key != 0 && !g_menu_open && runtime->is_key_pressed(g_sl_camera_key)) {
      g_sl_camera_rebuild = !g_sl_camera_rebuild;
      SaveIni();
    }
    hsf::slgame::SetCameraRebuild(g_sl_camera_rebuild);
    hsf::slgame::SetCutCheck(g_sl_cut_check);
    hsf::slgame::SetQuickOff(g_sl_quick_off);
    hsf::slgame::SetScoreGuard(g_sl_score_guard);
    hsf::slgame::SetGeneration(g_extra_enabled, g_dlssg_frames);
    // Generation off while another program's window is in front, and for FOCUS_BACK_MS after the game's comes back: Crimson Desert drops its device when its window gets focus
    // back with frame generation on. Nobody looks at a game behind another window, so this costs nothing anywhere else.
    {
      // 1.5 s was not enough in Crimson Desert (device lost 1 s after generation came back on).
      constexpr ULONGLONG FOCUS_BACK_MS = 5000;
      static ULONGLONG s_focus_back_at = 0;
      static bool s_was_front = true;
      DWORD front_process = 0;
      if (const HWND front = GetForegroundWindow()) GetWindowThreadProcessId(front, &front_process);
      const bool in_front = front_process == GetCurrentProcessId();
      const ULONGLONG now = GetTickCount64();
      if (in_front && !s_was_front) s_focus_back_at = now;
      if (in_front != s_was_front) Log(in_front ? "focus: the game's window is back in front, generation waits a moment" : "focus: the game's window is behind another window, generation off");
      s_was_front = in_front;
      hsf::slgame::SetFocusHold(!in_front || (s_focus_back_at != 0 && now - s_focus_back_at < FOCUS_BACK_MS));
    }
    hsf::slgame::SetReflex(CapInForce(), g_reflex_boost);
    DriveMotionChoice();
    RunSlChecks(runtime);
    SlhAtPresent(runtime);
    // Remembered per game, so that the next run has DLSS-G's dynamic resolution flag from its first frame.
    if (!g_sl_dynamic_saved && hsf::slgame::GetStats().dynamic_resolution) {
      g_sl_dynamic_saved = true;
      WritePrivateProfileStringW(L"HSF", L"DlssgDynamicRes", L"1", g_ini_path);
    }
  }
  hsf::slgame::Poll();
}
#endif

// HSF's presenters take the native device under ReShade's proxy of their own new device from here (they ignore every other device).
#ifdef HSF_SL_GAME
void OnSlBringPresent(effect_runtime*) { hsf::slbring::Poll(); }
#endif

void OnInitDevice([[maybe_unused]] device* dev) {
  // Vulkan probe: the planned bridge hands the game's frames to the helper through images and a fence shared with NT handles, made through
  // ReShade on the game's device. ReShade's layer can do that only when it turned on the external memory and semaphore extensions.
  if (dev->get_api() == device_api::vulkan) {
    char text[160];
    std::snprintf(text, sizeof(text), "Vulkan device: shared images %s (NT handles %s), shared fences %s (NT handles %s)",
                  dev->check_capability(device_caps::shared_resource) ? "yes" : "no", dev->check_capability(device_caps::shared_resource_nt_handle) ? "yes" : "no",
                  dev->check_capability(device_caps::shared_fence) ? "yes" : "no", dev->check_capability(device_caps::shared_fence_nt_handle) ? "yes" : "no");
    Log(text);
    // HSF's D3D11 device now, before the game's first frame, not at it. Making a D3D11 device makes RTSS hook DXGI; by the first frame
    // Discord's capture hook (OBS's graphics-hook64.dll, Discord Clips) may already sit on the same functions, and RTSS then writes into it
    // and crashes the game. Early, Discord has not hooked yet. UpdatePresenterVk takes the bridge as it is.
    char error[320];
    if (!hsf::vk::Start(dev, error, sizeof(error))) Log(error);
  }
#ifdef HSF_SL_GAME
  if (g_sl_bring && dev->get_api() == device_api::d3d12) hsf::slbring::NoteDevice(reinterpret_cast<void*>(dev->get_native()));
#endif
}

ULONGLONG g_menu_closed_at = 0;

bool OnMenuToggled(effect_runtime*, bool open, input_source) {
  g_menu_open = open;
  if (!open) g_menu_closed_at = GetTickCount64();
  return false;
}

// For a second after ReShade's menu closed. ReShade lets the pointer go while its menu is open, and HSF's DLSS-G window takes the mouse for
// it. Some games (Valheim) keep the pointer in their window only from when they gain the focus, which they never lost, so afterwards the
// hidden pointer could leave the window. A game in front with its pointer hidden and nothing holding it gets it held to its window again.
void ReclipAfterMenu(effect_runtime* runtime) {
  if (g_menu_closed_at == 0 || g_menu_open) return;
  if (GetTickCount64() - g_menu_closed_at > 1000) {
    g_menu_closed_at = 0;
    return;
  }
  const HWND window = static_cast<HWND>(runtime->get_hwnd());
  if (window == nullptr || GetForegroundWindow() != GetAncestor(window, GA_ROOT)) return;
  CURSORINFO cursor{sizeof(cursor)};
  if (!GetCursorInfo(&cursor) || (cursor.flags & CURSOR_SHOWING) != 0) return;  // a pointer the game shows may roam
  RECT clip{}, screen{GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0};
  screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
  screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
  if (!GetClipCursor(&clip) || !EqualRect(&clip, &screen)) return;  // something holds it already
  RECT client{};
  if (!GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0) return;
  POINT corners[2] = {{client.left, client.top}, {client.right, client.bottom}};
  MapWindowPoints(window, nullptr, corners, 2);
  const RECT held{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
  ClipCursor(&held);
  g_menu_closed_at = 0;
  Log("menu closed: the game's hidden pointer was free, held to its window again");
}

void OnReloaded(effect_runtime*) {
  ++g_effect_reloads;
  for (auto& variable : g_source_variables) variable = {};
  g_sources_listed = false;
  g_next_source_search = 0;
  g_motion_texture = nullptr;
  g_motion_srv11 = nullptr;
  g_depth_variable = {};
  g_next_depth_search = 0;
  g_depth_texture = nullptr;
}

void OnDestroyEffectRuntime(effect_runtime* runtime) {
  // Vulkan: the swap chain goes away (exit or a new swap chain): the bridge goes now, while the device is still whole; it starts again with the next one.
  if (runtime->get_device()->get_api() == device_api::vulkan) {
    if (hsf::vk::Device() == nullptr) return;
    const char* const late = hsf::dlssg::Stop(2000);
    if (late != nullptr) Log(late);
    runtime->get_command_queue()->wait_idle();
    StopVkDepth();
    ReleaseVkView(true);
    ReleaseScoreView9();
    hsf::score::Shutdown();
    hsf::vk::Stop();
    Log("Vulkan: ReShade's effect runtime goes away (exit or a new swap chain); HSF stopped DLSS-G and released its bridge");
    return;
  }
  if (runtime->get_device()->get_api() == device_api::d3d9) {
    ReleaseDx9ForReset();
    Log("DX9: ReShade's effect runtime goes away (game exit or Reset); HSF released its copies on the game's device");
  }
#ifdef HSF_SL_GAME
  if (g_sl_game) DropSlChecks();  // comes back at the next present, for the new swap chain
#endif
  if (reinterpret_cast<IDXGISwapChain*>(runtime->get_native()) == g_native_swap) {
    // The swap chain is going away: give the thread longer, but never wait without limit (that froze the game).
    const char* const late = StopAny(2000);
    if (late != nullptr) Log(late);
    hsf::score::Shutdown();
    g_clean_ready = false;
    if (g_clean_frame != nullptr) {
      g_clean_frame->Release();
      g_clean_frame = nullptr;
    }
    g_hud_listening.store(false, std::memory_order_relaxed);
    g_hud_logging.store(false, std::memory_order_relaxed);
    g_hud_on_back.store(false, std::memory_order_relaxed);
    g_hud_frame = nullptr;
    g_hud_back = 0;
    g_hud_on_source.store(false, std::memory_order_relaxed);
    g_hud_target = nullptr;
    g_hud_copied_from = nullptr;
    g_hud_replayed = false;
    g_hud_source_width = 0;
    g_hud_guard.Reset();
    g_hud_remover.Reset();
    g_hud_spans.Reset();
    g_hud_unused_frames = 0;
    g_hud_rest_until = 0;
    hsf::depth::Shutdown();
    g_depth_texture = nullptr;
    g_native_swap = nullptr;
  }
}

// The score of the source in use, as one short line: for the panel and the overlay.
void ScoreLine(char* text, size_t size) {
  if (!g_score_enabled) {
    std::snprintf(text, size, "motion score off");
    return;
  }
  if (hsf::score::Problem()[0] != '\0') {
    std::snprintf(text, size, "motion score: %s", hsf::score::Problem());
    return;
  }
  if (g_seen.source < 0) {
    std::snprintf(text, size, "motion score: no motion source");
    return;
  }
  const hsf::score::Result r = hsf::score::Recent(g_seen.source, 3000);
  if (r.valid && !r.thin)
    std::snprintf(text, size, "motion score %s %.0f%%   error %.1f vs %.1f without   better %.0f%%, worse %.0f%%", SOURCES[g_seen.source].key, r.gain * 100.f, r.error_warp,
                  r.error_none, r.better * 100.f, r.worse * 100.f);
  else
    std::snprintf(text, size, "motion score %s: too little movement to rate", SOURCES[g_seen.source].key);
}

// The input DLSS-G gets now, for the panel and the overlay.
const char* InputName() {
  if (g_seen.source < 0) return "none";
  return g_input_zero ? "ZERO (A/B)" : SOURCES[g_seen.source].key;
}

#ifdef HSF_SL_GAME
// The controls both Direct3D 12 routes share. max_frames: generated per real frame DLSS-G says this machine can do (0 until it says). motion_now: what DLSS-G is getting.
void DrawD3d12Controls(uint32_t max_frames, const char* motion_now, bool reflex_sleep) {
  bool changed = false;
  ImGui::SeparatorText("Frame generation");
  const char* const MODES[] = {"2x", "3x", "4x", "5x", "6x"};
  const int count = static_cast<int>(std::clamp(std::max(max_frames, g_dlssg_frames), 1u, 5u));
  int mode = static_cast<int>(g_dlssg_frames) - 1;
  if (ImGui::Combo("DLSS-G frame generation (F7 in game)", &mode, MODES, count)) {
    g_dlssg_frames = static_cast<uint32_t>(mode) + 1;
    changed = true;
  }
  const char* const FROM[] = {"auto (the game's DLSS data when it comes, else optical)", "optical (HSF's flow and depth)", "the game's own (its DLSS vectors and depth)"};
  if (ImGui::Combo("Motion from", &g_motion_from, FROM, static_cast<int>(std::size(FROM)))) changed = true;
  ImGui::TextDisabled("DLSS-G is getting: %s", motion_now);
  changed |= ImGui::Checkbox("Clean the flow with depth (compose pass)", &g_compose_enabled);
  ImGui::SameLine();
  changed |= ImGui::Checkbox("HUD mask", &g_checks_enabled);
  ImGui::TextDisabled("These two work on optical motion only.");
  const char* const VIEWS[] = {"the game", "depth (near bright, far dark)", "motion (still grey, moving coloured)"};
  ImGui::Combo("Show", &g_bring_view, VIEWS, static_cast<int>(std::size(VIEWS)));
  if (g_sl_drive && g_using_dlss) ImGui::TextDisabled("In this game, Show works with optical motion.");

  ImGui::SeparatorText("Frame rate");
  // FPS cap: applied while dragging, written to HSF.ini when let go.
  int cap = static_cast<int>(g_fps_cap);
  if (ImGui::SliderInt("FPS cap (on screen, 0 = off)", &cap, 0, 360, cap == 0 ? "off" : "%d fps")) g_fps_cap = CapRule(cap);
  changed |= ImGui::IsItemDeactivatedAfterEdit();
  const uint32_t monitor_hz = MainMonitorHz();
  if (monitor_hz == 0) {
    ImGui::TextDisabled("Main monitor's refresh rate: not found, set the limit with the slider");
  } else if (g_fps_cap == monitor_hz) {
    ImGui::TextDisabled("Limit matches the main monitor (%u Hz)", monitor_hz);
  } else {
    char label[64];
    std::snprintf(label, sizeof(label), "Set limit to my refresh rate (%u Hz)", monitor_hz);
    if (ImGui::Button(label)) {
      g_fps_cap = monitor_hz;
      changed = true;
    }
  }
  if (g_fps_cap == 0) {
    const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.15f, 0.15f, pulse), "WARNING: no FPS limit. It is BEST to set a limit.");
  }
  changed |= ImGui::Checkbox("Reflex boost", &g_reflex_boost);
  if (reflex_sleep) {
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Reflex sleep", &g_reflex_sleep);
  }
  if (changed) SaveIni();
}
#endif

// ---- LaunchPad tune ----
// The panel's "Tune LaunchPad for FG" button. Sets LaunchPad's own settings through ReShade, as a user would in ReShade's menu, and saves them to the
// preset: Flow Quality High, ReShade's depth settings for LaunchPad matching the depth HSF has, and LaunchPad first in the effect order. What was
// there before goes to HSF.ini [LaunchPadUndo] for the Undo button. Marty's file is never changed.
constexpr const char* LAUNCHPAD_FILE = "MartysMods_LAUNCHPAD.fx";
constexpr const char* LAUNCHPAD_TECHNIQUE = "MartysMods_Launchpad";
constexpr const char* DEPTH_REVERSED = "RESHADE_DEPTH_INPUT_IS_REVERSED";
constexpr const char* DEPTH_UPSIDE_DOWN = "RESHADE_DEPTH_INPUT_IS_UPSIDE_DOWN";

// The depth HSF has for this game. False while HSF does not know it; has_upside_down false when only that part is unknown.
bool KnownDepth(bool* reversed, bool* has_upside_down, bool* upside_down) {
#ifdef HSF_SL_GAME
  if ((g_sl_bring || g_sl_drive) && D3d12Run()) {
    // Direct3D 12: the same guess the add-on hands DLSS-G (reversed unless HSF.ini says otherwise); which way up only from HSF.ini.
    *reversed = g_depth_inverted_setting != 0;
    *has_upside_down = g_depth_upside_down_setting >= 0;
    *upside_down = g_depth_upside_down_setting == 1;
    return true;
  }
#endif
  if (g_native9 != nullptr) {  // DX9: ReShade hands D3D9 depth over the right way up
    // Only HSF.ini's direction. A new depth definition recompiles LaunchPad on the game thread, and a D3D9 compile of it in a 32-bit game
    // takes long enough that Windows closes the game as not responding (AC Revelations, at a loading screen where the measured depth flipped).
    if (g_depth_inverted_setting < 0) return false;
    *reversed = g_depth_inverted;
    *has_upside_down = true;
    *upside_down = g_depth_upside_down_setting == 1;
    return true;
  }
  const hsf::depth::Status& depth = hsf::depth::Current();
  if (g_depth_inverted_setting < 0 && !depth.measured) return false;
  // Vulkan: flat depth (a loading screen, Kyty's first seconds) says nothing about the direction; tuning LaunchPad on it meant a second
  // tune, and a second hitch, once real depth came.
  if (g_seen.api == device_api::vulkan && g_depth_inverted_setting < 0 && depth.flat) return false;
  *reversed = g_depth_inverted;
  *has_upside_down = g_depth_upside_down_setting >= 0 || depth.measured;
  *upside_down = g_depth_upside_down_setting >= 0 ? g_depth_upside_down_setting == 1 : depth.upside_down;
  return true;
}

// A depth setting as LaunchPad gets it: its own, else the global one, else ReShade's default.
bool LaunchPadDefinition(effect_runtime* runtime, const char* name, bool fallback) {
  char value[32] = {};
  if (runtime->get_preprocessor_definition_for_effect(LAUNCHPAD_FILE, name, value)) return hsf::launchpad::DefinitionOn(value);
  value[0] = '\0';
  if (runtime->get_preprocessor_definition(name, value)) return hsf::launchpad::DefinitionOn(value);
  return fallback;
}

// ReShade's techniques in render order, with their handles.
std::vector<hsf::launchpad::Technique> TechniqueOrder(effect_runtime* runtime, std::vector<effect_technique>* handles) {
  std::vector<hsf::launchpad::Technique> order;
  runtime->enumerate_techniques(nullptr, [&order, handles](effect_runtime* rt, effect_technique technique) {
    char effect[128] = {}, name[128] = {};
    rt->get_technique_effect_name(technique, effect);
    rt->get_technique_name(technique, name);
    hsf::launchpad::Technique t;
    t.launchpad = _stricmp(effect, LAUNCHPAD_FILE) == 0 && std::strcmp(name, LAUNCHPAD_TECHNIQUE) == 0;
    t.hsf = _stricmp(effect, "HSF.fx") == 0;
    t.enabled = rt->get_technique_state(technique);
    order.push_back(t);
    handles->push_back(technique);
  });
  return order;
}

struct LaunchPadState {
  bool loaded = false;
  int quality = -1;
  bool first = false;
  bool depth_known = false, depth_matches = false;
  bool reversed = false, has_upside_down = false, upside_down = false;  // the depth HSF has
};

LaunchPadState ReadLaunchPad(effect_runtime* runtime) {
  LaunchPadState state;
  state.loaded = runtime->find_technique(LAUNCHPAD_FILE, LAUNCHPAD_TECHNIQUE).handle != 0;
  if (!state.loaded) return state;
  const effect_uniform_variable quality = runtime->find_uniform_variable(LAUNCHPAD_FILE, "OPTICAL_FLOW_Q");
  if (quality.handle != 0) runtime->get_uniform_value_int(quality, &state.quality, 1);
  std::vector<effect_technique> handles;
  state.first = !hsf::launchpad::NeedsMove(TechniqueOrder(runtime, &handles));
  state.depth_known = KnownDepth(&state.reversed, &state.has_upside_down, &state.upside_down);
  if (state.depth_known) {
    state.depth_matches = LaunchPadDefinition(runtime, DEPTH_REVERSED, hsf::launchpad::DEFAULT_REVERSED) == state.reversed &&
                          (!state.has_upside_down || LaunchPadDefinition(runtime, DEPTH_UPSIDE_DOWN, hsf::launchpad::DEFAULT_UPSIDE_DOWN) == state.upside_down);
  }
  return state;
}

// The game's frame size, for AUTO.
void FrameSize(effect_runtime* runtime, uint32_t* width, uint32_t* height) {
  *width = *height = 0;
  runtime->get_screenshot_width_and_height(width, height);
}

// The Flow Quality the LAUNCHPAD row asks for now, or -1 if nothing is picked.
int WantedLaunchPadQuality(effect_runtime* runtime) {
  uint32_t width = 0, height = 0;
  FrameSize(runtime, &width, &height);
  return hsf::launchpad::ModeQuality(g_launchpad_mode, width, height);
}

void TuneLaunchPad(effect_runtime* runtime, const LaunchPadState& state, int wanted) {
  // What was there, for Undo. Only the first tune's, so tuning twice still undoes to the user's own settings.
  if (GetPrivateProfileIntW(L"LaunchPadUndo", L"Saved", 0, g_ini_path) == 0) {
    wchar_t value[16];
    swprintf(value, std::size(value), L"%d", state.quality);
    WritePrivateProfileStringW(L"LaunchPadUndo", L"Quality", value, g_ini_path);
    WritePrivateProfileStringW(L"LaunchPadUndo", L"Reversed", LaunchPadDefinition(runtime, DEPTH_REVERSED, hsf::launchpad::DEFAULT_REVERSED) ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"LaunchPadUndo", L"UpsideDown", LaunchPadDefinition(runtime, DEPTH_UPSIDE_DOWN, hsf::launchpad::DEFAULT_UPSIDE_DOWN) ? L"1" : L"0", g_ini_path);
    WritePrivateProfileStringW(L"LaunchPadUndo", L"Saved", L"1", g_ini_path);
  }
  const effect_uniform_variable quality = runtime->find_uniform_variable(LAUNCHPAD_FILE, "OPTICAL_FLOW_Q");
  if (quality.handle != 0) runtime->set_uniform_value_int(quality, wanted);
  std::vector<effect_technique> handles;
  const std::vector<hsf::launchpad::Technique> order = TechniqueOrder(runtime, &handles);
  const bool moved = hsf::launchpad::NeedsMove(order);
  if (moved) {
    std::vector<effect_technique> sorted;
    for (const size_t i : hsf::launchpad::LaunchPadFirst(order)) sorted.push_back(handles[i]);
    runtime->reorder_techniques(sorted.size(), sorted.data());
  }
  bool depth_set = false;
  if (state.depth_known && !state.depth_matches) {
    // LaunchPad's own definitions only: other effects keep ReShade's global ones.
    runtime->set_preprocessor_definition_for_effect(LAUNCHPAD_FILE, DEPTH_REVERSED, state.reversed ? "1" : "0");
    if (state.has_upside_down) runtime->set_preprocessor_definition_for_effect(LAUNCHPAD_FILE, DEPTH_UPSIDE_DOWN, state.upside_down ? "1" : "0");
    depth_set = true;
  }
  runtime->save_current_preset();
  char text[300];
  std::snprintf(text, sizeof(text), "LaunchPad tuned%s: Flow Quality %d -> %d, %s, depth %s", g_launchpad_mode == hsf::launchpad::MODE_AUTO ? " (AUTO)" : "", state.quality, wanted,
                moved ? "moved to the top" : "already at the top", depth_set ? (state.reversed ? "set to reversed" : "set to normal") : "left as it was");
  Log(text);
}

void UndoLaunchPad(effect_runtime* runtime) {
  if (GetPrivateProfileIntW(L"LaunchPadUndo", L"Saved", 0, g_ini_path) == 0) return;
  const int old_quality = static_cast<int>(GetPrivateProfileIntW(L"LaunchPadUndo", L"Quality", 0, g_ini_path));
  const effect_uniform_variable quality = runtime->find_uniform_variable(LAUNCHPAD_FILE, "OPTICAL_FLOW_Q");
  if (quality.handle != 0 && old_quality >= 0) runtime->set_uniform_value_int(quality, old_quality);
  const bool reversed = GetPrivateProfileIntW(L"LaunchPadUndo", L"Reversed", 1, g_ini_path) != 0;
  const bool upside_down = GetPrivateProfileIntW(L"LaunchPadUndo", L"UpsideDown", 0, g_ini_path) != 0;
  if (LaunchPadDefinition(runtime, DEPTH_REVERSED, hsf::launchpad::DEFAULT_REVERSED) != reversed)
    runtime->set_preprocessor_definition_for_effect(LAUNCHPAD_FILE, DEPTH_REVERSED, reversed ? "1" : "0");
  if (LaunchPadDefinition(runtime, DEPTH_UPSIDE_DOWN, hsf::launchpad::DEFAULT_UPSIDE_DOWN) != upside_down)
    runtime->set_preprocessor_definition_for_effect(LAUNCHPAD_FILE, DEPTH_UPSIDE_DOWN, upside_down ? "1" : "0");
  runtime->save_current_preset();
  WritePrivateProfileStringW(L"LaunchPadUndo", nullptr, nullptr, g_ini_path);
  g_launchpad_mode = hsf::launchpad::MODE_NONE;
  WritePrivateProfileStringW(L"HSF", L"LaunchPadQuality", LaunchPadModeName(g_launchpad_mode), g_ini_path);
  char text[160];
  std::snprintf(text, sizeof(text), "LaunchPad tune undone: Flow Quality back to %d, depth as before (it stays at the top)", old_quality);
  Log(text);
}

// The LaunchPad part of the panel.
void DrawLaunchPadTune(effect_runtime* runtime) {
  ImGui::SeparatorText("LaunchPad (optical flow)");
  const LaunchPadState state = ReadLaunchPad(runtime);
  if (!state.loaded) {
    ImGui::TextUnformatted("LaunchPad not found. Optical motion needs iMMERSE LaunchPad by Marty (martysmods.com) installed in ReShade.");
    return;
  }
  static const char* const QUALITY[] = {"Low", "Medium", "High"};
  ImGui::Text("Flow Quality %s, %s, depth settings %s", state.quality >= 0 && state.quality <= 2 ? QUALITY[state.quality] : "?",
              state.first ? "at the top" : "NOT at the top (an effect runs before it)",
              !state.depth_known ? "not checked yet (HSF has not measured depth)" : state.depth_matches ? "match HSF's" : "do NOT match HSF's");
  static const char* const MODE[] = {"Low", "Medium", "High", "AUTO"};
  ImGui::Text("HSF's pick: %s", g_launchpad_mode >= 0 && g_launchpad_mode <= 3 ? MODE[g_launchpad_mode] : "none (the player's own)");
  if (GetPrivateProfileIntW(L"LaunchPadUndo", L"Saved", 0, g_ini_path) != 0 && ImGui::Button("Undo the LaunchPad tune")) UndoLaunchPad(runtime);
  ImGui::TextDisabled("Pick the quality in the LAUNCHPAD row of HSF's menu. It sets LaunchPad's own settings and saves them to the ReShade preset.");
}

// The rest of the tune, besides the quality: LaunchPad first, and its depth settings as HSF's.
bool LaunchPadPlaced(const LaunchPadState& state) { return state.first && (!state.depth_known || state.depth_matches); }

// AUTO, the default: once a second, the whole tune for the game's size now (the player can change resolution in game): the quality, LaunchPad
// first in ReShade's order, its depth settings as HSF's once HSF knows the depth. Nothing while Lumenite gives the flow.
void KeepLaunchPadAuto(effect_runtime* runtime) {
  if (g_launchpad_mode != hsf::launchpad::MODE_AUTO || g_seen.source == 1) return;
  static ULONGLONG next = 0;
  const ULONGLONG now = GetTickCount64();
  if (now < next) return;
  next = now + 1000;
  const LaunchPadState state = ReadLaunchPad(runtime);
  if (!state.loaded || state.quality < 0) return;  // no quality setting found: not a LaunchPad HSF knows
  uint32_t width = 0, height = 0;
  FrameSize(runtime, &width, &height);
  if (width == 0 || height == 0) return;
  const int wanted = hsf::launchpad::AutoQuality(width, height);
  if (state.quality == wanted && LaunchPadPlaced(state)) return;
  // One try for one size and depth: each tune recompiles LaunchPad, a hitch of 100 ms or more in play (Valheim: two in a minute, from a
  // setting that did not stick). A setting that does not stick is left as it is.
  static int tries = 0, tried_for = -1;
  const int key = wanted * 8 + (state.depth_known ? 4 : 0) + (state.reversed ? 2 : 0) + (state.has_upside_down && state.upside_down ? 1 : 0);
  if (key != tried_for) {
    tried_for = key;
    tries = 0;
  }
  if (tries >= 1) return;
  ++tries;
  TuneLaunchPad(runtime, state, wanted);
}

// ---- Older-card MFG mods ----
// MFG-Unlocker (dashdogy, MIT) brings DLSS MFG to RTX 40, 30 and 20. HSF only reads the status file it writes next to its settings: in the game's
// folder when the game loaded it, in HSF_streamline when HSF's helper did. Its settings file is its own and HSF never writes it.
struct OlderCardMod {
  bool live = false;
  bool ready = false;  // it found Streamline's DLSS-G in that process
  bool fg_on = false;
  uint32_t family = 0;  // MFG-Unlocker's: 0 unknown, 1 Ada, 2 Ampere, 3 conflict, 4 Turing, 5 Blackwell
  uint32_t applied = 0;
  uint32_t safe_max = 0;
  const char* where = "";
};

// A number or true/false after "key": in a flat JSON object. False if the key is missing.
bool JsonValue(const std::string& text, const char* key, uint32_t* value) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = text.find(needle);
  if (at == std::string::npos) return false;
  const char* p = text.c_str() + at + needle.size();
  if (std::strncmp(p, "true", 4) == 0) { *value = 1; return true; }
  if (std::strncmp(p, "false", 5) == 0) { *value = 0; return true; }
  if (*p < '0' || *p > '9') return false;
  *value = static_cast<uint32_t>(std::strtoul(p, nullptr, 10));
  return true;
}

// Reads the status file if it was written in the last 10 seconds (it rewrites it while it runs).
bool ReadOlderCardMod(const std::wstring& path, OlderCardMod* mod) {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
  FILETIME now_file{};
  GetSystemTimeAsFileTime(&now_file);
  const auto ticks = [](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
  if (ticks(now_file) - ticks(info.ftLastWriteTime) > 10ull * 10000000ull) return false;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  std::string text(64 * 1024, '\0');
  DWORD got = 0;
  const BOOL ok = ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
  CloseHandle(file);
  if (!ok || got == 0) return false;
  text.resize(got);
  mod->live = true;
  JsonValue(text, "gpuFamily", &mod->family);
  JsonValue(text, "appliedMultiplier", &mod->applied);
  JsonValue(text, "safeMaximumMultiplier", &mod->safe_max);
  uint32_t on = 0;
  if (JsonValue(text, "appliedFrameGenerationOn", &on)) mod->fg_on = on != 0;
  if (JsonValue(text, "bridgeReady", &on)) mod->ready = on != 0;
  return true;
}

// The status as last read; read again at most once a second.
const OlderCardMod& OlderCardModNow() {
  static ULONGLONG last_read = 0;
  static OlderCardMod mod;
  const ULONGLONG now = GetTickCount64();
  if (now - last_read > 1000) {
    last_read = now;
    std::wstring game = g_ini_path;
    game.resize(game.find_last_of(L'\\'));
    // In a D3D11 64-bit game both the game and HSF's helper may load it, but only the copy that found DLSS-G does anything.
    OlderCardMod in_game, in_helper;
    const bool game_live = ReadOlderCardMod(game + L"\\RTXMFG-Universal.status.json", &in_game);
    const bool helper_live = ReadOlderCardMod(std::wstring(g_sl_folder) + L"\\RTXMFG-Universal.status.json", &in_helper);
    in_game.where = "in the game";
    in_helper.where = "in HSF's helper";
    mod = helper_live && (in_helper.ready || !game_live || !in_game.ready) ? in_helper : game_live ? in_game : OlderCardMod{};
  }
  return mod;
}

void DrawOlderCardMod() {
  const OlderCardMod& mod = OlderCardModNow();
  if (!mod.live) return;
  ImGui::SeparatorText("MFG-Unlocker (older RTX cards)");
  static const char* const FAMILY[] = {"card not identified yet", "RTX 40 (Ada): MFG unlocked", "RTX 30 (Ampere): its own frame generation",
                                       "more than one card type", "RTX 20 (Turing): its own frame generation", "RTX 50: native MFG, nothing changed"};
  ImGui::Text("Running %s. %s.", mod.where, mod.family < std::size(FAMILY) ? FAMILY[mod.family] : "unknown card type");
  // On RTX 40 and 50 it follows HSF's multiplier. On RTX 30 and 20 it runs its own frame generation and takes the multiplier from its own menu.
  const bool own = mod.family == 2 || mod.family == 4;
  if (mod.fg_on) ImGui::Text("Frame generation on at %ux (up to %ux on this card). %s", mod.applied, mod.safe_max, own ? "Set the multiplier in its menu (Backspace): HSF's has no effect here." : "It follows HSF's multiplier.");
  else ImGui::TextUnformatted("Frame generation off right now.");
  ImGui::TextDisabled("Its own settings are in its menu (Backspace). HSF only reads its status.");
}

// Every number HSF has, as the panel showed them before the menu (DrawPanel): under Debug, "All details".
void DrawDetails(effect_runtime* runtime) {
#ifdef HSF_SL_GAME
  if (g_sl_bring && D3d12Run()) {
    const hsf::slbring::Stats stats = hsf::slbring::GetStats();
    ImGui::Text("Streamline brought in by HSF: %s", stats.swap_chains != 0 ? "running" : "no swap chain yet");
    ImGui::Text("DLSS-G %s, asked for %ux, really showing %ux. Status 0x%x, up to %ux", stats.generating ? "generating" : "not generating", g_dlssg_frames + 1, stats.shown_per_real, stats.status, stats.max_frames + 1);
    DrawD3d12Controls(stats.max_frames, g_bring_using_game ? "the game's own DLSS vectors and depth" : MotionFrom() != 1 ? "optical (this game sends no DLSS data now)" : "optical", true);
    DrawLaunchPadTune(runtime);
    DrawOlderCardMod();
    return;
  }
  if (g_sl_drive && D3d12Run()) {
    const hsf::slgame::Stats stats = hsf::slgame::GetStats();
    ImGui::Text("DLSS-G inside the game: %s, asked for %ux", stats.generating ? "generating" : !stats.have_options ? "not found yet" : "waiting for vectors", stats.frames + 1);
    ImGui::Text("plugin loaded: %s", stats.plugin_loaded == 1 ? "yes" : stats.plugin_loaded == 0 ? "NO" : "not known yet");
    ImGui::Text("really showing %ux. Status 0x%x, up to %ux, vectors and depth tagged %llu%s", stats.presented, stats.status, stats.max_frames + 1, stats.tagged, stats.dynamic_resolution ? ", dynamic resolution" : "");
    DrawD3d12Controls(stats.max_frames, g_using_dlss ? "the game's own vectors and depth" : "optical (needs LaunchPad and HSF.fx loaded)", false);
    DrawLaunchPadTune(runtime);
    DrawOlderCardMod();
    ImGui::SeparatorText("The game's own vectors");
    if (ImGui::Checkbox("HSF rebuilds the camera matrices (F11 in game)", &g_sl_camera_rebuild)) SaveIni();
    if (ImGui::Checkbox("Reset DLSS-G when the camera jumps", &g_sl_cut_check)) SaveIni();
    if (ImGui::Checkbox("Stop generation sooner when vectors stop (menus)", &g_sl_quick_off)) SaveIni();
    if (ImGui::Checkbox("Stop generation while the score doubts the vectors", &g_sl_score_guard)) SaveIni();
    if (ImGui::Checkbox("Keep the game's HUD out of generation", &g_sl_hud)) SaveIni();
    ImGui::Text("HUD found on %llu of %llu frames, picture handed over %llu times", g_slh_found.load(), g_slh_evaluates.load(), g_slh_tags.load());
    ImGui::Text("HSF's resets %llu, picture cuts %llu, %llu of them with no reset", stats.cuts_by_hsf, stats.picture_cuts, stats.picture_cuts_no_reset);
    if (stats.score_pairs != 0) ImGui::Text("vectors' score %.2f (1 perfect, 0 no help) over %u pairs%s", stats.score_gain, stats.score_pairs, stats.vectors_doubted ? ", DOUBTED" : "");
    return;  // the rest of this panel is about the D3D11 path
  }
#endif
  if (!g_seen.have_api) {
    ImGui::TextUnformatted("Waiting for the first frame.");
    return;
  }
  bool changed = ImGui::Checkbox("Frame generation", &g_extra_enabled);
  {
    const char* const MODES[] = {"2x", "3x", "4x", "5x", "6x"};
    // Only the modes DLSS-G says this machine can do (known once it has started).
    const int count = static_cast<int>(std::max(std::max(g_dlssg_stats.max_frames, 1u), g_dlssg_frames));
    int mode = static_cast<int>(g_dlssg_frames) - 1;
    if (ImGui::Combo("DLSS-G frame generation", &mode, MODES, count)) {
      g_dlssg_frames = static_cast<uint32_t>(mode) + 1;
      changed = true;
    }
  }
  if (ImGui::Combo("Motion vectors from", &g_source_setting, SOURCE_NAMES, SOURCE_COUNT + 1)) changed = true;
  if (ImGui::Combo("Motion vectors mean", &g_motion_convention, MOTION_NAMES, MOTION_COUNT)) changed = true;
  if (ImGui::Combo("Overlay", &g_overlay_mode, OVERLAY_NAMES, 3)) changed = true;
  // FPS cap: applied while dragging, written to HSF.ini when let go.
  int cap = static_cast<int>(g_fps_cap);
  if (ImGui::SliderInt("FPS cap (on screen, 0 = off)", &cap, 0, 360, cap == 0 ? "off" : "%d fps")) g_fps_cap = CapRule(cap);
  changed |= ImGui::IsItemDeactivatedAfterEdit();
  // One click sets the cap to the main monitor's refresh rate.
  const uint32_t monitor_hz = MainMonitorHz();
  if (monitor_hz == 0) {
    ImGui::TextDisabled("Main monitor's refresh rate: not found, set the limit with the slider");
  } else if (g_fps_cap == monitor_hz) {
    ImGui::TextDisabled("Limit matches the main monitor (%u Hz)", monitor_hz);
  } else {
    char label[64];
    std::snprintf(label, sizeof(label), "Set limit to my refresh rate (%u Hz)", monitor_hz);
    if (ImGui::Button(label)) {
      g_fps_cap = monitor_hz;
      changed = true;
    }
  }
  if (g_fps_cap == 0) {
    // Pulses about once a second, from dim to full red.
    const float pulse = 0.65f + 0.35f * std::sin(static_cast<float>(ImGui::GetTime()) * 6.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.15f, 0.15f, pulse), "WARNING: no FPS limit. It is BEST to set a limit.");
  }
  {
    changed |= ImGui::Checkbox("Reflex boost", &g_reflex_boost);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Game queues at most 1 frame", &g_queue_one);
    changed |= ImGui::Checkbox("Reflex sleep on the game thread (off = A/B latency test)", &g_reflex_sleep);
  }
  changed |= ImGui::Checkbox("Motion score", &g_score_enabled);
  {
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Send the game's depth to DLSS-G", &g_depth_enabled);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Clean up motion with depth", &g_compose_enabled);
    changed |= ImGui::Checkbox("Scene cuts, HUD and still pictures", &g_checks_enabled);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Keep overlays out of generated frames", &g_keep_overlays_out);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Keep the game's HUD out too", &g_hud_enabled);
    if (g_hud_enabled) {
      ImGui::SameLine();
      if (g_hud_guard.InUse())
        ImGui::Text("covers %.1f%%", g_hud_share * 100.f);
      else
        ImGui::Text("covers %.1f%%: too much, not used", g_hud_share * 100.f);
    }
    changed |= ImGui::Checkbox("Exact camera motion", &g_camera_enabled);
    ImGui::SameLine();
    changed |= ImGui::Checkbox("Fit it from the flow when no matrix (can shake)", &g_camera_fit);
    if (g_camera_enabled) {
      static const char* const CAMERA_SOURCES[] = {"flow only (no matrix locked, or no real depth)", "fitted from the flow and depth", "the game's camera matrix"};
      ImGui::SameLine();
      ImGui::Text("%s, explains %d%% of the flow", CAMERA_SOURCES[static_cast<int>(g_dlssg_stats.camera_source)], g_dlssg_stats.camera_share);
      if (ImGui::Button(g_record_file.IsOpen() ? "Stop recording the camera data" : "Record the camera data (30 s, for study offline)")) g_record_request = true;
      if (g_record_file.IsOpen()) {
        ImGui::SameLine();
        ImGui::Text("recording, %u frames", g_record_file.Frames());
      }
    }
    static const char* const INVERTED_NAMES[] = {"find it", "normal (far = 1)", "reversed (far = 0)"};
    int inverted = g_depth_inverted_setting + 1;
    if (ImGui::Combo("Depth direction", &inverted, INVERTED_NAMES, 3)) {
      g_depth_inverted_setting = inverted - 1;
      changed = true;
    }
    static const char* const UPSIDE_DOWN_NAMES[] = {"find it", "as the game has it", "upside down"};
    int upside_down = g_depth_upside_down_setting + 1;
    if (ImGui::Combo("Depth way up", &upside_down, UPSIDE_DOWN_NAMES, 3)) {
      g_depth_upside_down_setting = upside_down - 1;
      changed = true;
    }
  }
  if (changed) SaveIni();
  DrawLaunchPadTune(runtime);
  DrawOlderCardMod();
  ImGui::TextDisabled("Sources: launchpad = iMMERSE LaunchPad's optical flow (HSF.fx asks for it),");
  ImGui::TextDisabled("lumenite = LumeniteFX Kernel's; auto = the first of these there. Mean: auto = what the source gives.");
  ImGui::TextDisabled("HSF.ini MfgKey (default F7) cycles 2x, 3x ... in game.");
  ImGui::TextDisabled("FPS cap counts frames on screen: with DLSS-G it is Reflex's cap (the game runs at cap / 2x, 3x ...), otherwise HSF's own.");
  ImGui::Separator();
  ImGui::Text("Game: %s, %s", BITNESS, ApiName(g_seen.api));
  ImGui::Text("Frame: %ux%u (format %u)", g_seen.width, g_seen.height, static_cast<unsigned>(g_seen.back_format));
  if (g_seen.have_motion)
    ImGui::Text("Motion vectors: %s, %ux%u (format %u)", SOURCES[g_seen.source].label, g_seen.mv_width, g_seen.mv_height,
                static_cast<unsigned>(g_seen.mv_format));
  else
    ImGui::Text("Motion vectors: %s not found (is its effect on?)", g_source_setting == 0 ? "no source" : SOURCES[g_source_setting - 1].label);
  ImGui::Text("Status: %s", g_note);
  // Quality level: always 0 until depth and camera data exist.
  char score_line[200];
  ScoreLine(score_line, sizeof(score_line));
  ImGui::Text("%s. DLSS-G's motion input: %s", DepthSent() ? "Level 1: optical flow + real depth" : "Level 0: optical flow only, depth constant", InputName());
  ImGui::TextUnformatted(score_line);
  char depth_line[300];
  DepthLine(depth_line, sizeof(depth_line));
  ImGui::TextUnformatted(depth_line);
  if (hsf::depth::Current().flat_for_long && g_flat_depth_noticed) {
    if (g_copy_before_clears_set) {
      ImGui::TextUnformatted("DepthCopyBeforeClears is on in ReShade.ini. Restart the game for it to take effect.");
    } else if (g_copy_before_clears) {
      ImGui::TextUnformatted("Depth is flat even with DepthCopyBeforeClears on: a menu, a 2D game, or no usable depth buffer.");
    } else {
      ImGui::TextUnformatted("Depth has only been flat so far. If this is gameplay and not a menu, the game may clear it before the frame ends.");
      if (ImGui::Button("Copy depth before clears (needs a game restart)")) {
        reshade::set_config_value(nullptr, "DEPTH", "DepthCopyBeforeClears", "1");
        g_copy_before_clears_set = true;
        Log("depth: DepthCopyBeforeClears=1 set in ReShade.ini from the panel; takes effect after a game restart");
      }
    }
  }
  if (g_depth_pin_note == 1)
    ImGui::Text("Depth size %ux%u saved. Restart the game for steady depth.", g_depth_pin_size[0], g_depth_pin_size[1]);
  else if (g_depth_pin_note == 2)
    ImGui::TextUnformatted("Depth size cleared. Restart the game to find the new one.");
  uint32_t bound_width = 0, bound_height = 0;
  if ((g_seen.api == device_api::vulkan || g_seen.api == device_api::d3d11 || g_seen.api == device_api::d3d9) && hsf::dlssg::Feeding() &&
      !BoundDepthSize(runtime, bound_width, bound_height)) {
    int aspect_check = 1;
    reshade::get_config_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", aspect_check);
    if (g_aspect_check_off_set) {
      ImGui::TextUnformatted("ReShade's depth shape check is off. Restart the game for it to take effect.");
    } else if (aspect_check != 0) {
      ImGui::TextUnformatted("No depth found. An emulator that draws a 16:9 picture on a wider screen has depth of another shape, which ReShade turns away.");
      if (ImGui::Button("Accept depth of another shape (needs a game restart)")) {
        reshade::set_config_value(nullptr, "DEPTH", "UseAspectRatioHeuristics", "0");
        g_aspect_check_off_set = true;
        Log("depth: UseAspectRatioHeuristics=0 set in ReShade.ini from the panel; takes effect after a game restart");
      }
    }
  }
  ImGui::TextDisabled("Score: 100%% = motion lines the frames up exactly, 0%% = no better than no motion, below 0 = worse. It rates the motion going in, not the picture coming out.");
  ImGui::TextDisabled("HSF.ini ViewKey (default F8) cycles the debug views; InputKey (default F9) cycles DLSS-G's motion input: each source, then zero motion; DepthKey (default F10) flips its depth, real or constant.");
  if (hsf::dlssg::Running()) {
    const hsf::dlssg::Stats& d = g_dlssg_stats;
    ImGui::Text("Reflex: %s, %s; game thread slept %.2f ms per frame; FPS cap %s", d.reflex_available ? "available" : "NOT available",
                d.reflex_game_paced ? "on the game's frame" : "not pacing the game yet", d.reflex_sleep_ms,
                g_fps_cap != 0 ? "on (Reflex)" : "off");
    ImGui::Text("Latency: HSF timer %.1f ms (worst %.1f); Reflex report %s %.1f ms (game %.1f, hand-off %.1f, wait %.1f, present %.1f, GPU %.1f)",
                d.latency_ms, d.latency_max_ms, d.report_frames != 0 ? "yes" : "NO", d.report_total_ms, d.report_game_ms, d.report_submit_ms,
                d.report_handoff_ms, d.report_present_ms, d.report_gpu_ms);
  } else if (g_fps_cap != 0) {
    ImGui::Text("FPS cap %u: HSF's own limiter (no Reflex without DLSS-G)", g_fps_cap);
  }
  ImGui::Text("Game presents %.1f/s (sync interval %u, flags 0x%x)", g_stats.game_fps, g_game_sync, g_game_flags);
  ImGui::Text("Motion vectors mean %s (%s)", hsf::motion::CONVENTION_NAMES[Convention()], g_motion_convention == 0 ? "the source's" : "HSF.ini");
}

// ---- The menu ----
// What users see in ReShade's HSF tab (ui.h has the look). Main: frame generation on/off, multiplier, motion, Reflex, FPS cap and one status line.
// Advanced and Debug fold away; Debug ends with every number the panel showed before (DrawDetails). Generation keeps running while ReShade's
// menu is open: on RTX 30 and 20, MFG-Unlocker offers its higher multipliers only while generation runs, and its menu is clicked with ReShade's open.

// Real frame times as ReShade presents them, on every route (D3D11's native Present hook only exists on D3D11).
constexpr size_t MENU_HISTORY = 120;
float g_menu_ms[MENU_HISTORY] = {};
size_t g_menu_write = 0;
float g_menu_fps = 0.f;

struct MenuView {
  const char* api = "";
  const char* route = "";
  bool d3d12 = false;
  bool drive = false;              // the game's own Streamline
  bool bring = false;              // Streamline that HSF brought
  float base_fps = 0.f, out_fps = 0.f;
  uint32_t max_frames = 0;         // generated per real frame this machine can do, 0 = not known yet
  bool generating = false;
  const char* problem = nullptr;   // red: something is wrong
  const char* waiting = nullptr;   // amber: not running yet, for a known reason
};

MenuView ReadMenuView() {
  MenuView v;
  v.base_fps = g_menu_fps;
  v.out_fps = g_menu_fps;
#ifdef HSF_SL_GAME
  // The D3D12 routes' views only once this run has shown more D3D12 frames than D3D11 ones: a D3D11 game with a D3D12 route left on in HSF.ini runs on the helper.
  const bool d3d12_run = D3d12Run();
  if (g_sl_bring && d3d12_run) {
    const hsf::slbring::Stats s = hsf::slbring::GetStats();
    v.api = "D3D12";
    v.route = "IN GAME // HSF STREAMLINE";
    v.d3d12 = v.bring = true;
    v.max_frames = s.max_frames;
    v.generating = s.generating && s.shown_per_real > 1;
    if (v.generating) v.out_fps = v.base_fps * static_cast<float>(s.shown_per_real);
    if (!s.streamline_up) v.waiting = "Streamline has not started yet.";
    else if (s.swap_chains == 0) v.waiting = "No swap chain through Streamline yet.";
    else if (s.plugin_loaded == 0) v.problem = "DLSS-G's plugin did not load (see HSF_sl_bring.log).";
    else if (s.status > 0) v.problem = "DLSS-G reports a problem (see HSF_sl_bring.log).";
    return v;
  }
  if (g_sl_drive && d3d12_run) {
    const hsf::slgame::Stats s = hsf::slgame::GetStats();
    v.api = "D3D12";
    v.route = "IN GAME // GAME STREAMLINE";
    v.d3d12 = v.drive = true;
    v.max_frames = s.max_frames;
    v.generating = s.generating && s.presented > 1;
    if (v.generating) v.out_fps = v.base_fps * static_cast<float>(s.presented);
    if (g_drive_left) v.waiting = "This game never starts its own Streamline. HSF will bring its own: restart the game once.";
    else if (s.old_streamline) v.waiting = "This game's Streamline is too old for HSF (1.x). Use the game's own frame generation.";
    else if (!s.have_options) v.waiting = "The game's DLSS-G is not found yet. Turn on DLSS (any mode) in the game.";
    else if (s.plugin_loaded == 0) v.problem = "DLSS-G's plugin did not load (see HSF_sl_probe.log).";
    else if (s.status != 0) v.problem = "DLSS-G reports a problem (see HSF_sl_probe.log).";
    else if (!s.data_flowing && MotionFrom() == 2) v.waiting = "Waiting for the game's motion vectors (menus send none).";
    return v;
  }
#endif
  v.api = g_seen.api == device_api::d3d11 ? "D3D11" : g_seen.api == device_api::d3d12 ? "D3D12" : g_seen.api == device_api::d3d9 ? "DX9" : g_seen.have_api ? ApiName(g_seen.api) : "";
  if (g_seen.api == device_api::d3d9)
    v.route = hsf::dx9::CurrentRoute() == hsf::dx9::Route::gpu ? (sizeof(void*) == 4 ? "32-BIT // DX9 GPU BRIDGE" : "DX9 GPU BRIDGE")
                                                              : (sizeof(void*) == 4 ? "32-BIT // DX9 CPU BRIDGE" : "DX9 CPU BRIDGE");
  else
    v.route = sizeof(void*) == 4 ? "32-BIT // HSF HELPER" : "HSF HELPER";
  if (hsf::dlssg::Running()) {
    const hsf::dlssg::Stats& d = g_dlssg_stats;
    v.max_frames = d.max_frames;
    v.generating = d.generating;
    if (d.real_fps > 0.f) v.base_fps = d.real_fps;
    v.out_fps = d.shown_fps > 0.f ? d.shown_fps : v.base_fps;
    if (d.gpu_hang) v.problem = d.note;
    else if (d.foreign_dxgi) v.problem = "ReShade loaded into HSF's helper (Game Pass): its effects and add-ons run twice and cost frames. Send hsf_dlssg.log to HSF's GitHub.";
    else if (d.status != 0) v.problem = d.note[0] != '\0' ? d.note : "DLSS-G reports a problem (see HSF_streamline\\hsf_dlssg.log).";
    else if (!d.generating && d.note[0] != '\0') v.waiting = d.note;
  } else if (!g_seen.have_api) {
    v.waiting = "Waiting for the first frame.";
  } else {
    v.waiting = g_note;
  }
  return v;
}

void OnMenuFrame(effect_runtime*) {
  static LONGLONG last = 0, window_start = 0;
  static uint32_t window_frames = 0;
  const LONGLONG now = Now();
#ifdef HSF_SL_GAME
  // In-game route: ReShade presents DLSS-G's generated frames too, so counting presents gave the shown rate as BASE (Shadows: ~182 for ~30 real) and the short
  // gaps between generated frames as hitches. Count only presents where the game has started a new frame since the last one: those are the real frames.
  // Before the game sends any frame token (menus, loading) every present counts, as before.
  if (g_sl_drive && D3d12Run()) {
    // Once tokens stop for a quarter second (a menu, a load), every present counts again, so the numbers never freeze.
    static uint64_t s_seen = 0;
    static LONGLONG s_seen_at = 0;
    const uint64_t frames = hsf::slgame::GameFrames();
    if (frames != 0) {
      if (frames == s_seen && Seconds(now - s_seen_at) < 0.25) return;
      if (frames != s_seen) s_seen_at = now;
      s_seen = frames;
    }
  }
#endif
  if (last != 0) g_menu_ms[g_menu_write++ % MENU_HISTORY] = static_cast<float>(std::min(Seconds(now - last) * 1000.0, 1000.0));
  last = now;
  if (window_start == 0) window_start = now;
  ++window_frames;
  const double seconds = Seconds(now - window_start);
  if (seconds < 0.5) return;
  g_menu_fps = static_cast<float>(window_frames / seconds);
  window_frames = 0;
  window_start = now;
}

// The GPU reserve, once per new load reading (about a second): the card's load, and how many of the game's real frames since the last reading
// were spikes. Frame generation's state and frame rates come from what the menu shows.
void OnReserveFrame(effect_runtime*) {
  const bool wanted = g_gpu_reserve && g_extra_enabled;
  g_load_reader->Want(wanted);
  static size_t s_read_to = 0;
  if (!wanted) {
    if (g_reserve.Cap() != 0 || g_reserve.Full()) {
      g_reserve.Clear();
      Log("GPU reserve: off, the FPS cap is the player's again");
    }
    s_read_to = g_menu_write;
    return;
  }
  static uint32_t s_serial = 0;
  uint32_t serial = 0;
  const float load = g_load_reader->Load(&serial);
  if (serial == s_serial) return;
  s_serial = serial;
  const size_t count = std::min(g_menu_write - s_read_to, MENU_HISTORY);
  float frames[MENU_HISTORY];
  for (size_t i = 0; i < count; ++i) frames[i] = g_menu_ms[(g_menu_write - count + i) % MENU_HISTORY];
  s_read_to = g_menu_write;
  const MenuView v = ReadMenuView();
  hsf::reserve::Input in;
  in.seconds = Seconds(Now());
  in.load = load;
  in.spikes = hsf::reserve::Spikes(frames, count);
  in.generating = g_extra_enabled && v.generating;
  in.hold = g_menu_open;
  in.out_fps = v.out_fps;
  in.multiplier = g_dlssg_frames + 1;  // the player's setting: a ratio of measured rates jitters, and each change starts the reserve again
  in.user_cap = g_fps_cap;
  in.on = g_gpu_reserve;
  const bool was_full = g_reserve.Full();
  if (g_reserve.Update(in)) {
    char line[220];
    if (g_reserve.Cap() == 0)
      std::snprintf(line, sizeof(line), "GPU reserve: let go, the FPS cap is the player's again (%u, 0 = none); card load %.0f%%, spikes %.0f%% of real frames", g_fps_cap,
                    load, in.spikes * 100.f);
    else
      std::snprintf(line, sizeof(line), "GPU reserve: card load %.0f%%, spikes %.0f%% of real frames: FPS cap %u on screen (game %u fps at %ux); player's cap %u (0 = none)",
                    load, in.spikes * 100.f, g_reserve.Cap(), g_reserve.Cap() / std::max(in.multiplier, 1u), in.multiplier, g_fps_cap);
    Log(line);
  }
  if (g_reserve.Full() && !was_full) Log("GPU reserve: still stuttering on a full card with the game at 30 fps: lower the multiplier or the game's settings");
}

// The NVIDIA card's name without "NVIDIA GeForce", in capitals (the first adapter if there is no NVIDIA one).
const char* GpuName() {
  static char name[64] = "";
  static bool done = false;
  if (done) return name;
  done = true;
  IDXGIFactory1* factory = nullptr;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return name;
  DXGI_ADAPTER_DESC1 best{};
  bool found = false;
  IDXGIAdapter1* adapter = nullptr;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    adapter->Release();
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
    if (!found || (desc.VendorId == 0x10DE && best.VendorId != 0x10DE)) best = desc;
    found = true;
  }
  factory->Release();
  if (!found) return name;
  char text[128];
  WideCharToMultiByte(CP_UTF8, 0, best.Description, -1, text, sizeof(text), nullptr, nullptr);
  const char* start = text;
  for (const char* prefix : {"NVIDIA GeForce ", "NVIDIA ", "AMD Radeon ", "Intel(R) "})
    if (std::strncmp(start, prefix, std::strlen(prefix)) == 0) { start += std::strlen(prefix); break; }
  size_t i = 0;
  for (; start[i] != '\0' && i + 1 < sizeof(name); ++i) name[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(start[i])));
  name[i] = '\0';
  return name;
}

// A virtual-key code as its key name: F1-F24, else its number.
void KeyName(uint32_t key, char* text, size_t size) {
  if (key >= VK_F1 && key <= VK_F24) std::snprintf(text, size, "F%u", key - VK_F1 + 1);
  else std::snprintf(text, size, "KEY %u", key);
}

// Every setting the menu shows, back to HSF's defaults. Route switches (InGameDlssg, SlBringIn and the like) and hotkeys stay as they are.
void ResetMenuSettings() {
  g_extra_enabled = true;
  g_dlssg_frames = 1;
  g_reflex_sleep = true;
  g_reflex_boost = false;
  g_fps_cap = 0;
  g_depth_enabled = true;
  g_compose_enabled = true;
  g_checks_enabled = true;
  g_keep_overlays_out = true;
  g_camera_enabled = true;
  g_camera_fit = false;
  g_hud_enabled = true;
  g_depth_inverted_setting = -1;
  g_depth_upside_down_setting = -1;
  g_score_view = hsf::score::View::off;
#ifdef HSF_SL_GAME
  g_motion_from = 0;
  g_bring_view = 0;
  g_sl_camera_rebuild = true;
  g_sl_cut_check = true;
  g_sl_quick_off = false;
  g_sl_score_guard = false;
  g_sl_hud = false;
#endif
  SaveIni();
  Log("menu: settings reset to defaults");
}

// The end of a log file, at most `bytes`, from a whole line on. Opened for sharing: the logs are open for writing while the game runs.
std::string LogTail(const std::wstring& path, size_t bytes) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  GetFileSizeEx(file, &size);
  const LONGLONG start = std::max<LONGLONG>(0, size.QuadPart - static_cast<LONGLONG>(bytes));
  LARGE_INTEGER at{};
  at.QuadPart = start;
  SetFilePointerEx(file, at, nullptr, FILE_BEGIN);
  std::string text(static_cast<size_t>(size.QuadPart - start), '\0');
  DWORD got = 0;
  if (!text.empty()) ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &got, nullptr);
  CloseHandle(file);
  text.resize(got);
  if (start > 0) {
    const size_t line = text.find('\n');
    text.erase(0, line == std::string::npos ? text.size() : line + 1);
  }
  return text;
}

// HSF's lines from ReShade.log and the end of this route's own log, on the clipboard, for a tester to paste.
void CopyLog(const MenuView& view) {
  std::wstring folder = g_ini_path;
  folder.resize(folder.find_last_of(L'\\'));
  std::string out = "HSF log: ";
  out += view.api;
  out += " // ";
  out += view.route;
  out += " // ";
  out += GpuName();
  out += "\r\n\r\n--- ReShade.log (HSF lines) ---\r\n";
  const std::string reshade_log = LogTail(folder + L"\\ReShade.log", 512 * 1024);
  size_t from = 0;
  std::string hsf_lines;
  while (from < reshade_log.size()) {
    size_t end = reshade_log.find('\n', from);
    if (end == std::string::npos) end = reshade_log.size();
    const std::string line = reshade_log.substr(from, end - from);
    if (line.find("HSF") != std::string::npos) hsf_lines += line + "\n";
    from = end + 1;
  }
  if (hsf_lines.size() > 48 * 1024) hsf_lines.erase(0, hsf_lines.size() - 48 * 1024);
  out += hsf_lines.empty() ? "(none found next to HSF)\r\n" : hsf_lines;
  std::vector<std::wstring> logs;
  if (view.bring) logs.push_back(folder + L"\\HSF_sl_bring.log");
  else if (view.drive) logs.push_back(folder + L"\\HSF_sl_probe.log");
  else {
    logs.push_back(std::wstring(g_sl_folder) + L"\\hsf_dlssg.log");
    logs.push_back(std::wstring(g_sl_folder) + L"\\hsf_dlssg_game.log");
  }
  for (const std::wstring& path : logs) {
    const std::string tail = LogTail(path, 48 * 1024);
    if (tail.empty()) continue;
    char name[MAX_PATH];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str() + path.find_last_of(L'\\') + 1, -1, name, sizeof(name), nullptr, nullptr);
    out += "\r\n--- ";
    out += name;
    out += " (end) ---\r\n";
    out += tail;
  }
  ImGui::SetClipboardText(out.c_str());
  Log("menu: log copied to the clipboard");
}

// A setting read only when the game starts, as a toggle that writes HSF.ini straight away.
void StartupToggle(const char* label, const wchar_t* key, int fallback) {
  namespace ui = hsf::ui;
  bool on = GetPrivateProfileIntW(L"HSF", key, fallback, g_ini_path) != 0;
  ui::SettingLabel(label);
  ImGui::PushID(label);
  if (ui::Toggle("##t", &on)) WritePrivateProfileStringW(L"HSF", key, on ? L"1" : L"0", g_ini_path);
  ImGui::PopID();
}

bool DrawOverlaySettings();

// The menu is its own window, "HSF", and it belongs as a tab next to ReShade's Home, Add-ons and Settings. ReShade opens add-on windows with a plain Begin and never
// docks them, so in a new game it came up tiny at the top left. Once a run, while the window is not docked, this docks it where ReShade's Add-ons window is. ReShade
// begins its own windows earlier in the same frame, so a second Begin("###addons") only appends to that window (no rename, no new window) and reads its dock node.
// The dock takes effect next frame, and ReShade.ini keeps it from then on. If the player drags the tab out it comes back at the next launch.
void DockMenuWithReShade() {
  static bool s_done = false;
  if (s_done) return;
  s_done = true;
  if (ImGui::IsWindowDocked()) return;
  ImGui::SetWindowSize(ImVec2(ImGui::GetFontSize() * 40.f, ImGui::GetFontSize() * 44.f), ImGuiCond_FirstUseEver);  // in case it cannot dock
  ImGui::Begin("###addons", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  const ImGuiID dock = ImGui::GetWindowDockID();
  ImGui::End();
  if (dock == 0) {
    Log("menu: ReShade's windows are not docked, so HSF's menu stays its own window");
    return;
  }
  ImGui::SetNextWindowDockID(dock, ImGuiCond_Always);
  ImGui::Begin("HSF", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  ImGui::End();
  Log("menu: HSF's menu docked as a tab next to ReShade's Home and Add-ons");
}

void DrawPanel(effect_runtime* runtime) {
  namespace ui = hsf::ui;
  namespace c = hsf::ui::colour;
  DockMenuWithReShade();
  const MenuView now = ReadMenuView();
  bool changed = false;

  ui::BeginPanel("##hsf_menu");
  char right[96];
  std::snprintf(right, sizeof(right), "%s%s%s", GpuName(), now.api[0] != '\0' ? " // " : "", now.api);
  ui::Header("HSF // FRAME GEN", right);
  // A game with its own DLSS frame generation (HSF drives the game's own Streamline). HSF can't reliably tell whether the game's FG setting is on (Shadows sets it before
  // HSF's hook is in), so this is advice on every such game until the player presses OK, which HSF.ini keeps for this game.
  if (now.drive && !g_game_fg_ok) {
    if (ui::AlertBox("##gamefgok", "THIS GAME HAS ITS OWN FRAME GENERATION", "TURN IT OFF IN THE GAME'S SETTINGS TO USE HSF", "OK")) {
      g_game_fg_ok = true;
      WritePrivateProfileStringW(L"HSF", L"GameFgWarningOk", L"1", g_ini_path);
      Log("menu: the player pressed OK on the game's own frame generation warning (GameFgWarningOk=1)");
    }
  }

  // ---- The big number: frames out, live (generation keeps running while this menu is open).
  const MenuView& shown = now;
  char number[16], line1[48], line2[64];
  std::snprintf(number, sizeof(number), "%.0f", shown.out_fps);
  std::snprintf(line1, sizeof(line1), "FPS OUT");
  std::snprintf(line2, sizeof(line2), "%.0f BASE // %.1f MS", shown.base_fps, shown.base_fps > 0.f ? 1000.f / shown.base_fps : 0.f);
  const char* tag = !g_extra_enabled ? "OFF" : shown.problem != nullptr ? "PROBLEM" : shown.generating ? "GENERATING" : "NOT GENERATING";
  const ImU32 tag_colour = !g_extra_enabled ? c::DIM : shown.problem != nullptr ? c::RED : shown.generating ? c::GREEN : c::AMBER;
  ui::BigReadout(number, shown.generating && g_extra_enabled ? c::WHITE : c::TEXT, line1, line2, tag, tag_colour);

  // ---- Frame generation on or off.
  ui::RowLabel("FRAME GEN", c::GREEN);
  if (ui::Toggle("##fg", &g_extra_enabled)) changed = true;
  // OFF keeps the helper for a quick ON; the red CLOSE HELPER box beside OFF stops it, so the game shows its own fps again (StopHelperWhileOff).
  if (!g_extra_enabled && !g_helper_closed && AnyRunning()) {
    ImGui::SameLine();
    if (ui::Button("##closehelper", "CLOSE HELPER", c::RED)) {
      g_helper_closed = true;
      Log("menu: CLOSE HELPER pressed with frame gen off: the helper stops");
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("The DLSS-G helper is still running with frame gen off,\nso turning it back ON is instant.\n\nClose it for the game's full fps.\nTurning frame gen ON starts it again (about 10 s).");
  }

  // ---- Multiplier: only what DLSS-G says this machine can do, once it has said.
  ui::RowLabel("MULTIPLIER", c::RED);
  const OlderCardMod& older = OlderCardModNow();
  // RTX 30 and 20: MFG-Unlocker runs its own frame generation and sets the multiplier itself, so HSF's choice does nothing there. Show its number and where to change it.
  const bool mod_sets_multiplier = older.live && (older.family == 2 || older.family == 4);
  if (mod_sets_multiplier) {
    char text[96];
    if (older.fg_on && older.applied != 0) std::snprintf(text, sizeof(text), "%uX // SET IN MFG-UNLOCKER (BACKSPACE)", older.applied);
    else std::snprintf(text, sizeof(text), "SET IN MFG-UNLOCKER (BACKSPACE)");
    ui::Caps(text, c::AMBER, 0.8f);
  } else {
    static const char* const MODES[] = {"2X", "3X", "4X", "5X", "6X"};
    int mode = static_cast<int>(g_dlssg_frames) - 1;
    uint32_t above = 0;
    if (now.max_frames != 0)
      for (int i = 0; i < 5; ++i)
        if (static_cast<uint32_t>(i) + 1 > now.max_frames) above |= 1u << i;
    if (ui::Choice("##mult", MODES, 5, &mode, above)) {
      g_dlssg_frames = static_cast<uint32_t>(mode) + 1;
      changed = true;
    }
  }
  if (g_mfg_key != 0 && !mod_sets_multiplier) {
    char key[16];
    KeyName(g_mfg_key, key, sizeof(key));
    ui::KeyChip(key);
  }
  if (older.live && older.family != 5) {
    // RTX 40, 30 and 20 with MFG-Unlocker: what it says. Never on RTX 50, which has MFG of its own.
    char text[160];
    static const char* const FAMILY[] = {"CARD NOT IDENTIFIED YET", "RTX 40 DETECTED", "RTX 30 DETECTED", "MORE THAN ONE CARD TYPE", "RTX 20 DETECTED", ""};
    if (older.safe_max != 0)
      std::snprintf(text, sizeof(text), "%s // UP TO %uX UNLOCKED (MFG-UNLOCKER)", older.family < 5 ? FAMILY[older.family] : "UNKNOWN CARD", older.safe_max);
    else
      std::snprintf(text, sizeof(text), "%s (MFG-UNLOCKER)", older.family < 5 ? FAMILY[older.family] : "UNKNOWN CARD");
    ImGui::SetCursorScreenPos(ImVec2(ui::ControlX(), ImGui::GetCursorScreenPos().y));
    ui::Caps(text, c::AMBER, 0.75f);
  }
  // 5X and 6X on flow in a D3D12 game that has DLSS: flow keeps up with the game's motion at 3x but falls apart at 6x by eye, and the game's DLSS data is
  // one click away. Not on D3D11 or DX9, which only have flow and pass the eye test at 6x. Advice, not a block: it goes away on DLSS or 4X and below.
  if (now.d3d12 && g_dlss_seen && !g_using_dlss && g_dlssg_frames >= 4)
    ui::AlertBox("##flowhigh", "5X AND 6X ARE NOT RECOMMENDED WITH FLOW", "FOR 5X OR 6X USE DLSS MOTION, OR STAY AT 4X OR BELOW", nullptr, IM_COL32(255, 140, 0, 255));
  // Luma: its DLSS hands HSF the game's own vectors and depth, but only in D3D11 and only while Luma's DLSS is on. Until its data has come once, say what is missing.
  const bool luma = LumaLoaded();
  if (luma && now.d3d12)
    ui::AlertBox("##lumadx12", "LUMA FOUND, BUT THE GAME RUNS IN DX12", "START THE GAME IN DX11 TO SWITCH TO LUMA (DLSS) MOTION", nullptr, IM_COL32(255, 220, 0, 255));
#ifdef HSF_SL_GAME
  else if (luma && !g_dlss_seen)
    ui::AlertBox("##lumawait", "LUMA FOUND: NO DLSS DATA FROM IT YET", "TURN ON DLSS IN LUMA'S SETTINGS: HSF SWITCHES TO LUMA (DLSS) ON ITS OWN", nullptr, IM_COL32(255, 220, 0, 255));
  // The player picked FLOW while Luma's data is there: one click back to it. Not with NVOF, which keeps MOTION on FLOW.
  else if (luma && g_motion_from == 1 && g_flow_engine == 0 &&
           ui::AlertBox("##lumaswitch", "LUMA (DLSS) DATA IS HERE, MOTION IS ON FLOW", "THE GAME'S OWN MOTION AND DEPTH LOOK BETTER THAN FLOW", "SWITCH TO LUMA (DLSS)",
                        IM_COL32(255, 220, 0, 255))) {
    g_motion_from = 2;
    changed = true;
  }
#endif

  // ---- Motion: the game's own data when it sends it, else optical flow. D3D11 games always use flow (and depth when found).
  ui::RowLabel("MOTION", c::BLUE);
#ifdef HSF_SL_GAME
  if (now.d3d12 || g_dlss_seen || luma) {
    // AUTO / DLSS / FLOW (LUMA in place of DLSS on D3D11 with Luma). The boxes are in that order, g_motion_from is 0 auto, 1 flow, 2 DLSS (kept so old HSF.ini files still read right).
    // DLSS is crossed out until this run has seen the game's own DLSS data; AUTO then uses it on its own.
    const bool luma_data = luma && !now.d3d12;
    const char* const FROM[] = {"AUTO", luma_data ? "LUMA" : "DLSS", "FLOW"};
    static const int BOX_TO_FROM[] = {0, 2, 1};
    // NVOF picked: FLOW only, AUTO and DLSS crossed out (MotionFrom).
    const bool nvof = g_flow_engine == 1;
    int box = MotionFrom() == 2 ? 1 : MotionFrom() == 1 ? 2 : 0;
    const uint32_t crossed = nvof ? 3u : g_dlss_seen || g_motion_from == 2 ? 0u : 2u;
    if (ui::Choice("##motion", FROM, 3, &box, crossed)) {
      g_motion_from = BOX_TO_FROM[box];
      changed = true;
    }
    const bool game_data = g_using_dlss;
    ui::RightNote(game_data ? (luma_data ? "> LUMA (DLSS) DATA" : "> GAME DATA") : "> FLOW", c::DIM);
    // D3D11 with a DLSS mod: the flow effect off while the mod's data feeds DLSS-G (Dlss11Motion). It needs a few frames to settle when it comes back.
    if (!now.d3d12) {
      ui::RowLabel("FLOW PAUSE", c::BLUE);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("ON: the flow effect is switched off while the DLSS data comes, and back on when it stops (cuts, menus).\n"
                          "Other effects that read the same flow stop moving while it is off. OFF keeps it running.");
      static const char* const PAUSE[] = {"ON", "OFF"};
      int pause = g_flow_pause ? 0 : 1;
      if (ui::Choice("##flowpause", PAUSE, 2, &pause)) {
        g_flow_pause = pause == 0;
        changed = true;
      }
      ui::RightNote(g_flow_pause ? (luma_data ? "> FLOW OFF WHILE LUMA (DLSS) DATA COMES" : "> FLOW OFF WHILE DLSS DATA COMES") : "> FLOW ALWAYS ON", c::DIM);
    }
  } else
#endif
  {
    static const char* const FLOW_ONLY[] = {"FLOW"};
    int only = 0;
    ui::Choice("##motion", FLOW_ONLY, 1, &only);
    ui::RightNote(DepthSent() ? "> FLOW + DEPTH" : "> FLOW", c::DIM);
  }

  // ---- Flow from: which effect's optical flow HSF reads (HSF.ini Source). AUTO takes the first one loaded; a source not loaded is crossed out.
  // Switchable in play (in RE2, Lumenite failed to compile and LaunchPad was added). The next frame reads the new source.
  // NVOF picked: every box is crossed out, as NVOF makes the flow itself. An effect still loaded only lends its texture's size, which the note says;
  // with none, NVOF picks the size.
  ui::RowLabel("FLOW FROM", c::BLUE);
  {
    static const char* const FLOW_FROM[] = {"AUTO", "LAUNCHPAD", "LUMENITE"};
    const bool nvof = g_flow_engine == 1;
    uint32_t missing = 0;
    for (int i = 0; i < SOURCE_COUNT; ++i)
      if (g_source_variables[i].handle == 0 && g_source_setting != i + 1) missing |= 1u << (i + 1);
    if (nvof) missing = (1u << (SOURCE_COUNT + 1)) - 1u;
    if (ui::Choice("##flowfrom", FLOW_FROM, SOURCE_COUNT + 1, &g_source_setting, missing)) {
      g_input_zero = false;
      changed = true;
    }
    static const char* const IN_USE[] = {"> LAUNCHPAD", "> LUMENITE"};
    static const char* const SIZE_FROM[] = {"> NVOF (SIZE FROM LAUNCHPAD)", "> NVOF (SIZE FROM LUMENITE)"};
    const bool found = g_seen.source >= 0 && g_seen.source < SOURCE_COUNT;
    const char* note = found ? (nvof ? SIZE_FROM[g_seen.source] : IN_USE[g_seen.source]) : NvofOn() ? "> NONE: NVOF MAKES THE FLOW" : "> NONE FOUND";
    ui::RightNote(note, c::DIM);
  }

  // ---- Flow engine: who works the flow out (HSF.ini FlowEngine). SHADER is the effect above; NVOF is the GPU's own optical flow engine, which HSF runs on its
  // Direct3D 12 work in place of the effect's result. Switchable in play, to compare the two on the same scene. The note says what the last frame really used.
  ui::RowLabel("FLOW ENGINE", c::BLUE);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Who works out the optical flow.\nSHADER: the ReShade effect picked in FLOW FROM.\nNVOF: NVIDIA Optical Flow, the graphics card's own engine (RTX 20 and newer).\n\n"
                      "SHADER looks better and runs faster. NVOF is a backup:\nwith no flow effect loaded, HSF switches to it by itself.");
  {
    static const char* const ENGINE[] = {"SHADER", "NVOF"};
    if (ui::Choice("##flowengine", ENGINE, 2, &g_flow_engine)) changed = true;
    int state = g_dlssg_stats.nvof_state;
#ifdef HSF_SL_GAME
    if (now.d3d12) state = hsf::slbring::GetStats().nvof_state;
#endif
    static const char* const STATE[] = {"> NVOF NOT RUNNING YET", "> NVOF STARTING", "> NVOF", "> NVOF FAILED (SEE LOG): SHADER FLOW USED"};
    if (!NvofOn()) ui::RightNote("> SHADER", c::DIM);
    else if (NvofBackup()) ui::RightNote(state == 3 ? "> NVOF BACKUP FAILED (SEE LOG)" : "> NVOF BACKUP (NO FLOW EFFECT)", c::AMBER);
    else ui::RightNote(STATE[std::clamp(state, 0, 3)], state == 3 ? c::AMBER : c::DIM);
  }
  // NVOF is a backup for players with no flow effect: by eye in Village it looked worse than LaunchPad and ran slower. A big yellow box says so whenever it
  // is in use. Picked by hand with a flow effect loaded, its button goes back to SHADER; as the backup, it tells the player to install a flow effect.
  if (NvofBackup()) {
    ui::AlertBox("##nvofbackup", "NO FLOW EFFECT: NVOF BACKUP IN USE", "INSTALL LAUNCHPAD: IT LOOKS BETTER AND RUNS FASTER", nullptr, IM_COL32(255, 220, 0, 255));
  } else if (g_flow_engine == 1) {
    const bool effect_loaded = g_seen.source >= 0 && g_seen.source < SOURCE_COUNT;
    if (ui::AlertBox("##nvofwarn", "NVOF IS A BACKUP ONLY", "SHADER FLOW (LAUNCHPAD, LUMENITE) LOOKS BETTER AND RUNS FASTER",
                     effect_loaded ? "USE SHADER" : nullptr, IM_COL32(255, 220, 0, 255))) {
      g_flow_engine = 0;
      changed = true;
    }
  }

  // ---- LaunchPad's flow quality (HSF.ini LaunchPadQuality), only while LaunchPad gives the flow. A click does the whole tune: that quality, LaunchPad
  // first in ReShade's order, its depth settings as HSF's. A box is lit only when LaunchPad really is set that way, so a change made in ReShade's own
  // menu shows. AUTO, the default, tunes by itself and picks again when the game's size changes (KeepLaunchPadAuto). UNDO puts back what the player had before the first tune.
  {
    const bool launchpad_flow = g_flow_engine == 0 && g_seen.source != 1 && !(now.d3d12 && g_using_dlss);
    const LaunchPadState launchpad = launchpad_flow ? ReadLaunchPad(runtime) : LaunchPadState{};
    if (launchpad.loaded) {
      ui::RowLabel("LAUNCHPAD", c::BLUE);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How hard LaunchPad works on the optical flow.\nMore quality costs more GPU time, and the cost grows with resolution.\n\n"
                          "Every pick also puts LaunchPad first in ReShade's effect order\nand matches its depth settings to HSF's.");
      static const char* const QUALITY_BOXES[] = {"AUTO", "LOW", "MEDIUM", "HIGH"};
      static const char* const QUALITY_TIPS[] = {
          "The default. HSF picks the quality from the game's resolution:\nHIGH up to 1920x1200, MEDIUM up to 3440x1440, LOW above (4K).\nIt picks again if you change resolution, and keeps LaunchPad first.",
          "Fastest. Flow at low detail.\nBest at 4K or on a weaker card.\nFine motion can smear a little.",
          "Balanced. Good for most players at 1440p and 4K.",
          "Most motion detail, heaviest.\nFor 1080p, or a strong card with room to spare."};
      static const int BOX_TO_MODE[] = {hsf::launchpad::MODE_AUTO, 0, 1, 2};
      const int wanted = WantedLaunchPadQuality(runtime);
      int box = -1;
      if (LaunchPadPlaced(launchpad)) {
        if (g_launchpad_mode == hsf::launchpad::MODE_AUTO && launchpad.quality == wanted) box = 0;
        else if (launchpad.quality >= 0 && launchpad.quality <= 2) box = launchpad.quality + 1;
      }
      if (ui::Choice("##lpquality", QUALITY_BOXES, 4, &box, 0, QUALITY_TIPS)) {
        g_launchpad_mode = BOX_TO_MODE[box];
        WritePrivateProfileStringW(L"HSF", L"LaunchPadQuality", LaunchPadModeName(g_launchpad_mode), g_ini_path);
        TuneLaunchPad(runtime, launchpad, WantedLaunchPadQuality(runtime));
      }
      if (GetPrivateProfileIntW(L"LaunchPadUndo", L"Saved", 0, g_ini_path) != 0) {
        ImGui::SameLine(0.f, ImGui::GetFontSize() * 0.3f);
        if (ui::Button("##lpundo", "UNDO", c::DIM)) UndoLaunchPad(runtime);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Puts back LaunchPad's quality and depth settings from before HSF's first tune.");
      }
    }
  }

  // ---- Reflex. The game's own Streamline runs the game's Reflex: HSF only adds boost there.
  ui::RowLabel("REFLEX", c::PURPLE);
  static const char* const REFLEX[] = {"OFF", "ON", "ON+BOOST"};
  int reflex = now.drive ? (g_reflex_boost ? 2 : 1) : !g_reflex_sleep ? 0 : g_reflex_boost ? 2 : 1;
  if (ui::Choice("##reflex", REFLEX, 3, &reflex, now.drive ? 1u : 0u)) {
    if (!now.drive) g_reflex_sleep = reflex != 0;
    g_reflex_boost = reflex == 2;
    changed = true;
  }
  if (!now.d3d12 && hsf::dlssg::Running() && g_dlssg_stats.report_frames != 0) {
    char latency[48];
    std::snprintf(latency, sizeof(latency), "LATENCY %.1f MS", g_dlssg_stats.report_total_ms);
    ui::RightNote(latency);
  }

  // ---- FPS cap: frames on screen. Applied while dragging, saved when let go. The mouse wheel over the bar moves it by 1 (Shift: 10), and a click
  // on the number types it. With no cap set, a warning icon and the line under the row pulse: a still line was missed in playtesting.
  ui::RowLabel("FPS CAP", c::TEXT);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("Frames on screen per second, real + generated.\nSet it to your monitor's refresh rate.\n\nDrag the bar, use the mouse wheel over it (Shift: 10 at a time),\nor click the number to type it. %u to %u, or OFF.", FPS_CAP_MIN, FPS_CAP_MAX);
  const float font = ImGui::GetFontSize();
  int cap = static_cast<int>(g_fps_cap);
  bool released = false;
  if (ui::Bar("##cap", &cap, static_cast<int>(FPS_CAP_MIN), 360, font * 11.f, c::TEXT, &released)) g_fps_cap = CapRule(cap);
  changed |= released;
  const ImVec2 bar_min = ImGui::GetItemRectMin(), bar_max = ImGui::GetItemRectMax();
  const bool bar_hovered = ImGui::IsItemHovered();
  if (bar_hovered && !ImGui::IsItemActive()) ImGui::SetTooltip("Mouse wheel: 1 at a time, Shift: 10.\nClick the number to type a cap.");
  // The wheel: the mouse is over where the bar was last frame (this frame the window may already have scrolled away from it), and DrawPanel
  // puts the window's scroll back afterwards so only the cap moves. A plain rectangle test, not IsItemHovered: while the wheel turns, ImGui
  // holds the hover on the window that scrolls, so the bar never counts as hovered.
  static ImVec2 s_bar_min, s_bar_max;
  static bool s_bar_seen = false;
  g_cap_wheel_used = false;
  // ReShade's own input first: the ImGui wheel never reached HSF's panel in RE Village. Its delta may come in notches or in WHEEL_DELTA units.
  int16_t reshade_wheel = 0;
  uint32_t mouse_x = 0, mouse_y = 0;
  if (runtime != nullptr) runtime->get_mouse_cursor_position(&mouse_x, &mouse_y, &reshade_wheel);
  const float wheel = reshade_wheel != 0 ? (std::abs(reshade_wheel) >= WHEEL_DELTA ? static_cast<float>(reshade_wheel) / WHEEL_DELTA : static_cast<float>(reshade_wheel))
                                         : ImGui::GetIO().MouseWheel;
  const bool shift = (runtime != nullptr && runtime->is_key_down(VK_SHIFT)) || ImGui::GetIO().KeyShift;
  const ImVec2 mouse = ImGui::GetMousePos();
  // Only where the mouse is: docked in ReShade's window, the hover goes to ReShade's window while the wheel turns, not to HSF's.
  const bool over_bar = s_bar_seen && mouse.x >= s_bar_min.x && mouse.x < s_bar_max.x && mouse.y >= s_bar_min.y && mouse.y < s_bar_max.y;
  if (over_bar && wheel != 0.f && !ImGui::IsAnyItemActive()) {
    const long long step = shift ? 10 : 1;
    const long long ticks = wheel > 0.f ? std::max(1LL, static_cast<long long>(wheel + 0.5f)) : std::min(-1LL, static_cast<long long>(wheel - 0.5f));
    // Up from no cap starts at the lowest cap; down never turns the cap off.
    const uint32_t next = g_fps_cap == 0 ? (ticks > 0 ? FPS_CAP_MIN : 0u) : CapRule(std::max<long long>(FPS_CAP_MIN, static_cast<long long>(g_fps_cap) + ticks * step));
    if (next != g_fps_cap) {
      g_fps_cap = next;
      changed = true;
    }
    g_cap_wheel_used = true;
  }
  // Not on a wheel frame: the window has scrolled for it this frame and is put back next frame, so this frame's place is not where the bar stays.
  if (!g_cap_wheel_used) {
    s_bar_min = bar_min;
    s_bar_max = bar_max;
    s_bar_seen = true;
  }
  if (g_fps_cap == 0) ui::WarningIcon(ImVec2(ui::ControlX() - font * 1.6f, (bar_min.y + bar_max.y) * 0.5f), font * 1.05f, ui::Pulsing(c::AMBER));
  ImGui::SameLine(0.f, font * 0.8f);
  {
    // The number in a fixed width, so the buttons after it do not move while dragging. A click turns it into a box to type in: Enter or a click
    // elsewhere keeps what was typed, Esc keeps the old cap. 0 or nothing typed is no cap.
    static bool s_typing = false, s_focus = false;
    static char s_typed[8] = {};
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = font * 2.6f;
    if (s_typing) {
      if (s_focus) {
        ImGui::SetKeyboardFocusHere();
        s_focus = false;
      }
      ImGui::PushStyleColor(ImGuiCol_FrameBg, c::PANEL);
      ImGui::PushStyleColor(ImGuiCol_Border, c::TEXT);
      ImGui::PushStyleColor(ImGuiCol_Text, c::WHITE);
      ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.f);
      ImGui::SetNextItemWidth(width - font * 0.3f);
      ImGui::InputText("##captype", s_typed, sizeof(s_typed), ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_AutoSelectAll);
      ImGui::PopStyleVar();
      ImGui::PopStyleColor(3);
      if (ImGui::IsItemDeactivated()) {
        if (!ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
          g_fps_cap = CapRule(std::atoll(s_typed));
          changed = true;
        }
        s_typing = false;
      }
    } else {
      char value[16];
      std::snprintf(value, sizeof(value), g_fps_cap == 0 ? "---" : "%u", g_fps_cap);
      ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (ImGui::GetFrameHeight() - font * 0.9f) * 0.5f));
      ui::Caps(value, g_fps_cap == 0 ? c::FAINT : c::WHITE, 0.9f);
      ImGui::SetCursorScreenPos(at);
      if (ImGui::InvisibleButton("##capnumber", ImVec2(width - font * 0.3f, ImGui::GetFrameHeight()))) {
        if (g_fps_cap != 0) std::snprintf(s_typed, sizeof(s_typed), "%u", g_fps_cap);
        else s_typed[0] = '\0';
        s_typing = s_focus = true;
      }
      if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddLine(ImVec2(at.x, at.y + ImGui::GetFrameHeight() - 1.f), ImVec2(at.x + width - font * 0.5f, at.y + ImGui::GetFrameHeight() - 1.f), c::DIM);
        ImGui::SetTooltip("Click to type a cap (%u to %u, 0 = off).", FPS_CAP_MIN, FPS_CAP_MAX);
      }
    }
    ImGui::SameLine(0.f, 0.f);
    ImGui::SetCursorScreenPos(ImVec2(at.x + width, at.y));
  }
  if (ui::LitButton("##capoff", "OFF", g_fps_cap == 0)) {
    g_fps_cap = 0;
    changed = true;
  }
  const uint32_t monitor_hz = MainMonitorHz();
  if (monitor_hz != 0) {
    ImGui::SameLine(0.f, ImGui::GetFontSize() * 0.3f);
    char hz[24];
    std::snprintf(hz, sizeof(hz), "%u HZ", monitor_hz);
    if (ui::LitButton("##capmon", hz, g_fps_cap == monitor_hz)) {
      g_fps_cap = monitor_hz;
      changed = true;
    }
  }
  if (g_fps_cap == 0) {
    ImGui::SetCursorScreenPos(ImVec2(ui::ControlX(), ImGui::GetCursorScreenPos().y));
    ui::Caps("NO CAP SET // A CAP GIVES SMOOTHER FRAME GENERATION", ui::Pulsing(c::AMBER), 0.72f);
  } else {
    // What the cap leaves the game itself, with frame generation on: the cap counts generated frames too, so a low cap at a high multiplier
    // starves the game. Amber below 30.
    const uint32_t multiplier = !g_extra_enabled ? 1u : mod_sets_multiplier ? (older.fg_on ? older.applied : 1u) : g_dlssg_frames + 1;
    if (multiplier > 1) {
      const uint32_t game = g_fps_cap / multiplier;
      char text[96];
      if (game < 30)
        std::snprintf(text, sizeof(text), "GAME RUNS AT %u FPS (%u / %uX) // TOO LOW", game, g_fps_cap, multiplier);
      else std::snprintf(text, sizeof(text), "GAME RUNS AT %u FPS (%u / %uX)", game, g_fps_cap, multiplier);
      ImGui::SetCursorScreenPos(ImVec2(ui::ControlX(), ImGui::GetCursorScreenPos().y));
      ui::Caps(text, game < 30 ? c::AMBER : c::DIM, 0.72f);
    }
  }
  // The GPU reserve while it holds the cap lower than the player's, and its warning when even the floor leaves the card full.
  if (g_reserve.Full()) {
    ImGui::SetCursorScreenPos(ImVec2(ui::ControlX(), ImGui::GetCursorScreenPos().y));
    ui::Caps("GPU FULL AND STUTTERING // LOWER THE MULTIPLIER OR SETTINGS", ui::Pulsing(c::AMBER), 0.72f);
  } else if (g_reserve.Cap() != 0) {
    char text[96];
    std::snprintf(text, sizeof(text), "GPU RESERVE: CAP %u (STUTTER ON A FULL CARD)", g_reserve.Cap());
    ImGui::SetCursorScreenPos(ImVec2(ui::ControlX(), ImGui::GetCursorScreenPos().y));
    ui::Caps(text, c::AMBER, 0.72f);
  }

  // ---- The overlay on screen: off, simple or advanced.
  ui::RowLabel("OVERLAY", c::AMBER);
  if (ui::Choice("##overlay", OVERLAY_NAMES, 3, &g_overlay_mode)) changed = true;

  // ---- Status: the route, and a plain verdict.
  ImGui::Dummy(ImVec2(1.f, ImGui::GetFontSize() * 0.2f));
  char route[96];
  std::snprintf(route, sizeof(route), "%s%s%s", now.api, now.api[0] != '\0' ? " // " : "", now.route);
  const char* verdict = "OK";
  ImU32 verdict_colour = c::GREEN;
  const char* reason = nullptr;
  if (!g_extra_enabled) {
    verdict = "OFF";
    verdict_colour = c::DIM;
  } else if (shown.problem != nullptr) {
    verdict = "PROBLEM";
    verdict_colour = c::RED;
    reason = shown.problem;
  } else if (shown.waiting != nullptr && !shown.generating) {
    verdict = "WAITING";
    verdict_colour = c::AMBER;
    reason = shown.waiting;
  }
  ui::StatusBox(route, verdict, verdict_colour);
  if (reason != nullptr) ui::Note(reason, verdict_colour);
  {
    float frames[MENU_HISTORY];
    const size_t count = std::min(g_menu_write, MENU_HISTORY);
    for (size_t i = 0; i < count; ++i) frames[i] = g_menu_ms[(g_menu_write - count + i) % MENU_HISTORY];
    ui::Sparkline(frames, count, ImGui::GetFontSize() * 1.6f);
  }

  // ---- Advanced.
  static bool advanced = false, debug = false, details = false, overlay = false;
  if (ui::Section("##advanced", "ADVANCED", "", &advanced)) {
    ui::SettingLabel("PROTECT GAME HUD");
    bool* hud = &g_hud_enabled;
#ifdef HSF_SL_GAME
    if (now.drive) hud = &g_sl_hud;
    else if (now.bring) hud = &g_checks_enabled;
#endif
    if (ui::Toggle("##hud", hud)) changed = true;
    if (!now.d3d12) {
      ui::SettingLabel("KEEP OVERLAYS OUT");
      if (ui::Toggle("##overlays", &g_keep_overlays_out)) changed = true;
      ui::RightNote("RTSS / STEAM / DISCORD", c::FAINT);
    }
    ui::SettingLabel("HOTKEYS");
    {
      char keys[96], a[16], b[16];
      KeyName(g_mfg_key, a, sizeof(a));
      KeyName(g_view_key, b, sizeof(b));
      std::snprintf(keys, sizeof(keys), "%s MULTIPLIER%s%s%s", g_mfg_key != 0 ? a : "NONE", g_view_key != 0 && !now.d3d12 ? " // " : "",
                    g_view_key != 0 && !now.d3d12 ? b : "", g_view_key != 0 && !now.d3d12 ? " VIEW" : "");
      const float y = ImGui::GetCursorScreenPos().y;
      ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + (ImGui::GetFrameHeight() - ImGui::GetFontSize() * 0.8f) * 0.5f));
      ui::Caps(keys, c::TEXT, 0.8f);
    }
    ui::SettingLabel("GPU RESERVE");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Steps in only when frame generation starves the game:\nthe graphics card is full AND the game's own frames stutter.\n\n"
                        "Then HSF lowers the FPS cap a step at a time until the stutter stops,\nand gives the frames back once it stays smooth.\n"
                        "A full card with smooth frames costs nothing.\nNever above your own cap, never under 30 fps for the game itself.");
    if (ui::Toggle("##reserve", &g_gpu_reserve)) changed = true;
    if (g_reserve.Cap() != 0) {
      char note[32];
      std::snprintf(note, sizeof(note), "> ACTING: CAP %u", g_reserve.Cap());
      ui::RightNote(note, c::AMBER);
    } else if (g_load_reader->Failed()) {
      ui::RightNote("> NO LOAD READING", c::AMBER);
    } else if (g_gpu_reserve) {
      ui::RightNote("> WATCHING FOR STUTTER", c::FAINT);
    }
    if (g_native9 != nullptr) {
      // DX9 games: whether frames reach DLSS-G on the GPU (9Ex) or through CPU copies, and why; the 9Ex crash rule can be undone here.
      ui::SettingLabel("DX9 GPU ROUTE");
      const bool crash_now = GetPrivateProfileIntW(L"HSF", L"Dx9ExCrash", 0, g_ini_path) != 0;
      const float y = ImGui::GetCursorScreenPos().y;
      // Either way takes effect at the next launch (the device is made once), and is saved per game in HSF.ini.
      const auto set_ex = [](bool on) {
        g_dx9_ex = on;
        WritePrivateProfileStringW(L"HSF", L"Dx9Ex", on ? L"1" : L"0", g_ini_path);
        Log(on ? "DX9 GPU route: turned on from the menu, the next launch asks for 9Ex" : "DX9 GPU route: turned off from the menu, the next launch uses plain D3D9 (CPU copies)");
      };
      if (g_dx9_device_ex && !g_dx9_ex_asked) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + (ImGui::GetFrameHeight() - ImGui::GetFontSize() * 0.8f) * 0.5f));
        ui::Caps("ON (THE GAME'S OWN D3D9EX)", c::GREEN, 0.8f);
      } else if (g_dx9_device_ex && crash_now) {
        if (ui::Button("##dx9ex", "ON // GAME FAULTED WITH IT: OFF AT NEXT LAUNCH // KEEP ON", c::AMBER)) {
          WritePrivateProfileStringW(L"HSF", L"Dx9ExCrash", L"0", g_ini_path);
          Log("9Ex crash rule: cleared from the menu, the next launch stays on 9Ex");
        }
      } else if (g_dx9_device_ex && g_dx9_ex) {
        if (ui::Button("##dx9ex", "ON (D3D9EX) // TURN OFF (NEXT LAUNCH)", c::GREEN)) set_ex(false);
      } else if (g_dx9_device_ex) {
        if (ui::Button("##dx9ex", "ON // OFF AT NEXT LAUNCH // KEEP ON", c::AMBER)) set_ex(true);
      } else if (g_dx9_ex_crash && crash_now) {
        if (ui::Button("##dx9ex", "OFF: GAME CRASHED WITH IT // TRY AGAIN", c::AMBER)) {
          WritePrivateProfileStringW(L"HSF", L"Dx9ExCrash", L"0", g_ini_path);
          Log("9Ex crash rule: cleared from the menu, the next launch tries 9Ex again");
        }
      } else if (g_dx9_ex_crash) {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + (ImGui::GetFrameHeight() - ImGui::GetFontSize() * 0.8f) * 0.5f));
        ui::Caps("OFF // TRIES 9EX AT NEXT LAUNCH", c::TEXT, 0.8f);
      } else if (!g_dx9_ex) {
        if (ui::Button("##dx9ex", "OFF // CPU COPIES // TURN ON (NEXT LAUNCH)", c::TEXT)) set_ex(true);
      } else if (!g_dx9_ex_at_launch) {
        if (ui::Button("##dx9ex", "OFF // ON AT NEXT LAUNCH // KEEP OFF", c::AMBER)) set_ex(false);
      } else {
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + (ImGui::GetFrameHeight() - ImGui::GetFontSize() * 0.8f) * 0.5f));
        ui::Caps("OFF // CPU COPIES", c::TEXT, 0.8f);
      }
    }
    // A game that chose windowed mode keeps its window unless the player asks for borderless fullscreen here (saved per game, live both ways;
    // WINDOWED puts back the game's own style and size).
    ui::SettingLabel("WINDOW");
    if (g_borderless_window != nullptr && g_borderless) {
      ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + (ImGui::GetFrameHeight() - ImGui::GetFontSize() * 0.8f) * 0.5f));
      ui::Caps("BORDERLESS FULLSCREEN (THE GAME ASKED FOR FULLSCREEN)", c::GREEN, 0.8f);
    } else {
      static const char* const WINDOW_BOXES[] = {"WINDOWED", "BORDERLESS"};
      static const char* const WINDOW_TIPS[] = {"The game's own window, as the game made it.",
                                                "Borderless fullscreen over the game's monitor, live, no restart.\nFor games running in a window. Some games crash when their window changes size."};
      int box = g_force_borderless ? 1 : 0;
      if (ui::Choice("##winmode", WINDOW_BOXES, 2, &box, 0, WINDOW_TIPS)) {
        g_force_borderless = box == 1;
        WritePrivateProfileStringW(L"HSF", L"ForceBorderless", g_force_borderless ? L"1" : L"0", g_ini_path);
        if (g_force_borderless && g_borderless_crash) {
          g_borderless_crash = false;
          WritePrivateProfileStringW(L"HSF", L"BorderlessCrash", L"0", g_ini_path);
        }
        Log(g_force_borderless ? "window: borderless fullscreen asked from the menu (ForceBorderless=1); if the game crashes when its window changes size, set it back"
                               : "window: back to the game's own windowed mode, from the menu (ForceBorderless=0)");
      }
      if (g_borderless_crash && !g_force_borderless) {
        ui::RightNote("> GAME CRASHED ON RESIZE: KEPT WINDOWED", c::AMBER);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("This game crashed right after HSF made its window borderless,\nso HSF turned borderless off to let the game start again.\n\n"
                            "Tip: set the game's own resolution to your monitor's size;\nthen borderless has nothing to resize.");
      }
    }
    ui::SettingLabel("SETTINGS");
    static ULONGLONG armed = 0;
    const ULONGLONG tick = GetTickCount64();
    const bool asking = armed != 0 && tick - armed < 3000;
    if (ui::Button("##reset", asking ? "CLICK AGAIN TO RESET" : "RESET TO DEFAULTS", c::RED)) {
      if (asking) {
        ResetMenuSettings();
        armed = 0;
      } else {
        armed = tick;
      }
    }
  }

  // ---- Overlay: size, opacity, place.
  if (ui::Section("##overlaylook", "OVERLAY", "// SIZE, OPACITY, POSITION", &overlay)) changed |= DrawOverlaySettings();

  // ---- Debug, for testers.
  if (ui::Section("##debug", "DEBUG", "// FOR TESTERS", &debug)) {
    ui::SettingLabel("VIEW");
#ifdef HSF_SL_GAME
    // In-game route on DLSS: the game's vectors and depth go to its own Streamline mid-frame and HSF has no copy at present, so the views and HSF's depth settings
    // have nothing to work on. Crossed out, and the view goes back to GAME, until motion is on flow.
    const bool dlss_only = now.drive && g_using_dlss;
    if (dlss_only) g_bring_view = 0;
#else
    const bool dlss_only = false;
#endif
#ifdef HSF_SL_GAME
    if (now.d3d12) {
      static const char* const VIEWS[] = {"GAME", "DEPTH", "MOTION"};
      ui::Choice("##view", VIEWS, 3, &g_bring_view, dlss_only ? 6u : 0u);
      if (dlss_only) ui::RightNote("FLOW ONLY", c::FAINT);
    } else
#endif
    {
      static const char* const VIEWS[] = {"OFF", "MOTION", "ERROR", "HELPED", "DEPTH"};
      int view = static_cast<int>(g_score_view);
      if (ui::Choice("##view", VIEWS, 5, &view)) g_score_view = static_cast<hsf::score::View>(view);
    }
    ui::SettingLabel("DEPTH DIRECTION");
    static const char* const DIRECTION[] = {"AUTO", "NORMAL", "REVERSED"};
    int direction = g_depth_inverted_setting + 1;
    if (ui::Choice("##depthdir", DIRECTION, 3, &direction, dlss_only ? 7u : 0u)) {
      g_depth_inverted_setting = direction - 1;
      changed = true;
    }
    ui::SettingLabel("DEPTH WAY UP");
    static const char* const WAY_UP[] = {"AUTO", "AS GAME", "FLIPPED"};
    int way_up = g_depth_upside_down_setting + 1;
    if (ui::Choice("##depthup", WAY_UP, 3, &way_up, dlss_only ? 7u : 0u)) {
      g_depth_upside_down_setting = way_up - 1;
      changed = true;
    }
    if (!now.d3d12) {
      ui::SettingLabel("SEND DEPTH");
      if (ui::Toggle("##depthon", &g_depth_enabled)) changed = true;
    }
#ifdef HSF_SL_GAME
    const bool flow_in_use = !now.drive || !g_using_dlss;
#else
    const bool flow_in_use = true;
#endif
    if (flow_in_use) {
      ui::SettingLabel("CLEAN FLOW (DEPTH)");
      if (ui::Toggle("##compose", &g_compose_enabled)) changed = true;
    }
    if (!now.d3d12) {
      ui::SettingLabel("FRAME CHECKS");
      if (ui::Toggle("##checks", &g_checks_enabled)) changed = true;
      ui::RightNote("CUTS / HUD / STILLS", c::FAINT);
      ui::SettingLabel("CAMERA MOTION");
      if (ui::Toggle("##camera", &g_camera_enabled)) changed = true;
      ui::SettingLabel("CAMERA FIT");
      if (ui::Toggle("##camfit", &g_camera_fit)) changed = true;
      ui::RightNote("CAN SHAKE", c::FAINT);
      ui::SettingLabel("CAMERA RECORD");
      char rec[48], key[16];
      KeyName(g_record_key, key, sizeof(key));
      if (g_record_file.IsOpen()) std::snprintf(rec, sizeof(rec), "STOP (%u FRAMES)", g_record_file.Frames());
      else std::snprintf(rec, sizeof(rec), "%s%sRECORD 30 S", g_record_key != 0 ? key : "", g_record_key != 0 ? " // " : "");
      if (ui::Button("##record", rec, g_record_file.IsOpen() ? c::RED : c::TEXT)) g_record_request = true;
    }
#ifdef HSF_SL_GAME
    if (now.drive) {
      ui::SettingLabel("REBUILD CAMERA");
      if (ui::Toggle("##rebuild", &g_sl_camera_rebuild)) changed = true;
      ui::SettingLabel("CUT CHECK");
      if (ui::Toggle("##cut", &g_sl_cut_check)) changed = true;
      ui::SettingLabel("QUICK OFF");
      if (ui::Toggle("##quick", &g_sl_quick_off)) changed = true;
      ui::SettingLabel("SCORE GUARD");
      if (ui::Toggle("##guard", &g_sl_score_guard)) changed = true;
    }
    if (now.bring) {
      ui::Rule();
      ui::Caps("STREAMLINE // APPLIES AFTER A GAME RESTART", c::DIM, 0.8f);
      StartupToggle("SL LOG", L"SlLog", 0);
      StartupToggle("SL FACTORY PROXY", L"SlFactoryProxy", 1);
      StartupToggle("SL MANUAL HOOKING", L"SlManualHooking", 0);
      StartupToggle("SL EARLY LOAD", L"SlEarlyLoad", 1);
      StartupToggle("NGX INPUT", L"NgxInput", 1);
    }
#endif
    ui::Rule();
    static ULONGLONG copied = 0;
    if (ui::Button("##copylog", "COPY LOG TO CLIPBOARD", c::TEXT)) {
      CopyLog(now);
      copied = GetTickCount64();
    }
    if (copied != 0 && GetTickCount64() - copied < 2500) ui::RightNote("COPIED", c::GREEN);
    if (ui::Section("##details", "ALL DETAILS", "// EVERY NUMBER", &details)) {
      ImGui::PushStyleColor(ImGuiCol_Text, c::TEXT);
      DrawDetails(runtime);
      ImGui::PopStyleColor();
    }
  }
  ui::Rule();
  ui::Caps("HSF V1 // BY CAPT-HUNTER", c::FAINT, 0.75f);
  ui::EndPanel();
  // The wheel moved the FPS cap: undo the scroll ImGui gave the window for it. Otherwise remember where the window is scrolled to.
  static float s_scroll = 0.f;
  if (g_cap_wheel_used) ImGui::SetScrollY(s_scroll);
  else s_scroll = ImGui::GetScrollY();
  if (changed) SaveIni();
}

// The OVERLAY section of the menu: the size, panel opacity and place of the overlay on screen (simple and advanced keep their own). Its
// OFF / SIMPLE / ADVANCED is the same setting as the OVERLAY row above, so what is edited is always what is shown. Changes show live while
// dragging; true when something should be saved (a bar let go, a choice or reset clicked).
bool DrawOverlaySettings() {
  namespace ui = hsf::ui;
  namespace c = hsf::ui::colour;
  bool changed = false;
  ui::SettingLabel("OVERLAY");
  if (ui::Choice("##omode", OVERLAY_NAMES, 3, &g_overlay_mode)) changed = true;
  if (g_overlay_mode == 0) {
    ImGui::SetCursorScreenPos(ImVec2(ui::ControlX(), ImGui::GetCursorScreenPos().y));
    ui::Caps("TURN AN OVERLAY ON TO CHANGE HOW IT LOOKS", c::AMBER, 0.72f);
    return changed;
  }
  const int editing = g_overlay_mode == 2 ? 1 : 0;
  OverlayLook& look = g_overlay_look[editing];

  // A bar with its number after it.
  const auto slider = [&](const char* label, const char* id, int* value, int min, int max, const char* format) {
    ui::SettingLabel(label);
    bool released = false;
    ui::Bar(id, value, min, max, ImGui::GetFontSize() * 14.f, c::TEXT, &released);
    changed |= released;
    ImGui::SameLine(0.f, ImGui::GetFontSize() * 0.8f);
    char text[16];
    std::snprintf(text, sizeof(text), format, *value);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + (ImGui::GetFrameHeight() - ImGui::GetFontSize() * 0.9f) * 0.5f));
    ui::Caps(text, c::WHITE, 0.9f);
  };
  slider("SIZE", "##osize", &look.size, 50, 200, "%d %%");
  slider("OPACITY", "##oopacity", &look.opacity, 0, 100, "%d %%");
  ui::SettingLabel("CORNER");
  if (ui::Choice("##ocorner", CORNER_NAMES, 4, &look.corner)) changed = true;
  slider("X OFFSET", "##ox", &look.x, 0, 1000, "%d PX");
  slider("Y OFFSET", "##oy", &look.y, 0, 1000, "%d PX");
  ui::SettingLabel("RESET");
  if (ui::Button("##oreset", "RESET THIS OVERLAY", c::RED)) {
    look = OVERLAY_DEFAULT[editing];
    changed = true;
  }
  ImGui::SetCursorScreenPos(ImVec2(ui::ControlX(), ImGui::GetCursorScreenPos().y));
  ui::Caps("OFFSETS IN PIXELS AT 1440P // SCALED WITH YOUR RESOLUTION", c::FAINT, 0.72f);
  return changed;
}

float Percentile(std::vector<float> values, float fraction) {
  if (values.empty()) return 0.f;
  const size_t i = std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<float>(values.size())));
  std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(i), values.end());
  return values[i];
}

// ---- DLSS-G overlay ----
// Big enough to read in a video. The numbers are seven-segment shapes, so they stay sharp at any size (ReShade's font is a small bitmap that
// blurs when scaled up); only the labels use the font. Drawn into the game's frame like the rest of the OSD, so DLSS-G generates it too.
// Segments a-g as bits 0-6 (a top, b top right, c bottom right, d bottom, e bottom left, f top left, g middle).
constexpr uint8_t DIGIT_SEGMENTS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

// Draws `text` (digits, '.', 'x' and spaces) `h` high from `at`; returns its width. With `draw` null it only measures.
float SegmentText(ImDrawList* draw, ImVec2 at, float h, const char* text, ImU32 colour) {
  const float w = h * 0.55f, t = h * 0.12f, gap = t * 0.2f, mid = h * 0.5f;
  float x = at.x;
  for (const char* c = text; *c != '\0'; ++c) {
    if (*c == '.') {
      if (draw) draw->AddRectFilled(ImVec2(x, at.y + h - t), ImVec2(x + t, at.y + h), colour);
      x += t * 2.f;
      continue;
    }
    if (*c == 'x') {
      if (draw) {
        draw->AddLine(ImVec2(x, at.y + mid - t), ImVec2(x + w * 0.8f, at.y + h), colour, t);
        draw->AddLine(ImVec2(x + w * 0.8f, at.y + mid - t), ImVec2(x, at.y + h), colour, t);
      }
      x += w * 0.8f + t * 1.5f;
      continue;
    }
    if (*c < '0' || *c > '9') {
      x += w * 0.6f;
      continue;
    }
    const uint8_t s = DIGIT_SEGMENTS[*c - '0'];
    if (draw) {
      const float y = at.y;
      auto bar = [&](bool on, float x0, float y0, float x1, float y1) {
        if (on) draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), colour);
      };
      bar(s & 0x01, x + t + gap, y, x + w - t - gap, y + t);                                   // a
      bar(s & 0x02, x + w - t, y + t + gap, x + w, y + mid - gap);                             // b
      bar(s & 0x04, x + w - t, y + mid + gap, x + w, y + h - t - gap);                         // c
      bar(s & 0x08, x + t + gap, y + h - t, x + w - t - gap, y + h);                           // d
      bar(s & 0x10, x, y + mid + gap, x + t, y + h - t - gap);                                 // e
      bar(s & 0x20, x, y + t + gap, x + t, y + mid - gap);                                     // f
      bar(s & 0x40, x + t + gap, y + mid - t * 0.5f, x + w - t - gap, y + mid + t * 0.5f);     // g
    }
    x += w + t * 1.2f;
  }
  return x - at.x;
}

// Font text `size` high with a dark shadow; returns its width.
float Label(ImDrawList* draw, ImVec2 at, float size, const char* text, ImU32 colour) {
  ImFont* const font = ImGui::GetFont();
  const float shadow = std::max(1.f, size * 0.06f);
  draw->AddText(font, size, ImVec2(at.x + shadow, at.y + shadow), IM_COL32(0, 0, 0, 200), text);
  draw->AddText(font, size, at, colour, text);
  return size / ImGui::GetFontSize() * ImGui::CalcTextSize(text).x;
}

// The game's frame times, oldest first (at most FRAME_HISTORY).
size_t FrameHistory(float* out) {
  const size_t count = std::min(g_frame_write, FRAME_HISTORY);
  for (size_t i = 0; i < count; ++i) out[i] = g_frame_ms[(g_frame_write - count + i) % FRAME_HISTORY];
  return count;
}

// ---- The overlay ----
// OFF, SIMPLE or ADVANCED (HSF.ini Overlay=0/1/2), in the menu's look (ui.h). Simple: frames out and in, latency, frame time, a pacing verdict and
// the frame graph. Advanced adds what HSF knows about generation, Reflex, the FPS cap and latency's parts. Drawn into the game's frame like the
// rest of ReShade's OSD, so DLSS-G generates it too. Only numbers the route really has: the D3D12 routes have no latency figure, so none is shown.

// Text in spaced capitals, `size` high, at `at`; returns its width. With `draw` null it only measures.
float Spaced(ImDrawList* draw, ImVec2 at, float size, const char* text, ImU32 colour) {
  ImFont* const font = ImGui::GetFont();
  const float scale = size / ImGui::GetFontSize(), gap = size * 0.07f;
  float x = at.x;
  for (const char* ch = text; *ch != '\0'; ++ch) {
    const char one[2] = {*ch, '\0'};
    if (draw) draw->AddText(font, size, ImVec2(x, at.y), colour, one);
    x += ImGui::CalcTextSize(one).x * scale + gap;
  }
  return std::max(0.f, x - at.x - gap);
}

// How even the game's frames are: a hitch is a frame over twice the median.
struct PaceVerdict {
  float median = 0.f, p1 = 0.f, p99 = 0.f, low_fps = 0.f;
  uint32_t hitches = 0;
  const char* verdict = "---";
  ImU32 colour = hsf::ui::colour::DIM;
};

PaceVerdict ReadPace(const float* frames, size_t count) {
  namespace c = hsf::ui::colour;
  PaceVerdict p;
  if (count < 10) return p;
  const std::vector<float> values(frames, frames + count);
  p.median = Percentile(values, 0.5f);
  p.p1 = Percentile(values, 0.01f);
  p.p99 = Percentile(values, 0.99f);
  p.low_fps = p.p99 > 0.f ? 1000.f / p.p99 : 0.f;
  for (float ms : values) p.hitches += ms > p.median * 2.f ? 1 : 0;
  if (p.hitches != 0) {
    p.verdict = "HITCHING";
    p.colour = c::RED;
  } else if (p.p99 > p.median * 1.3f) {
    p.verdict = "UNEVEN";
    p.colour = c::AMBER;
  } else {
    p.verdict = "SMOOTH";
    p.colour = c::GREEN;
  }
  return p;
}

// Latency in ms where the route measures it (the D3D11 helper: the driver's Reflex report, else HSF's timer), else -1.
float OverlayLatency() {
  if (!hsf::dlssg::Running()) return -1.f;
  const hsf::dlssg::Stats& d = g_dlssg_stats;
  if (d.report_frames != 0) return d.report_total_ms;
  return d.latency_ms > 0.f ? d.latency_ms : -1.f;
}

// Frame graph, drawn as RTSS draws its own: the game's frame times as a thin line (red into a hitch, a frame over twice the median), each frame's
// latency as a green line where the route measures it, and the FPS cap's frame time for the game as a dashed amber line. One scale in ms, written
// at the top left. `slots` frames fit across.
void FrameGraph(ImDrawList* draw, ImVec2 at, float width, float height, float text_size, const float* frames, size_t frame_count, size_t slots,
                const float* latency, size_t latency_count, float target_ms) {
  namespace c = hsf::ui::colour;
  draw->AddLine(ImVec2(at.x, at.y + height), ImVec2(at.x + width, at.y + height), c::LINE, std::max(1.f, height / 60.f));
  const float median = Percentile(std::vector<float>(frames, frames + frame_count), 0.5f);
  float top = std::max({median * 2.5f, target_ms * 1.5f, 1.f});
  for (size_t i = 0; i < latency_count; ++i) top = std::max(top, std::min(latency[i] * 1.15f, median * 8.f + 1.f));
  const float step = width / static_cast<float>(slots);
  const auto y_of = [&](float ms) { return at.y + height - std::min(ms / top, 1.f) * height; };
  // The frame times as one line, newest at the right; the part into and out of a hitch red.
  const float x0 = at.x + width - step * static_cast<float>(frame_count), thick = std::max(1.5f, height / 40.f);
  for (size_t i = 1; i < frame_count; ++i) {
    const bool hitch = frames[i] > median * 2.f || frames[i - 1] > median * 2.f;
    draw->AddLine(ImVec2(x0 + step * static_cast<float>(i - 1), y_of(frames[i - 1])), ImVec2(x0 + step * static_cast<float>(i), y_of(frames[i])),
                  hitch ? c::RED : IM_COL32(220, 226, 218, 230), thick);
  }
  if (target_ms > 0.f) {
    const float y = y_of(target_ms), dash = width / 60.f;
    for (float dx = 0; dx < width; dx += dash * 2.f)
      draw->AddLine(ImVec2(at.x + dx, y), ImVec2(at.x + std::min(dx + dash, width), y), c::AMBER, std::max(1.f, height / 60.f));
  }
  if (latency_count >= 2) {
    // One latency sample per real frame: spread at the same step, newest at the right.
    const size_t n = std::min(latency_count, slots);
    std::vector<ImVec2> points(n);
    for (size_t i = 0; i < n; ++i)
      points[i] = ImVec2(at.x + width - step * static_cast<float>(n - 1 - i), y_of(latency[latency_count - n + i]));
    draw->AddPolyline(points.data(), static_cast<int>(n), c::GREEN, ImDrawFlags_None, std::max(1.5f, height / 45.f));
  }
  char scale[32];
  std::snprintf(scale, sizeof(scale), "%.0f MS", top);
  Spaced(draw, ImVec2(at.x + text_size * 0.3f, at.y + text_size * 0.15f), text_size * 0.7f, scale, c::DIM);
}

void DrawOverlay(bool advanced) {
  namespace c = hsf::ui::colour;
  const MenuView v = ReadMenuView();
  ImDrawList* const draw = ImGui::GetForegroundDrawList();
  const OverlayLook& look = g_overlay_look[advanced ? 1 : 0];
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  const float frame_unit = std::max(screen.y, 480.f) / 1440.f;  // sized for the frame's height (1440 lines = 1)
  const float unit = frame_unit * static_cast<float>(look.size) / 100.f;
  const float text = 21.f * unit, row = text * 1.5f, pad = 14.f * unit, big = (advanced ? 62.f : 56.f) * unit;
  // At least this wide, wider when last frame's rows needed more.
  static float needed[2] = {};
  const float gap = text * 1.5f;
  const float inner = std::max((advanced ? 600.f : 300.f) * unit, needed[advanced ? 1 : 0]);
  float need = 0.f;

  // The game's frames, as ReShade presents them (every route), and how even they are.
  float frames[MENU_HISTORY];
  const size_t count = std::min(g_menu_write, MENU_HISTORY);
  for (size_t i = 0; i < count; ++i) frames[i] = g_menu_ms[(g_menu_write - count + i) % MENU_HISTORY];
  const PaceVerdict pace = ReadPace(frames, count);
  const float latency = OverlayLatency();
  const uint32_t per_real = v.generating ? g_dlssg_frames + 1 : 1;
  const float target_ms = g_fps_cap != 0 ? 1000.f * static_cast<float>(per_real) / static_cast<float>(g_fps_cap) : 0.f;

  const char* tag = "WAITING";
  ImU32 tag_colour = c::AMBER;
  if (!g_extra_enabled) {
    tag = "OFF";
    tag_colour = c::DIM;
  } else if (v.problem != nullptr) {
    tag = "PROBLEM";
    tag_colour = c::RED;
  } else if (v.generating) {
    tag = advanced ? "GENERATING" : "ON";
    tag_colour = c::GREEN;
  }

  // The panel goes behind the contents, so it is drawn first, at the size the contents took last frame (ReShade's ImGui has no draw list
  // channels to draw it afterwards underneath).
  // Placed from its corner with last frame's size too.
  static ImVec2 panel_size[2] = {};
  ImVec2& panel = panel_size[advanced ? 1 : 0];
  const float off_x = static_cast<float>(look.x) * frame_unit, off_y = static_cast<float>(look.y) * frame_unit;
  const bool right_side = look.corner == 1 || look.corner == 3, bottom = look.corner >= 2;
  const ImVec2 at(std::max(right_side ? screen.x - panel.x - off_x : off_x, 0.f), std::max(bottom ? screen.y - panel.y - off_y : off_y, 0.f));
  ImVec2 end(at.x + panel.x, at.y + panel.y);
  if (panel.x > 0.f) {
    // The simple one see-through by default, as a typical RTSS overlay; the advanced one near solid, for a lot of small text.
    const int alpha = look.opacity * 255 / 100;
    draw->AddRectFilled(at, end, advanced ? IM_COL32(20, 28, 23, alpha) : IM_COL32(12, 18, 14, alpha));
    if (advanced && look.opacity > 0) draw->AddRect(at, end, IM_COL32(74, 90, 79, std::min(alpha, 220)));
    const float b = 16.f * unit, t = std::max(2.f, 2.5f * unit);
    draw->AddRectFilled(at, ImVec2(at.x + b, at.y + t), c::BRACKET);
    draw->AddRectFilled(at, ImVec2(at.x + t, at.y + b), c::BRACKET);
  }
  const float x = at.x + pad, right = x + inner;
  float y = at.y + pad;
  const auto rule = [&]() {
    draw->AddLine(ImVec2(x, y), ImVec2(right, y), c::LINE, std::max(1.f, unit));
    y += pad * 0.6f;
  };
  const auto right_text = [&](float edge, float yy, float size, const char* t, ImU32 colour) {
    Spaced(draw, ImVec2(edge - Spaced(nullptr, ImVec2(0, 0), size, t, colour), yy), size, t, colour);
  };
  // A row: the label at `from`, the value at the right of a column `w` wide, `extra` dim after the value.
  const auto line = [&](float from, float w, float& yy, const char* label, ImU32 label_colour, const char* value, ImU32 value_colour,
                        const char* extra = nullptr) {
    float used = Spaced(draw, ImVec2(from, yy), text, label, label_colour) + text * 1.2f;
    float edge = from + w;
    if (extra != nullptr && extra[0] != '\0') {
      const float ew = Spaced(nullptr, ImVec2(0, 0), text * 0.85f, extra, c::DIM);
      Spaced(draw, ImVec2(edge - ew, yy + text * 0.1f), text * 0.85f, extra, c::DIM);
      edge -= ew + text * 0.5f;
      used += ew + text * 0.5f;
    }
    used += Spaced(nullptr, ImVec2(0, 0), text, value, value_colour);
    right_text(edge, yy, text, value, value_colour);
    need = std::max(need, advanced ? used * 2.f + gap : used);
    yy += row;
  };

  // ---- Header.
  char head[128];
  if (advanced)
    std::snprintf(head, sizeof(head), "HSF // %s%s%s", GpuName(), v.api[0] != '\0' ? " // " : "", v.api);
  else if (v.generating)
    std::snprintf(head, sizeof(head), "HSF // DLSS-G %uX", per_real);
  else
    std::snprintf(head, sizeof(head), "HSF // FRAME GEN");
  float head_used = Spaced(draw, ImVec2(x, y), text * 0.9f, head, c::DIM) + gap;
  if (advanced) {
    if (g_seen.width != 0) {
      char size[32];
      std::snprintf(size, sizeof(size), "%uX%u", g_seen.width, g_seen.height);
      right_text(right, y, text * 0.9f, size, c::DIM);
      head_used += Spaced(nullptr, ImVec2(0, 0), text * 0.9f, size, c::DIM);
    }
  } else {
    right_text(right, y, text * 0.9f, tag, tag_colour);
    head_used += Spaced(nullptr, ImVec2(0, 0), text * 0.9f, tag, tag_colour);
  }
  need = std::max(need, head_used);
  y += row;
  rule();

  // ---- The big number: frames out, base under it.
  char number[16], line2[64];
  std::snprintf(number, sizeof(number), "%.0f", v.out_fps);
  const float number_width = SegmentText(draw, ImVec2(x, y), big, number, v.generating ? c::GREEN : c::TEXT);
  const float lx = x + number_width + text * 0.7f;
  if (advanced)
    std::snprintf(line2, sizeof(line2), "%.1f BASE // %.2fX", v.base_fps, v.base_fps > 0.f ? v.out_fps / v.base_fps : 0.f);
  else
    std::snprintf(line2, sizeof(line2), "BASE %.0f", v.base_fps);
  Spaced(draw, ImVec2(lx, y + big - text * 2.35f), text, advanced ? "FPS OUT" : "FPS", c::TEXT);
  Spaced(draw, ImVec2(lx, y + big - text * 1.05f), text * 0.9f, line2, c::DIM);
  if (advanced) {
    // The verdict in an outlined box at the right.
    const float tw = Spaced(nullptr, ImVec2(0, 0), text * 0.9f, tag, tag_colour), bx = right - tw - text * 0.8f, by = y + big * 0.5f - text * 0.75f;
    draw->AddRect(ImVec2(bx, by), ImVec2(right, by + text * 1.5f), tag_colour, 0.f, ImDrawFlags_None, std::max(1.f, unit * 1.5f));
    Spaced(draw, ImVec2(bx + text * 0.4f, by + text * 0.3f), text * 0.9f, tag, tag_colour);
  }
  y += big + pad;

  char value[64], extra[64];
  if (!advanced) {
    if (latency >= 0.f) {
      std::snprintf(value, sizeof(value), "%.1f MS", latency);
      line(x, inner, y, "LATENCY", c::BLUE, value, c::TEXT);
    }
    std::snprintf(value, sizeof(value), "%.1f MS", pace.median);
    std::snprintf(extra, sizeof(extra), "P99 %.1f", pace.p99);
    line(x, inner, y, "FRAME TIME", c::PURPLE, value, c::TEXT, extra);
    line(x, inner, y, "PACING", c::AMBER, pace.verdict, pace.colour);
  } else {
    if (v.problem != nullptr || (v.waiting != nullptr && !v.generating && g_extra_enabled)) {
      std::string why(v.problem != nullptr ? v.problem : v.waiting);
      for (char& ch : why) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      // Wrapped at word breaks to the overlay's width: one long line ran out of the box (Returnal).
      std::string line_text;
      size_t from = 0;
      while (from < why.size()) {
        size_t stop = why.find(' ', from);
        if (stop == std::string::npos) stop = why.size();
        const std::string word = why.substr(from, stop - from);
        const std::string tried = line_text.empty() ? word : line_text + " " + word;
        if (!line_text.empty() && Spaced(nullptr, ImVec2(x, y), text * 0.8f, tried.c_str(), 0) > inner) {
          Spaced(draw, ImVec2(x, y), text * 0.8f, line_text.c_str(), v.problem != nullptr ? c::RED : c::AMBER);
          y += row;
          line_text = word;
        } else {
          line_text = tried;
        }
        from = stop + 1;
      }
      if (!line_text.empty()) {
        Spaced(draw, ImVec2(x, y), text * 0.8f, line_text.c_str(), v.problem != nullptr ? c::RED : c::AMBER);
        y += row;
      }
    }
    const float w = (inner - gap) * 0.5f, x2 = x + w + gap;
    float y1 = y, y2 = y;
    const auto section = [&](float from, float& yy, const char* title) {
      yy += pad * 0.3f;
      draw->AddLine(ImVec2(from, yy), ImVec2(from + w, yy), c::LINE, std::max(1.f, unit));
      yy += pad * 0.4f;
      Spaced(draw, ImVec2(from, yy), text * 0.8f, title, c::FAINT);
      yy += row * 0.85f;
    };

    // ---- Left: generation, Reflex, FPS cap.
    section(x, y1, "GENERATION");
    if (v.generating) std::snprintf(value, sizeof(value), "DLSS-G %uX", per_real);
    else std::snprintf(value, sizeof(value), "%s", g_extra_enabled ? "NOT GENERATING" : "OFF");
    extra[0] = '\0';
    if (v.max_frames != 0) std::snprintf(extra, sizeof(extra), "MAX %uX", v.max_frames + 1);
    line(x, w, y1, "MODE", c::DIM, value, v.generating ? c::TEXT : c::AMBER, extra);
    const char* route = v.route;
    if (std::strncmp(route, "IN GAME // ", 11) == 0) route += 11;
    line(x, w, y1, "ROUTE", c::DIM, route, c::TEXT);
    bool game_data = false;
#ifdef HSF_SL_GAME
    game_data = g_using_dlss;
#endif
    line(x, w, y1, "MOTION", c::DIM, game_data ? (!v.d3d12 && LumaLoaded() ? "LUMA (DLSS)" : "GAME DATA") : "OPTICAL FLOW", c::TEXT);
    if (!v.d3d12) line(x, w, y1, "DEPTH", c::DIM, DepthSent() ? "FOUND" : "NONE (FLAT)", DepthSent() ? c::TEXT : c::AMBER);
    bool hud = g_hud_enabled;
#ifdef HSF_SL_GAME
    if (v.drive) hud = g_sl_hud;
    else if (v.bring) hud = g_checks_enabled;
#endif
    line(x, w, y1, "HUD", c::DIM, hud ? "PROTECTED" : "NOT PROTECTED", hud ? c::TEXT : c::DIM);
    const OlderCardMod& older = OlderCardModNow();
    if (older.live && older.family != 5) {
      if (older.safe_max != 0) std::snprintf(value, sizeof(value), "UP TO %uX", older.safe_max);
      else std::snprintf(value, sizeof(value), "LOADED");
      line(x, w, y1, "MFG-UNLOCKER", c::DIM, value, c::AMBER);
    } else {
      line(x, w, y1, "MFG-UNLOCKER", c::DIM, "NOT IN USE", c::DIM);
    }

    section(x, y1, "REFLEX");
    const char* reflex_mode = v.drive ? (g_reflex_boost ? "GAME + BOOST" : "GAME") : !g_reflex_sleep ? "OFF" : g_reflex_boost ? "ON + BOOST" : "ON";
    line(x, w, y1, "MODE", c::DIM, reflex_mode, c::TEXT);
    if (hsf::dlssg::Running()) {
      const hsf::dlssg::Stats& d = g_dlssg_stats;
      const char* verdict = "WORKING";
      ImU32 verdict_colour = c::GREEN;
      if (!d.reflex_available) {
        verdict = "NOT AVAILABLE";
        verdict_colour = c::RED;
      } else if (!d.reflex_sleep_on) {
        verdict = "SLEEP OFF";
        verdict_colour = c::AMBER;
      } else if (!d.reflex_game_paced) {
        verdict = "NOT PACING";
        verdict_colour = c::RED;
      } else if (d.report_frames == 0) {
        verdict = "NO DRIVER REPORT";
        verdict_colour = c::AMBER;
      }
      line(x, w, y1, "STATUS", c::DIM, verdict, verdict_colour);
      std::snprintf(value, sizeof(value), "%.2f MS", d.reflex_sleep_ms);
      line(x, w, y1, "SLEEP", c::DIM, value, c::TEXT, "PER FRAME");
    }

    section(x, y1, "FPS CAP");
    if (g_fps_cap != 0) {
      std::snprintf(value, sizeof(value), "%u", g_fps_cap);
      std::snprintf(extra, sizeof(extra), "BASE %.1f", static_cast<float>(g_fps_cap) / static_cast<float>(per_real));
      line(x, w, y1, "CAP", c::DIM, value, c::TEXT, extra);
    } else {
      line(x, w, y1, "CAP", c::DIM, "OFF", c::AMBER);
    }

    // ---- Right: latency and its parts, the game's frames.
    if (latency >= 0.f) {
      std::snprintf(value, sizeof(value), "LATENCY %.1f MS", latency);
      section(x2, y2, value);
      const hsf::dlssg::Stats& d = g_dlssg_stats;
      if (d.report_frames != 0) {
        const auto part = [&](const char* label, float ms) {
          std::snprintf(value, sizeof(value), "%.1f", ms);
          line(x2, w, y2, label, c::DIM, value, c::TEXT);
        };
        part("GAME", d.report_game_ms);
        part("HAND-OFF", d.report_submit_ms);
        part("WAIT", d.report_handoff_ms);
        part("PRESENT", d.report_present_ms);
        if (d.report_gpu_ms > 0.f) part("GPU", d.report_gpu_ms);
      }
      std::snprintf(value, sizeof(value), "%.1f", d.latency_ms);
      std::snprintf(extra, sizeof(extra), "WORST %.1f", d.latency_max_ms);
      line(x2, w, y2, "HSF TIMER", c::DIM, value, c::TEXT, extra);
    } else {
      section(x2, y2, "LATENCY");
      Spaced(draw, ImVec2(x2, y2), text, "NOT MEASURED HERE", c::DIM);
      y2 += row;
    }
    section(x2, y2, "BASE FRAMES");
    std::snprintf(value, sizeof(value), "%.1f MS", pace.median);
    line(x2, w, y2, "FRAME TIME", c::DIM, value, c::TEXT);
    std::snprintf(value, sizeof(value), "%.1f / %.1f MS", pace.p1, pace.p99);
    line(x2, w, y2, "P1 / P99", c::DIM, value, c::TEXT);
    std::snprintf(value, sizeof(value), "%.0f FPS", pace.low_fps);
    line(x2, w, y2, "1% LOW", c::DIM, value, c::TEXT);
    std::snprintf(value, sizeof(value), "%u", pace.hitches);
    std::snprintf(extra, sizeof(extra), "LAST %zu", count);
    line(x2, w, y2, "HITCHES", c::DIM, value, pace.hitches != 0 ? c::RED : c::TEXT, extra);
    line(x2, w, y2, "PACING", c::DIM, pace.verdict, pace.colour);
    y = std::max(y1, y2) + pad * 0.3f;
  }

  // ---- Frame graph and its legend.
  static float latencies[FRAME_HISTORY];
  const size_t latency_count = latency >= 0.f ? hsf::dlssg::LatencyHistory(latencies, FRAME_HISTORY) : 0;
  const float graph_height = (advanced ? 90.f : 64.f) * unit;
  y += pad * 0.2f;
  FrameGraph(draw, ImVec2(x, y), inner, graph_height, text, frames, count, MENU_HISTORY, latencies, latency_count, target_ms);
  y += graph_height + pad * 0.5f;
  float kx = x;
  const auto key = [&](const char* mark, ImU32 mark_colour, const char* name) {
    kx += Spaced(draw, ImVec2(kx, y), text * 0.75f, mark, mark_colour) + text * 0.3f;
    kx += Spaced(draw, ImVec2(kx, y), text * 0.75f, name, c::DIM) + text * 0.9f;
  };
  key("-", c::TEXT, "FRAME");
  if (latency_count >= 2) key("-", c::GREEN, "LATENCY");
  if (target_ms > 0.f) key("--", c::AMBER, "CAP");
  key("-", c::RED, "HITCH");
  y += row * 0.85f;

  if (advanced) {
    rule();
    char keys[96], a[16], b[16];
    KeyName(g_mfg_key, a, sizeof(a));
    KeyName(g_view_key, b, sizeof(b));
    const bool view_key = g_view_key != 0 && !v.d3d12;
    std::snprintf(keys, sizeof(keys), "%s%s%s%s%s", g_mfg_key != 0 ? a : "", g_mfg_key != 0 ? " MULTIPLIER" : "", g_mfg_key != 0 && view_key ? "   " : "",
                  view_key ? b : "", view_key ? " VIEW" : "");
    Spaced(draw, ImVec2(x, y), text * 0.75f, keys, c::FAINT);
    y += row * 0.85f;
  }
  needed[advanced ? 1 : 0] = need;
  end = ImVec2(right + pad, y + pad * 0.4f);
  panel = ImVec2(end.x - at.x, end.y - at.y);
}

// The debug view (ViewKey) in the top right corner, whatever else is on screen. It updates a few times a second, with the score.
void DrawScoreView() {
  const bool depth_view = g_score_view == hsf::score::View::depth;
  if (g_score_view == hsf::score::View::off) return;
  // DX9 games: ImGui draws with D3D9, so the views are D3D9 copies: depth from the bridge's depth grid (UpdateDepth9), the rest from the
  // score on HSF's D3D11 device (UpdateScoreView9). The D3D11 views are never handed to ImGui there.
  // Vulkan games: copies on the game's device too, depth from UpdateVkView and the rest from UpdateScoreView9.
  const bool dx9 = g_native9 != nullptr, vk = hsf::vk::Device() != nullptr;
  ID3D11ShaderResourceView* const view = dx9 || vk ? nullptr : depth_view ? hsf::depth::View() : hsf::score::ViewOf(g_seen.source, g_score_view);
  const uint64_t view9 = !dx9 && !vk ? 0 : depth_view ? (vk ? g_vkv.view.handle : g_d9.view.handle) : g_v9.serial != 0 ? g_v9.view.handle : 0;
  const bool dx9_depth = view9 != 0;
  const ImTextureID image = dx9_depth ? static_cast<ImTextureID>(view9) : static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(view));
  const ImVec2 screen = ImGui::GetIO().DisplaySize;
  if (screen.x <= 0.f || screen.y <= 0.f) return;
  ImDrawList* const draw = ImGui::GetForegroundDrawList();
  const float unit = std::max(screen.y, 480.f) / 1440.f, text_size = 26.f * unit, pad = 28.f * unit;
  const float width = screen.x * 0.3f, height = width * screen.y / screen.x;
  const ImVec2 at(screen.x - width - pad, pad);
  static const char* const CAPTIONS[] = {"", "MOTION: colour = direction, bright = fast", "ERROR LEFT AFTER THE WARP: black = none, white = large",
                                         "MOTION HELPED (green) OR HURT (red)", "DEPTH: white = near, black = far"};
  draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height + text_size * 1.4f), IM_COL32(0, 0, 0, 200));
  if (view != nullptr || dx9_depth) draw->AddImage(image, at, ImVec2(at.x + width, at.y + height));
  Label(draw, ImVec2(at.x + text_size * 0.3f, at.y + height + text_size * 0.1f), text_size,
        view != nullptr || dx9_depth ? CAPTIONS[static_cast<int>(g_score_view)] : "no measurement yet",
        IM_COL32(240, 240, 240, 255));
}

void DrawOsd(effect_runtime*) {
  DrawScoreView();
  if (g_overlay_mode == 0) return;
  // Nothing measured yet: the first frames.
  if (g_menu_write < 2) return;
  DrawOverlay(g_overlay_mode == 2);
}

// True when this add-on was loaded into HSF's own DLSS-G helper, not a game. A child process of a packaged game (Game Pass) searches the
// game's package folder for DLLs first, so a ReShade dxgi.dll there can load into the helper. The helper then does its own guard (system
// DLLs first, dlssg_helper.cpp); this is the second line, for a ReShade that still gets in.
bool InHelperProcess() {
  wchar_t exe[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  const wchar_t* const slash = wcsrchr(exe, L'\\');
  return _wcsicmp(slash != nullptr ? slash + 1 : exe, hsf::dlssg_link::HELPER_EXE) == 0;
}

// Inside the helper: ReShade's effects stay off, so the frames DLSS-G shows are not processed a second time.
void OnHelperRuntime(reshade::api::effect_runtime* runtime) { runtime->set_effects_state(false); }

bool g_in_helper = false;

// Set by DllMain when Windows loads the add-on, cleared by the AddonInit call ReShade makes right after.
bool g_fresh_attach = false;

// Every ReShade event and overlay HSF uses. DllMain calls it on the first load; AddonInit calls it again when ReShade is loaded a second time
// into the same process, where Windows keeps HSF loaded and skips DllMain.
void RegisterEvents() {
  if (g_in_helper) {
    reshade::register_event<reshade::addon_event::init_effect_runtime>(OnHelperRuntime);
    return;
  }
#ifdef HSF_SL_GAME
  if (g_sl_bring) reshade::register_event<reshade::addon_event::reshade_present>(OnSlBringPresent);
  if (g_sl_game) {
    reshade::register_event<reshade::addon_event::reshade_present>(OnReshadePresent);
    if (g_sl_drive) {
      reshade::register_event<reshade::addon_event::init_pipeline>(OnSlhInitPipeline);
      reshade::register_event<reshade::addon_event::destroy_pipeline>(OnSlhDestroyPipeline);
      reshade::register_event<reshade::addon_event::bind_pipeline>(OnSlhBindPipeline);
      reshade::register_event<reshade::addon_event::begin_render_pass>(OnSlhRenderPass);
      reshade::register_event<reshade::addon_event::copy_resource>(OnSlhCopyResource);
      reshade::register_event<reshade::addon_event::copy_texture_region>(OnSlhCopyRegion);
      reshade::register_event<reshade::addon_event::barrier>(OnSlhBarrier);
    }
  }
#endif
  reshade::register_event<reshade::addon_event::reshade_finish_effects>(OnFinishEffects);
  reshade::register_event<reshade::addon_event::reshade_begin_effects>(OnBeginEffects);
  reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindTargets);
  reshade::register_event<reshade::addon_event::draw>(OnDraw);
  reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
  reshade::register_event<reshade::addon_event::map_buffer_region>(OnMapBuffer);
  reshade::register_event<reshade::addon_event::unmap_buffer_region>(OnUnmapBuffer);
  reshade::register_event<reshade::addon_event::update_buffer_region>(OnUpdateBuffer);
  reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
  reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
  reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(OnReloaded);
  reshade::register_event<reshade::addon_event::reshade_open_overlay>(OnMenuToggled);
  reshade::register_event<reshade::addon_event::destroy_effect_runtime>(OnDestroyEffectRuntime);
  reshade::register_event<reshade::addon_event::destroy_swapchain>(OnDestroySwapchain);
  reshade::register_event<reshade::addon_event::present>(OnPresentColorSpace);
  RegisterCreateSwapchain();
  reshade::register_event<reshade::addon_event::create_resource>(OnCreateResource9);
  reshade::register_event<reshade::addon_event::init_resource>(OnInitResource9);
  reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
  reshade::register_event<reshade::addon_event::reshade_present>(OnVkPresent);
  RegisterVkFinishPresent();
  reshade::register_event<reshade::addon_event::reshade_present>(OnMenuFrame);
  reshade::register_event<reshade::addon_event::reshade_present>(OnReserveFrame);
  reshade::register_event<reshade::addon_event::reshade_present>(OnWindowFrame);
  reshade::register_event<reshade::addon_event::reshade_present>(KeepLaunchPadAuto);
  reshade::register_event<reshade::addon_event::reshade_present>(ReclipAfterMenu);
  // The menu is its own tab next to ReShade's, not inside the Add-ons tab. DrawPanel docks it there, see DockMenuWithReShade.
  reshade::register_overlay("HSF", DrawPanel);
  reshade::register_overlay("OSD", DrawOsd);
}

}  // namespace

// ReShade calls this each time it loads the add-on, right after LoadLibrary. On the first load DllMain already registered everything. An
// emulator that stops one game and boots another (RPCS3) unloads ReShade's Vulkan layer and loads it again in the same process; HSF stays
// loaded (its GPU reserve thread holds it), so Windows skips DllMain and only this call can register HSF with the new ReShade.
extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
  if (g_fresh_attach) {
    g_fresh_attach = false;
    return true;
  }
  // The new ReShade is often at another address than the first one. reshade.hpp (changed for HSF) takes the new handle here and looks its
  // functions up on each call, so nothing points at the unloaded one.
  const bool moved = reshade_module != reshade::internal::get_reshade_module_handle();
  if (!reshade::register_addon(addon_module, reshade_module)) {
    char text[200];
    std::snprintf(text, sizeof(text), "HSF: ReShade was loaded again (%s address) but did not take HSF back; HSF stays off until the emulator is restarted",
                  moved ? "another" : "the same");
    OutputDebugStringA(text);
    if (reshade::internal::get_reshade_module_handle() == reshade_module) reshade::log_message(reshade::log_level::error, text);
    return false;
  }
  RegisterEvents();
  reshade::log_message(reshade::log_level::info, moved ? "HSF: ReShade was loaded again at another address (an emulator booting another game); HSF registered again"
                                                       : "HSF: ReShade was loaded again (an emulator booting another game); HSF registered again");
  return true;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(module)) return FALSE;
      // In HSF's own helper the add-on does nothing else: no route pick, no HSF.ini writes, no menu or overlay.
      g_fresh_attach = true;
      g_in_helper = InHelperProcess();
      if (g_in_helper) {
        RegisterEvents();
        reshade::log_message(reshade::log_level::warning,
                             "HSF: loaded into HSF's own DLSS-G helper, not a game. HSF does nothing here and ReShade's effects are off.");
        break;
      }
      FindIni(module);
      // Vulkan games: same-frame hand-off needs shared semaphores on the game's device, added at its vkCreateDevice (vk_hook.h).
      if (GetPrivateProfileIntW(L"HSF", L"VkDeviceHook", 1, g_ini_path) != 0)
        if (const char* const hooked = hsf::vk::HookCreateDevice()) reshade::log_message(reshade::log_level::info, (std::string("HSF: ") + hooked).c_str());
#ifdef HSF_SL_GAME
      // A native D3D12 game with its own Streamline. StreamlineProbe=1 logs what it sends and changes nothing; InGameDlssg=1 also runs DLSS-G inside the game.
      g_sl_drive = GetPrivateProfileIntW(L"HSF", L"InGameDlssg", 0, g_ini_path) != 0;
      g_sl_game = g_sl_drive || GetPrivateProfileIntW(L"HSF", L"StreamlineProbe", 0, g_ini_path) != 0;
      // A native D3D12 game with no Streamline. On its own this only starts Streamline in the game: nothing is generated yet, as nothing feeds DLSS-G.
      g_sl_bring = GetPrivateProfileIntW(L"HSF", L"SlBringIn", 0, g_ini_path) != 0;
      if (g_sl_bring) {
        wchar_t folder[MAX_PATH];
        wcscpy_s(folder, g_ini_path);
        if (wchar_t* const slash = wcsrchr(folder, L'\\')) *slash = L'\0';
        const std::wstring bring_log = std::wstring(folder) + L"\\HSF_sl_bring.log";
        // HSF's own Streamline if it is there, so that a game with an old copy, or none, gets the version HSF was tested with.
        const std::wstring own = std::wstring(folder) + L"\\HSF_streamline";
        const bool have_own = GetFileAttributesW((own + L"\\sl.interposer.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
        hsf::slbring::StartOptions sl_options;
        sl_options.log = GetPrivateProfileIntW(L"HSF", L"SlLog", 0, g_ini_path) != 0;
        sl_options.factory_proxy = GetPrivateProfileIntW(L"HSF", L"SlFactoryProxy", 1, g_ini_path) != 0;
        sl_options.manual_hooking = GetPrivateProfileIntW(L"HSF", L"SlManualHooking", 0, g_ini_path) != 0;
        sl_options.early_load = GetPrivateProfileIntW(L"HSF", L"SlEarlyLoad", 1, g_ini_path) != 0;
        hsf::slbring::Start(bring_log.c_str(), have_own ? own.c_str() : folder, sl_options);
        g_ngx_input = GetPrivateProfileIntW(L"HSF", L"NgxInput", 1, g_ini_path) != 0;
        if (g_ngx_input) hsf::ngx::Start(&hsf::slbring::LogLine);
      }
      if (g_sl_game) {
        wchar_t log_path[MAX_PATH];
        wcscpy_s(log_path, g_ini_path);
        if (wchar_t* const slash = wcsrchr(log_path, L'\\')) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - log_path), L"HSF_sl_probe.log");
        g_sl_dynamic_saved = GetPrivateProfileIntW(L"HSF", L"DlssgDynamicRes", 0, g_ini_path) != 0;
        hsf::slgame::Start(log_path, g_sl_drive, g_sl_dynamic_saved);
      }
#endif
      g_fault9_handler = AddVectoredExceptionHandler(0, OnFault9);
      RegisterEvents();
      break;
    case DLL_PROCESS_DETACH:
      if (g_in_helper) {
        reshade::unregister_addon(module);
        break;
      }
#ifdef HSF_SL_GAME
      if (g_sl_game) hsf::slgame::Stop(reserved != nullptr);
      if (g_sl_bring && g_ngx_input) hsf::ngx::Stop(reserved != nullptr);
      hsf::ngx::Stop11(reserved != nullptr);
      if (g_sl_bring) hsf::slbring::Stop(reserved != nullptr);
      if (g_sl_game && reserved == nullptr) DropSlChecks();  // at process exit the game's device and this code's order are not ours to rely on
#else
      (void)reserved;
#endif
      if (reserved == nullptr) hsf::vk::UnhookCreateDevice();  // ReShade unloads the add-on but the game goes on: no hook into a freed HSF
      UnhookNativePresent();
      UnhookNativePresent9();
      ClearDx9Run();  // a normal exit (a crash never gets here): the 9Ex crash rule must not count it
      if (g_fault9_handler != nullptr) RemoveVectoredExceptionHandler(g_fault9_handler);
      reshade::unregister_addon(module);
      break;
  }
  return TRUE;
}
