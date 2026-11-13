# FG showcase (sprite sheets + slow motion)

Turns a recording of the same jump with FG on and FG off into tagged sprite sheets and a slow-motion side-by-side video.

## 1. Record
Use **NVIDIA ShadowPlay** at 120 fps (it captures the display output, so DLSS-G's generated frames are in it; checked: 73/s distinct frames
with FG on). OBS Game Capture does not work (it hooks the game's Present and only sees the real frames) and ffmpeg desktop capture
drops/bursts frames. Record FG on, then FG off, same spot, camera still, jump with SPACE (one clip or two).
`record.ps1` (ffmpeg ddagrab, presses SPACE for you) is kept as a fallback but is not frame-accurate.

## 2. Run
```
python HSF\tools\showcase\showcase.py --off clip.mp4 --on clip.mp4 --cfr 120 --auto-align --off-jump <s> --on-jump <s> ^
   --crop x,y,w,h --base-fps 25 --fg-fps 75 --title "..." --out HSF\build\showcase\<name>
```
- Same file for `--off`/`--on` is fine; `--off-jump` / `--on-jump` = approximate seconds of the SPACE press in each part, `--auto-align` finds the exact onset.
- `--crop` = the character region in source pixels (the auto-detector fails when the background animates). `--cfr` = the clip's fps.
- `--base-fps` / `--fg-fps` = the numbers on the tags (default: measured). Take them from the HSF overlay / log.

Outputs: `compare_sheet.png` (consecutive recorded frames, repeats dimmed), `unique_ladder.png` (only the frames that changed, with ms gaps),
`slowmo.mp4` (1/N speed side by side, NEW/repeat light), `report.json`; `--onion` adds `onion_skin.png` (static camera only).
Every tile is tagged `FG OFF - <base> fps` / `FG ON - <fg> fps (Nx of <base>)`.

Other flags: `--slow`, `--dur`, `--tiles`, `--cols`, `--tile-w`, `--dup-thresh`, `--trail-ms`.

## Self-test
`python make_synthetic.py <dir>` writes synthetic 33/66 fps jump clips at 240 fps; the script measures 33.0 / 66.1 fps.
