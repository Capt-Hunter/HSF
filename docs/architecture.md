# HSF architecture

This page explains how HSF works inside a game: the routes, how a frame travels to DLSS-G, where motion and depth come from, and how Reflex, the HUD and overlays are handled. File names point into `HSF/src/`. [source-map.md](source-map.md) lists every file.

## 1. The big picture

HSF is one ReShade add-on, `hsf.addon64` or `hsf.addon32`, built from `hsf_addon.cpp` and its modules. It does three things in every game:

1. **Find the inputs.** These are the finished frame, motion for that frame (the game's DLSS data or optical flow), and depth.
2. **Clean them up.** The compose pass sharpens and steadies the flow with depth. Frame checks find scene cuts and still pictures. The HUD and overlays are kept out.
3. **Hand them to DLSS-G** through NVIDIA Streamline 2.14, with Reflex markers and frame tokens on the game's own frame.

DLSS-G only runs on Direct3D 12, only on a real window's swap chain, and only in 64-bit code (Streamline has no 32-bit build). These three limits set the routes.

```
              ┌──────────── D3D12 game ────────────┐      ┌──── D3D11 / DX9 game, 32- or 64-bit ────┐
              │ ships Streamline?                  │      │                                          │
              │   yes → IN-GAME route (sl_game)    │      │ DX9 → DX9 bridge (dx9_bridge) → D3D11    │
              │   no  → BRING-IN route (sl_bring)  │      │ D3D11 frame + flow + depth               │
              │         + NGX input (ngx_input)    │      │   → shared slots (dlssg_slots)           │
              │ DLSS-G inside the game process     │      │   → hsf-dlssg-helper64.exe (64-bit)      │
              └────────────────────────────────────┘      │     own D3D12 device, child window,      │
                                                          │     compose + checks + DLSS-G (dlssg_core)│
                                                          └──────────────────────────────────────────┘
```

## 2. Choosing the route

- **D3D12 with Streamline 2.x** (`sl.interposer.dll` loaded): the in-game route, `InGameDlssg=1`.
- **D3D12 without Streamline**: the bring-in route, `SlBringIn=2`.
- **D3D12 with Streamline 1.x** (no `slGetFeatureFunction` export): HSF installs no hooks, sends nothing, and the menu says the game's Streamline is too old.
- **D3D11**: the helper route.
- **DX9**: the DX9 bridge, then the helper route.
- **Vulkan**: the Vulkan bridge, then the helper route.

Both D3D12 routes must start in `DllMain`, before the game makes its device. So on the first D3D12 present with neither route set, `PickD3d12Route` (in `hsf_addon.cpp`) writes the right key to `HSF.ini` and asks for one restart. A key set to 0 by hand is respected. A route is picked only once D3D12 has shown more frames than D3D11 (at least 300), so a launcher or a D3D11 game that makes a D3D12 device on the side is left alone. `LeaveAutoD3d12Route` takes out an HSF-written route when a run is mostly D3D11 (FF7R runs either way). A game Streamline that is loaded but never started gets HSF's own Streamline instead (bring-in).

## 3. The D3D12 routes

### 3.1 In-game route (`sl_game.cpp`, `sl_game.h`)

This route is for games that ship Streamline 2.x, usually because they have DLSS-G of their own.

- **Hooks.** HSF hooks the game's `sl.interposer.dll` exports with its own hook code. It writes a 5-byte jump at the start of each function to a relay page near the module, and uses a trampoline that holds the moved prologue. If a prologue looks unexpected, HSF installs no hook there and only writes a log line. Game memory is read under structured exception handling, so a bad read never takes the game down.
- **Drive.** With `InGameDlssg=1`, HSF turns DLSS-G on through the game's own Streamline, whatever the game's FG setting says. It sets the multiplier and tags the motion and depth the game gives its DLSS. If the game does not call DLSS, or `MotionFrom=optical`, HSF tags its own flow and depth instead (`slgame::TagOptical`). Unreal's Streamline plugin tags depth and motion for DLSS-G itself and runs DLSS outside Streamline, so no evaluate reaches HSF: the game's own motion tags then count as its DLSS data, and give HSF the frame token and viewport to tag with.
- **Camera.** With `DlssgRebuildCamera=1`, HSF rebuilds `clipToPrevClip` and `prevClipToClip` from the camera. With `DlssgCutCheck`, a camera jump that came with no reset from the game gets one.
- **Readouts.** ReShade presents DLSS-G's generated frames too on this route. So the menu counts a real frame only when the game's own frame token moves forward (`slgame::GameFrames`).
- **Reflex.** The game runs its own Reflex. Some games send `slReflexSetOptions` only once, at start (AC Shadows does). So HSF keeps the game's options and sends them again with its cap and boost whenever those change. The cap goes as 1000000/cap, because Reflex's limit already counts shown frames.
- **Focus guard.** Generation is held off while another process's window is in front, and for a moment after the game's window comes back (`slgame::SetFocusHold`). Crimson Desert drops its device on focus return with FG on.
- **Settle.** Once DLSS-G is turned on, no gap in the motion turns it off again for 1 s (`ON_SETTLE_MS`).

### 3.2 Bring-in route (`sl_bring.cpp`, `sl_bring.h`)

This route is for D3D12 games without Streamline.

1. In `DllMain`, HSF loads Streamline from `HSF_streamline` (or the add-on's folder). With `SlEarlyLoad=1` it loads only `sl.interposer.dll` at first, so other add-ons find it as they start.
2. With the first D3D12 device, HSF calls `slInit`, sets the device, and starts catching swap-chain creation on the DXGI factory's method table.
3. When the game makes its swap chain, HSF makes it through a Streamline factory instead, below ReShade. ReShade's effects then run once per real frame, before DLSS-G takes the frame.
4. Each frame, HSF sends the Reflex markers and frame tokens a Streamline game would send, then the motion and depth (`slbring::Frame`).
5. The motion is the game's DLSS data from `ngx_input`, when there is any and `MotionFrom` allows it. Otherwise it is the flow source plus depth, run through the compose pass on the game's device.
6. `debug_view.cpp` can replace the picture with exactly what DLSS-G was tagged with (the DEPTH and MOTION views).

**The game's own DLSS.** Streamline starts NGX before the game does, with only its plugin folders to look in. A game that runs DLSS through NGX itself (Unreal's DLSS plugin) would then find no DLSS and hide it. HSF gives Streamline the folder of the game's `nvngx_dlss.dll` as a second plugin folder: the copy the game has loaded, else the newest one under the game folder. The answer is saved in `HSF.ini` as `SlNgxPath`, so the search runs once per game (again only when the exe changes).

**Full-size vectors.** When the game's DLSS gives full-resolution vectors with render-size depth (Unreal), the depth fit pass (`depth_fit.cpp`) brings the depth up to the vectors' size on the game's device every frame, so DLSS-G gets both at one size.

**RenoDX HDR mods.** ReShade wraps the D3D12 device, and DLSS-G makes calls of its own on the device it is given. With a RenoDX HDR mod loaded, those calls on ReShade's device showed stale, jumbled frames and removed ReShade's overlay. HSF puts thunks on the method table of ReShade's device: a call made from DLSS-G's code (found by return address, including the driver's NGX copies and NVAPI) goes to the native device, and every other caller still goes through ReShade. It is on only with a RenoDX HDR mod (`SlRouteDlssg=-1`); the RenoDX DLSS mods (`renodx-dlss*`) do not count.

**DXGI guard (`dxgi_guard.cpp`).** ReShade 6.8.0's DXGI factory wraps any adapter `EnumAdapterByLuid` returns, even for interfaces it does not know. D3D11 asks for such internal interfaces when RTSS makes a D3D11 device, and the game crashed. HSF patches that one slot of ReShade's factory method table: interfaces ReShade knows still go through it, others go to the real adapter. The slot is put back when HSF unloads, so ReShade can reload its add-ons.

Start-up switches for games that crash while Streamline starts: `SlFactoryProxy`, `SlManualHooking` and `SlLog` (see settings.md).

### 3.3 NGX input (`ngx_input.cpp`, `ngx_input.h`)

A game that calls DLSS through NGX, without Streamline, hands the driver's `_nvngx.dll` its motion, depth, jitter and reset every frame. HSF hooks that evaluate call, always calls through (so the game's own DLSS still runs), and copies the motion and depth on the game's command list just after it. These copies are what the bring-in route uses as "DLSS" motion. It was proven in Jusant.

## 4. The D3D11 and DX9 routes

### 4.1 The helper (`dlssg_remote.cpp`, `dlssg_helper.cpp`, `dlssg_link.h`, `dlssg_core.*`)

DLSS-G needs Direct3D 12 and 64-bit code. So for D3D11 games (and DX9 games after the bridge), DLSS-G runs in `hsf-dlssg-helper64.exe`, a 64-bit process the add-on starts from `HSF_streamline`.

64-bit D3D11 games use the helper too: RTSS's overlay ended the game when DLSS-G ran in the same process as ReShade. DLSS-G is HSF's only generator; HSF's early in-house presenter and the in-process DLSS-G path were removed after the `pre-cleanup` tag.

**The hand-off, per game frame M** (game thread, the game's D3D11 device):
1. Check slot `M % RING` on the CPU against the `consumed` fence. The game thread never waits on the GPU.
2. Copy the frame, motion and depth into the slot's shared textures (`dlssg_slots.h`).
3. Signal `ready` = M.

**Helper threads:**
- **Presenter thread** (`dlssg_core` `Run`): waits until `ready` reaches a new frame. It copies the frame into its swap chain's buffer and the inputs into DLSS-G's. It runs the compose pass and frame checks on its own D3D12 device, tags, sets constants, and presents. DLSS-G adds and paces its frames. Then it signals `consumed` = M.
- **Reflex thread**: acts on the add-on's messages: a token per game frame, Reflex's sleep (while the game thread waits for the answer) and the frame's markers.
- **Window thread**: owns a **disabled child window** over the game window's client area, which follows its size. Being disabled, it lets mouse and keyboard go to the game. DLSS-G's swap chain sits on this window, so what you see is HSF's window over the game's.
- **Main thread**: waits for the add-on's stop flag or for the game process to end. A job object makes the helper die with the game.

**Life of the helper.** It starts when FRAME GEN is on and there is motion (a flow texture, Luma's data, or NVOF), and stops after 3 s without motion. FRAME GEN OFF leaves it idle (no copies, no Reflex, its window hidden) so ON is instant; the menu's CLOSE HELPER stops it. On some cards a helper that runs without generating passes frames far below the game's own rate, which is why it does not linger. The **GPU guard** turns DLSS-G off and closes the helper when Streamline's GPU fence waits keep running over 500 ms, before the driver resets.

**Keeping ReShade out of the helper.** A packaged (Game Pass) process searches its package folder for DLLs before System32, so a ReShade `dxgi.dll` there loaded into the helper and ran every effect and add-on twice. The helper loads `dxgi`, `d3d12` and `d3d11` by their full System32 path, logs which `dxgi.dll` it has, and tells the add-on (`foreign_dxgi`) when it is not Windows' own; the menu shows a red problem line. The add-on does nothing inside the helper.

**Pacing.** Once the helper takes frames, the game waits for it to take the last one, at the frame start before input is read. A wait that runs out pauses this for 1 s.

`dlssg_link.h` is the shared block between the two processes. It uses fixed-size fields only, so the 32-bit add-on and the 64-bit helper agree on its layout. The shared textures and fences are made on the game's device, and their NT handles are duplicated into the helper.

**Older RTX cards.** Mods that unlock multi-frame generation (MFG-Unlocker, `dlssg_for_sm86`) hook `slInit` and NVIDIA's DLL loads, so they must be in the process before `sl.interposer.dll`. In the helper they would never load by themselves. So the helper loads the user's own copy: from `HSF_streamline\mods`, or the one next to the game's exe (matched by its version resource's ProductName). HSF ships none of them. The add-on reads MFG-Unlocker's status file for the menu line.

### 4.2 The game side on D3D11 (`hsf_addon.cpp`)

- At ReShade's `finish_effects`, HSF looks up the flow source's texture by name (`SOURCES[]`), the depth that `HSF.fx` binds, and a clean copy of the frame from before overlays.
- HSF hooks the native DXGI `Present`. There it hands the finished frame, flow and depth to the helper.
- `depth.cpp` copies the game's depth buffer on the game's own device, once a frame, into an R32_FLOAT texture at the flow's size. Game depth buffers usually cannot be shared directly (R24G8, multisampled). A few times a second it reads a copy back, without ever waiting, to find out whether depth is reversed, upside down or flat.
- `hud.cpp` finds the moment the game starts drawing its HUD: the first blended draw with no depth test on the back buffer. It copies the back buffer there and again when ReShade's effects start. The difference between the two is the HUD, so DLSS-G gets a HUD-less frame. A guard drops it when "the HUD" covers most of the picture (a vignette or a fade).
- `camera.h` scans what the game writes to its constant buffers for matrices shaped like a camera. In the helper, a chooser locks the one whose motion explains the flow. The still world then gets exact motion from the camera. When none locks, the game stays on flow.
- `score.cpp` rates each motion source, a few times a second. It warps the earlier of two real frames along the motion and compares it to the later one. The DEBUG views show this.

### 4.3 The DX9 bridge (`dx9_bridge.cpp`, `dx9_bridge.h`)

The helper only takes D3D11 textures. So for DX9 games HSF makes a D3D11 device of its own on the game's GPU, and copies each frame, the flow (LaunchPad on DX9) and HSF.fx's depth copy into it. From there it works like a D3D11 game.

- **GPU route**: the game's device is D3D9Ex, so D3D9 can make shared textures that D3D11 opens. With `Dx9GpuFence=1` (the default) it hands over this frame's copies in the same frame and the game thread never waits: the game's GPU queue signals a kernel fence behind its copies, and D3D11 waits for it on the GPU (`gpu_fence9.cpp`, ported from RenoDX's `gpu_fence.hpp`; the device's kernel handles are read through three MinHook hooks in `win32u.dll`). Where the fences cannot be made it runs one frame behind, with event queries. A GPU wait has no time limit, so a watchdog checks both fences once a frame: one that stands still for half a second, or is a dozen signals behind, is signalled from the CPU and the bridge uses the event queries for the rest of the session (without it a lost signal means a blank screen and a game at one frame a second). It costs about 0.01 ms. HSF gets 9Ex by asking ReShade 6.5+ to raise the API version at its `create_device` event (number 96, not in HSF's older headers). ReShade then moves managed-pool resources itself.
- **CPU route**: a plain D3D9 device. Frame N's read-back is queued with an event query at N and taken across at N+2, so the game thread never waits. The upload goes through a dynamic texture, split over the Windows thread pool.
- **Half inputs** (`Dx9HalfInputs=1`): flow and depth go at half width and height.
- **The 9Ex crash rule**: `Dx9ExRun` marks a launch until its first frame. If a 9Ex launch dies before its first frame, `Dx9ExCrash=1` and later launches use plain D3D9. MGR needed this. The same happens when the game faults later in its own code on a thread where the 9Ex device did not make a texture or buffer it asked for . AC Revelations did that with DXT textures that are not whole blocks (2x16, 16x1), which Direct3D 9 refuses; on a 9Ex device HSF asked for, HSF now makes those as whole blocks (4x16, 16x4: the same bytes), and the game runs. The menu (ADVANCED, DX9 GPU ROUTE) shows the state and switches the GPU route on or off for the next launch.
- **Hooks**: the native D3D9 `Present`, `PresentEx` and the implicit swap chain's `Present` are patched at the **first frame**, not at device creation. Hooked at creation, AC Revelations' Present never reached HSF. `ReclaimSlots9` takes a vtable slot back when another hook puts the system Present back (Dragon's Dogma with Uplift). `AdoptSwap9` follows an additional swap chain when only that one presents (The Darkness II).
- **Window**: exclusive fullscreen becomes borderless, because DLSS-G needs a window. ReShade 6.5+ moved `create_swapchain` to event 97 with an API argument, so `RegisterCreateSwapchain` registers by the running ReShade's version. A game that chose windowed mode can be made borderless from the menu (`ForceBorderless`) in DX9, D3D11 and D3D12, live both ways: `KeepBorderless` follows the window of the API the game runs on, saves its style and position on the first change, and puts them back when the option is turned off. Window changes run on a short-lived HSF thread, never inside ReShade's frame callback (The Darkness II rebuilt its swap chain mid-callback), so a game that presents from a thread that does not own its window (Dragon Age, Tomb Raider) never waits on them; Dragon Age hung when the change was made on its present thread.
- **Reset**: HSF's D3D9 copies are freed at `destroy_swapchain` and `destroy_effect_runtime`, before every device Reset.

### 4.4 The Vulkan bridge (`vk_bridge.cpp`, `vk_hook.cpp`, `vk_extend.h`)

Like DX9, a Vulkan game gets a D3D11 device of HSF's own on the same GPU, picked by the Vulkan device's vendor and device ID, and from there it works like a D3D11 game.

- **Shared textures**: HSF's D3D11 device makes three sets of shared textures (NT handles) and the game's Vulkan device imports them through `VK_KHR_external_memory_win32`, which ReShade 6.8's layer turns on. At ReShade's present (back buffer in present state) the game's queue copies the frame, LaunchPad's flow and `HSF.fx`'s depth copy into a free set, through ReShade's immediate command list. At `finish_effects` the back buffer from before overlays is copied too, as the HUD-less image.
- **Same frame on the GPU**: ReShade does not turn on external semaphores. So the add-on hooks `vulkan-1.dll`'s `vkCreateDevice` with MinHook from `DllMain` (ReShade loads add-ons inside the game's `vkCreateInstance`, so the hook is in before the device) and adds `VK_KHR_external_semaphore(_win32)`, external memory and the timeline semaphore feature (`vk_extend.h`). If the driver refuses, the device is made exactly as the game asked. A shared D3D11 fence is then imported as a timeline semaphore: the game's queue signals it behind the copies, and D3D11 waits on the GPU. Another add-on that already gave the device shared fences works the same way.
- **CPU wait**: without semaphores (RTSS in the process keeps the hook out, or `VkDeviceHook=0`), HSF waits on the CPU for ReShade's fence, up to 16 ms, and still hands over the same frame. A frame that takes longer goes one frame behind.
- **Safety net**: if the game's GPU never signals the shared fence for 2 s, HSF signals it from the CPU so D3D11 goes on, and frames go one frame behind from then on.
- **Window**: by default (`VkOverlayWindow=1`) DLSS-G presents from a click-through top-level window of its own over the game, as AeonSR does, which keeps the mouse smooth. D3D11 and DX9 do the same (`OverlayWindow=1`). While ReShade's menu is open with MFG-Unlocker loaded, the window docks into the game's window as a disabled child, enabled only while the unlocker's own menu is open, so both menus work; it goes back on top when the menus close. 0 uses a child window, as before.
- **Reflex**: the sleep runs at ReShade's `finish_present` (event 100, after `vkQueuePresentKHR`, registered by number), so it is counted in the frame. Older ReShade keeps it at present.
- **Depth**: the depth module runs on HSF's D3D11 device on the bridged depth. Depth of another shape than the picture (an emulator's 16:9 picture on a wider window) is fitted into the picture's middle, and a square depth under a non-square picture is taken for a shadow map and replaced by a constant.
- **Shut-down**: the bridge goes at `destroy_effect_runtime` (queue idle first). At `destroy_device` ReShade has already removed the device from its tables.
- **Fallback layer**: `vk_layer.cpp` (`VK_LAYER_HSF_bridge`) does the same as the hook from outside, for a game whose device the hook misses. It needs no registry entry: it is turned on per run through `VK_ADD_LAYER_PATH`.

