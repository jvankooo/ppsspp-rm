#!/usr/bin/env python3
# Copyright (c) 2012- PPSSPP Project.
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 2.0 or later versions.
#
# Research tools for rumble profiles (docs/rumble.md), on the PC, through PPSSPP's WebSocket debugger.
#
# Start PPSSPP (or PPSSPPHeadless) with --debugger=PORT and a game, then:
#
#   python rumble_research.py PORT script.txt       run the commands in a file
#   python rumble_research.py PORT -c "run 5" -c "shot C:/tmp/a.png"
#   python rumble_research.py diff A.bin B.bin [C.bin ...] [--type float|u32|u16|u8]
#                                                   candidate addresses whose value changes between RAM dumps
#   python rumble_research.py cheats cheat.db UCUS98653
#                                                   the CWCheat codes for a game (often a health address)
#
# Commands (one per line in a script; # starts a comment):
#   run SECONDS                 run the emulator for that much emulated time, then stop it
#   press BUTTON [FRAMES]       press and release a PSP button (cross, circle, square, triangle, start, select,
#                               up, down, left, right, ltrigger, rtrigger)
#   hold BUTTON [BUTTON...]     keep buttons down (until release)
#   release                     let go of all buttons
#   stick X Y [left|right]      analog stick, -1..1
#   shot FILE.png               screenshot (headless: needs --graphics=software)
#   dump FILE.bin               user RAM 0x08000000..0x0A000000
#   read ADDR [u8|u16|u32|float]
#   profile FILE.ini            evaluate a rumble profile's values the way PPSSPP does, and print them
#   status                      emulated time
#
# Needs Python 3.10+ and the "websockets" package.

import argparse
import asyncio
import base64
import json
import re
import struct
import sys
from pathlib import Path

RAM_BASE = 0x08000000
RAM_SIZE = 0x02000000

BUTTONS = {"cross", "circle", "square", "triangle", "start", "select", "up", "down", "left", "right",
           "ltrigger", "rtrigger", "home"}


class Debugger:
    def __init__(self, port):
        self.port = port
        self.ws = None
        self.ticket = 0
        self.held = {}

    async def connect(self):
        import websockets
        self.ws = await websockets.connect(f"ws://127.0.0.1:{self.port}/debugger", subprotocols=["debugger.ppsspp.org"],
                                           max_size=None, open_timeout=10)
        # Stay quiet: log broadcasts would drown the answers.
        await self.call("broadcast.config.set", disallowed={"logger": True})

    async def call(self, event, timeout=60, wait_event=None, **params):
        self.ticket += 1
        ticket = self.ticket
        msg = {"event": event, "ticket": ticket}
        msg.update(params)
        await self.ws.send(json.dumps(msg))
        while True:
            raw = await asyncio.wait_for(self.ws.recv(), timeout)
            data = json.loads(raw)
            if data.get("event") == "error" and data.get("ticket") == ticket:
                raise RuntimeError(f"{event}: {data.get('message')}")
            if wait_event is not None:
                if data.get("event") == wait_event:
                    return data
                continue
            if data.get("ticket") == ticket:
                return data

    async def wait_event(self, name, timeout=600):
        while True:
            raw = await asyncio.wait_for(self.ws.recv(), timeout)
            data = json.loads(raw)
            if data.get("event") == name:
                return data

    async def stop(self):
        status = await self.call("cpu.status")
        if not status.get("stepping"):
            await self.ws.send(json.dumps({"event": "cpu.stepping"}))
            await self.wait_event("cpu.stepping", 30)

    async def run(self, seconds):
        await self.stop()
        await self.call("cpu.runUntilTime", relativeUs=int(seconds * 1e6))
        await self.wait_event("cpu.stepping", max(60, seconds * 30))

    async def read(self, addr, size):
        data = await self.call("memory.read", address=addr, size=size)
        return base64.b64decode(data["base64"])

    async def read_u32(self, addr):
        return (await self.call("memory.read_u32", address=addr))["value"]

    async def buttons(self):
        await self.call("input.buttons.send", buttons={b: True for b in self.held})


