# HSF.ini reference

`HSF.ini` sits next to the add-on (`hsf.addon64` or `hsf.addon32`), and every key is in the `[HSF]` section unless this page says otherwise. HSF reads it at start-up, then reads it again whenever the file's time stamp changes, so most keys can be changed while the game runs. The menu writes the file when you change something in it.

Write it with a plain text editor, or with `[System.IO.File]::WriteAllLines` from a script. Never write it with `printf` or a bash heredoc: a `\r` in a path turns into a carriage return.

Source of truth: `LoadIni` and `SaveIni` in `HSF/src/hsf_addon.cpp` (about line 317 to 490), and `DllMain` for the start-up-only keys (about line 5280).

Legend:
- **Live**: read again when the file changes.
- **Start**: read only when the game starts, so a change needs a restart.
- **HSF writes**: HSF sets the key itself, and you normally leave it alone.
- **Route**: which routes read the key. D3D12-G is the in-game route (the game's Streamline), D3D12-B the bring-in route (HSF's Streamline), D3D11 the helper route, DX9 the DX9 bridge, Vulkan the Vulkan bridge. "All" means every route.

## 1. Core

Older files may still have `Presenter`, `Generated` and `Pacing`. They belonged to HSF's early in-house presenter, which is gone: HSF ignores them and takes them out of the file the next time it saves.

| Key | Default | Values | When | Route | What it does |
|---|---|---|---|---|---|
| `Extra` | `1` | 0, 1 | Live | All | The menu's FRAME GEN. 0 shows real frames only. |
| `DlssgFrames` | `1` | 1 to 5 | Live | All | Generated frames per real frame: 1 = 2x, up to 5 = 6x. It is capped to what DLSS-G reports this GPU can do. |
| `MfgKey` | `118` (F7) | key code, 0 = none | Live | All | Cycles `DlssgFrames` in game. |
| `Overlay` | `1` | 0 off, 1 simple, 2 advanced | Live | All | The on-screen overlay. |
| `OverlaySimple`, `OverlayAdvanced` | `100,59,0,28,28` and `100,96,0,28,28` | size %, opacity %, corner (0 top left, 1 top right, 2 bottom left, 3 bottom right), x, y | Live | All | Each overlay's look. The offsets are pixels at 1440 lines, scaled to the frame. |
| `GameFgWarningOk` | `0` | 0, 1 | HSF writes | D3D12-G | 1 after you pressed OK on "THIS GAME HAS ITS OWN FRAME GENERATION". |
| `VkOverlayWindow` | `1` | 0, 1 | Start | Vulkan | DLSS-G shows its frames in a click-through window of its own over the game. 0 puts them in a child window of the game's window, as before, which made the mouse choppy with the pointer inside the window. |

## 2. Motion and depth

