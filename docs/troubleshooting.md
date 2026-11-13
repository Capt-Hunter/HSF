# Troubleshooting

Start with the menu's status box. It names the route and gives the reason under WAITING or PROBLEM. Then use COPY LOG TO CLIPBOARD (DEBUG section) and read, or send, the log.

## 1. Symptoms

| What you see | Likely cause | What to do |
|---|---|---|
| The HSF tab does not appear | ReShade without add-on support, or the wrong bitness of add-on | Install ReShade "with full add-on support". Use `hsf.addon32` for 32-bit games and `hsf.addon64` for 64-bit ones. |
| "no motion vectors" or FLOW FROM says NONE FOUND | No flow source is loaded or compiled | Install LaunchPad (or LumeniteFX with its `include` folder). Check that it compiles in ReShade's Home tab. |
| LumeniteFX fails to compile | Only `lumenite_Kernel.fx` was copied | Copy the whole set with `include/lumenite_*.fxh`. |
| D3D12: "D3D12: HSF HAS SET UP ITS D3D12 ROUTE. RESTART THE GAME ONCE" | First run in a D3D12 game | Restart the game. |
| D3D12: "HSF's D3D12 routes are off in HSF.ini" | `SlBringIn=0` or `InGameDlssg=0` set by hand | Remove the key. HSF picks the route again at the next launch. |
| "This game's Streamline is too old for HSF (1.x)" | The game ships Streamline 1.x (Returnal) | Not supported in version 1. Use the game's own frame generation. |
| Red box "THIS GAME HAS ITS OWN FRAME GENERATION" | In-game route | Turn the game's own FG off in its settings, then press OK. |
| Orange box "5X AND 6X ARE NOT RECOMMENDED WITH FLOW" | D3D12 game with DLSS, flow at 5x or 6x | Set MOTION to DLSS, or go to 4X or below. |
| MULTIPLIER boxes crossed out above 2X | DLSS-G reports only 2x on this card | RTX 20/30/40: install MFG-Unlocker yourself. RTX 50 has 6x. |
| Smearing on the HUD | The HUD is in the generated frames | Turn PROTECT GAME HUD on (ADVANCED). |
| RTSS or Steam counter flickers or ghosts | The overlays are generated (D3D11/DX9) | Turn KEEP OVERLAYS OUT on. |
| Ghosting at object edges with flow | Flat or wrong depth | DEBUG > VIEW > DEPTH: near should be white. Fix DEPTH DIRECTION or WAY UP, or pick a quality in the LAUNCHPAD row. If depth is flat, try ReShade's "Copy depth before clears" (ALL DETAILS), which needs a restart. |
| Uneven frames | No FPS cap, or the game's base rate swings | Set an FPS CAP a few frames below your monitor's Hz. A cap at exactly the refresh rate waits on VSync. |
| FRAME GEN turned itself off, red GPU guard line | DLSS-G's GPU waits kept timing out: the card could not keep up | Lower the multiplier, the game's settings or the FPS cap, then turn FRAME GEN on again. |
| Fps far below the game's own with frame generation off | The helper still runs while OFF (for a quick ON) | Press CLOSE HELPER beside FRAME GEN. |
| Red line "ReShade loaded into HSF's helper (Game Pass)" | A packaged game made ReShade load into `hsf-dlssg-helper64.exe`, so effects and add-ons run twice | Update HSF: the helper now loads Windows' graphics DLLs by their full path. If the line stays, send `HSF_streamline\hsf_dlssg.log`. |
| Yellow box "NVOF IS A BACKUP ONLY" | No flow effect loaded for 10 s, so NVIDIA Optical Flow took over | Install LaunchPad or Lumenite, or press USE SHADER once one is loaded. |
| Luma game: MOTION stays on FLOW | The game runs in DX12, or DLSS is off in Luma's settings | Start the game in DX11 and turn on DLSS in Luma. The menu offers SWITCH TO LUMA (DLSS). |
| Emulator or game: "No depth found" | Depth of another shape than the picture (a 16:9 picture on a wider screen), which ReShade turns away | DEBUG > ALL DETAILS: press "Accept depth of another shape", then restart. HSF then saves the depth's exact size; restart once more when it says so. |
| Depth flickers between real and flat | ReShade jumps between the depth and a square shadow map | HSF takes a square depth under a non-square picture as a shadow map and sends constant depth for those frames. Saving the depth size (above) stops it. |
| DX9 game crashes before its first frame, once | D3D9Ex does not suit this game (MGR) | Start it again. HSF switches that game to plain D3D9 by itself (ADVANCED > DX9 GPU ROUTE). |
| DX9 game runs out of memory compiling LaunchPad | A 32-bit exe that is not large-address-aware (Dragon Age: Origins) | Set the exe's 4 GB flag, keeping a copy of the original. |
| Game crashes when its window changes | The game's own limit (The Darkness II) | Leave WINDOW on WINDOWED, and use a tool like Borderless Gaming. |
| Game freezes on start with other mods | A clash with OptiScaler, REFramework or similar | Move the other mods into `_mods_off`, then try HSF alone. |
| Memory climbs fast | ShortFuse's NR mod build of 11 Sep (also with HSF off) | Use the 25 Oct build or newer. |
| Jusant (or similar) crashes when Streamline starts | Streamline's own logging | Keep `SlLog=0` (the default). |
| Vulkan game: no HSF tab | A `dxgi.dll` copy of ReShade in the game folder keeps ReShade's Vulkan layer out | Remove that `dxgi.dll` (keep a copy), and use ReShade's Vulkan layer. |
| Vulkan game: higher latency with RTSS | RTSS keeps HSF's `vkCreateDevice` hook out, so frames wait for their copies on the CPU | Expected. Closing RTSS gives the GPU route. |
| Vulkan game crashes at random with RTSS and Discord | Discord's capture hook for Clips and RTSS clash | Turn off Discord's Clips (Settings > Clips). |
| HDR game looks washed out or SDR with frame generation on | An old HSF build (before HDR10 support on the helper) | Update HSF: install every file from the new zip together. |
| Helper does not start after an update ("of another version" in `hsf_dlssg.log`) | The add-on and `hsf-dlssg-helper64.exe` come from different releases | Copy the whole zip again, `HSF_streamline` included. |

