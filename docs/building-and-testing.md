# Building and testing HSF

## 1. What you need to build

- Windows 11, with Visual Studio 18 (C++ desktop workload). `build.cmd` uses its developer environment, CMake 3.23 or newer, and Ninja.
- The Streamline SDK 2.14 in a `Streamline SDK` folder next to the `HSF` folder (CMake reads `../Streamline SDK`). It is not in the repo. Only `include/`, `lib/x64/sl.interposer.lib` and `bin/x64/*.dll` are used.
- Everything else is in the repo: the ReShade v6.0.0 add-on headers and the matching ImGui headers, under `HSF/third_party`.

## 2. Build

```bash
cmd /c HSF\tools\build.cmd x64
```

```bash
cmd /c HSF\tools\build.cmd x86
```

With no argument it builds both.

| Output | Where |
|---|---|
| `hsf.addon64`, `hsf-dlssg-helper64.exe`, 64-bit tests | `HSF/build/x64` |
| `hsf.addon32`, 32-bit tests, `helper_exit_test` | `HSF/build/x86` |

The 32-bit build has no D3D12 routes (`HSF_SL_GAME` is 64-bit only). 32-bit games use the 64-bit helper from the x64 build. **Build both** whenever shared code changes (the menu, `dlssg_link.h`, depth, overlays), or the 32-bit add-on falls behind.

To identify a build, use the first 8 hex digits of the file's hash or CRC. Test reports name add-ons by these, for example "add-on 85A23680".

## 3. Install for a test

`HSF/tools/guard.ps1` must pass first. It exits 1 while any anti-cheat game runs. **Never run a test tool, probe or install while Call of Duty or any other anti-cheat game is running.** The install scripts call it themselves.

| Script | Use |
|---|---|
| `tools/install.ps1 -Game <folder> [-Bits 32\|64] [-DlssgFrames 1-5] [-FreshIni]` | D3D11 and DX9 games: copies the add-on, `HSF.fx`, Streamline's DLLs and the helper into `HSF_streamline`. An existing `HSF.ini` is kept; `-FreshIni` replaces it with a short test file. The old add-on is kept as `_hsf_experimental\hsf.addon<bits>.prev`. |
| `tools/install_sl_game.ps1 -Game <folder> [-BringIn] [-LaunchPadFrom <folder>] [-LogOnly] [-DynamicRes] [-Remove]` | D3D12 games: the add-on, an `HSF.ini` for the in-game route (or `-BringIn` for the bring-in route), and Streamline's DLSS-G DLLs. With `-BringIn` it also copies `HSF.fx`, and LaunchPad from the `-LaunchPadFrom` game when this one has none. |

To keep a backup per build, copy the add-on by hand instead:

1. Close the game. A running game locks the add-on.
2. Move the old add-on to `<game>\_hsf_experimental\hsf.addon64.<crc>.bak`.
3. Copy the new `hsf.addon64` (and the helper, if it changed) in.

House rules for test installs:
- Install only while the game is closed, and never start a second instance of the game.
- Never delete other mods. Move them into `<game>\_mods_off` and say so in the test report.
- Never put LaunchPad, LumeniteFX, MFG-Unlocker or any other third-party binary or shader into the repo or a release. Testers download their own.
- Write `.ini` files only with `[System.IO.File]::WriteAllLines`.

## 4. Offline tests

These need no game. Most need an NVIDIA GPU, and the D3D12 ones need D3D12.

