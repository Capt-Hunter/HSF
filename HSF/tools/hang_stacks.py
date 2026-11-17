"""Reads a full memory dump of a hung 64-bit game (Task Manager: right-click the game > Create memory dump file) and prints, for each thread, where
it stands and the calls on its stack as module+offset. No debugger or symbols needed: it shows which DLL each thread is stuck in (HSF, Streamline,
DLSS-G, NGX, RenoDX, ReShade, the driver), which is what a hang needs first.

    python -I tools\\hang_stacks.py <game.DMP> [--all]

Return addresses are found by scanning each thread's stack for values that point just after a call instruction in a loaded module, so a line can
be a leftover from an earlier call: the first lines of each thread are the sure ones. Without --all, threads that only wait in Windows (every frame
in ntdll, KernelBase, kernel32 and the like) are left out.
"""
import bisect
import mmap
import struct
import sys

WINDOWS = {"ntdll.dll", "kernelbase.dll", "kernel32.dll", "win32u.dll", "user32.dll", "combase.dll", "rpcrt4.dll", "ucrtbase.dll", "msvcp_win.dll", "sechost.dll"}
STACK_SCAN = 96 * 1024
MAX_FRAMES = 30


class Dump:
    def __init__(self, path):
        # Mapped, not read: a full dump of a game is often 10 GB or more, and only the stacks and a few bytes of code are looked at.
        self._file = open(path, "rb")
        self.data = mmap.mmap(self._file.fileno(), 0, access=mmap.ACCESS_READ)
        signature, _, streams, directory = struct.unpack_from("<4sIII", self.data, 0)
        if signature != b"MDMP":
            raise SystemExit("not a minidump: " + path)
        self.streams = {}
        for i in range(streams):
            kind, size, rva = struct.unpack_from("<III", self.data, directory + i * 12)
            self.streams[kind] = (size, rva)
        self.modules = self._modules()
        self.ranges = self._memory()
        self._starts = [r[0] for r in self.ranges]

    def _modules(self):
        if 4 not in self.streams:
            return []
        _, rva = self.streams[4]
        (count,) = struct.unpack_from("<I", self.data, rva)
        out = []
        for i in range(count):
            at = rva + 4 + i * 108
            base, size, _, _, name_rva = struct.unpack_from("<QIIII", self.data, at)
            (length,) = struct.unpack_from("<I", self.data, name_rva)
            name = self.data[name_rva + 4:name_rva + 4 + length].decode("utf-16-le", "replace")
            out.append((base, size, name))
        out.sort()
        return out

    def _memory(self):
        ranges = []
        if 9 in self.streams:  # Memory64ListStream: a full dump
            _, rva = self.streams[9]
            count, data_rva = struct.unpack_from("<QQ", self.data, rva)
            for i in range(count):
                start, size = struct.unpack_from("<QQ", self.data, rva + 16 + i * 16)
                ranges.append((start, size, data_rva))
                data_rva += size
        if 5 in self.streams:  # MemoryListStream: a small dump
            _, rva = self.streams[5]
            (count,) = struct.unpack_from("<I", self.data, rva)
            for i in range(count):
                start, size, mem_rva = struct.unpack_from("<QII", self.data, rva + 4 + i * 16)
                ranges.append((start, size, mem_rva))
        ranges.sort()
        return ranges

    def read(self, address, size):
        i = bisect.bisect_right(self._starts, address) - 1
        if i < 0:
            return b""
        start, length, rva = self.ranges[i]
        if address >= start + length:
            return b""
        size = min(size, start + length - address)
        return self.data[rva + address - start:rva + address - start + size]

    def module_of(self, address):
        lo, hi = 0, len(self.modules) - 1
        while lo <= hi:
            mid = (lo + hi) // 2
            base, size, name = self.modules[mid]
            if address < base:
                hi = mid - 1
            elif address >= base + size:
                lo = mid + 1
            else:
                return base, name
        return None, None

    def where(self, address):
        base, name = self.module_of(address)
        if base is None:
            return "0x%016x (no module)" % address
        return "%s+0x%x" % (name.split("\\")[-1], address - base)

    def after_call(self, address):
        """True if the bytes before address end in a call: E8 rel32, FF 15 [rip], FF /2 forms."""
        before = self.read(address - 7, 7)
        if len(before) < 7:
            return False
        if before[2] == 0xE8:
            return True
        if before[1] == 0xFF and before[2] == 0x15:
            return True
        for back in (2, 3, 4, 5, 6, 7):  # FF /2 with a register, a register and offset, or a SIB byte
            op, modrm = before[7 - back], before[7 - back + 1]
            if op == 0xFF and (modrm >> 3) & 7 == 2:
                return True
        return False

    def threads(self):
        if 3 not in self.streams:
            return []
        _, rva = self.streams[3]
        (count,) = struct.unpack_from("<I", self.data, rva)
        out = []
        for i in range(count):
            at = rva + 4 + i * 48
            tid, suspend, _, _, _, stack_start, stack_size, stack_rva, context_size, context_rva = struct.unpack_from("<IIIIQQIIII", self.data, at)
            rsp, = struct.unpack_from("<Q", self.data, context_rva + 0x98)
            rip, = struct.unpack_from("<Q", self.data, context_rva + 0xF8)
            out.append((tid, rip, rsp))
        return out

    def stack(self, rsp):
        raw = self.read(rsp, STACK_SCAN)
        frames = []
        for off in range(0, len(raw) - 7, 8):
            (value,) = struct.unpack_from("<Q", raw, off)
            base, _ = self.module_of(value)
            if base is not None and self.after_call(value):
                frames.append(value)
                if len(frames) >= MAX_FRAMES:
                    break
        return frames


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not args:
        raise SystemExit(__doc__)
    show_all = "--all" in sys.argv
    dump = Dump(args[0])
    threads = dump.threads()
    print("%d threads, %d modules, %d memory ranges" % (len(threads), len(dump.modules), len(dump.ranges)))
    if not dump.ranges:
        print("this dump has no memory: make a full dump (Task Manager > Create memory dump file)")
    left_out = 0
    for tid, rip, rsp in threads:
        frames = [rip] + dump.stack(rsp)
        names = [(dump.module_of(a)[1] or "").split("\\")[-1].lower() for a in frames]
        if not show_all and all(n in WINDOWS or n == "" for n in names):
            left_out += 1
            continue
        print("\nthread %d (0x%x)" % (tid, tid))
        print("  now  " + dump.where(rip))
        for address in frames[1:]:
            print("       " + dump.where(address))
    if left_out:
        print("\n%d threads only waiting in Windows left out (--all shows them)" % left_out)


if __name__ == "__main__":
    main()
