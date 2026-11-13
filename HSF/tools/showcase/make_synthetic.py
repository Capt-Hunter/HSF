"""Self-test clips: a sprite jumping in an arc, recorded at 240 fps, content updated at 33 fps (off) and 66 fps (on).
Usage: python make_synthetic.py <out_dir>"""
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import showcase

ffmpeg = showcase.find_tool("ffmpeg")
W, H, REC, SECS = 960, 540, 240, 3.0
bg = (np.linspace(30, 90, W)[None, :, None] * np.ones((H, 1, 3))).astype(np.uint8)
yy, xx = np.mgrid[0:H, 0:W]


def render(t):
    img = bg.copy()
    tt = max(0.0, t - 0.5)  # the jump starts at 0.5 s
    x = 200 + 260 * tt
    y = min(400 - 600 * tt + 1000 * tt * tt, 400)
    img[(xx - x) ** 2 + (yy - y) ** 2 < 28 ** 2] = (250, 200, 60)
    return img


for name, rate in (("synthetic_off", 33), ("synthetic_on", 66)):
    p = subprocess.Popen([ffmpeg, "-y", "-v", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                          "-r", str(REC), "-i", "-", "-c:v", "libx264", "-crf", "12", "-pix_fmt", "yuv420p",
                          os.path.join(sys.argv[1], name + ".mp4")], stdin=subprocess.PIPE)
    last, cur = -1, None
    for i in range(int(SECS * REC)):
        step = int(i / REC * rate)
        if step != last:
            cur, last = render(step / rate), step
        p.stdin.write(cur.tobytes())
    p.stdin.close()
    p.wait()