### 4.5 HDR (`hdr_out.cpp`, `dlssg_core.cpp`)

All HDR code follows RenoDX (MIT): its colour maths, its HDR display check and its colour space mapping.

- **HDR10 games**: the add-on reads the colour space of the game's swap chain from the private data that RenoDX, ReShade and Special K write on it (`SKID_SwapChainColorSpace`), with ReShade's own value as the fallback and for Vulkan. It goes to the helper in `dlssg_link.h` (`color_space`), and the helper's swap chain takes the same colour space with `SetColorSpace1`, as RenoDX's `ChangeColorSpace` does. Without that, an HDR10 frame would be shown as SDR.
- **FP16 (scRGB) games**: DLSS-G takes no FP16. The frame and the HUD-less image are turned into HDR10 (when the display is in HDR mode) or sRGB, into an R10G10B10A2 swap chain.

## 5. Motion and depth

### 5.1 Flow sources (`SOURCES[]` in `hsf_addon.cpp`)

| Source | Texture | Size | Meaning |
|---|---|---|---|
| iMMERSE LaunchPad | `Deferred::MotionVectorsTex` | full | delta UV, this frame to the previous one |
| LumeniteFX Kernel | `Kernel::tFlow` | 1/8 | delta UV |

HSF reads each source's own texture by its variable name among the loaded effects, not a copy made by another add-on. The variable is looked up again after every effect reload. LaunchPad computes flow only when an effect asks, and `HSF.fx` is the effect that asks. The flow's meaning is known per source (`convention`), so no calibration is needed.

