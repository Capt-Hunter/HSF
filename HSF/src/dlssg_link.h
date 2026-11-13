// The block shared between the add-on (dlssg_remote.cpp) and the 64-bit DLSS-G helper (dlssg_helper.cpp, hsf-dlssg-helper64.exe).
// Streamline has no 32-bit build, and RTSS's overlay ends a game that runs DLSS-G next to ReShade, so DLSS-G runs in the helper for every D3D11
// and DX9 game (like GITC Uplift's gitc-uplift-helper64.exe for NR):
// the add-on makes the shared frame slots and fences on the game's Direct3D 11 device and duplicates their NT handles into the helper; the
// helper opens them on its own Direct3D 12 device, puts a disabled child window over the game window (the cross-process route)
// and presents through DLSS-G there. CPU slot check on the game side, never a GPU wait.
// Fixed-size fields only (no pointers, no size_t): the layout must be the same in a 32-bit and a 64-bit build.
#pragma once

#include "camera.h"

#include <cstddef>
#include <cstdint>

namespace hsf::dlssg_link {

constexpr uint32_t MAGIC = 0x47534648;  // "HFSG"
constexpr uint32_t VERSION = 14;  // 14: the game's colour space; 13: overlay window (Vulkan); 12: helper's dxgi.dll not Windows' own (foreign_dxgi); 11: NVIDIA Optical Flow (FIX_NVOF, nvof_state); 10: ReShade menu open (mouse to the helper's window); 2: Reflex on the game's frame (messages, sleep handshake, FPS cap); 3: latency for the overlay; 4: per-frame reset; 5: real depth; 6: compose pass; 7: frame checks; 8: HUD-less image; 9: camera
constexpr uint32_t RING = 5;
constexpr int32_t FIX_COMPOSE = 1, FIX_CHECKS = 2, FIX_CAMERA = 4, FIX_FIT = 8, FIX_RECORD = 16;  // Block::slot_compose. FIX_RECORD: write this frame's camera data to a file (camera_record.h)
// Version 11: the frame's motion comes from NVIDIA Optical Flow in the helper (nvof_flow.h) in place of the effect's flow, whose texture still gives the size, format and
// scale. Bits 8 to 23 of slot_compose then hold the height of the grey image the engine gets (0 = the picture's own).
constexpr int32_t FIX_NVOF = 32, NVOF_SIZE_SHIFT = 8, NVOF_SIZE_MASK = 0xffff;
constexpr uint32_t MESSAGES = 16;  // Reflex messages in flight (game -> helper)
constexpr uint32_t LATENCY_HISTORY = 256;  // HSF's per-frame latency timer (helper -> add-on), for the overlay's graph

// Reflex on the game's frame, 32-bit: Streamline is in the helper, but the sleep has to stop the game thread. The add-on sends one message per
// event (kind in the top 8 bits, the game frame's Streamline index in the low 32) through `messages` and sets `request_event`; the helper's
// Reflex thread acts on each at once: MSG_SLEEP = new token for the index, slReflexSleep, simulation start marker, then `slept` = index and
// `done_event`; the add-on's game thread waits for `slept` to reach its index (with a time-out). The others are markers only.
enum : uint32_t { MSG_SLEEP = 1, MSG_FRAME = 2, MSG_SUBMIT_START = 3, MSG_SUBMIT_END = 4 };  // MSG_FRAME: a token without a sleep (first frame)
constexpr const wchar_t* HELPER_EXE = L"hsf-dlssg-helper64.exe";

// Helper state.
enum : int32_t { STATE_STARTING = 0, STATE_RUNNING = 1, STATE_FAILED = 2, STATE_STOPPED = 3 };
// Why DLSS-G is not generating (helper -> add-on), as an index into NOTE_TEXT.
enum : int32_t { NOTE_NONE = 0, NOTE_NO_MOTION = 1, NOTE_PAUSED = 2, NOTE_NO_SWAP = 3, NOTE_GPU_HANG = 4 };
inline const char* const NOTE_TEXT[] = {"", "no motion vectors: real frames only", "DLSS-G paused (menu open or extra frames off)",
                                        "the DLSS-G swap chain could not be made (see hsf_dlssg.log)",
                                        "Frame gen stopped: the GPU could not keep up. Pick a lower multiplier or cap the fps, then turn FRAME GEN off and on."};

#pragma pack(push, 8)
struct Block {
  uint32_t magic, version;
  // Set by the add-on before the helper runs.
  uint32_t game_pid;
  uint32_t width, height, format;  // the game's back buffer (DXGI_FORMAT)
  uint64_t parent_window;          // the game's window (HWND)
  uint32_t adapter_luid_low;
  int32_t adapter_luid_high;
  uint64_t slots[RING];            // frame slots, NT handles valid in the helper
  uint64_t ready_fence;            // game -> helper: frame id M is in slot M % RING
  uint64_t consumed_fence;         // helper -> game: done with frame M's slot
  // Motion vector slots: made by the add-on on the first frame that has them; `motion_ready` goes to 1 once the handles are written.
  volatile int32_t motion_ready;
  uint32_t motion_width, motion_height, motion_format;
  uint64_t motion[RING];
  volatile int64_t motion_frame[RING];  // slot i holds frame motion_frame[i]'s vectors (written after motion_scale)
  float motion_scale[RING][2];
  // Options (add-on -> helper).
  volatile int32_t on, frames, stop;
  // Helper -> add-on.
  volatile int32_t state;
  volatile int32_t real, shown, skipped, status, generated_seen;  // counters; the add-on takes them with InterlockedExchange
  volatile int32_t max_frames;
  volatile int32_t note;
  char error[240];  // written before state = STATE_FAILED
  // Version 2: Reflex on the game's frame.
  uint64_t request_event, done_event;  // events (handles valid in the helper): add-on -> helper "messages waiting", helper -> add-on "slept"
  volatile int32_t fps_cap, boost;     // Reflex options (add-on -> helper): frames on screen per second (0 = no cap), boost 0/1
  volatile int32_t message_write;      // messages written by the add-on (it only writes)
  volatile int32_t message_read;       // messages taken by the helper (it only writes)
  volatile int64_t messages[MESSAGES];
  volatile int64_t slot_index[RING];   // the game frame's Streamline index of the frame in slot i (written before `ready`; 0 = none)
  volatile int64_t slept;              // helper -> add-on: the last index whose Reflex sleep is done
  volatile int32_t sleep_us, sleeps;   // helper -> add-on counters: Reflex sleep time and count (taken with InterlockedExchange)
  volatile int32_t reflex_available;   // helper -> add-on: slReflexGetState's lowLatencyAvailable
  volatile int32_t sleep_on;           // add-on -> helper: 0 = no Reflex sleep (markers only), for an A/B latency test
  // Version 3: latency for the overlay. HSF's timer: the add-on writes QPC at each game frame's start for its slot (QPC is one clock for every
  // process); the helper measures to DLSS-G's Present and writes the result into the ring and the per-second counters.
  volatile int64_t slot_start[RING];
  volatile int32_t latency_write;  // samples written (helper only writes)
  volatile int32_t latency_us[LATENCY_HISTORY];
  volatile int32_t latency_sum_us, latency_count, latency_max_us;  // taken with InterlockedExchange
  // Reflex's latency report (reflex_report.h), the helper's last one-second summary; report_frames = 0: none.
  volatile int32_t report_frames, report_total_us, report_game_us, report_submit_us, report_handoff_us, report_present_us, report_gpu_us;
  int32_t pad;
  // Version 4: DLSS-G's reset for the frame in slot i (written before `ready`).
  volatile int32_t slot_reset[RING];
  int32_t pad2;
  // Version 5: real depth, R32_FLOAT at the motion vectors' size, handed over like them; `depth_ready` goes to 1 once the handles are written.
  uint64_t depth[RING];
  volatile int64_t depth_frame[RING];  // slot i holds frame depth_frame[i]'s depth (written after depth_inverted)
  volatile int32_t depth_ready;
  uint32_t depth_width, depth_height, depth_format;
  volatile int32_t depth_inverted[RING];
  int32_t pad3;
  // Version 6: clean up the motion of the frame in slot i with its depth (compose.h). Version 7: FIX_ bits, adding the frame checks (frame_checks.h). Written before `ready`.
  volatile int32_t slot_compose[RING];
  int32_t pad4;
  // Version 8: the frame from before the overlays were drawn, the frame's size and format, handed over like depth.
  uint64_t hudless[RING];
  volatile int64_t hudless_frame[RING];  // slot i holds frame hudless_frame[i]'s image
  volatile int32_t hudless_ready;
  int32_t pad5;
  // Version 9: the camera (camera.h). The matrices found while the frame in slot i was drawn; camera_frame numbers the game's frames, so a gap shows. Written before `ready`.
  camera::Candidate camera[RING][camera::CANDIDATES];
  volatile int32_t camera_count[RING];
  int32_t pad6;
  volatile int64_t camera_frame[RING];
  volatile int64_t camera_hint;        // add-on -> helper: the matrix that won in this game before, or 0
  volatile int64_t camera_locked;      // helper -> add-on: the matrix locked now, or 0
  volatile int32_t camera_source;      // helper -> add-on: camera::Source this frame
  volatile int32_t camera_share;       // helper -> add-on: how much of the flow it explains, in percent
  // Version 10: add-on -> helper, 1 while ReShade's menu is open. The helper's window then takes the mouse, so a mod's menu drawn on its swap
  // chain (MFG-Unlocker's) can be clicked; clicks the mod does not take go on to the game's window, where ReShade gets them.
  volatile int32_t menu_open;
  // Version 11: helper -> add-on, nvof::State of NVIDIA Optical Flow (0 off, 1 waiting, 2 active, 3 failed).
  volatile int32_t nvof_state;
  // Version 12: helper -> add-on, 1 when the helper's dxgi.dll is not Windows' own (a ReShade dxgi.dll in a Game Pass game's folder got in), so
  // ReShade's effects and add-ons run in the helper too and cost frames.
  volatile int32_t foreign_dxgi;
  // Version 13: add-on -> helper, set before the helper runs. 1 on the Vulkan route unless HSF.ini has VkOverlayWindow=0: a click-through window of
  // the helper's own over the game's in place of a child window, so the two never share an input queue (dlssg_core.cpp, OverlayThread).
  int32_t overlay_window;
  // Version 14: add-on -> helper, set before the helper runs. The game swap chain's colour space (DXGI_COLOR_SPACE_TYPE), so an HDR10 frame
  // (R10G10B10A2 in ST.2084 with BT.2020 primaries, as RenoDX outputs) is shown as HDR10 by the helper's swap chain too.
  int32_t color_space;
  int32_t pad7;
};
#pragma pack(pop)
// Same layout in the 32-bit add-on and the 64-bit helper (checked in both builds).
static_assert(offsetof(Block, slots) == 40 && offsetof(Block, motion_ready) == 96 && offsetof(Block, motion_frame) == 152 &&
              offsetof(Block, on) == 232 && offsetof(Block, error) == 276 && offsetof(Block, request_event) == 520 &&
              offsetof(Block, messages) == 552 && offsetof(Block, slot_index) == 680 && offsetof(Block, slept) == 720 &&
              offsetof(Block, reflex_available) == 736 && offsetof(Block, slot_start) == 744 && offsetof(Block, latency_us) == 788 &&
              offsetof(Block, report_frames) == 1824 && offsetof(Block, slot_reset) == 1856 && offsetof(Block, depth) == 1880 && offsetof(Block, depth_ready) == 1960 && offsetof(Block, slot_compose) == 2000 && offsetof(Block, hudless) == 2024 && offsetof(Block, hudless_ready) == 2104 && offsetof(Block, camera) == 2112 && offsetof(Block, camera_count) == 7872 && offsetof(Block, camera_frame) == 7896 && offsetof(Block, camera_hint) == 7936 && offsetof(Block, menu_open) == 7960 && offsetof(Block, foreign_dxgi) == 7968 && offsetof(Block, overlay_window) == 7972 && offsetof(Block, color_space) == 7976 && sizeof(Block) == 7984,
              "dlssg_link::Block layout changed");

inline int64_t Message(uint32_t kind, uint32_t index) { return static_cast<int64_t>((static_cast<uint64_t>(kind) << 56) | index); }
inline uint32_t MessageKind(int64_t m) { return static_cast<uint32_t>(static_cast<uint64_t>(m) >> 56); }
inline uint32_t MessageIndex(int64_t m) { return static_cast<uint32_t>(static_cast<uint64_t>(m) & 0xffffffffu); }

}  // namespace hsf::dlssg_link
