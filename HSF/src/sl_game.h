// The game's own Streamline, for native D3D12 games that ship it. Hooks what the game sends through its sl.interposer.dll and logs it. With drive on it also turns
// DLSS-G on inside the game, on the game's device and swap chain, whatever the game's own FG setting says, and tags the vectors and depth the game gives DLSS. With drive off every
// call goes on unchanged. Off unless HSF.ini has StreamlineProbe=1 (log only) or InGameDlssg=1 (drive).
#pragma once

#include <cstddef>
#include <cstdint>

namespace hsf::slgame {

// HSF's hook code for a call in another module (ngx_input.cpp), which works with or without the rest of this file running. real gets what calls the original. The hook's messages go to log.
bool HookFunction(const char* name, void* target, void* hook, void** real, void (*log)(const char* text));
void UnhookFunctions();

// Hooks the game's sl.interposer.dll if it is loaded; Poll hooks it later if not. Safe under the loader lock, so DllMain may call it.
// dynamic_resolution_known: an earlier run saw this game's vectors change size, so DLSS-G's dynamic resolution flag is on from the first frame. Otherwise it comes on at the first change
// seen: Streamline's guide says not to leave it set with fixed-ratio DLSS, as it may cost performance or image quality, so a game cannot just always have it.
void Start(const wchar_t* log_path, bool drive, bool dynamic_resolution_known = false);
// Call once a frame on the thread that presents: notices the interposer arriving, going away or coming back, and with drive on sets DLSS-G's mode.
void Poll();
// What HSF wants, with drive on. allowed false holds generation off. frames is generated per real frame (1 = 2x).
void SetGeneration(bool allowed, uint32_t frames);
// True holds generation off while the game's window is not in front and for a moment after it comes back. Crimson Desert drops its device (DXGI_ERROR_ACCESS_DENIED)
// when its window gets focus back with frame generation on, its own or HSF's (players report the same with the game's own FG).
void SetFocusHold(bool hold);
// Takes generation off if HSF turned it on, puts the game's bytes back and waits briefly for calls in flight. ReShade unloads and reloads add-ons during startup, so this runs for real.
// process_exiting: only close the log, as other threads are gone and the game's modules may be too.
void Stop(bool process_exiting);

// Whether DLSS-G gets clipToPrevClip and prevClipToClip rebuilt by HSF from the camera, or the game's own. Works at once, so it can be flipped in play.
void SetCameraRebuild(bool on);
// On by default. A camera that jumps between two frames is a cut even if the game sends no reset, so HSF sets the reset. Needs no GPU, and it is on time, unlike a look at the picture.
void SetCutCheck(bool on);
// Off by default. Generation stops after about 3 frames with no vectors instead of 250 ms, which leaves fewer generated frames going into a menu. A hitch can then stop it for a frame.
void SetQuickOff(bool on);
// Off by default. Generation stops while the score says the game's vectors do clearly worse than no motion. Without this the score only logs what it would do.
void SetScoreGuard(bool on);

// Optical motion: DLSS-G gets HSF's flow and depth, tagged with TagOptical at present, instead of the game's vectors and depth. The game's own tags are
// left out, and the constants it sends get the flow's scale, no jitter and HSF's dilation. Only while TagOptical keeps coming: a few frames without it and the game's vectors are back.
void SetOptical(bool on);
// Tags HSF's depth and motion for the latest frame, on `command_list` (open, on the game's queue, Streamline copies them there). Same shape as slbring::FrameFeed's tag.
bool TagOptical(void* command_list, void* depth, uint32_t depth_state, void* motion, uint32_t motion_state, const uint32_t extent[4], const float scale[2], bool dilated);
// HSF's FPS cap (on screen, 0 = leave the game's own) and Reflex boost, through the game's own Reflex. The cap becomes the game's frame limit for its real frames.
void SetReflex(uint32_t fps_cap, bool boost);

// Called with each frame's vectors on the game's open command list, before they are tagged. Clear it before what it points at goes away; Stop waits for calls in flight.
using VectorsHook = void (*)(void* command_list, void* motion, uint32_t state, const uint32_t extent[4], const float scale[2]);
void SetVectorsHook(VectorsHook hook);
// What the checks on the game's device found (frame_checks.h GameChecks). kind: 0 counted, 1 still, 2 a cut.
void NoteScore(float warp, float none, float better, float worse, float moving, int kind);
void NotePictureCut(float changed, float shape);
void NoteChecksProblem(const char* problem);

// The picture before the game's HUD, for DLSS-G. `command_list` is the game's, open, with `back_buffer` in render target state: Streamline copies it there and then.
// Does nothing, and answers false, while HSF is not generating.
bool TagHudless(void* command_list, void* back_buffer, uint32_t width, uint32_t height);
// No HUD-free picture for this frame: DLSS-G goes back to working without one, rather than using an old one.
void ClearHudless();
// One line in HSF's Streamline log.
void LogLine(const char* text);

struct Stats {
  bool generating;           // HSF has DLSS-G's mode on right now
  bool have_options;         // the game's DLSS-G is there to be driven
  uint32_t frames;           // generated per real frame, as last sent
  uint32_t max_frames;       // what DLSS-G says this machine can do (0 until it says)
  uint32_t status;           // sl::DLSSGStatus, 0 = fine
  uint32_t presented;        // frames DLSS-G puts on screen per real frame, from its last report: 3 means 3x
  uint64_t tagged;           // vectors and depth HSF has tagged
  bool dynamic_resolution;   // the vectors have been seen changing size, in this run or an earlier one
  int plugin_loaded;         // is DLSS-G's plugin loaded: 1 yes, 0 no, -1 not known yet
  bool data_flowing;         // the game is sending vectors and depth now
  uint64_t cuts_by_hsf;      // resets HSF set from a camera jump
  uint64_t picture_cuts;     // cuts the picture showed afterwards
  uint64_t picture_cuts_no_reset;  // ... with no reset, from the game or HSF, near them
  float score_gain;          // the score over its last pairs: 1 perfect, 0 no help, below 0 worse than no motion
  uint32_t score_pairs;      // pairs behind score_gain, 0 until there are some
  bool vectors_doubted;      // score_gain is low enough that the score guard stops generation, if it is on
  bool old_streamline;       // the game's Streamline is 1.x, which HSF leaves alone (no slGetFeatureFunction)
};
Stats GetStats();
// Real frames the game has made so far (new frame tokens). No lock: cheap enough for every present. On this route ReShade also sees DLSS-G's generated frames,
// so this, not ReShade's present count, is the real frame count.
uint64_t GameFrames();

// A camera as sl::Constants gives it. Row vectors, as Streamline wants: the view looks along -z, so view space's third axis is -fwd.
struct CameraPose {
  float right[3], up[3], fwd[3], pos[3];
};
// to_prev = clipToView(cur) * viewToViewPrev * viewToClip(prev), Streamline's formula for clipToPrevClip. With cur and prev swapped it gives prevClipToClip. False if a camera's axes are not
// a sound right-handed set or the result is not finite, and then out is left alone. Public for the test.
bool RebuildClipToPrev(const float clip_to_view_cur[4][4], const CameraPose& cur, const float view_to_clip_prev[4][4], const CameraPose& prev, float out[4][4]);

// How many bytes of whole instructions at code cover at least min_bytes, or 0 if an instruction is not on the short list of prologue instructions HSF can move. A function that does
// not start with those is left alone. rip_at, if given, receives the offsets of the instructions relative to rip (loads, and a jump that ends a small forwarding function), which need a
// new displacement when moved; rip_count is how many, at most MAX_RIP_LOADS. Public for the test.
constexpr size_t MAX_RIP_LOADS = 4;
size_t PrologueLength(const uint8_t* code, size_t min_bytes, size_t* rip_at = nullptr, size_t* rip_count = nullptr);
// How many hooks are in place now. For the test.
int HooksInstalled();
// Hooks a function as if the game had just asked Streamline for it by this name. For the test.
bool HookFeatureFunctionForTest(const char* name, void* function);

}  // namespace hsf::slgame
