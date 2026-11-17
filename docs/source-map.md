# Source map

Every file tracked in `HSF/`, and what it is for.

## Build targets (`HSF/CMakeLists.txt`)

| Target | Output | Sources |
|---|---|---|
| `hsf_addon` | `hsf.addon64` / `hsf.addon32` | `hsf_addon.cpp ui.cpp dlssg_remote.cpp dx9_bridge.cpp gpu_fence9.cpp vk_bridge.cpp vk_hook.cpp score.cpp depth.cpp hud.cpp dxgi_guard.cpp safe_try.cpp hsf_log.cpp`. The 64-bit build adds `dlssg_core.cpp hdr_out.cpp` and the D3D12 modules (`HSF_SL_GAME`): `sl_game.cpp sl_bring.cpp depth_fit.cpp ngx_input.cpp frame_checks.cpp compose.cpp debug_view.cpp`, and delay-loads `sl.interposer.dll`. |
| `hsf_dlssg_helper` | `hsf-dlssg-helper64.exe` (64-bit only) | `dlssg_helper.cpp dlssg_core.cpp hsf_log.cpp hdr_out.cpp compose.cpp frame_checks.cpp nvof_flow.cpp` |
| `d3d12_scene` | `d3d12_scene.exe` (64-bit only, built only when the NGX SDK is in the Luma checkout next to HSF) | `tests/d3d12_scene.cpp` |
| `hsf_vk_layer` | `hsf_vk_layer64.dll` and its manifest (64-bit only) | `vk_layer.cpp` |
| `vk_scene` | `vk_scene.exe` (64-bit only) | `tests/vk_scene.cpp` |
| tests | see below | one `tests/*.cpp` each, plus the module it tests |

D3D11 and DX9 games, 32-bit and 64-bit, run DLSS-G in the helper. Streamline 2.14 comes from `../Streamline SDK` (include and `lib/x64/sl.interposer.lib`, delay-loaded). That folder is outside the repo and must exist to build.

## `HSF/src`

### Add-on core

| File | What it holds |
|---|---|
| `hsf_addon.cpp` (≈8,700 lines) | The add-on itself: the ReShade events, `HSF.ini` load and save, the flow sources, route choice, the D3D11 native Present hook and hand-off, all DX9 hooks (Present, Reset, window, 9Ex, crash rule), the Vulkan glue, the game's colour space for HDR, Luma's DLSS data, the D3D12 route glue, the menu (`DrawPanel`), the overlays (`DrawOsd`), the LaunchPad tune, MFG-Unlocker status, the log copy, and `DllMain`. |
| `ui.h`, `ui.cpp` | The menu's look: panel, header, row labels, choice boxes with a crossed-out mask, toggles, bars, segment digits, status box, sparkline, alert boxes. |
| `motion_convention.h` | What a flow source's numbers mean (`uv`, `mvpx`, `px` and their negatives) and how to scale them to frame pixels. |
| `compute_state.h` | Saves and restores the game's D3D11 compute state around HSF's dispatches (score, depth). |
| `gpu_reserve.h` | GPU RESERVE: lowers the FPS cap a step at a time only while the card is full and the game's real frames stutter, then gives the frames back. Reads the card's load from Windows. |
| `safe_try.h`, `safe_try.cpp` | CRASH PROTECTION's hook steps: marks a new way in `HSF_safe_try.ini` before it is used, and turns it off for the game when a launch ended while it was marked. The steps' names and menu tooltips. |
| `hsf_log.h`, `hsf_log.cpp` | `HSF.log`: one file for the add-on, the Streamline routes, the helper and the Vulkan layer. Fresh per game run, the last run kept as `HSF.previous.log`, one `WriteFile` per line. |
| `version.h.in` | CMake makes `version.h` from it, with `HSF_VERSION` from the project version in `CMakeLists.txt`, the one place the version lives. |
| `motion_timing.h` | The timings that decide between the game's DLSS data, HSF's flow and none, for the source pick and the in-game route, in one place. |
| `score_limits.h` | The motion score's thresholds, shared by `score.cpp` and `frame_checks.cpp` so both rate alike. |
| `gpu_spans.h` | GPU time of a few spans of D3D11 work in one frame, read back a few frames later and never waited for. A report only. |

