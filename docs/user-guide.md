# HSF user guide

This guide is for players and testers. It covers what HSF needs, how to install it for each kind of game, and every part of the in-game menu and overlay.

## 1. What HSF does

HSF runs inside the game as a ReShade add-on. It takes each finished game frame, together with motion and depth for that frame, and hands them to NVIDIA DLSS Frame Generation (DLSS-G). DLSS-G then adds 1 to 5 generated frames after each real one, so you see 2x to 6x the frames.

The motion comes from one of two places:

- **The game's own DLSS data**, on DirectX 12 games that run DLSS. This is the motion the game engine computed itself, so it is the most exact. The menu calls it **DLSS**. In DirectX 11 games that a Luma mod adds DLSS to, HSF reads the motion and depth Luma hands to DLSS, and the menu calls it **LUMA**.
- **Optical flow** from a ReShade effect, together with the game's depth buffer. This works in any game ReShade can run in. The menu calls it **FLOW**.

HSF then sends the motion through its own clean-up (the compose pass), which uses depth to sharpen motion at object edges and steady it over time. It also checks for scene cuts, still pictures (menus, loading screens), the HUD and overlays, so these are not smeared by generation.

## 2. What you need

| Need | Notes |
|---|---|
| NVIDIA RTX GPU | DLSS-G needs RTX 40 or newer. RTX 50 can do 6x by itself. RTX 20, 30 and 40 can get multi-frame generation through **MFG-Unlocker** by dashdogy (MIT licence), which you download and install yourself. HSF never ships it. |
| Windows 10 or 11 | Tested on Windows 11. |
| ReShade 6.8 with add-on support | The "with full add-on support" build. DirectX 9 needs ReShade 6.5 or newer. ReShade 6.8.0 is what everything was tested with. |
| HSF files | `hsf.addon64` or `hsf.addon32`, the `HSF_streamline` folder, `HSF.fx`. |
| A flow source (only when the game gives no DLSS data) | **iMMERSE LaunchPad** by Marty (`MartysMods_LAUNCHPAD.fx` plus its `MartysMods` folder and textures), or **LumeniteFX Kernel** (`lumenite_Kernel.fx` **with its `include` folder**). You download these yourself. HSF never ships them. |
| Windowed or borderless window | Needed on DirectX 9, DirectX 11 and Vulkan, where DLSS-G shows on a window of HSF's own over the game. HSF turns exclusive fullscreen into borderless on DirectX 9 by itself. |

HDR works. A game in HDR10 (its own HDR, or a RenoDX mod set to HDR10) stays HDR10 through DLSS-G. A game that outputs FP16 (scRGB) is turned into HDR10 while Windows HDR is on. Nothing needs setting.

Not supported in version 1: OpenGL, and any game with online anti-cheat. Never use HSF in an online anti-cheat game, as you risk your account.

## 3. Which route your game takes

HSF picks a route by itself. Knowing which one you are on helps when something goes wrong. The menu's status box shows the API and route.

| Game | Route | Motion | Where DLSS-G runs |
|---|---|---|---|
| DirectX 12 with its own Streamline (it ships `sl.interposer.dll`, usually because it has DLSS-G) | **In-game route** (`InGameDlssg=1`) | DLSS when the game sends it, else flow | Inside the game, on the game's own Streamline |
| DirectX 12 without Streamline | **Bring-in route** (`SlBringIn=1` or `2`) | DLSS through the game's NGX calls when it runs DLSS, else flow | Inside the game, on HSF's own Streamline 2.14 from `HSF_streamline` |
| DirectX 11, 32-bit or 64-bit | **Helper route** | Flow and depth | `hsf-dlssg-helper64.exe`, on a child window over the game |
| DirectX 9, 32-bit or 64-bit | **DX9 bridge**, then the helper route | Flow and depth | `hsf-dlssg-helper64.exe` |
| Vulkan (games and emulators such as RPCS3) | **Vulkan bridge**, then the helper route | Flow and depth | `hsf-dlssg-helper64.exe`, on a click-through window over the game |

