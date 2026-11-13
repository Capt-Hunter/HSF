#!/usr/bin/env python3
"""FG showcase: sprite sheets + slow-motion video from two recordings (FG off / FG on).

Needs ffmpeg (winget install Gyan.FFmpeg), numpy, Pillow. Record both clips at >= 2x the FG output fps
(240 fps is best) with OBS / ShadowPlay, never a GDI grab (DLSS-G stops generating). See README.md.

Outputs (in --out): compare_sheet.png, unique_ladder.png, slowmo.mp4, report.json (+ onion_skin.png with --onion)
"""
import argparse, glob, json, os, shutil, subprocess, sys
import numpy as np
from PIL import Image, ImageDraw, ImageFont

GREEN, RED, GREY, WHITE, BG = (60, 220, 110), (235, 80, 70), (150, 150, 150), (240, 240, 240), (18, 18, 22)


def find_tool(name):
    p = shutil.which(name)
    if p:
        return p
    pats = [os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Packages\Gyan.FFmpeg*\ffmpeg-*\bin\\" + name + ".exe"),
            r"C:\ffmpeg\bin\\" + name + ".exe", r"C:\ProgramData\chocolatey\bin\\" + name + ".exe"]
    for pat in pats:
        hits = glob.glob(pat)
        if hits:
            return hits[0]
    sys.exit(f"{name} not found. Install with: winget install Gyan.FFmpeg")


FFMPEG, FFPROBE = None, None


