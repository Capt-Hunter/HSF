# Installing HSF from a tool

This page is for installers and mod managers that put HSF into games for their users, such as [RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI) by RankFTW. It lists what an HSF release contains, where each file goes, what HSF needs next to it, and how to update and remove it. Players who install by hand should read the [user guide](user-guide.md) instead.

## 1. Where releases come from

Official builds are the `HSF-<version>.zip` assets on this repository's [releases page](https://github.com/Capt-Hunter/HSF/releases). Download them from there. Do not rebuild, repack or rehost them as the official build.

The [licence](../LICENSE) allows a free tool to download and install HSF for its users. Keep `LICENSE.txt` and the `licenses` folder from the zip with the installed files, and do not charge for HSF or put it behind a paywall. The [README](../README.md#sharing-hsf) has the terms in plain words.

## 2. What the zip holds

The zip mirrors a game folder: its root is the folder HSF goes into.

| Path in the zip | What it is | Install it |
|---|---|---|
| `hsf.addon64` | the add-on for 64-bit games | 64-bit games only |
| `hsf.addon32` | the add-on for 32-bit games | 32-bit games only |
| `HSF_streamline\` | NVIDIA Streamline 2.14 (`sl.*.dll`, `nvngx_dlssg.dll`) and `hsf-dlssg-helper64.exe` | always, the whole folder, for 32-bit games too |
| `reshade-shaders\Shaders\HSF.fx` | the effect that hands HSF the flow and the depth buffer | always |
| `HSF.ini` | a clean settings file | always (see 5) |
| `LICENSE.txt`, `licenses\`, `README.txt` | licence texts and a short readme | keep them with the files |

Install exactly one add-on: `hsf.addon64` for a 64-bit exe, `hsf.addon32` for a 32-bit exe. ReShade loads only the one that matches its own bitness, so a copy of both does no harm, but it is untidy.

## 3. Where the files go

HSF finds everything relative to its own add-on file:

- `HSF_streamline` must sit **next to the add-on**.
- `HSF.ini` is read and written **next to the add-on**.
- `HSF.fx` must be in a folder that ReShade searches for effects (`EffectSearchPaths` in `ReShade.ini`). The usual place is `reshade-shaders\Shaders`.

The add-on goes where ReShade loads add-ons from: the folder of ReShade's DLL by default, or the `AddonPath` in `ReShade.ini`'s `[ADDON]` section. For a Vulkan game that uses ReShade's global Vulkan layer, that is the game's exe folder, the one that holds `ReShade.ini`.

## 4. What HSF needs

| Need | Notes |
|---|---|
| ReShade with full add-on support | Without add-on support (RHI's "No Addons" channel, for example) HSF does not load. ReShade 6.8 is what HSF is tested with. DirectX 9 needs 6.5 or newer. |
| An NVIDIA RTX GPU | DLSS-G itself decides which cards and multipliers work. HSF crosses out what the card cannot do. |
| A flow source, for games that give HSF no DLSS data | iMMERSE LaunchPad (`MartysMods_LAUNCHPAD.fx`, its `MartysMods` folder and the `iMMERSE_bluenoise*.png` textures) or LumeniteFX Kernel (`lumenite_Kernel.fx` with its whole `include` folder). Install them from their official sources. HSF never ships them. Every DirectX 9, 11 and Vulkan game needs one, and so does a DirectX 12 game that does not run DLSS. |
| No online anti-cheat | Never install HSF into a game with online anti-cheat. |

HSF sets LaunchPad up for frame generation by itself (the LAUNCHPAD row's AUTO, the default), through ReShade, without changing Marty's file. An installer does not need to change LaunchPad's settings or the effect order.

### Games and APIs

| Game | What happens | Install notes |
|---|---|---|
| DirectX 12 | DLSS-G runs inside the game, on the game's own Streamline or on HSF's | The first run writes the route into `HSF.ini` and asks for one restart. Turn off the game's own frame generation. |
| DirectX 11, 32-bit or 64-bit | DLSS-G runs in `hsf-dlssg-helper64.exe`, over the game's window | Windowed or borderless. |
| DirectX 9, 32-bit or 64-bit | a bridge to Direct3D 11, then as DirectX 11 | ReShade as `d3d9.dll`. |
| Vulkan (tested in 64-bit games) | a bridge to Direct3D 11, then as DirectX 11 | ReShade's Vulkan layer. A DXGI copy of ReShade (`dxgi.dll`) in the same folder keeps the Vulkan layer out, and then HSF does not load. |
| OpenGL | not supported | do not install |

### HDR

HSF passes the game's HDR on to DLSS-G: HDR10 output (from RenoDX, for example) reaches the screen as HDR10. FP16 scRGB output is turned into HDR10 while Windows HDR is on. No setting is needed.

### Frame generation unlockers

On RTX 20, 30 and 40 cards, MFG-Unlocker by dashdogy adds multi-frame generation. HSF never ships it. Where it goes:

- **64-bit games**: next to the game's exe, as its own guide says. The helper finds it there by its product name, whatever the file is called.
- **32-bit games**: in `HSF_streamline\mods`, never next to the 32-bit exe. The helper loads every DLL in that folder.

## 5. Updates

- Replace every file from the zip at once. The add-on and `hsf-dlssg-helper64.exe` talk over a shared block with a version number, and a helper from another release refuses to run.
- Replace `HSF.ini` with the clean one from the zip. HSF sets itself up again on the first launch, and a DirectX 12 game may ask for one restart.
- Install only while the game is closed: a running game locks the add-on file.

## 6. Uninstall

Delete these and nothing else:

- `hsf.addon64` or `hsf.addon32`
- `HSF.ini`
- the `HSF_streamline` folder, with its logs and any `mods` folder in it
- `reshade-shaders\Shaders\HSF.fx`
- HSF's logs in the game folder: `HSF_sl_bring.log`, `HSF_sl_probe.log`

HSF changes nothing else in the game folder. It may have changed LaunchPad's settings in ReShade's preset (AUTO), and the LAUNCHPAD row's UNDO puts them back.

## 7. Telling players what is going on

The HSF tab in ReShade's menu shows the route, the motion source and, under WAITING or PROBLEM, the reason. COPY LOG TO CLIPBOARD (DEBUG section) collects HSF's log lines for a bug report. [troubleshooting.md](troubleshooting.md) lists the common messages and their fixes.