### Inputs and clean-up

| File | What it holds |
|---|---|
| `depth.h`, `depth.cpp` | The game's depth into R32_FLOAT at the flow's size (D3D11, game device), plus the read-back that finds reversed, upside-down and flat depth, and the DEPTH debug view. |
| `compose.h`, `compose.cpp` | The compose pass: depth-guided dilation, bilateral filter, temporal steadying, camera motion, and scaling of small flow. On D3D12 (the helper or the game's device). |
| `frame_checks.h`, `frame_checks.cpp` | Scene cuts, still pictures, the HUD mask and UI alpha, from a luma grid. On D3D12. |
| `hud.h`, `hud.cpp` | The game's own HUD taken out of the frame DLSS-G gets (D3D11): Watch, Remover, Guard. |
| `camera.h` | Camera matrices: the Scanner (game thread), the Chooser (helper) and `FitClipToPrev`. No graphics API, so it is testable. |
| `camera_record.h` | The F6 recording format (`*-matrices.bin` from the add-on, `*-flow.bin` from the helper). |
| `score.h`, `score.cpp` | The motion quality score and its debug views (D3D11, game device). |
| `launchpad_tune.h` | The rules for the "Tune for FG" button (quality, order, depth defines), kept apart from ReShade so they can be tested. |

### DLSS-G: helper route

| File | What it holds |
|---|---|
| `dlssg.h` | The D3D11/DX9 presenter's public face: `Start`, `Submit`, `Stats`, `Running`. |
| `dlssg_remote.cpp` | The game side of the helper route: makes the shared slots and fences, starts the helper, and sends the Reflex messages. |
| `dlssg_link.h` | The shared block between the add-on and the helper, with fixed-size fields only. |
| `dlssg_slots.h` | The rings of shared textures on the game's D3D11 device. |
| `dlssg_core.h`, `dlssg_core.cpp` | The presenter side, in the helper: Streamline set-up, the swap chain, options, tags, constants, Present, and mod loading (MFG-Unlocker). |
| `hdr_out.h`, `hdr_out.cpp` | DLSS-G takes no FP16 (scRGB): an FP16 game's frame and HUD-less image are converted to HDR10 (display in HDR mode) or sRGB, into an R10G10B10A2 swap chain. Colour maths from RenoDX (MIT). |
| `dlssg_helper.cpp` | `hsf-dlssg-helper64.exe`: the presenter, Reflex, window and main threads. |
| `reflex_report.h` | Boils the driver's Reflex latency report down to averages. |

### DLSS-G: D3D12 routes (64-bit only, `HSF_SL_GAME`)

| File | What it holds |
|---|---|
| `sl_game.h`, `sl_game.cpp` | The in-game route: HSF's hooks on the game's `sl.interposer.dll` (plugin hook, load watch, two copies), logging, drive, motion order, camera rebuild, cut check, focus hold, frame tokens. |
| `hook_patch.h`, `hook_patch.cpp` | HSF's own hook patcher: 5-byte jump, relay page near the module, trampoline, the full length decoder, chaining behind another tool's jump. Its own leaf lock. |
| `sl_bring.h`, `sl_bring.cpp` | The bring-in route: loads Streamline 2.14, catches swap-chain creation on the DXGI factory, sends Reflex, tokens, tags and frames, and shuts Streamline down before the game exits. |
| `fp16_swap.h`, `fp16_swap.cpp` | Bring-in route, FP16 (scRGB) games: the swap chain is made as R10G10B10A2, the game draws into FP16 textures of HSF's own, and each frame is converted to HDR10 or sRGB before Streamline takes it. |
| `depth_fit.h`, `depth_fit.cpp` | Compute pass that brings the game's render-size depth up to the size of its full-size DLSS vectors, since DLSS-G takes one size for both. |
| `dxgi_guard.h`, `dxgi_guard.cpp` | Keeps ReShade's DXGI factory from wrapping adapter interfaces it does not know (d3d11.dll asks for one when RTSS makes its device), which crashed the game. |
| `ngx_input.h`, `ngx_input.cpp` | Hooks `_nvngx.dll`'s DLSS evaluate (D3D12 and D3D11) and copies the game's motion and depth. D24 and D16 depth goes through a conversion shader. |
| `ngx_call.h` | One reading of a DLSS call (part in use, scale, jitter, flags, reset) for all three NGX hooks. |
| `debug_view.h`, `debug_view.cpp` | The D3D12 DEPTH and MOTION views: exactly what DLSS-G was tagged with, drawn over the picture. |

### DX9

| File | What it holds |
|---|---|
| `dx9_bridge.h`, `dx9_bridge.cpp` | HSF's D3D11 device for DX9 games, with the GPU (9Ex shared) and CPU (read-back) routes, half inputs, the depth grid and the HUD-less lane. |
| `gpu_fence9.h`, `gpu_fence9.cpp` | GPU fences between the game's D3D9Ex device and HSF's D3D11 device, so the GPU route needs no CPU wait and runs in the same frame. Ported from RenoDX's `gpu_fence.hpp` (MIT); uses MinHook. |
### Vulkan

| File | What it holds |
|---|---|
| `vk_bridge.h`, `vk_bridge.cpp` | The Vulkan bridge: HSF's D3D11 device for Vulkan games, shared textures imported into the game's device, the copies at present, the same-frame semaphore route, the CPU wait, the safety net. |
| `vk_hook.h`, `vk_hook.cpp` | The add-on's MinHook hook on `vulkan-1.dll`'s `vkCreateDevice`, installed from `DllMain`, kept out while RTSS is in the process. |
| `ngx_vk.h`, `ngx_vk.cpp` | Hooks NGX's Vulkan DLSS calls (a DLSS mod on a Vulkan game) and copies the vectors and depth into images of HSF's own, which the bridge sends in place of the flow. |
| `vk_extend.h` | What the bridge adds to the game's device request (external memory and semaphores, timeline semaphores), shared by the hook and the layer. |
| `vk_layer.cpp`, `hsf_vk_layer64.json` | `VK_LAYER_HSF_bridge` (`hsf_vk_layer64.dll`, 64-bit only): the same additions from a Vulkan layer, the fallback for a game the hook misses. Not shipped in releases. |

### Other

| File | What it holds |
|---|---|
| `nvof_flow.h`, `nvof_flow.cpp` | NVIDIA Optical Flow as a motion source (64-bit: the helper and the D3D12 routes). Shrinks the picture to a grey image, asks the driver's engine (`nvofapi64.dll`) for flow both ways with cost, and a compute shader turns the answer into motion in the effect's size, format and scale, so the compose pass and DLSS-G take either. The queue waits for the engine on the GPU; a watchdog lets a missing signal go. Ported from DLSS5 ReShade AIO (Apache-2.0). |

## `HSF/shaders`

| File | What it holds |
|---|---|
| `HSF.fx` | Asks LaunchPad for flow, binds ReShade's depth, and makes the DX9 depth copy. |

## `HSF/tests`

Offline tests and probes. CMake builds the ones it lists into `build/x64` (D3D12 tests are 64-bit only) or `build/x86`. `helper_exit_test` is 32-bit only. The early probes are kept outside the repo; commits before the `pre-cleanup` tag have them.

| File | What it checks |
|---|---|
| `compose_test.cpp` | the compose pass on made-up scenes |
| `frame_checks_test.cpp` | cut, still and HUD classification |
| `hdr_out_test.cpp` | FP16 to HDR10 and sRGB codes against the ST.2084 and sRGB curves |
| `depth_test.cpp` | the depth copy and its direction finding |
| `score_test.cpp` | the motion score |
| `nvof_test.cpp` | NVIDIA Optical Flow on a picture that moves by a known amount: direction, size and time |
| `dx9_bridge_test.cpp` | the DX9 bridge's GPU route on a D3D9Ex device of its own, with and without GPU fences, through a device Reset |
| `hud_test.cpp` | HUD watch and removal |
| `camera_test.cpp` | camera scanner, chooser and fit on synthetic matrices |
| `camera_replay.cpp` | replays an F6 recording through the same camera code |
| `debug_view_test.cpp` | the D3D12 debug view |
| `launchpad_tune_test.cpp` | the tune rules |
| `sl_game_test.cpp` | the hooks on a real `sl.interposer.dll` copy |
| `hook_test.cpp` | the patcher on made-up functions: the full length decoder, a function that is not 8-byte aligned, and the safe-try switch for the decoder |
| `sl_copies_test.cpp`, `fake_interposer.cpp`, `fake_interposer.def` | two `sl.interposer.dll` copies loaded at once (a stray one and the game's), with a stand-in interposer |
| `safe_try_test.cpp` | a mark cleared after a stable run, a mark left by a dead launch turns its step off, a normal exit clears the marks |
| `hsf_log_test.cpp` | `HSF.log`: fresh per run, previous run kept, appends from a reload or a second process, lines never mix |
| `d3d12_scene.cpp` | a stand-in D3D12 game, with or without Streamline, DLSS through Streamline or NGX, SDR, HDR10 or FP16, for `tools\stack_sweep.ps1` (not in the suite) |
| `sl_bring_test.cpp` | a small D3D12 program with no Streamline, which HSF brings in (needs a GPU and Streamline's DLLs) |
| `ngx_input_test.cpp` | the NGX hook |
| `depth_fit_test.cpp` | the depth fit pass |
| `helper_exit_test.cpp` | the helper dies with its game (job or watchdog); with `hdr10` / `hdr10bug`, an eye test of HDR10 through the helper |
| `gpu_reserve_test.cpp` | the GPU reserve's spike count and governor against a simple card model, and one live load reading |
| `vk_scene.cpp` | a small Vulkan program for bringing up the Vulkan route without a game (run by hand, not in the suite) |

## `HSF/tools`

| File | What it does |
|---|---|
| `build.cmd` | builds x64 and/or x86 (VS 18, Ninja) into `build/x64` and `build/x86` |
| `test.cmd`, `test.ps1` | runs every offline test in `build/x64` and `build/x86`, one PASS or FAIL line each and a total; exit code 0 = all passed. Calls `guard.ps1` itself. |
| `stack_sweep.ps1`, `sweep_renodx_dlss.ini` | runs `d3d12_scene.exe` with the real mod stack (ReShade 6.8.0, RenoDX DLSS, GITC Uplift; Streamline none, 2.14 or 2.13) in every mix that has broken before, one PASS or FAIL per case |
| `hang_stacks.py` | reads a full memory dump of a hung 64-bit game and prints where each thread is |
| `guard.ps1` | exits 1 while any anti-cheat game runs. Run it before every test tool and install. |
| `install.ps1` | dev install of the add-on, `HSF.fx`, Streamline and the helper into a D3D11 or DX9 game. Keeps an existing `HSF.ini` (`-FreshIni` replaces it), and the old add-on in `_hsf_experimental`. |
| `install_sl_game.ps1` | dev install for D3D12 games (`-LogOnly`, `-BringIn`, `-LaunchPadFrom`, `-DynamicRes`, `-Remove`) |
| `dump_process.ps1` | full-memory minidump of a hung test process |
| `helper_exit_test.ps1` | runs `helper_exit_test` |
| `showcase/` | `record.ps1` records the screen with FG off and on (every frame the display showed); `showcase.py` turns the recordings into sprite sheets and slow-motion side-by-sides (`README.md`); `make_synthetic.py` makes self-test clips |

## `HSF/third_party`

| Folder | What it is |
|---|---|
| `reshade/include` | ReShade add-on API headers, tag v6.0.0 (from before events 96 and 97, which HSF registers by number), BSD-3-Clause |
| `imgui` | `imgui.h` and `imconfig.h` matching ReShade's ImGui, MIT |
| `minhook` | MinHook, for the DX9 fence hooks and the Vulkan `vkCreateDevice` hook, BSD-2-Clause |
| `nvof` | NVIDIA Optical Flow SDK 5.0 interface headers |
| `vulkan-headers` | Khronos Vulkan-Headers (`VK_HEADER_VERSION` 296), only the C headers HSF includes, Apache-2.0 or MIT |
| `renodx`, `kibblerz-aio` | the licences of code ported from RenoDX (MIT) and DLSS5 ReShade AIO (Apache-2.0) |
