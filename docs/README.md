# HSF documentation

HSF (Hunter's Sloppy Frames) by Capt-Hunter is a ReShade add-on that brings NVIDIA DLSS Frame Generation (DLSS-G), up to 6x, to almost any game. It works with games that have no frame generation of their own: DirectX 9, 11 and 12 and Vulkan, 32-bit and 64-bit, SDR and HDR. It feeds DLSS-G with the motion that the game gives through its own DLSS (or through a Luma mod's DLSS). When the game has no such motion, HSF uses optical flow from a ReShade effect (iMMERSE LaunchPad or LumeniteFX) together with the game's depth.

Preview (0.9.8): HSF runs in a wide range of DX9, DX11, DX12 and Vulkan games, and in PS3 and PS5 emulators. HSF is being added to [RHI (ReShade HDR Installer)](https://github.com/RankFTW/RHI), which will install it for you.

## Documents

| Document | For | What it covers |
|---|---|---|
| [user-guide.md](user-guide.md) | players and testers | what you need, how to install per game type, the in-game menu row by row, the overlays, hotkeys |
| [settings.md](settings.md) | testers, developers | every `HSF.ini` key: default, values, what it does, which route reads it |
| [architecture.md](architecture.md) | developers | the routes (D3D12 in-game, D3D12 bring-in, NGX, D3D11 helper, 32-bit helper, DX9 bridge, Vulkan bridge), the frame hand-off, motion and depth, HDR, Reflex, pacing |
| [source-map.md](source-map.md) | developers | every source file, test, tool and shader, and what it holds |
| [building-and-testing.md](building-and-testing.md) | developers | build, install scripts, offline tests, the rules for testing in games |
| [troubleshooting.md](troubleshooting.md) | everyone | known problems, per-game notes, logs, and lessons learned the hard way |
| [installers.md](installers.md) | installers and mod managers (RHI) | the release zip, where each file goes, what HSF needs, updates, uninstall |

## The short version

Download the latest `HSF-<version>.zip` from the [releases page](https://github.com/Capt-Hunter/HSF/releases).

1. Install ReShade 6.8 with add-on support into the game.
2. Copy `hsf.addon64` (64-bit games) or `hsf.addon32` (32-bit games) next to ReShade.
3. Copy the `HSF_streamline` folder next to it: Streamline 2.14 and `hsf-dlssg-helper64.exe`.
4. Copy `HSF.fx` into `reshade-shaders\Shaders`.
5. If the game has no DLSS of its own, add a flow source: iMMERSE LaunchPad (download it yourself from Marty's iMMERSE) or LumeniteFX Kernel, with its include folder.
6. Start the game, open ReShade, then open the HSF tab. Turn FRAME GEN on and pick a MULTIPLIER.
7. A DirectX 12 game asks for one restart the first time, while HSF sets up its route.

The user guide explains each step and the exceptions.