| Run | Checks |
|---|---|
| `build\x64\compose_test.exe` | compose pass |
| `build\x64\hdr_out_test.exe` | FP16 to HDR10 and sRGB codes against the ST.2084 and sRGB curves |
| `build\<arch>\gpu_reserve_test.exe` | GPU reserve: spike count, governor against a simple card model, one live load reading |
| `build\x64\frame_checks_test.exe` | cuts, stills, HUD mask |
| `build\x64\debug_view_test.exe` | D3D12 debug view |
| `build\x64\sl_game_test.exe` | hooks on a copy of `sl.interposer.dll` |
| `build\x64\sl_bring_test.exe` | HSF bringing Streamline into a small D3D12 program (needs Streamline's DLLs next to it) |
| `build\x64\ngx_input_test.exe` | the NGX hook on this machine's `_nvngx.dll` |
| `build\<arch>\camera_test.exe` | camera scanner, chooser and fit, no GPU |
| `build\<arch>\camera_replay.exe <recording>` | replays an F6 recording |
| `build\<arch>\score_test.exe`, `hud_test.exe`, `depth_test.exe`, `launchpad_tune_test.exe` | score, HUD, depth, tune rules |
| `build\x64\nvof_test.exe [size] [copy]` | NVIDIA Optical Flow: a picture moving 7 right and 4 up a frame must come out as that motion; prints the time per frame. `size` is `NvofSize`, `copy` takes the picture as a back buffer is taken. Needs an RTX card. |
| `build\x64\sl_bring_test.exe <folder> <log> nvof` | the bring-in route with NVIDIA Optical Flow giving the flow off the back buffer, through compose, the HUD mask and DLSS-G |
| `build\<arch>\dx9_bridge_test.exe [frames] [nofence\|stall]` | DX9 bridge GPU route: with GPU fences every frame read back must be this frame's; with `nofence`, the frame before's; with `stall` one fence signal is left out and the watchdog must let go within a second |
| `tools\helper_exit_test.ps1` | the helper dies with its game (needs the x86 build) |
| `build\x64\vk_scene.exe [--fps N] [--seconds N] [--size WxH]` | by hand, not in the suite: a small Vulkan program to bring the Vulkan route up without a game. ReShade's Vulkan layer and HSF must be set up next to it |
| `build\x86\helper_exit_test.exe <helper> <ms> hdr10\|hdr10bug` | eye test on an HDR display, Windows HDR on: `hdr10` must show the right half (400 nits) glowing brighter than the left (100 nits); `hdr10bug` sends the same picture as sRGB, two flat greys |

Each test prints its checks as it runs. Look for `FAIL` in the output. The exit code is not reliable across all tests yet: some return non-zero on failure, and others only print.

## 5. Testing in games

- **The eye test decides.** What the tester sees outranks any number. A measurement that says something does not help never overrides a visible improvement.
- **No A/B quality tests.** Every improvement goes in unless it destroys performance.
- **Back-check on shared code.** A change to shared code (menu, depth, overlays, link) gets a back-check in one D3D11 game (Castlevania) and one D3D12 game (RE Village) before it merges.
- **Record results.** They go in the maintainer's own test record, which is the source of truth.
- **Use RTSS.** Every test runs with RTSS's overlay on.

Main test games:

| API | Game |
|---|---|
| D3D11 | Castlevania: Belmont's Curse (main), AC3 Remastered |
| D3D11, 32-bit | Tomb Raider (2013) |
| D3D12 in-game route | AC Shadows, The Witcher 3 |
| D3D12 bring-in route | RE Village (main), Jusant (NGX), RE2, FF7R |
| DX9 | Dead Space 2 (main), MGR (9Ex crash rule) |
| D3D11 with Luma | FF7R (`-dx11`) |
| Vulkan | Valheim (main, real depth), Quake II RTX (no depth), RPCS3 and Kyty (emulators) |

## 6. Hung processes

`tools/dump_process.ps1` writes a full-memory minidump of a hung test process, for reading in a debugger.

HSF's early in-house presenter (its code, `interp_test`, the PresentMon and smoothness measurements) and the early probes (`dlssg_probe`, `on12_probe`, `harness`, `debug_strings`, `smooth_capture`), are kept by the maintainer outside the repo.

## 7. Git

- `main` is the only long-lived branch, on GitHub `Capt-Hunter/HSF`.
- Work happens on a branch (`hsf-<topic>`), which is merged into `main` with a merge commit, pushed, then deleted locally and on GitHub.
- Only the maintainer (Capt-Hunter) merges into `main` and pushes it. Outside changes come in as pull requests (see [CONTRIBUTING.md](../CONTRIBUTING.md)).
- Git ignores `build/`, `*.addon32` and `*.addon64`, the `Streamline SDK` folder next to `HSF`, and local working folders and notes.