## 2. Logs

| Log | Read it for |
|---|---|
| `ReShade.log` | HSF's route, a line every 10 s with rates, flow and depth sizes, DX9 bridge timings, depth decisions, and route choices (`route: ...`) |
| `HSF_sl_bring.log` | the bring-in route: Streamline start, swap-chain catch, NGX input |
| `HSF_sl_probe.log` | the in-game route: what the game sends its Streamline, and what HSF changed |
| `HSF_streamline\hsf_dlssg.log` | the helper: Streamline set-up, DLSS-G status, mod loading (`mod loaded`, `mod NOT loaded`) |
| `HSF_streamline\hsf_dlssg_game.log` | the game side of the helper link |

For a crash without a log line, Windows crash dumps (`%LOCALAPPDATA%\CrashDumps`, or the game's own crash folder) name the faulting module. That is how the Jusant crash was traced to Streamline's logging.

## 3. Per-game notes

Notes from testing that matter for set-up:

| Game | Note |
|---|---|
| Resident Evil 2 (DX12) | OptiScaler, REFramework, DFC NR, loose `sl.*`/`nvngx` files and dx12agility moved to `_mods_off`. LaunchPad and Lumenite both work and switch in play. |
| Final Fantasy VII Remake (DX12; `-dx11` for D3D11) | No Streamline of its own: the bring-in route. Lumenite works as the source. Other mods in `_mods_off`. In D3D11 (`-dx11`) with Luma, HSF uses Luma's DLSS data (works at 6x). Its exit-time access violation in the NVIDIA driver after Alt+F4 is not an HSF crash. |
| RE Village (DX12) | Bring-in route. Uplift and DFC NR work too (DFC needs its "Present with frame generation" on). |
| Jusant (DX12) | NGX route, with motion from the game's DLSS. `SlLog=0`. |
| AC Shadows, The Witcher 3 (DX12) | In-game route. Turn their own FG off. FLOW works in The Witcher 3. In AC Shadows FLOW works with TAA (DLSS off); with DLSS on it shakes (low base rate) and the GPU hung twice, so use MOTION AUTO or DLSS there. |
| Crimson Desert (DX12) | Fails. Since its 4 Nov update the game refuses ReShade and crashes even without HSF. RTSS also crashes it at start. HSF's swap-chain flag fix (`KeepDxgiFlags`) is in, but not proven there. |
| Returnal (DX12) | Streamline 1.x: HSF stays idle. |
| Sekiro, Mirror's Edge Catalyst (DX11) | Need LaunchPad. Use a recent NR mod build. |
| Dead Space 2 (DX9) | GPU bridge. Its depth is reversed, and is found by itself. The exit leak warning is the game's own. |
| Metal Gear Rising (DX9) | Crashes on 9Ex once, then the crash rule runs it on plain D3D9. |
| The Darkness II (DX9) | Presents through an extra swap chain. Do not force borderless. |
| Dragon's Dogma (DX9) | With Uplift: the slot reclaim keeps HSF's Present hook. |
| Dragon Age: Origins (DX9) | 4 GB flag needed for LaunchPad. |
| AC Revelations (DX9) | Stutters near 60 fps by itself, so it is a poor test game. Older builds froze it at a loading screen when LaunchPad AUTO changed LaunchPad's depth setting; on DX9 that setting now comes only from `HSF.ini`. |
| Valheim (Vulkan, `-force-vulkan`) | Real depth, reversed and upside down, found by itself. Its folder's `dxgi.dll` ReShade had to go for the Vulkan layer to load. |
| Quake II RTX (Vulkan) | Path traced: no depth buffer, so DLSS-G gets constant depth. |
| RPCS3 (Vulkan) | Depth of another shape (see Symptoms). Stopping one game and booting another in the same run works: HSF registers again with the new ReShade. |
| Kyty (Vulkan) | Turn Kyty's own frame generation off in its launcher. Its stutter is its own. |
| Burnout Paradise Remastered | MSAA means no usable depth. Flow only. |

## 4. Lessons learned

These cost real time. Check them before going down the same road again.

- **Before saying HSF lacks a route, read `PickD3d12Route`, `SlBringIn` and `InGameDlssg`.** D3D12 without DLSS already worked when it was claimed to need days of new work.
- **DX9 hooks go in at the first frame**, not at device creation. Hooked at creation, AC Revelations' Present never arrived.
- **ReShade 6.5+ renumbered events.** `create_swapchain` became 97 with an API argument, and `create_device` is 96. HSF's headers are v6.0.0, so register by the running ReShade's version.
- **ReShade presents DLSS-G's generated frames too** on the in-game route. Count real frames by the game's frame token, never by presents.
- **Reflex's FPS cap already counts shown frames** with DLSS-G on. Send 1000000/cap, not a per-real-frame value.
- **Some games send Reflex options once, at start.** Keep them, and send them again with HSF's changes.
- **Never write `.ini` files with printf or a heredoc.** `\r` in `.\reshade-shaders` became a carriage return.
- **Copy LumeniteFX with its include folder.**
- **Old `install.ps1` overwrote `HSF.ini`.** It now keeps an existing one unless `-FreshIni` is given.
- **ReShade's D3D9 "reference count inconsistent" warning at exit** comes from more than 5 of the game's own resources still being alive. HSF's resources are on the native device and never count.
- **Flow is fine at 3x and breaks at 6x on D3D12 games that have DLSS.** Prefer the game's DLSS data there. D3D11 and DX9 flow passes the eye test at 6x.
- **Never change the window from inside ReShade's frame callback.** Use a short-lived thread with `SWP_ASYNCWINDOWPOS`.
- **A game can lose its swap-chain flags on resize** (Crimson Desert: created with 0x840, resized with 0). `KeepDxgiFlags` keeps them.
- **Every LaunchPad setting change recompiles it on the game thread.** In a 32-bit DX9 game that took long enough for Windows to close AC Revelations as not responding. Tune as rarely as possible.
- **HDR is the game's colour space, not just its format.** An R10G10B10A2 frame can be HDR10 or SDR. Read the colour space RenoDX, ReShade and Special K write on the swap chain, and copy it to the helper's swap chain. Take HDR code from RenoDX first.
- **A packaged (Game Pass) process searches its own folder for DLLs before System32**, even with `LOAD_LIBRARY_SEARCH_SYSTEM32`. The helper loads `dxgi`, `d3d12` and `d3d11` by full path, or ReShade loads into it.
- **ReShade does not turn on external semaphores on Vulkan.** HSF hooks `vkCreateDevice` itself; RTSS's hooks crash with that hook in, so it stays out while RTSS is in the process.
- **A child window on another process's window shares its input queue.** On Vulkan that made the mouse choppy; a click-through top-level window fixed it.
- **A cap at exactly the refresh rate waits on VSync.** Cap a few frames below it.