# --- rumble profiles (the same rules as Core/Rumble/Rumble.cpp) ---

def parse_ini(path):
    sections, cur = {}, None
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.split(";", 1)[0].split("#", 1)[0].strip()
        if not line:
            continue
        if line.startswith("[") and line.endswith("]"):
            cur = sections.setdefault(line[1:-1].strip(), {})
        elif "=" in line and cur is not None:
            k, v = line.split("=", 1)
            cur[k.strip().lower()] = v.strip()
    return sections


def parse_num(s):
    s = s.strip()
    return int(s, 0) if re.match(r"^[-+]?(0x[0-9a-fA-F]+|\d+)$", s) else float(s)


class Memory:
    """Reads through a RAM dump (bytes from RAM_BASE) or a live debugger (synchronously through a cache)."""

    def __init__(self, ram):
        self.ram = ram

    def valid(self, addr, size=4):
        return RAM_BASE <= addr and addr + size <= RAM_BASE + len(self.ram)

    def u32(self, addr):
        return struct.unpack_from("<I", self.ram, addr - RAM_BASE)[0]

    def read(self, addr, typ):
        fmt = {"u8": "<B", "u16": "<H", "u32": "<I", "s32": "<i", "float": "<f"}[typ]
        return struct.unpack_from(fmt, self.ram, addr - RAM_BASE)[0]


def resolve_path(mem, base, path):
    addr = base
    for tok in re.findall(r"find\([^)]*\)|\*|[+-]0x[0-9a-fA-F]+|[+-]\d+", path):
        if tok == "*":
            if not mem.valid(addr):
                return None
            addr = mem.u32(addr)
        elif tok.startswith("find("):
            count, off, key = [parse_num(x) for x in tok[5:-1].split(",")]
            found = None
            for i in range(count):
                if not mem.valid(addr + 4 * i):
                    break
                p = mem.u32(addr + 4 * i)
                if mem.valid(p + off) and mem.u32(p + off) == key:
                    found = p
                    break
            if found is None:
                return None
            addr = found
        else:
            addr += parse_num(tok)
    return addr


def eval_profile(mem, profile):
    out = []
    for name, sec in profile.items():
        if not name.lower().startswith("value."):
            continue
        typ = sec.get("type", "u32")
        size = {"u8": 1, "u16": 2}.get(typ, 4)
        addr = resolve_path(mem, parse_num(sec.get("address", "0")), sec.get("path", ""))
        if addr is None or not mem.valid(addr, size):
            out.append(f"{name[6:]:>16} = (not found)")
            continue
        v = mem.read(addr, typ)
        if "mask" in sec:
            v = int(v) & parse_num(sec["mask"])
        out.append(f"{name[6:]:>16} = {v} @ {addr:08X}")
    return out


# --- offline tools ---

def diff_dumps(files, typ):
    dumps = [Path(f).read_bytes() for f in files]
    size = {"u8": 1, "u16": 2}.get(typ, 4)
    fmt = {"u8": "<B", "u16": "<H", "u32": "<I", "float": "<f"}[typ]
    n = min(len(d) for d in dumps)
    hits = 0
    for off in range(0x800000, n - size, size if typ != "u8" else 1):  # skip the kernel's 8 MB
        vals = [struct.unpack_from(fmt, d, off)[0] for d in dumps]
        if len(set(vals)) == 1:
            continue
        if typ == "float" and not all(0.001 < abs(v) < 100000 or v == 0 for v in vals):
            continue
        print(f"{RAM_BASE + off:08X}: " + "  ".join(f"{v:.3f}" if typ == "float" else str(v) for v in vals))
        hits += 1
        if hits >= 400:
            print("... (more; narrow it down with another dump)")
            break


def list_cheats(db, disc_id):
    show = False
    for line in Path(db).read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("_S "):
            show = line[3:].strip().replace("-", "").upper() == disc_id.replace("-", "").upper()
        if show:
            print(line)