| Key | Default | Values | When | Route | What it does |
|---|---|---|---|---|---|
| `Source` | `auto` | `auto`, `launchpad`, `lumenite` | Live | All flow routes | The menu's FLOW FROM. `auto` takes the first one loaded, in this order. LaunchPad: `Deferred::MotionVectorsTex`, full size, delta UV. Lumenite: `Kernel::tFlow`, 1/8 size, delta UV, which the compose pass scales to depth size. An old `uplift` reads as `auto`. |
| `MotionFrom` | `auto` | `auto`, `optical`, `game` | Live | D3D12-G, D3D12-B, D3D11 | The menu's MOTION: `auto` = AUTO, `game` = DLSS (LUMA in a D3D11 game with Luma), `optical` = FLOW. AUTO uses the game's DLSS data whenever it comes, else flow. The file only has this key once a D3D12 route is on, or a D3D11 game has sent DLSS data through Luma. |
| `FlowPause` | `1` | 0, 1 | Live | D3D11 | The menu's FLOW PAUSE. 1 turns the flow effect (LaunchPad, Lumenite Kernel) off while Luma's DLSS data feeds frame generation, and back on when it stops. Other effects that read the same flow stop moving while it is off. Written once Luma's data has come. |
| `Motion` | `auto` | `auto`, `uv`, `-uv`, `mvpx`, `-mvpx`, `px`, `-px` | Live | D3D11, DX9 | What the flow's numbers mean. `auto` uses what the source is known to give. Change it only for a new, unknown source. |
| `Depth` | `1` | 0, 1 | Live | D3D11, DX9 | The menu's SEND DEPTH. 0 sends a constant depth instead. |
| `DepthInverted` | `-1` | -1 found, 0 normal, 1 reversed (far = 0) | Live | All flow routes | The menu's DEPTH DIRECTION. |
| `DepthUpsideDown` | `-1` | -1 found, 0 as the game has it, 1 upside down | Live | All flow routes | The menu's DEPTH WAY UP. |
| `Compose` | `1` | 0, 1 | Live | All flow routes | The menu's CLEAN FLOW (DEPTH): the compose pass that cleans up flow with depth (see architecture.md 5.2). |
| `FrameChecks` | `1` | 0, 1 | Live | D3D11, DX9, D3D12-B | Scene cuts, still pictures and the HUD mask. On D3D12-B this is what PROTECT GAME HUD switches. |
| `KeepOverlaysOut` | `1` | 0, 1 | Live | D3D11, DX9 | DLSS-G gets the frame from before the overlays (ReShade's menu, RTSS, Steam, Discord). |
| `GameHud` | `1` | 0, 1 | Live | D3D11 | Takes the game's own HUD out of generation (hud.h). |
| `CameraMotion` | `1` | 0, 1 | Live | D3D11 | Exact motion for the still world from the game's camera matrices, when one is found and locked. |
| `CameraFit` | `0` | 0, 1 | Live | D3D11 | With no matrix, fit one from flow and depth. Off by default, because it made the camera shake in Tomb Raider. |

## 3. Reflex and frame rate

| Key | Default | Values | When | Route | What it does |
|---|---|---|---|---|---|
| `FpsCap` | `0` | 0 none, 30 to 1000 (1 to 29 read as 30) | Live | All | A cap on frames **shown** (real plus generated). With DLSS-G it is Reflex's frame limit, sent as 1000000/cap. Without DLSS-G, HSF's own limiter holds the game thread after its Present. |
| `ReflexSleep` | `1` | 0, 1 | Live | D3D11, DX9, D3D12-B | The menu's REFLEX OFF/ON. 0 sends Reflex markers but never sleeps the game thread. |
| `ReflexBoost` | `0` | 0, 1 | Live | All | Reflex low latency plus boost (keeps GPU clocks up). On D3D12-G HSF sends the game's Reflex options again with its cap and boost. |
| `GameQueue1` | `1` | 0, 1 | Live | D3D11, DX9 | With DLSS-G, the game's device queues at most 1 frame (`SetMaximumFrameLatency`). |

## 4. D3D12 routes

| Key | Default | Values | When | Route | What it does |
|---|---|---|---|---|---|
| `InGameDlssg` | not set | 0, 1, 2 | Start; HSF writes 2 | D3D12-G | Runs DLSS-G inside a game that ships Streamline 2.x. Once D3D12 has shown more frames than D3D11 (at least 300), HSF writes 2 if the game's `sl.interposer.dll` is loaded. A run that is mostly D3D11 takes 1 or 2 out again. 0 by hand keeps it off for good. |
| `SlBringIn` | not set | 0, 1, 2 | Start; HSF writes 2 | D3D12-B | Starts HSF's own Streamline 2.14 (from `HSF_streamline`) in a D3D12 game without one. 2 means HSF wrote it (once D3D12 has shown more frames than D3D11), and a later run that is mostly D3D11 takes it out again. 1 is set by hand and stays. 0 keeps it off for good. |
| `StreamlineProbe` | `0` | 0, 1 | Start | D3D12-G | Hooks the game's Streamline and logs what it sends (`HSF_sl_probe.log`), changing nothing. |
| `NgxInput` | `1` | 0, 1 | Start | D3D12-B | Takes the game's own DLSS data (motion, depth, jitter, reset) from its NGX calls, in games with DLSS but no Streamline. |
| `SlLog` | `0` | 0, 1 | Start | D3D12-B | Streamline's own logging. It crashed Jusant, so it is off by default. |
| `SlFactoryProxy` | `1` | 0, 1 | Start | D3D12-B | Makes the swap chain through Streamline's factory proxy. Needed with ShortFuse's NR mod. |
| `SlManualHooking` | `0` | 0, 1 | Start | D3D12-B | Streamline's manual hooking mode, for games that crash while Streamline starts. |
| `SlEarlyLoad` | `1` | 0, 1 | Start | D3D12-B | Loads `sl.interposer.dll` when HSF loads, before `slInit`, so add-ons that look for Streamline at start-up find it (RenoDX DLSS picks its Present hook then). |
| `SlNgxPath` | empty | a folder | Start | D3D12-B | Folder of the game's own `nvngx_dlss.dll`, given to Streamline after `HSF_streamline`. Streamline starts NGX before the game does, with only its plugin folders to find DLSS in, so a game that runs DLSS through NGX (Unreal's DLSS plugin) otherwise finds no DLSS and hides it. HSF writes it when it picks this route in a game with DLSS loaded, or after its own search of the game folder (a copy the game has loaded comes first; with several copies the newest). `none:` plus a time means the search found none; it searches again when the game's exe changes. Empty = search at the next start. |
| `SlRouteDlssg` | `-1` | -1, 0, 1 | Start | D3D12-B | DLSS-G's own calls on ReShade's D3D12 device go to the game's native device. -1 = on only when a RenoDX HDR mod is loaded (the RenoDX DLSS NR mods, `renodx-dlss*`, do not count), 0 = never, 1 = always. |
| `DlssgRebuildCamera` | `1` | 0, 1 | Live | D3D12-G | DLSS-G gets `clipToPrevClip` and `prevClipToClip` rebuilt by HSF from the camera, not the game's. |
| `DlssgCameraKey` | `122` (F11) | key code, 0 = none | Live | D3D12-G | Flips `DlssgRebuildCamera` in game. |
| `DlssgCutCheck` | `1` | 0, 1 | Live | D3D12-G | When the camera jumps and the game sent no reset, HSF sends one. |
| `DlssgQuickOff` | `0` | 0, 1 | Live | D3D12-G | Generation stops after about 3 frames with no motion data, not after 250 ms. |
| `DlssgScoreGuard` | `0` | 0, 1 | Live | D3D12-G | Generation stops while the motion score doubts the game's motion. |
| `DlssgHud` | `0` | 0, 1 | Live | D3D12-G | DLSS-G gets the picture from before the game's HUD. On this route this is what PROTECT GAME HUD switches. |
| `DlssgHudWatch` | `0` | 0, 1 | Live | D3D12-G | Watches and logs the HUD draws even with `DlssgHud` off. |
| `DlssgChecks` | `1` | 0, 1 | Live | D3D12-G | The picture check and the score run on the game's device. |
| `DlssgDynamicRes` | `0` | 0, 1 | Start; HSF writes | D3D12-G | This game's motion changes size (dynamic resolution). HSF writes 1 when it sees it, so later runs set DLSS-G's dynamic-resolution flag from the first frame. |

