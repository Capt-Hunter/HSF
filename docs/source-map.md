# Source map

Every file tracked in `HSF/`, and what it is for.

## Build targets (`HSF/CMakeLists.txt`)

| Target | Output | Sources |
|---|---|---|
| `hsf_addon` | `hsf.addon64` / `hsf.addon32` | `hsf_addon.cpp ui.cpp dlssg_remote.cpp dx9_bridge.cpp gpu_fence9.cpp vk_bridge.cpp vk_hook.cpp score.cpp depth.cpp hud.cpp`. The 64-bit build adds `dlssg_core.cpp hdr_out.cpp` and the D3D12 modules (`HSF_SL_GAME`): `sl_game.cpp sl_bring.cpp ngx_input.cpp frame_checks.cpp compose.cpp debug_view.cpp`, and delay-loads `sl.interposer.dll`. |
| `hsf_dlssg_helper` | `hsf-dlssg-helper64.exe` (64-bit only) | `dlssg_helper.cpp dlssg_core.cpp hdr_out.cpp compose.cpp frame_checks.cpp` |
| `hsf_vk_layer` | `hsf_vk_layer64.dll` and its manifest (64-bit only) | `vk_layer.cpp` |
| `vk_scene` | `vk_scene.exe` (64-bit only) | `tests/vk_scene.cpp` |
| tests | see below | one `tests/*.cpp` each, plus the module it tests |

D3D11 and DX9 games, 32-bit and 64-bit, run DLSS-G in the helper. Streamline 2.14 comes from `../Streamline SDK` (include and `lib/x64/sl.interposer.lib`, delay-loaded). That folder is outside the repo and must exist to build.

## `HSF/src`

### Add-on core

| File | What it holds |
|---|---|
| `hsf_addon.cpp` (≈6,700 lines) | The add-on itself: the ReShade events, `HSF.ini` load and save, the flow sources, route choice, the D3D11 native Present hook and hand-off, all DX9 hooks (Present, Reset, window, 9Ex, crash rule), the Vulkan glue, the game's colour space for HDR, Luma's DLSS data, the D3D12 route glue, the menu (`DrawPanel`), the overlays (`DrawOsd`), the LaunchPad tune, MFG-Unlocker status, the log copy, and `DllMain`. |
| `ui.h`, `ui.cpp` | The menu's look: panel, header, row labels, choice boxes with a crossed-out mask, toggles, bars, segment digits, status box, sparkline, alert boxes. |
| `motion_convention.h` | What a flow source's numbers mean (`uv`, `mvpx`, `px` and their negatives) and how to scale them to frame pixels. |
| `compute_state.h` | Saves and restores the game's D3D11 compute state around HSF's dispatches (score, depth). |
| `gpu_reserve.h` | GPU RESERVE: lowers the FPS cap a step at a time only while the card is full and the game's real frames stutter, then gives the frames back. Reads the card's load from Windows. |
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
| `sl_game.h`, `sl_game.cpp` | The in-game route: HSF's own hooks on the game's `sl.interposer.dll`, logging, drive, camera rebuild, cut check, focus hold, frame tokens. |
| `sl_bring.h`, `sl_bring.cpp` | The bring-in route: loads Streamline 2.14, catches swap-chain creation on the DXGI factory, and sends Reflex, tokens, tags and frames. |
| `ngx_input.h`, `ngx_input.cpp` | Hooks `_nvngx.dll`'s DLSS evaluate and copies the game's motion and depth. |
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
| `sl_bring_test.cpp` | a small D3D12 program with no Streamline, which HSF brings in (needs a GPU and Streamline's DLLs) |
| `ngx_input_test.cpp` | the NGX hook |
| `helper_exit_test.cpp` | the helper dies with its game (job or watchdog); with `hdr10` / `hdr10bug`, an eye test of HDR10 through the helper |
| `gpu_reserve_test.cpp` | the GPU reserve's spike count and governor against a simple card model, and one live load reading |
| `vk_scene.cpp` | a small Vulkan program for bringing up the Vulkan route without a game (run by hand, not in the suite) |

## `HSF/tools`

| File | What it does |
|---|---|
| `build.cmd` | builds x64 and/or x86 (VS 18, Ninja) into `build/x64` and `build/x86` |
| `guard.ps1` | exits 1 while any anti-cheat game runs. Run it before every test tool and install. |
| `install.ps1` | dev install of the add-on, `HSF.fx`, Streamline and the helper into a D3D11 or DX9 game. Keeps an existing `HSF.ini` (`-FreshIni` replaces it), and the old add-on in `_hsf_experimental`. |
| `install_sl_game.ps1` | dev install for D3D12 games (`-LogOnly`, `-BringIn`, `-LaunchPadFrom`, `-DynamicRes`, `-Remove`) |
| `dump_process.ps1` | full-memory minidump of a hung test process |
| `helper_exit_test.ps1` | runs `helper_exit_test` |
| `showcase/` | turns FG on/off recordings into sprite sheets and slow-motion side-by-sides (`README.md`); `make_synthetic.py` makes self-test clips |

## `HSF/third_party`

| Folder | What it is |
|---|---|
| `reshade/include` | ReShade add-on API headers, tag v6.0.0 (from before events 96 and 97, which HSF registers by number), BSD-3-Clause |
| `imgui` | `imgui.h` and `imconfig.h` matching ReShade's ImGui, MIT |
| `minhook` | MinHook, for the DX9 fence hooks and the Vulkan `vkCreateDevice` hook, BSD-2-Clause |
| `nvof` | NVIDIA Optical Flow SDK 5.0 interface headers |
| `vulkan-headers` | Khronos Vulkan-Headers (`VK_HEADER_VERSION` 296), only the C headers HSF includes, Apache-2.0 or MIT |
| `renodx`, `kibblerz-aio` | the licences of code ported from RenoDX (MIT) and DLSS5 ReShade AIO (Apache-2.0) |
