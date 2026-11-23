"""Reads HSF.log and ReLimiter's log from one game folder and says whether ReLimiter saw HSF's frame generation the way DLSS-G got it.

Usage: python relimiter_check.py <game exe folder>      (exit code 0 = pass, 1 = fail, 2 = logs missing)

Pass means: ReLimiter never read mode 0 (off) from slDLSSGSetOptions while HSF had DLSS-G on (the game's own "off" getting through), and its
present hand-off never disagreed with ETW. The two logs' clocks are matched on the game's first Reflex call, which both log (HSF's "game's
slReflexSetOptions", ReLimiter's "game SetSleepMode"), so a mode 0 read is checked against HSF's own on and off lines within a second and a half.
"""
import glob
import os
import re
import sys


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read().splitlines()


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    folder = sys.argv[1]
    hsf_path = os.path.join(folder, "HSF.log")
    rl_paths = [p for p in glob.glob(os.path.join(folder, "relimiter_*.log")) if not p.endswith(".prev.log")]
    if not os.path.exists(hsf_path) or not rl_paths:
        print("missing: " + ("HSF.log " if not os.path.exists(hsf_path) else "") + ("relimiter_*.log" if not rl_paths else ""))
        return 2
    hsf = read(hsf_path)
    rl = read(rl_paths[0])
    fails, warns = [], []

    # ---- HSF ----
    entry = "not seen"
    for line in hsf:
        if "hook slDLSSGSetOptions installed at" in line:
            entry = "patch on DLSS-G's code"
        elif "through the plugin hook: DLSS-G's code is not patched" in line or "HSF's stub from the last load" in line:
            entry = "plugin route stub"
    sets = [l for l in hsf if "HSF sets DLSS-G " in l]
    timeline = []  # (seconds of the day, HSF has DLSS-G on)
    for l in sets:
        m = re.match(r"(\d+):(\d+):(\d+)\.(\d+)", l)
        if m:
            timeline.append((int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000, "DLSS-G ON" in l))
    first_on = next((i for i, l in enumerate(sets) if "HSF sets DLSS-G ON" in l), None)
    after_on = sets[first_on:] if first_on is not None else []
    hsf_off = sum(1 for l in after_on if "HSF sets DLSS-G off" in l)
    off_steps = sum(1 for l in sets if "for a new frame count" in l)
    direct = sum(1 for l in sets if "a new frame count with no off step" in l)
    echoes = sum(1 for l in sets if "again after the game's own options" in l)
    carried = any("now carry HSF's mode and frame count" in l for l in hsf)
    not_written = any("could not be changed: HSF sends its own again" in l for l in hsf)
    reasons = {}
    for l in sets:
        m = re.search(r"optimal \d+x\d+(, [^:]+)?: result", l)
        why = (m.group(1) or "").lstrip(", ") if m else "?"
        state = "ON" if "DLSS-G ON" in l else "off"
        key = f"{state} {why}".strip()
        reasons[key] = reasons.get(key, 0) + 1

    print("HSF.log")
    print(f"  options entry HSF hooked: {entry}")
    print(f"  HSF set DLSS-G {len(sets)} times, off {hsf_off} times after it first went on")
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f"    {v:4}  {k}")
    print(f"  new frame count: {direct} with no off step, {off_steps} with an off step")
    print(f"  game's struct carries HSF's mode: {'yes' if carried else 'no'}; could not be written: {'yes' if not_written else 'no'}; resends after the game's options: {echoes}")
    if first_on is None:
        fails.append("HSF never turned DLSS-G on: nothing to check (was HSF driving DLSS-G?)")
    if off_steps:
        warns.append(f"{off_steps} frame count change(s) took an off step: DLSS-G did not take the count directly")
    if not_written:
        warns.append("the game's options struct could not be written: ReLimiter relies on HSF's resends (at most every 500 ms)")

    # ---- ReLimiter ----
    states = []
    for l in rl:
        m = re.match(r"\[\s*([\d.]+)\] \[\w+\] FG state \(\w+\): presenting (yes|no), multiplier (\d+).*?statement \d+, mode (unknown )?(\d+).*FG input route (\w+)", l)
        if m:
            states.append((float(m.group(1)), m.group(2) == "yes", int(m.group(3)), m.group(4) is None, int(m.group(5)), m.group(6)))
    first_yes = next((i for i, s in enumerate(states) if s[1]), None)
    rl_after = states[first_yes:] if first_yes is not None else []
    rl_no = sum(1 for s in rl_after if not s[1])
    mode_reads = [s for s in rl_after if s[3] and s[4] == 0]
    # ReLimiter's clock (ms from its start) onto HSF's (time of day), from the game's first Reflex call: both log it as it comes.
    offset = None
    hsf_reflex = next((l for l in hsf if "game's slReflexSetOptions" in l), None)
    rl_reflex = next((l for l in rl if "NvAPI: game SetSleepMode" in l), None)
    if hsf_reflex and rl_reflex:
        h = re.match(r"(\d+):(\d+):(\d+)\.(\d+)", hsf_reflex)
        r = re.match(r"\[\s*([\d.]+)\]", rl_reflex)
        if h and r:
            offset = int(h.group(1)) * 3600 + int(h.group(2)) * 60 + int(h.group(3)) + int(h.group(4)) / 1000 - float(r.group(1)) / 1000

    def hsf_on_at(t):
        state = False
        for at, on in timeline:
            if at > t:
                break
            state = on
        return state

    mode_off = []  # mode 0 read while HSF had DLSS-G on the whole time around it
    for st in mode_reads:
        t = st[0] / 1000 + offset if offset is not None else None
        if t is None or all(hsf_on_at(t + d) for d in (-1.5, -0.75, 0.0, 0.75, 1.5)):
            mode_off.append(st)
    disagrees = sum(1 for l in rl if "disagrees with ETW" in l)
    errors = [l for l in rl if "[ERROR]" in l or "MH_ERROR" in l]

    print(os.path.basename(rl_paths[0]))
    print(f"  FG state changed {len(states)} times, 'not presenting' {rl_no} times after FG first presented")
    for t, yes, mult, known, mode, route in states:
        clock = ""
        if offset is not None:
            c = t / 1000 + offset
            clock = f" ({int(c // 3600) % 24:02}:{int(c % 3600 // 60):02}:{c % 60:06.3f}, HSF {'on ' if hsf_on_at(c) else 'off'})"
        print(f"    {t / 1000:8.1f} s{clock}  presenting {'yes' if yes else 'no '}  x{mult}  mode {mode if known else '?'}  route {route}")
    print(f"  read mode 0 after FG first presented: {len(mode_reads)}, of them while HSF had DLSS-G on: {len(mode_off)}")
    print(f"  present hand-off disagrees with ETW: {disagrees}")
    print(f"  errors: {len(errors)}")
    for l in errors[:5]:
        print("    " + l[:200])

    # Time in each pacing reason, from one 'Pacing pipeline' line to the next.
    pipes = []
    for l in rl:
        m = re.match(r"\[\s*([\d.]+)\] \[\w+\] Pacing pipeline \d+: mode=\w+ placement=\w+ rung=(\w+) reason=(\w+)", l)
        if m:
            pipes.append((float(m.group(1)), m.group(2), m.group(3)))
    if pipes:
        end = max(float(re.match(r"\[\s*([\d.]+)\]", l).group(1)) for l in rl if re.match(r"\[\s*[\d.]+\]", l))
        spent, longest_unq = {}, 0.0
        for i, (t, rung, reason) in enumerate(pipes):
            nxt = pipes[i + 1][0] if i + 1 < len(pipes) else end
            key = f"rung {rung}, reason {reason}"
            spent[key] = spent.get(key, 0.0) + (nxt - t)
            if reason == "fg_unqualified":
                longest_unq = max(longest_unq, nxt - t)
        print("  pacing time:")
        for k, v in sorted(spent.items(), key=lambda kv: -kv[1]):
            print(f"    {v / 1000:8.1f} s  {k}")
        print(f"  longest fg_unqualified stretch: {longest_unq / 1000:.1f} s")
        drops = [t for i, (t, rung, reason) in enumerate(pipes) if reason == "fg_unqualified" and i > 0 and pipes[i - 1][2] == "none"]
        print(f"  times the FG route was lost while pacing on it: {len(drops)}")
        if offset is not None and drops:
            c = drops[0] / 1000 + offset
            print(f"    first at {int(c // 3600) % 24:02}:{int(c % 3600 // 60):02}:{c % 60:06.3f}")
        if len(drops) > 10:
            warns.append(f"ReLimiter lost the FG route {len(drops)} times while pacing on it (each a short fg_unqualified stretch)")
        if longest_unq > 3000:
            warns.append(f"ReLimiter waited {longest_unq / 1000:.1f} s in fg_unqualified once (normal is well under a second after a change)")

    if first_yes is None and first_on is not None:
        fails.append("HSF turned DLSS-G on, but ReLimiter never saw frame generation presenting")
    if mode_off:
        fails.append(f"ReLimiter read mode 0 {len(mode_off)} time(s) while HSF had DLSS-G on: it saw the game's own options, not HSF's")
    # 'Not presenting' while ReLimiter reads mode on is DLSS-G itself showing no generated frames for a moment (it starts again on a new viewport when a
    # game loads a level): ReLimiter read HSF's options right. Only more of those than HSF's own offs is worth a look.
    rl_no_mode_on = sum(1 for s in rl_after if not s[1] and s[3] and s[4] != 0)
    if first_on is not None and rl_no > hsf_off:
        warns.append(f"ReLimiter saw 'not presenting' {rl_no} times, HSF turned DLSS-G off {hsf_off} times; {rl_no_mode_on} of them with mode on "
                     "(DLSS-G itself paused, e.g. a level load; compare with 'DLSS-G now shows 1 per real frame' in HSF.log)")
    if disagrees:
        fails.append(f"{disagrees} present hand-off disagreements with ETW")

    print()
    for w in warns:
        print("WARN  " + w)
    for f in fails:
        print("FAIL  " + f)
    print("PASS" if not fails else "FAILED")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