The first time a DirectX 12 game runs with HSF, HSF writes the route into `HSF.ini` and the menu says **D3D12: HSF HAS SET UP ITS D3D12 ROUTE. RESTART THE GAME ONCE**. Restart the game. A DirectX 12 route must start before the game makes its device, so it cannot start in the same run.

Streamline 1.x games (for example Returnal, which shipped in 2023) are not supported. HSF leaves them alone and says so in the menu.

## 4. Install

HSF is being added to [RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI) by RankFTW. Once it is in, RHI installs HSF, ReShade and a flow source for you, and you can skip this section.

To install by hand, download the latest `HSF-<version>.zip` from the [releases page](https://github.com/Capt-Hunter/HSF/releases). `<game>` below is the folder that holds the game's exe.

### 4.1 Every game

1. Install ReShade 6.8 with add-on support into `<game>`. Pick the game's API: DirectX 9 (`d3d9.dll`), DirectX 10/11/12 (`dxgi.dll`), or Vulkan (ReShade's Vulkan layer; `ReShade.ini` sits in `<game>`). In a Vulkan game, leave no `dxgi.dll` copy of ReShade in `<game>`: it keeps the Vulkan layer out.
2. Copy `hsf.addon64` (64-bit game) or `hsf.addon32` (32-bit game) into `<game>`, next to ReShade's DLL.
3. Copy the `HSF_streamline` folder into `<game>`. It holds Streamline 2.14 (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll` and the rest) and `hsf-dlssg-helper64.exe`. 32-bit games need it too: Streamline has no 32-bit build, so the 64-bit helper runs DLSS-G for them.
4. Copy `HSF.fx` into `<game>\reshade-shaders\Shaders`. It asks LaunchPad for its flow and hands HSF the depth buffer ReShade picked.
5. Copy `HSF.ini` from the zip into `<game>`, replacing an old one. HSF sets itself up again with it, and the menu saves your choices to it. (Without a file, HSF uses its defaults and writes one.)

### 4.2 Add a flow source

You need this on every DirectX 9, DirectX 11 and Vulkan game, and on DirectX 12 games that do not run DLSS. A DirectX 11 game with a Luma mod that runs DLSS does not need one while Luma's data comes, but keep one for menus and for when DLSS is off.

- **LaunchPad**: `MartysMods_LAUNCHPAD.fx` and the `MartysMods` folder into `reshade-shaders\Shaders`, and the `iMMERSE_bluenoise*.png` textures into `reshade-shaders\Textures`.
- **LumeniteFX**: `lumenite_Kernel.fx` and its `include` folder (`lumenite_*.fxh`) together. Without the include folder it does not compile.

Without a flow source on a DirectX 11 game, HSF says "no motion vectors".

### 4.3 Game settings

- Turn **off** the game's own frame generation. On the in-game route the menu shows a red box, **THIS GAME HAS ITS OWN FRAME GENERATION**, until you press OK.
- Use windowed or borderless mode on DirectX 11.
- VSync on or off both work. A frame cap gives smoother generation (see FPS CAP).

### 4.4 Other mods

These were tested alongside HSF:

- **ShortFuse's NR mod (`renodx-dlss`)**: works on DirectX 11 and 12, 64-bit only. Use the 25 Oct build or newer. The 11 Sep build leaks memory (35 GB in 3 minutes, also with HSF off).
- **DFC NR add-on**: works once DFC's own "Present with frame generation" is on (DEBUG tab > Placement > ADVANCED).
- **GITC Uplift**: works alongside HSF, but not as a flow source: use LaunchPad or Lumenite for the flow.
- **MFG-Unlocker**: works on DirectX 12 in the game, and through the helper on DirectX 11, Vulkan and 32-bit games. In 64-bit games put it next to the game's exe. In 32-bit games put it in `HSF_streamline\mods`, never next to the 32-bit exe.
- **RenoDX**: works, HDR10 included. RenoDX HDR mods in DirectX 12 games work with frame generation (tested in Scorn and BLACKTAIL).
- **Luma**: works in DirectX 11, and HSF uses Luma's DLSS data (see MOTION).
- **RTSS and ReShade overlays**: work. On DirectX 11, keep KEEP OVERLAYS OUT on, so the overlays are not generated.

Heavily modded games (OptiScaler, REFramework and others) can clash. When a game crashes, move the other mods into a `_mods_off` folder and try HSF alone first.

### 4.5 A game's Streamline is too old for MFG

Some DirectX 12 games ship their own copy of NVIDIA Streamline. Streamline older than 2.7.2 (from before DLSS 4, January 2025) only allows 2x frame generation, so DLSS-G cannot do more there, even on an RTX 50 card. Streamline 2.7.2 to 2.10 allows up to 4x; 5x and 6x need 2.11 or newer. HSF's own copy is 2.14.

When this is the case, the menu says so under MULTIPLIER: GAME'S STREAMLINE 2.0.0 IS TOO OLD FOR MFG // 2X ONLY, and 3X to 6X are crossed out. Hover over the line to see the folder that holds the game's copy. HSF does not change game files, so the update is a manual step:

1. Close the game.
2. Open the folder the menu names (for example `<game>\Engine\Plugins\NVidiaDLSS\Streamline\Binaries\ThirdParty\Win64` in Unreal games). Copy the whole folder somewhere safe as a backup.
3. From HSF's `HSF_streamline` folder, copy every `sl.*.dll` file and `nvngx_dlssg.dll` into that folder, replacing the old ones. Copy only these files.
4. Start the game. The line is gone, and 3X to 6X are open.

If the game no longer starts, put the backup back. A game update or a file check in Steam or another launcher can bring the old files back; then the line shows again.

## 5. The menu

Open ReShade's overlay (Home key by default). HSF has its own tab, **HSF**, next to ReShade's Home, Add-ons and Settings. It docks itself there the first time. If you drag it out, it comes back at the next launch.

Generation keeps running while ReShade's menu is open, and the big number is live. On DirectX 9, 11 and Vulkan, HSF's window takes the mouse while the menu is open, so a mod's menu on it (MFG-Unlocker's) can be clicked.

### 5.1 Header and readout

- **HSF // FRAME GEN**, with your GPU and the API on the right. Left of the GPU, five small squares are colour presets (see 5.7): click one and the menu and the overlay fade to it.
- **The big number**: frames shown per second (FPS OUT). Under it: the real (BASE) frame rate and its frame time in ms.
- **The tag**: GENERATING (green), NOT GENERATING (amber), PROBLEM (red) or OFF.

### 5.2 Main rows

| Row | Choices | What it does |
|---|---|---|
| FRAME GEN | on / off | Turns generation on or off (`Extra`). On DirectX 9, 11 and Vulkan, OFF keeps the helper running so ON is instant, and a red **CLOSE HELPER** box appears beside it: press it to stop the helper, so the game shows its own frame rate. With FRAME GEN on but no motion (no flow effect, no NVOF), the helper does not start, and a running one stops after 3 s without motion. |
| MULTIPLIER | 2X 3X 4X 5X 6X | Real frame plus 1 to 5 generated (`DlssgFrames`). Choices above what DLSS-G says this card can do are crossed out. F7 cycles it in game. On RTX 20, 30 and 40 with MFG-Unlocker, a line shows the card and how far it is unlocked. On RTX 20 and 30, MFG-Unlocker runs its own frame generation and sets the multiplier itself: the row shows its number and points to its menu. |
| MOTION | DirectX 12: AUTO / DLSS / FLOW. DirectX 11 with Luma: AUTO / LUMA / FLOW. Others: FLOW | AUTO uses the game's DLSS data (or Luma's) while it comes, else flow. DLSS is crossed out until the game has sent DLSS data in this run. The note on the right shows what is in use: GAME DATA, FLOW, or FLOW + DEPTH. With Luma, a line says when the game runs in DX12 (start it in DX11), when Luma sends no DLSS data yet (turn on DLSS in Luma's settings), and offers **SWITCH TO LUMA (DLSS)** while MOTION is on FLOW. |
| FLOW PAUSE | ON / OFF | DirectX 11 with Luma. ON (default, `FlowPause`) turns the flow effect off while Luma's DLSS data comes, and back on when it stops (cuts, menus). Other effects that read the same flow stop moving while it is off. |
| FLOW FROM | AUTO / LAUNCHPAD / LUMENITE | Which effect's optical flow HSF reads (`Source`). AUTO takes the first one loaded, in that order. A source that is not loaded is crossed out. It switches on the next frame, in play. The note on the right shows the source in use, or NONE FOUND. |
| LAUNCHPAD | AUTO / LOW / MEDIUM / HIGH, UNDO | Shown while LaunchPad gives the flow. Sets LaunchPad's Flow Quality (`LaunchPadQuality`). Higher quality costs more GPU time, and the cost grows with resolution. AUTO is the default: HSF tunes LaunchPad by itself, picks the quality from the game's resolution (HIGH up to 1920x1200, MEDIUM up to 3440x1440, LOW above, so LOW at 4K) and picks again if you change resolution. Every pick also moves LaunchPad to the top of the effect order and sets its depth settings to what HSF found (on DirectX 9 only from `HSF.ini`'s depth settings, because a LaunchPad recompile there can freeze a 32-bit game). Each tune recompiles LaunchPad, so AUTO tunes once per setting. It works only through ReShade, as you would by hand, and never changes Marty's file. A box is lit only while LaunchPad is set that way. Hover over a box to see what it does. UNDO puts back what was there before HSF's first tune, and stops AUTO. |
| REFLEX | OFF / ON / ON+BOOST | NVIDIA Reflex low latency. On the in-game route the game runs its own Reflex, so OFF is crossed out and HSF only adds boost. On DirectX 11 the right side shows the latency measured by the driver. |
| FPS CAP | bar 30 to 360, a typed number 30 to 1000, OFF, your monitor's Hz | A cap on frames **shown** (real plus generated). With DLSS-G it is Reflex's frame limit. Drag the bar, turn the mouse wheel over it (Shift: 10 at a time), or click the number to type a cap (Enter keeps it, Esc cancels, 0 is OFF). With frame generation on, the line under it shows the rate the game itself runs at, in amber below 30. Without a cap a warning icon and NO CAP SET // A CAP GIVES SMOOTHER FRAME GENERATION pulse. |
| OVERLAY | OFF / SIMPLE / ADVANCED | The on-screen overlay (see 6). |
| FLOW ENGINE | SHADER / NVOF | Who works out the optical flow (`FlowEngine`). SHADER: the effect picked in FLOW FROM. NVOF: NVIDIA Optical Flow, the card's own engine (RTX 20 and newer). SHADER looks better and runs faster; NVOF is a backup. With no flow effect loaded for 10 s, HSF switches to NVOF by itself, and a loaded effect takes over again. While NVOF runs, a yellow box says **NVOF IS A BACKUP ONLY**, with a USE SHADER button. Picking NVOF locks MOTION to FLOW and crosses out FLOW FROM. |

Under MULTIPLIER, an orange box can appear: **5X AND 6X ARE NOT RECOMMENDED WITH FLOW**. It shows on DirectX 12 games that have DLSS when flow runs at 5X or 6X. Flow keeps up at 3x but breaks apart at 6x there. Switch MOTION to DLSS, or stay at 4X or below. It is advice only. DirectX 9, 11 and Vulkan games only have flow (or Luma's data), and they look fine at 6x.

### 5.3 Status box

It shows the API and route, and a verdict. The route names are:

- `IN GAME // HSF STREAMLINE`: the bring-in route.
- `IN GAME // GAME STREAMLINE`: the in-game route.
- `HSF HELPER` (or `32-BIT // HSF HELPER`): the DirectX 11 helper route.
- `DX9 GPU BRIDGE` or `DX9 CPU BRIDGE` (or the same with `32-BIT //` in front): DirectX 9.
- `HSF HELPER` with the API shown as Vulkan: a Vulkan game.

The verdicts are:

- **OK**: generating.
- **WAITING** (amber): something is still missing, and the reason is shown under it (for example no motion found yet).
- **PROBLEM** (red): something failed, and the reason is shown under it.
- **OFF**: FRAME GEN is off.
- **CONFLICT** (red): a mod known to break HSF's frame generation is loaded (see MOD STACK below). It names the mod. HSF keeps running; remove the mod from the game folder to fix it.

The **GPU guard** turns FRAME GEN off and closes the helper by itself when DLSS-G's GPU waits keep timing out: the card cannot keep up, and the driver would reset next. The menu says so in red. Turn FRAME GEN on to try again. The guard's OFF is not saved to `HSF.ini`.

Under it, a small line graph shows the recent frame times.

**MOD STACK**, under ADVANCED and closed until you click it, shows what each tool in the game is doing, as far as HSF can see. It only shows things; it changes nothing. Each row has the tool's name on the left and a dot with its status on the right: green working, amber a warning or not tested, red a problem, grey idle. Short lines under a row give the details; hover over one to see the file's full path.

| Row | What it shows |
|---|---|
| HSF | Generating (and at what multiplier), waiting or a problem, with the reason. Under it, the route and where motion comes from. |
| RENODX | Each RenoDX add-on loaded and its version. For RenoDX DLSS, where it hooks DLSS (after upscaling or at present). |
| UPLIFT | ON or OFF (its Enabled setting in `ReShade.ini`) and its version, and its PresentWithFrameGen setting. |
| MFG-UNLOCKER | Whether it runs and found DLSS-G, and its multiplier. |
| Other mods | Mods HSF finds by what they are, not by their file name: a Windows DLL name (such as `dxgi.dll` or `version.dll`) loaded from the game's folder, an ASI plugin, a DLL that answers the game's DLSS or Streamline calls, or a mod HSF knows by its version info. Each shows in red when it is known to break HSF (OptiScaler), amber when it is not tested with HSF (Special K, unknown mods), and green when it works with HSF. A red one also turns the status box to CONFLICT. Files made by Microsoft, NVIDIA, Intel or AMD (such as `dbghelp.dll` or the driver's DLSS models) are never listed, and `StackIgnore` in `HSF.ini` leaves out more. |
| GAME | The DLSS the game makes (super resolution, frame generation, ray reconstruction) and on which API (D3D12, D3D11 or Vulkan), and each Streamline copy loaded with its version. DLSS made by a mod (Luma, Uplift, RenoDX) shows under that mod's row instead, and DLSS frame generation from HSF's own Streamline under HSF. DX9 and 32-bit games have no DLSS, and the row says so. A copy no one uses next to the game's own shows in amber: it is another mod's, or a leftover file. |
| OTHER ADD-ONS | Any other ReShade add-on loaded. |
| NOT LOADED | RenoDX, Uplift and MFG-Unlocker, when they are not there. |

DLSS features are seen only on DirectX 12 and 11 games that call NVIDIA's DLSS directly (NGX), and Streamline copies only in 64-bit games. Each mod found is written to the log once.

### 5.4 ADVANCED

| Setting | What it does |
|---|---|
| PROTECT GAME HUD | Keeps the game's HUD out of generation, so it does not smear. Which setting it switches depends on the route: `GameHud` on DirectX 11, `DlssgHud` on the in-game route, `FrameChecks` on the bring-in route. |
| KEEP OVERLAYS OUT | DirectX 9, 11 and Vulkan. DLSS-G gets the frame from before RTSS, Steam, Discord and ReShade drew their overlays (`KeepOverlaysOut`). |
| DX9 GPU ROUTE | DirectX 9 only. Shows this launch: ON (D3D9EX) means frames reach DLSS-G on the GPU, at almost no cost. OFF means CPU copies. The switch is DX9 9EX in CRASH PROTECTION. |
| WINDOW | WINDOWED / BORDERLESS. DirectX 9, 11 and 12. BORDERLESS makes a game in windowed mode borderless fullscreen while it runs; WINDOWED puts it back as it was (`ForceBorderless`). A DirectX 9 game that asked for fullscreen is made borderless fullscreen by itself. Some games crash when their window changes size (The Darkness II does). For those, use a tool like Borderless Gaming. |
| GPU RESERVE | On by default (`GpuReserve`). Only when the card is at 95% load or more AND the game's real frames stutter, HSF lowers the FPS cap a small step at a time until the stutter stops, then slowly gives the frames back. It never goes above your own cap, and never takes the game under 30 fps. The line beside it says WATCHING FOR STUTTER, or ACTING with the cap in use. |
| SETTINGS | RESET TO DEFAULTS. Click twice within 3 seconds. Route switches and hotkeys are kept. |
| CRASH PROTECTION | Every switch that HSF turns OFF by itself after a crash, in one list. Only the rows that apply to this game are shown. SAFE TRY, on by default (`SafeTry`), covers the hook steps: NGX MOTION and PLUGIN HOOK (games with their own Streamline), LOAD WATCH (64-bit games that use Streamline) and HOOK DECODER (every game). DX9 9EX shows in DirectX 9 games and BORDERLESS when WINDOW is set to BORDERLESS; these two have their own crash rules. When the game crashes, HSF switches the row OFF for this game and marks it OFF AFTER A CRASH; you can switch any row back ON, or OFF yourself. Hover over a switch to see what it does and what changes when it is off. Changes apply after a game restart, except BORDERLESS, which applies at once. A step that is off is always shown, so it can be turned back on. |

### 5.5 OVERLAY section

Settings for the on-screen overlay: OFF / SIMPLE / ADVANCED (the same setting as the OVERLAY row), SIZE 50 to 200 %, OPACITY 0 to 100 % (the panel behind the text), CORNER, X and Y OFFSET in pixels at 1440p (scaled to your resolution), and RESET THIS OVERLAY. Simple and advanced each keep their own look.

### 5.6 KEYBINDS

The in-game hotkeys. Click a key, then press the new one: ESC keeps the old key, BACKSPACE sets none. A key another HSF hotkey had is taken from it. The keys are saved to `HSF.ini`, and work while ReShade's menu is closed.

| Setting | What it does |
|---|---|
| FRAME GEN ON / OFF | Turns frame generation on and off (`FrameGenKey`). None by default. |
| MULTIPLIER | Cycles the multiplier, 2x up to what this card can do (`MfgKey`, F7). With MFG-Unlocker on RTX 30 or 20, the unlocker sets the multiplier instead. |
| OVERLAY ON / OFF | Turns the on-screen overlay off, and back to SIMPLE or ADVANCED, whichever it was (`OsdKey`). None by default. |
| DEBUG VIEW | DirectX 9, 11 and Vulkan: cycles the debug view (`ViewKey`, F8). |

A key set here shows beside its row at the top of the menu, as F7 does beside MULTIPLIER.

### 5.7 COLOURS

The menu's and the overlay's colours. PANEL is the panel behind the text (its border and lines follow it), ACCENT the corner mark. Click the square to pick a colour; RESET puts the default back. PRESET (DEFAULT, CRIMSON, NAVY, VIOLET, MONO) sets both at once, as the swatches in the header do; none is lit while the colours are your own. A change fades in over 0.3 s. Saved to `HSF.ini` (`UiPanel`, `UiAccent`). Green, red and amber stay as they are, because they mean good, error and warning.

### 5.8 DEBUG, for testers

| Setting | What it does |
|---|---|
| VIEW | DirectX 12: GAME / DEPTH / MOTION replaces the picture with what DLSS-G gets. Depth: near is bright, far is dark. Motion: still is grey, moving is coloured. DirectX 9, 11 and Vulkan: OFF / MOTION / ERROR / HELPED / DEPTH, shown in a box at the top right. F8 cycles it. On the in-game route with DLSS motion there is nothing to show, so it says FLOW ONLY. |
| DEPTH DIRECTION | AUTO / NORMAL / REVERSED. HSF finds it by itself. Set it by hand only if the DEPTH view looks wrong (near should be white). |
| DEPTH WAY UP | AUTO / AS GAME / FLIPPED, for depth that is upside down. |
| SEND DEPTH | DirectX 9, 11 and Vulkan. Off sends a constant depth instead. |
| CLEAN FLOW (DEPTH) | The compose pass (`Compose`). |
| FRAME CHECKS | DirectX 9, 11 and Vulkan. Scene cuts, the HUD mask and still pictures. |
| CAMERA MOTION, CAMERA FIT, CAMERA RECORD | DirectX 11. Exact motion for the still world from the game's camera matrices. CAMERA FIT can shake. CAMERA RECORD (F6) records 30 s for offline study. |
| REBUILD CAMERA, CUT CHECK, QUICK OFF, SCORE GUARD | In-game route only. See [settings.md](settings.md). |
| STREAMLINE switches | Bring-in route only: SL LOG, SL FACTORY PROXY, SL MANUAL HOOKING, SL EARLY LOAD, NGX INPUT. They apply after a game restart. |
| COPY LOG TO CLIPBOARD | Copies `HSF.log`. Paste it when you report a problem. |
| ALL DETAILS | Every number HSF knows. |

## 6. Overlays

- **SIMPLE**: a see-through panel in the style of RTSS. It shows FPS out, base FPS, latency (DirectX 11 helper route only), frame time and p99, and a pacing verdict: SMOOTH, UNEVEN (p99 over 1.3x the median) or HITCHING (a frame over 2x the median, in the last 120 frames). It also has an RTSS-style line graph: white frames, red hitches, green latency and the cap as an amber dashed line.
- **ADVANCED**: a solid panel with generation (mode, route, motion, depth, HUD, MFG-Unlocker), Reflex, FPS cap, latency parts from the driver report, and base frames (frame time, p1/p99, 1% low, hitches, pacing).

## 7. Hotkeys

Frame generation, the multiplier, the overlay and the debug view are set in the menu's KEYBINDS section. Every key can also be changed or turned off in `HSF.ini` (a virtual-key code, 0 = none).

| Key | Does | HSF.ini |
|---|---|---|
| none | turns frame generation on and off | `FrameGenKey=0` |
| F7 | cycles the multiplier | `MfgKey=118` |
| none | turns the on-screen overlay off and on | `OsdKey=0` |
| F8 | cycles the debug view | `ViewKey=119` |
| F9 | test: cycles DLSS-G's motion input through each source, then zero motion | `InputKey=120` |
| F10 | test: real depth, or a constant | `DepthKey=121` |
| F11 | in-game route: HSF's rebuilt camera matrices, or the game's | `DlssgCameraKey=122` |
| F6 | records 30 s of camera data | `RecordKey=117` |

Frame generation and the overlay have no key until you set one, because the free keys (F5 is quicksave in many games) differ from game to game.

## 8. Where things are written

| File | Where | What |
|---|---|---|
| `HSF.ini` | next to the add-on | all settings. The menu writes it, and HSF reads it again when it changes. |
| `HSF.log` | next to the add-on | HSF's one log, started fresh at each game start. Send this file when you report a problem. |
| `HSF.previous.log` | next to the add-on | the log of the run before |

## 9. Uninstall

Delete `hsf.addon64` or `hsf.addon32`, `HSF.ini`, `HSF.log`, `HSF.previous.log`, the `HSF_streamline` folder and `reshade-shaders\Shaders\HSF.fx`. HSF changes nothing else in the game folder. There is one exception: on DirectX 9 games that ran out of memory, the 4 GB flag may have been set by hand in the exe, and the original was kept next to it.