**NVIDIA Optical Flow** (`nvof_flow.cpp`, ported from kibblerz's DLSS5 ReShade AIO, Apache-2.0) can make the flow instead, on every route (`FlowEngine=nvof`). It runs on the GPU's optical flow engine at `NvofSize` height (default 360), and needs no effect loaded: then it makes motion at the depth's size. It is a backup: on SHADER with no flow effect for 10 s, HSF switches to it by itself, and a loaded effect takes over again. Shader flow looks better and runs faster.

**Luma.** A Luma mod adds DLSS to a D3D11 game and calls NGX's D3D11 functions with the game's own vectors and depth. `ngx_input` hooks those calls, always calls through, and keeps the inputs. While they keep coming and MOTION is not FLOW, DLSS-G gets them in place of the flow (the menu says LUMA). The mod pauses its DLSS in cutscenes and menus, so the flow takes over there, at the same size; with `FlowPause=1` the flow effect is off while Luma's data comes.

### 5.2 Compose pass (`compose.cpp`, `compose.h`)

Optical flow is soft at the edges of objects and spills across them, while depth has sharp edges. The compose pass fixes the flow in three steps:

1. **Dilation.** For each pixel it takes the nearest surface in its 3x3, so a foreground edge keeps the foreground's motion (as FSR 3 does).
2. **Joint bilateral filter.** It averages the motion around that surface, weighing each sample by how close its depth is.
3. **Steadying over time.** Each pixel follows its motion back to last frame and blends in the motion it had there, but only if it is the same surface and the two motions nearly agree. A real change of motion goes through at once.

If camera motion is locked (D3D11), the still world takes the camera's exact motion wherever the flow says about the same. It also runs the scale-up of Lumenite's 1/8 flow to depth size. It runs on HSF's own D3D12 device in the helper, and on the game's device on the bring-in route, so one shader serves every API.

Its cost grows with the pixel count, so above 1920x1200 it works at a smaller size (`WorkDivisor`): half from 1440p to 4K, a third above, with the result at full size.

### 5.3 Frame checks (`frame_checks.cpp`, `frame_checks.h`)

These work on a small luma grid of every frame:

- **Scene cuts**: most cells change *and* the histogram's shape changes. HSF then sends DLSS-G's reset.
- **Still pictures** (menus, loading screens): get zero motion.
- **HUD mask**: what stays put while the flow says it moves gets zero motion.
- **UI alpha**: the finished frame against the copy from before overlays. Where they differ is overlay, and DLSS-G keeps it out of generation.

## 6. Reflex and pacing

Reflex runs on the game's own frame. Right after the game's Present returns (FrameStart), the game thread sleeps on Reflex and HSF marks simulation start. Simulation end and render submit go around the hand-off. Every slot carries the game frame's Streamline index, and the presenter's constants, tags and present markers use it. So Reflex's FPS cap holds the game itself back, not just HSF's presents.

In the 32-bit case Streamline lives in the helper, but the sleep must stop the game thread. So the add-on sends a message and waits for the helper's answer, with a time-out. The driver's latency report (`reflex_report.h`) proves the driver sees the markers. It is shown as LATENCY in the menu.

DLSS-G paces its own generated frames.

## 7. Menu and overlay

- The menu is a ReShade overlay window called "HSF". `DockMenuWithReShade` docks it once a run next to ReShade's Home and Add-ons tabs.
- `DrawPanel` draws it with `ui.cpp`, a hand-drawn look over invisible ImGui items. It has segment-shape digits (which stay sharp at any size), outlined choice boxes, thin bars and one colour per row label.
- `DrawOsd` draws the SIMPLE and ADVANCED overlays and the D3D11 debug views.
- The menu works from `ReadMenuView()`, a snapshot of the route, API, rates and problem text.
- The menu writes `HSF.ini` through `SaveIni` only when something changed.

## 8. The HSF.fx shader (`HSF/shaders/HSF.fx`)

`HSF.fx` does two small jobs:

1. **The LaunchPad request.** If `MartysMods/mmx_deferred.fxh` exists, its technique asks LaunchPad for flow each frame. Its place in the effect list does not matter.
2. **The depth copy.** It binds ReShade's depth texture, so the add-on can read what Generic Depth picked. On DX9 it also makes the depth copy (`HSFDepthCopy`) that the bridge takes across.

Without LaunchPad installed, it draws one point into a 1x1 texture.

## 9. Logs

| Log | Written by |
|---|---|
| `ReShade.log` | the add-on (every 10 s: route, rates, bridge timings, depth decisions) |
| `HSF_sl_bring.log` | the bring-in route and NGX input |
| `HSF_sl_probe.log` | the in-game route |
| `HSF_streamline\hsf_dlssg_game.log` | the helper link, game side |
| `HSF_streamline\hsf_dlssg.log` | the helper, including Streamline's own warnings and errors |

COPY LOG TO CLIPBOARD in the menu collects HSF's lines from `ReShade.log` plus the active route's log.