def font(size):
    for f in ("segoeuib.ttf", "arialbd.ttf", "consolab.ttf", "DejaVuSans-Bold.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def probe(path):
    out = subprocess.run([FFPROBE, "-v", "error", "-select_streams", "v:0", "-show_entries",
                          "stream=width,height,avg_frame_rate,r_frame_rate:format=duration",
                          "-of", "json", path], capture_output=True, text=True, check=True).stdout
    j = json.loads(out)
    s = j["streams"][0]
    num, den = (s.get("avg_frame_rate") or s["r_frame_rate"]).split("/")
    fps = float(num) / float(den)
    return dict(w=int(s["width"]), h=int(s["height"]), fps=fps, duration=float(j["format"]["duration"]))


def read_frames(path, start, dur, w, h, vf=None):
    """Decode every frame in [start, start+dur) as rgb24 arrays (no dropping, no duplicating)."""
    cmd = [FFMPEG, "-v", "error", "-ss", f"{start:.4f}", "-t", f"{dur:.4f}", "-i", path, "-fps_mode", "passthrough"]
    if vf:
        cmd += ["-vf", vf]
    cmd += ["-f", "rawvideo", "-pix_fmt", "rgb24", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    n, frames = w * h * 3, []
    while True:
        buf = p.stdout.read(n)
        if len(buf) < n:
            break
        frames.append(np.frombuffer(buf, np.uint8).reshape(h, w, 3))
    p.stdout.close()
    p.wait()
    return frames


def even(v):
    return int(v) // 2 * 2


def motion_onset(path, info, t0, t1, pre_vf="", drop=8):
    """Time (s) of the first big change in [t0, t1) (the jump / key press), from a downscaled scan of the crop."""
    cw, ch = (info["crop"][2], info["crop"][3]) if info.get("crop") else (info["w"], info["h"])
    w, h = even(cw // drop), even(ch // drop)
    fr = read_frames(path, t0, t1 - t0, w, h, vf=f"{pre_vf}scale={w}:{h}")
    g = np.stack([f.mean(axis=2) for f in fr]).astype(np.float32)
    d = np.abs(g[2:] - g[:-2]).mean(axis=(1, 2))
    base = d[: max(10, int(info["fps"] * 0.4))]
    thr = max(base.mean() + 6 * base.std(), base.mean() * 3, 0.15)
    hit = np.nonzero(d > thr)[0]
    return t0 + (int(hit[0]) + 1) / info["fps"] if len(hit) else t0


def auto_crop(path, info, start, dur, drop=4, pad=0.35):
    w, h = even(info["w"] // drop), even(info["h"] // drop)
    fr = read_frames(path, start, dur, w, h, vf=f"scale={w}:{h}")
    g = np.stack([f.mean(axis=2) for f in fr]).astype(np.float32)
    act = g.std(axis=0)
    mask = act > max(6.0, 0.35 * act.max())
    rows, cols = np.nonzero(mask.sum(axis=1) >= 2)[0], np.nonzero(mask.sum(axis=0) >= 2)[0]
    if len(rows) == 0 or len(cols) == 0:
        return 0, 0, even(info["w"]), even(info["h"])
    y0, y1, x0, x1 = rows[0], rows[-1], cols[0], cols[-1]
    bw, bh = (x1 - x0 + 1) * drop, (y1 - y0 + 1) * drop
    side = max(bw, bh) * (1 + 2 * pad)
    cx, cy = (x0 + x1 + 1) * drop / 2, (y0 + y1 + 1) * drop / 2
    cw, ch = min(info["w"], max(side, 320)), min(info["h"], max(side, 320))
    x, y = max(0, min(info["w"] - cw, cx - cw / 2)), max(0, min(info["h"] - ch, cy - ch / 2))
    return even(x), even(y), even(cw), even(ch)


def dup_flags(frames, thresh=None):
    """True where a frame repeats the previous one. Returns (flags, diffs)."""
    g = [f.astype(np.int16).mean(axis=2) for f in frames]
    d = np.array([0.0] + [float(np.abs(g[i] - g[i - 1]).mean()) for i in range(1, len(g))])
    if thresh is None:
        thresh = max(0.12, 3 * np.percentile(d[1:], 25), 0.08 * np.percentile(d[1:], 90))  # lossy clips: codec noise sits well below real motion
    flags = d < thresh
    flags[0] = False
    return flags, d


def tag_text(kind, base, fg, mult):
    if kind == "off":
        return f"FG OFF  -  {base:g} fps"
    return f"FG ON  -  {fg:g} fps  ({mult:g}x of {base:g})"


def draw_tile(frame, kind, label, t_ms, dup, base, fg, mult, tw, th, extra=None):
    im = Image.fromarray(frame).resize((tw, th), Image.LANCZOS)
    if dup:
        im = Image.blend(im, Image.new("RGB", im.size, (0, 0, 0)), 0.55)
    d = ImageDraw.Draw(im)
    col = RED if kind == "off" else GREEN
    d.rectangle([0, 0, tw - 1, th - 1], outline=(GREY if dup else col), width=1 if dup else 3)
    f, fs = font(max(11, tw // 17)), font(max(10, tw // 20))
    d.rectangle([0, 0, tw, f.size + 6], fill=(0, 0, 0))
    d.text((5, 2), tag_text(kind, base, fg, mult), fill=col, font=f)
    foot = label + (f"  t={t_ms:6.1f} ms" if t_ms is not None else "") + (f"  {extra}" if extra else "")
    d.rectangle([0, th - fs.size - 8, tw, th], fill=(0, 0, 0))
    d.text((5, th - fs.size - 6), foot, fill=GREY if dup else WHITE, font=fs)
    badge = "REPEAT" if dup else "NEW FRAME"
    bw = d.textlength(badge, font=fs) + 10
    d.rectangle([tw - bw - 4, f.size + 10, tw - 4, f.size + fs.size + 16], fill=GREY if dup else col)
    d.text((tw - bw + 1, f.size + 11), badge, fill=(0, 0, 0), font=fs)
    return im


def header(width, title, sub):
    im = Image.new("RGB", (width, 84), BG)
    d = ImageDraw.Draw(im)
    d.text((16, 8), title, fill=WHITE, font=font(34))
    d.text((16, 52), sub, fill=GREY, font=font(20))
    return im


def sheet(rows, title, sub, cols, tile_w, tile_h):
    """rows: list of (section_title, colour, [tiles])."""
    pad = 8
    W = cols * tile_w + (cols + 1) * pad
    parts = [header(W, title, sub)]
    for sec, col, tiles in rows:
        nrows = (len(tiles) + cols - 1) // cols
        sec_im = Image.new("RGB", (W, 44 + nrows * (tile_h + pad) + pad), BG)
        d = ImageDraw.Draw(sec_im)
        d.rectangle([0, 6, 10, 34], fill=col)
        d.text((22, 6), sec, fill=col, font=font(26))
        for i, t in enumerate(tiles):
            sec_im.paste(t, (pad + (i % cols) * (tile_w + pad), 44 + (i // cols) * (tile_h + pad)))
        parts.append(sec_im)
    out = Image.new("RGB", (W, sum(p.height for p in parts)), BG)
    y = 0
    for p in parts:
        out.paste(p, (0, y))
        y += p.height
    return out


def onion(frames, flags, kind, base, fg, mult, tw, th, rec_fps, trail_ms):
    uniq = [f for i, (f, dup) in enumerate(zip(frames, flags)) if not dup and i / rec_fps * 1000 <= trail_ms][:60]
    bg = np.median(np.stack(frames[::2]).astype(np.int16), axis=0)
    acc = bg.copy()
    for i, f in enumerate(uniq):
        m = (np.abs(f.astype(np.int16) - bg).max(axis=2) > 28)[..., None]
        a = 0.35 + 0.65 * (i + 1) / len(uniq)
        acc = np.where(m, acc * (1 - a) + f * a, acc)
    return draw_tile(acc.clip(0, 255).astype(np.uint8), kind, f"{len(uniq)} poses in {trail_ms:g} ms", None, False,
                     base, fg, mult, tw, th)


def main():
    global FFMPEG, FFPROBE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--off", required=True, help="recording with FG off")
    ap.add_argument("--on", required=True, help="recording with FG on")
    ap.add_argument("--base-fps", type=float, help="real/base fps (default: measured from the FG-off clip)")
    ap.add_argument("--fg-fps", type=float, help="output fps with FG (default: measured from the FG-on clip)")
    ap.add_argument("--title", default="DLSS Frame Generation - slow motion")
    ap.add_argument("--out", default=None)
    ap.add_argument("--start", type=float, help="window start (s) for both clips")
    ap.add_argument("--off-start", type=float)
    ap.add_argument("--on-start", type=float)
    ap.add_argument("--auto-align", action="store_true",
                    help="start each window at the first big motion (e.g. the SPACE jump) minus --pre")
    ap.add_argument("--onion", action="store_true", help="also write onion_skin.png (static camera only)")
    ap.add_argument("--trail-ms", type=float, default=300, help="time span of the motion-trail image")
    ap.add_argument("--off-jump", type=float, help="approx time (s) of the SPACE press in the FG-off clip (record.ps1 .json)")
    ap.add_argument("--on-jump", type=float, help="approx time (s) of the SPACE press in the FG-on clip")
    ap.add_argument("--cfr", type=float, help="resample variable-fps clips (record.ps1) to this constant fps, e.g. 240")
    ap.add_argument("--pre", type=float, default=0.03, help="seconds before motion onset with --auto-align")
    ap.add_argument("--dur", type=float, default=1.2, help="window length (s)")
    ap.add_argument("--crop", help="x,y,w,h in source pixels (default: auto-detect the moving region)")
    ap.add_argument("--tiles", type=int, default=16, help="consecutive recorded frames in the compare sheet")
    ap.add_argument("--cols", type=int, default=8)
    ap.add_argument("--tile-w", type=int, default=300)
    ap.add_argument("--slow", type=float, default=8, help="slow-motion factor for the video")
    ap.add_argument("--dup-thresh", type=float, help="mean-abs-diff below this = repeated frame")
    ap.add_argument("--max-height", type=int, default=720, help="downscale the crop to at most this height")
    a = ap.parse_args()

    FFMPEG, FFPROBE = find_tool("ffmpeg"), find_tool("ffprobe")
    out = a.out or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "showcase",
                                os.path.splitext(os.path.basename(a.on))[0])
    os.makedirs(out, exist_ok=True)
    vids = {"off": a.off, "on": a.on}
    info = {k: probe(v) for k, v in vids.items()}
    if a.cfr:
        for v in info.values():
            v["fps"] = a.cfr
    prefix = f"fps={a.cfr:g}:round=near," if a.cfr else ""
    crop = tuple(int(v) for v in a.crop.split(",")) if a.crop else None
    for v in info.values():
        v["crop"] = crop
    pre_vf = prefix + (f"crop={crop[2]}:{crop[3]}:{crop[0]}:{crop[1]}," if crop else "")
    print({k: (v["w"], v["h"], round(v["fps"], 2)) for k, v in info.items()})
    for k in info:
        if info[k]["fps"] < 100:
            print(f"WARNING: {k} clip is {info[k]['fps']:.0f} fps; record at 120-240 fps or FG frames get dropped.")

    start = {}
    for k in vids:
        s = getattr(a, f"{k}_start")
        if s is None:
            s = a.start
        if a.auto_align:
            jt = getattr(a, f"{k}_jump")
            t0, t1 = (max(0.0, jt - 1.0), jt + 2.5) if jt is not None else (0.0, 20.0)
            onset = motion_onset(vids[k], info[k], t0, t1, pre_vf)
            s = max(0.0, onset - a.pre)
            print(f"{k}: motion onset {onset:.3f}s -> window start {s:.3f}s")
        start[k] = s or 0.0

    if not crop:
        crop = auto_crop(vids["on"], info["on"], start["on"], min(a.dur, 1.0))
    cx, cy, cw, ch = crop
    sc = min(1.0, a.max_height / ch)
    ow, oh = even(cw * sc), even(ch * sc)
    print(f"crop {cx},{cy},{cw},{ch} -> {ow}x{oh}")
    vf = f"{prefix}crop={cw}:{ch}:{cx}:{cy},scale={ow}:{oh}"

    frames, flags, uniq_fps = {}, {}, {}
    for k in vids:
        fr = read_frames(vids[k], start[k], a.dur, ow, oh, vf=vf)
        if len(fr) < 4:
            sys.exit(f"{k}: only {len(fr)} frames decoded in the window; check --start/--dur")
        frames[k] = fr
        flags[k], _ = dup_flags(fr, a.dup_thresh)
        ui = [i for i in range(1, len(fr)) if not flags[k][i]]  # frame 0 is trivially "new"
        uniq_fps[k] = (len(ui) - 1) / ((ui[-1] - ui[0]) / info[k]["fps"]) if len(ui) > 2 else 0.0
        print(f"{k}: {len(fr)} recorded frames, {(~flags[k]).sum()} unique, measured {uniq_fps[k]:.1f} fps")

    base = a.base_fps or round(uniq_fps["off"], 1)
    fg = a.fg_fps or round(uniq_fps["on"], 1)
    mult = fg / base if base else 0
    sub = (f"Base {base:g} fps -> FG {fg:g} fps ({mult:.2f}x)  |  recorded at {info['on']['fps']:g} fps, "
           f"measured unique frames: off {uniq_fps['off']:.1f}/s, on {uniq_fps['on']:.1f}/s")
    tw = a.tile_w
    th = even(tw * oh / ow)

    # 1. compare sheet: consecutive recorded frames, same moment in both clips
    rows = []
    i0 = max(0, int(a.pre * info["on"]["fps"]) - 1)
    for k, sec, col in (("off", f"FG OFF - base {base:g} fps", RED), ("on", f"FG ON - {fg:g} fps ({mult:.2f}x)", GREEN)):
        tiles = [draw_tile(frames[k][i], k, f"rec #{i}", i / info[k]["fps"] * 1000, flags[k][i], base, fg, mult, tw, th)
                 for i in range(i0, min(i0 + a.tiles, len(frames[k])))]
        rows.append((sec, col, tiles))
    sheet(rows, a.title, "Consecutive recorded frames, dimmed = repeat of the previous one.  " + sub,
          a.cols, tw, th).save(os.path.join(out, "compare_sheet.png"))

    # 2. unique ladder: only the frames that changed, with the gap between them
    rows = []
    for k, sec, col in (("off", f"FG OFF - base {base:g} fps: distinct frames", RED),
                        ("on", f"FG ON - {fg:g} fps: distinct frames", GREEN)):
        idx = [i for i in range(len(frames[k])) if not flags[k][i]][: a.cols * 2]
        tiles = []
        for n, i in enumerate(idx):
            gap = (i - idx[n - 1]) / info[k]["fps"] * 1000 if n else 0
            tiles.append(draw_tile(frames[k][i], k, f"#{n + 1}", i / info[k]["fps"] * 1000, False, base, fg, mult,
                                   tw, th, extra=f"+{gap:.1f}ms" if n else None))
        rows.append((sec, col, tiles))
    sheet(rows, a.title + " - distinct frames",
          "Only frames that changed. +ms = time since the previous new frame (smaller and more even = smoother).  " + sub,
          a.cols, tw, th).save(os.path.join(out, "unique_ladder.png"))

    # 3. onion skin (opt-in: needs a static camera, the median background smears when the camera follows the character)
    if a.onion:
        otw = tw * 2
        oth = even(otw * oh / ow)
        rows = [(f"FG OFF - base {base:g} fps", RED, [onion(frames["off"], flags["off"], "off", base, fg, mult, otw, oth, info["off"]["fps"], a.trail_ms)]),
                (f"FG ON - {fg:g} fps", GREEN, [onion(frames["on"], flags["on"], "on", base, fg, mult, otw, oth, info["on"]["fps"], a.trail_ms)])]
        sheet(rows, a.title + " - motion trail", "Each pose = one distinct frame in the window, newest is solid.  " + sub,
              1, otw, oth).save(os.path.join(out, "onion_skin.png"))

    # 4. slow-motion side by side video
    n = min(len(frames["off"]), len(frames["on"]))
    vw, vh = tw * 2, even(tw * 2 * oh / ow)
    vfps = max(1.0, info["on"]["fps"] / a.slow)
    W, H = even(vw * 2 + 30), even(vh + 150)
    enc = subprocess.Popen([FFMPEG, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                            "-r", f"{vfps:g}", "-i", "-", "-c:v", "libx264", "-crf", "14", "-preset", "medium",
                            "-pix_fmt", "yuv420p", os.path.join(out, "slowmo.mp4")], stdin=subprocess.PIPE)
    fT, fS, fTag = font(34), font(22), font(30)
    for i in range(n):
        canvas = Image.new("RGB", (W, H), BG)
        d = ImageDraw.Draw(canvas)
        d.text((16, 8), a.title, fill=WHITE, font=fT)
        d.text((16, 52), f"SLOW MOTION 1/{a.slow:g}x   |   real time {i / info['on']['fps'] * 1000:6.1f} ms", fill=GREY, font=fS)
        for j, (k, col) in enumerate((("off", RED), ("on", GREEN))):
            x0 = 10 + j * (vw + 10)
            canvas.paste(Image.fromarray(frames[k][i]).resize((vw, vh), Image.LANCZOS), (x0, 100))
            d.rectangle([x0 - 2, 98, x0 + vw + 1, 100 + vh + 1], outline=col, width=3)
            d.text((x0, 100 + vh + 8), tag_text(k, base, fg, mult), fill=col, font=fTag)
            new = not flags[k][i]
            d.ellipse([x0 + vw - 36, 112, x0 + vw - 10, 138], fill=col if new else (60, 60, 60), outline=(0, 0, 0))
            d.text((x0 + vw - 130, 112), "NEW" if new else "repeat", fill=col if new else GREY, font=fS)
        enc.stdin.write(np.asarray(canvas).tobytes())
    enc.stdin.close()
    enc.wait()

    with open(os.path.join(out, "report.json"), "w") as fh:
        json.dump(dict(base_fps=base, fg_fps=fg, multiplier=mult, crop=crop, window=start, dur=a.dur,
                       recorded_fps={k: info[k]["fps"] for k in info}, unique_fps=uniq_fps,
                       recorded_frames={k: len(frames[k]) for k in frames}), fh, indent=2)
    print("wrote", os.path.abspath(out))


if __name__ == "__main__":
    main()
