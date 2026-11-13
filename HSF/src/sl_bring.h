// Streamline for native D3D12 games that do not ship it. A game with Streamline creates its swap chain through a factory from sl.interposer.dll, and that is what lets DLSS-G
// present. This gives any other game the same thing: HSF loads Streamline and catches the game's swap chain creation on the factory's method table, then makes the swap chain through a
// Streamline factory, below ReShade, so ReShade's effects run once per real frame and before DLSS-G takes it. HSF also sends the Reflex markers and frame tokens a game with Streamline would, and
// through Frame the flow and depth from the effects. Off unless HSF.ini has SlBringIn=1.
#pragma once

#include <d3d12.h>

#include <cstdint>

namespace hsf::slbring {

// How Streamline is started. The defaults are what worked in Village and Red Dead Redemption 2; the rest are for games that crash while Streamline starts (Jusant).
struct StartOptions {
  bool log = false;            // SlLog=1: Streamline log file and log callback (off by default: its logging crashed Jusant)
  bool factory_proxy = true;   // SlFactoryProxy=0: Streamline changes the DXGI factory's method table instead of wrapping it. On by default: with RenoDX DLSS, a swap chain made the other way came back as plain DXGI and DLSS-G showed no frames (Jusant)
  bool manual_hooking = false; // SlManualHooking=1: Streamline hooks nothing itself, and HSF upgrades the factory, as OptiScaler does
  bool early_load = true;      // SlEarlyLoad=0: load sl.interposer.dll only with the first device, as before
};
// folder holds sl.interposer.dll and its plugins. Only loads sl.interposer.dll (no slInit) when early_load is set, so other add-ons that look for Streamline
// as they start (RenoDX DLSS picks its Present hook then) find it. DllMain calls it.
void Start(const wchar_t* log_path, const wchar_t* folder, const StartOptions& options = {});
// With each D3D12 device as it is made, before any swap chain on it. The first one loads Streamline and starts catching swap chains. device is the native ID3D12Device.
void NoteDevice(void* device);
// Once a frame on the thread that presents: logs now and then what DLSS-G says.
void Poll();
// Tags this frame's depth and motion for someone else's Streamline (the game's own, slgame::TagOptical) on `command_list`. extent is left, top, width, height.
using TagFn = bool (*)(void* command_list, void* depth, uint32_t depth_state, void* motion, uint32_t motion_state, const uint32_t extent[4], const float scale[2], bool dilated);
// What DLSS-G gets for one real frame. All native objects. `motion` and `depth` are readable as shader resources, the same size, and stay as they are.
struct FrameFeed {
  ID3D12Device* device = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12Resource* back_buffer = nullptr;
  unsigned back_buffer_state = 0;  // D3D12_RESOURCE_STATES it is in, and is left in
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;  // R32_FLOAT
  float scale[2] = {};              // DLSS-G's mvecScale for `motion`
  bool depth_inverted = false;
  bool reset = false;
  bool on = false;                  // generate now
  uint32_t frames = 1;              // generated per real frame
  bool compose = true;              // clean the flow with depth first
  bool mask = true;                 // zero motion on what stays put, mostly the HUD
  bool nvof = false;                // the motion's contents come from NVIDIA Optical Flow (nvof_flow.h); `motion` still gives size, format and scale
  uint32_t nvof_size = 360;         // the height of the grey image the engine gets, 0 = the picture's own
  // From the game's upscaler. All zero for flow, which fills its textures.
  uint32_t x = 0, y = 0, width = 0, height = 0;  // the part of motion and depth in use, which can change every frame
  float jitter[2] = {};             // in pixels of the render size
  bool jittered = false;            // the vectors include the jitter
  bool dilated = false;             // the vectors are already dilated: full-resolution ones
  TagFn tag = nullptr;               // set: the inputs go to this instead of HSF's Streamline, and constants, options and markers are left to that Streamline's owner
  unsigned view = 0;                // debugview::Kind: 0 the game, 1 depth, 2 motion, as DLSS-G gets them, in place of the picture
};
// After the frame is finished and before Streamline gets its Present, on the thread that presents, with anything recorded for the same queue already submitted. False: this frame has no inputs.
bool Frame(const FrameFeed& feed);
void SetReflex(uint32_t fps_cap, bool boost, bool sleep);
// A line in this file's log, for ngx_input.cpp.
void LogLine(const char* text);
// Stops catching swap chains. Streamline itself stays, as a swap chain made through it may be in use. A process that is exiting has nothing to put back.
void Stop(bool process_exiting);

struct Stats {
  bool streamline_up;        // slInit worked
  uint32_t swap_chains;      // made through Streamline
  uint32_t failed;           // Streamline could not make, so the game got a plain one
  int plugin_loaded;         // is DLSS-G's plugin loaded: 1 yes, 0 no, -1 not known yet
  int supported;             // Streamline's answer on DLSS-G and this adapter, 0 = yes, -1 not known yet
  int status;                // sl::DLSSGStatus, 0 = fine, -1 not known yet
  uint32_t max_frames;       // generated per real frame this machine can do, 0 until known
  bool generating;           // HSF has DLSS-G on
  uint32_t shown_per_real;   // what DLSS-G last said it shows per real frame
  int nvof_state;            // NVIDIA Optical Flow on the last frame fed: 0 off, 1 waiting, 2 active, 3 failed (nvof::State)
};
Stats GetStats();

}  // namespace hsf::slbring