## 5. DirectX 9

| Key | Default | Values | When | Route | What it does |
|---|---|---|---|---|---|
| `Dx9Ex` | `1` | 0, 1 | Start | DX9 | Asks ReShade (6.5+, create_device event 96) to make the game's device Direct3D 9Ex. The bridge can then share textures on the GPU (about 0.01 ms a frame). 0 means plain D3D9 and CPU copies. The menu (ADVANCED, DX9 GPU ROUTE) switches it for the next launch. |
| `Dx9HalfInputs` | `1` | 0, 1 | Start | DX9 | Flow and depth go to DLSS-G at half width and height. |
| `Dx9GpuFence` | `1` | 0, 1 | Start | DX9 | On a Direct3D 9Ex device the bridge hands DLSS-G this frame's copies in the same frame, behind GPU fences, and the game thread does not wait. 0, or a device or driver that cannot, means one frame behind with event queries. The log line of the DX9 bridge says which. |
| `FlowEngine` | `shader` | `shader`, `nvof` | Live | all | Who works out the optical flow. `shader`: the ReShade effect picked in FLOW FROM. `nvof`: NVIDIA Optical Flow, the graphics card's own engine (RTX 20 and newer, from the driver), run on HSF's Direct3D 12 work in the same frame. The effect must still be loaded: its texture gives the size, format and scale. The menu's FLOW ENGINE row switches it in play; the note beside it says what the last frame used. If the engine fails, the effect's flow is used and the log says why. |
| `NvofSize` | `360` | 0 to 2160 | Start | all | Height of the grey image NVIDIA Optical Flow gets; the width keeps the picture's shape. 0 means the picture's own size. Larger is finer and slower (offline at 1080p: about 2 ms at 180, 3.5 ms at 360, 10 ms at 720, on the GPU). |
| `Dx9ExRun` | none | `ex`, `plain` | HSF writes | DX9 | Marks a launch that has not reached its first frame yet. Cleared at the first frame and at a normal exit. |
| `Dx9ExCrash` | `0` | 0, 1 | HSF writes | DX9 | 1 once a 9Ex launch died before its first frame, or the game faulted later on 9Ex where a texture or buffer it asked for was not made. Later launches then use plain D3D9. It goes back to 0 if a plain launch also dies early, or from the menu's TRY AGAIN. |
| `Borderless` | `1` | 0, 1 | Start | DX9 | Turns exclusive fullscreen into a borderless window. DLSS-G needs a window. |
| `ForceBorderless` | `0` | 0, 1 | Live | DX9, D3D11, D3D12 | Makes a game that chose windowed mode borderless fullscreen. Turning it off puts the window back as it was. The menu has it under ADVANCED > WINDOW. |
| `BorderlessCrash` | `0` | 0, 1 | HSF writes | DX9, D3D11, D3D12 | 1 once a game died right after HSF resized its window. `ForceBorderless` then stays off at the next launch, and the menu says GAME CRASHED ON RESIZE: KEPT WINDOWED. |
| `GpuReserve` | `1` | 0, 1 | Live | All | The menu's GPU RESERVE. Only when the card is at 95% load or more and the game's real frames stutter, HSF lowers the FPS cap a step at a time, then gives the frames back once the game stays smooth. Never above your own cap, never under 30 real fps. |
| `VkDeviceHook` | `1` | 0, 1 | Start | Vulkan | HSF hooks `vkCreateDevice` to add shared semaphores and memory to the game's device, so frames reach DLSS-G in the same frame on the GPU. 0, or RTSS in the game, means the bridge waits for its copies on the CPU instead. |
| `LaunchPadQuality` | `auto` | auto, low, medium, high, off | Live | All | LaunchPad's Flow Quality, from the menu's LAUNCHPAD row. `auto` picks from the game's resolution (high up to 1920x1200, medium up to 3440x1440, low above) and also keeps LaunchPad first and its depth settings matched, by itself. `low`, `medium` and `high` are set once, on the click. `off` leaves LaunchPad alone; UNDO sets it. |

