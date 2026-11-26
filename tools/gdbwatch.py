"""Lichii's (Eden's) GDB stub: let the game run, or catch what writes an address.

Turn on Lichii's GDB stub (Emulation > Configure > Debug > Enable GDB stub, port 6543; qt-config.ini use_gdbstub)
and start the game: it waits for a debugger. On Windows (the stub listens on Windows' localhost):

    py tools\\gdbwatch.py go                                   let it run, and detach
    py tools\\gdbwatch.py watch <addr> [--size 8] [--hits 5] [--seconds 60]
        a write watchpoint: each write prints the writing thread's pc, lr, x0..x7 and its frame-pointer chain
        (main offsets), then the game carries on; detaches cleanly at the end (FINDINGS 9.37)
    py tools\\gdbwatch.py break <main offset> [...] [--hits 5] [--seconds 60]
        breakpoints on main's code, the same print
    py tools\\gdbwatch.py read <addr> <size>                  memory, as hex

Only one connection at a time; a client killed mid-run can leave a breakpoint in (see gdbtrace.py's notes). Only the
standard library is used.
"""
import argparse
import re
import socket
import struct
import sys
import time


class TimeUp(Exception):
    pass


class Stub:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port))
        self.buf = b""
        self.deadline = None

    def _byte(self):
        while not self.buf:
            if self.deadline is not None:
                left = self.deadline - time.time()
                if left <= 0:
                    raise TimeUp
                self.sock.settimeout(left)
            try:
                data = self.sock.recv(65536)
            except socket.timeout:
                raise TimeUp
            finally:
                self.sock.settimeout(None)
            if not data:
                raise ConnectionError("the stub closed the connection")
            self.buf += data
        c, self.buf = self.buf[:1], self.buf[1:]
        return c

    def send(self, payload):
        data = payload.encode("latin-1")
        self.sock.sendall(b"$" + data + b"#" + b"%02x" % (sum(data) & 0xFF))
        return self.recv()

    def recv(self):
        while True:
            c = self._byte()
            if c != b"$":
                continue
            body = b""
            while (c := self._byte()) != b"#":
                body += c
            self._byte(), self._byte()
            self.sock.sendall(b"+")
            text = body.decode("latin-1")
            if text.startswith("O") and len(text) > 1 and all(ch in "0123456789abcdef" for ch in text[1:]):
                continue
            return text

    def read(self, addr, size):
        reply = self.send(f"m{addr:x},{size:x}")
        if not reply or (len(reply) == 3 and reply[0] == "E"):
            return None
        return bytes.fromhex(reply)

    def regs(self, thread):
        self.send(f"Hg{thread}")
        return list(struct.unpack_from("<33Q", bytes.fromhex(self.send("g"))))

    def threads(self):
        ids, reply = [], self.send("qfThreadInfo")
        while reply.startswith("m"):
            ids += reply[1:].split(",")
            reply = self.send("qsThreadInfo")
        return ids


def main_base(stub):
    xml, off = "", 0
    while True:
        reply = stub.send(f"qXfer:libraries:read::{off:x},fff")
        if not reply or reply[0] not in "ml":
            break
        xml += reply[1:]
        off += len(reply) - 1
        if reply[0] == "l":
            break
    libs = re.findall(r'<library name="([^"]*)"><segment address="(0x[0-9a-fA-F]+)"', xml)
    for name, addr in libs:
        if "Owl" in name or name.endswith("main"):
            return int(addr, 16)
    return sorted(int(a, 16) for _, a in libs)[1] if len(libs) > 1 else None


def describe(stub, base, thread, out):
    r = stub.regs(thread)

    def name(addr):
        for mask in ((1 << 48) - 1, (1 << 39) - 1):
            a = addr & mask
            if base and base <= a < base + 0x6000000:
                return f"main+{a - base:#x}"
        return f"{addr:#x}"

    out(f"  thread {thread} pc {name(r[32])} lr {name(r[30])} sp {r[31]:#x}")
    out("  " + "  ".join(f"x{i}={r[i]:#x}" for i in range(9)))
    out("  " + "  ".join(f"x{i}={r[i]:#x}" for i in range(19, 29)))
    fp, chain = r[29], []
    for _ in range(24):
        frame = stub.read(fp, 16) if fp else None
        if not frame:
            break
        fp, lr = struct.unpack("<QQ", frame)
        chain.append(name(lr))
    out("  callers: " + " <- ".join(chain))
    return r


def resume(stub):
    """Continue and close: this stub's D leaves a stopped game stopped."""
    data = b"c"
    stub.sock.sendall(b"$" + data + b"#" + b"%02x" % (sum(data) & 0xFF))
    time.sleep(1)
    stub.sock.close()


def run_until(stub, seconds):
    stub.deadline = time.time() + seconds
    try:
        return stub.send("c")
    finally:
        stub.deadline = None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["go", "watch", "break", "read"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--port", type=int, default=6543)
    ap.add_argument("--size", type=int, default=8)
    ap.add_argument("--hits", type=int, default=5)
    ap.add_argument("--seconds", type=float, default=60)
    a = ap.parse_args()
    out = lambda s: (print(s), sys.stdout.flush())
    stub = Stub(a.port)
    stub.send("qSupported:swbreak+;hwbreak+;xmlRegisters=aarch64")
    out("stop state: " + stub.send("?"))
    base = main_base(stub)
    out(f"main = {base:#x}" if base else "main not found")
    if a.mode == "go":  # continue, and leave it running (this stub's D doesn't resume a stopped game)
        resume(stub)
        out("continued: the game runs")
        return
    if a.mode == "read":
        addr, size = int(a.args[0], 16), int(a.args[1], 0)
        data = stub.read(addr, size)
        out(data.hex() if data else "unreadable")
        resume(stub)
        return
    if a.mode == "watch":
        points = [("Z2", int(a.args[0], 16), a.size)]
    else:
        points = [("Z0", base + int(o, 16), 4) for o in a.args]
    for kind, addr, size in points:
        out(f"{kind} {addr:#x}: " + stub.send(f"{kind},{addr:x},{size}"))
    hits, end = 0, time.time() + a.seconds
    try:
        while hits < a.hits:
            stop = run_until(stub, end - time.time())
            out(f"\n[{time.strftime('%H:%M:%S')}] stop {stop}")
            fields = dict(f.split(":", 1) for f in stop[3:].split(";") if ":" in f)
            thread = fields.get("thread")
            if thread is None:
                for t in stub.threads():
                    thread = t
                    break
            describe(stub, base, thread, out)
            hits += 1
            # step the stopped thread past it, with the points out, then put them back
            for kind, addr, size in points:
                stub.send(f"z{kind[1]},{addr:x},{size}")
            stub.send(f"Hc{thread}")
            stub.send("s")
            for kind, addr, size in points:
                stub.send(f"{kind},{addr:x},{size}")
    except TimeUp:
        out("time up: pausing to take the points out")
        stub.sock.sendall(b"\x03")
        stub.recv()
    for kind, addr, size in points:
        try:
            stub.send(f"z{kind[1]},{addr:x},{size}")
        except Exception:
            pass
    resume(stub)
    out("detached: the game runs")


if __name__ == "__main__":
    main()