async def run_commands(port, commands):
    dbg = Debugger(port)
    await dbg.connect()
    for line in commands:
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        cmd, *args = line.split()
        print(f"> {line}", flush=True)
        if cmd == "run":
            await dbg.run(float(args[0]))
        elif cmd == "press":
            frames = int(args[1]) if len(args) > 1 else 4
            # input.buttons.press answers once released, which needs the CPU running.
            await dbg.stop()
            await dbg.ws.send(json.dumps({"event": "input.buttons.press", "button": args[0], "duration": frames}))
            await dbg.call("cpu.runUntilTime", relativeUs=int((frames + 6) * 16683))
            await dbg.wait_event("cpu.stepping", 60)
        elif cmd == "hold":
            for b in args:
                dbg.held[b] = True
            await dbg.buttons()
        elif cmd == "release":
            dbg.held = {}
            await dbg.call("input.buttons.send", buttons={b: False for b in BUTTONS})
        elif cmd == "stick":
            await dbg.call("input.analog.send", x=float(args[0]), y=float(args[1]), stick=args[2] if len(args) > 2 else "left")
        elif cmd == "shot":
            await dbg.stop()
            data = await dbg.call("gpu.buffer.screenshot", type="uri")
            Path(args[0]).write_bytes(base64.b64decode(data["uri"].split(",", 1)[1]))
            print(f"  saved {args[0]} ({data['width']}x{data['height']})")
        elif cmd == "dump":
            await dbg.stop()
            blob = bytearray()
            for off in range(0, RAM_SIZE, 0x100000):
                blob += await dbg.read(RAM_BASE + off, 0x100000)
            Path(args[0]).write_bytes(blob)
            print(f"  saved {args[0]}")
        elif cmd == "read":
            typ = args[1] if len(args) > 1 else "u32"
            addr = parse_num(args[0])
            raw = await dbg.read(addr, 4)
            print(f"  {addr:08X} = {Memory(raw).read(RAM_BASE, typ) if False else struct.unpack_from({'u8': '<B', 'u16': '<H', 'u32': '<I', 'float': '<f'}[typ], raw)[0]}")
        elif cmd == "profile":
            await dbg.stop()
            blob = bytearray()
            for off in range(0, RAM_SIZE, 0x100000):
                blob += await dbg.read(RAM_BASE + off, 0x100000)
            for l in eval_profile(Memory(bytes(blob)), parse_ini(args[0])):
                print("  " + l)
        elif cmd == "status":
            s = await dbg.call("cpu.status")
            print(f"  emulated {s.get('us', 0) / 1e6:.2f} s, stepping={s.get('stepping')}")
        else:
            print(f"  unknown command {cmd}")
    await dbg.ws.close()


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "diff":
        ap = argparse.ArgumentParser()
        ap.add_argument("cmd")
        ap.add_argument("files", nargs="+")
        ap.add_argument("--type", default="float", choices=["float", "u32", "u16", "u8"])
        a = ap.parse_args()
        diff_dumps(a.files, a.type)
        return
    if len(sys.argv) > 1 and sys.argv[1] == "cheats":
        list_cheats(sys.argv[2], sys.argv[3])
        return
    if len(sys.argv) > 1 and sys.argv[1] == "evaldump":
        # evaldump PROFILE.ini DUMP.bin: the values a profile finds in a RAM dump (a Switch snapshot too).
        for l in eval_profile(Memory(Path(sys.argv[3]).read_bytes()), parse_ini(sys.argv[2])):
            print(l)
        return
    ap = argparse.ArgumentParser()
    ap.add_argument("port", type=int)
    ap.add_argument("script", nargs="?")
    ap.add_argument("-c", action="append", default=[], help="a command (several allowed)")
    a = ap.parse_args()
    commands = list(a.c)
    if a.script:
        commands = Path(a.script).read_text(encoding="utf-8").splitlines() + commands
    asyncio.run(run_commands(a.port, commands))


if __name__ == "__main__":
    main()