## 6. Testing and debugging

| Key | Default | Values | When | Route | What it does |
|---|---|---|---|---|---|
| `Score` | `1` | 0, 1 | Live | D3D11, DX9 | Rates every motion source found, a few times a second (score.h). A score debug view runs it even with 0. |
| `ViewKey` | `119` (F8) | key code | Live | All | Cycles the debug view. |
| `InputKey` | `120` (F9) | key code | Live | D3D11, DX9 | Test: DLSS-G's motion input cycles through each source, then zero motion. It is not saved. |
| `DepthKey` | `121` (F10) | key code | Live | D3D11, DX9 | Test: real depth, or a constant. It is not saved. |
| `RecordKey` | `117` (F6) | key code | Live | D3D11 | Records 30 s of camera data (camera_record.h). |

## 7. Other sections and files

- `[LaunchPadUndo]` `Saved`, `Quality`, `Reversed`, `UpsideDown`: HSF writes these. They hold what LaunchPad had before HSF's first tune from the LAUNCHPAD row, so UNDO can put it back.
- `HSF_profiles\<exe>.ini`, section `[Camera]`, key `Matrix`: the camera matrix HSF locked in this game, so the next run starts with it (D3D11).
- `ReShade.ini [DEPTH] DepthCopyBeforeClears`: ReShade's own setting. HSF offers a button for it in ALL DETAILS when depth reads flat for a long time. It needs a game restart.

## 8. Example files

DirectX 11 or DirectX 9 game, 4x, LaunchPad:

```ini
[HSF]
Extra=1
DlssgFrames=3
Source=launchpad
Overlay=1
```

DirectX 12 game without Streamline (what HSF writes by itself, plus your choices):

```ini
[HSF]
SlBringIn=2
MotionFrom=auto
Extra=1
DlssgFrames=5
Source=auto
```
