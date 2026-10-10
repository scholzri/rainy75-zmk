#!/usr/bin/env python3
"""
Read the open BLE controller's power counters (mcumgr group 66, command 0,
and the per-link arbiter counters of command 1 where the firmware has it).

Works over USB CDC-ACM serial (default) or BLE SMP GATT (--ble). The firmware
must be built with the open controller (CONFIG_OPENLL_MGMT, enabled by
conf/openll.conf). BLE needs `bleak`; reuses the helpers of rainy75_rgb.py and
rainy75_rgb_ble.py (connected-peripheral handling: the device is resolved via
BlueZ and an already-live link is never disconnected).

Usage:
    python3 openll_stats.py                 # one read over USB serial
    python3 openll_stats.py --ble           # one read over BLE
    python3 openll_stats.py --ble -n 5 -i 10  # 5 reads, 10 s apart, with deltas
    python3 openll_stats.py --port /dev/ttyACM0 --address XX:XX:XX:XX:XX:XX

Fields: up (ms), idle (CPU idle ms, if built in), plan/listen/skip (connection events planned / listened /
skipped by peripheral latency), kick, ev, miss, wake (controller thread
wakeups), mv (battery, 0 = unavailable). With multilink firmware (slice 6a)
the connection counters are sums over the links, and the reply adds links
(links up), link (per link id: up, listen, skip, coll = events yielded to
the arbiter, miss) and adv (advertising events, slid, drop, cut, stuck).
Firmware with the flash window adds flash (win = flash operations covered,
wait = waits for the links, force = opened without them, wmax / hmax =
longest wait / longest window in us (maxima, no deltas), pause / cut =
connection events not issued / ended by it, fkick = events pulled in for
it, abort = radio activity ended, pskip = RX DMA ring overruns, which must
stay 0).
Firmware with the starvation bound sends "link" as positional lists
[up, listen, skip, coll, miss] and adds command 1 ("arb": per link id
[gmax, gus, gx, elen, clip, lost...]: longest listen gap in events / us /
events beyond the latency window, longest event in us, clipped starts, events
lost to each arbiter priority); both are merged into one dict per link.
Counters are uint32, cumulative since boot; deltas are computed modulo 2**32.
"""

import argparse
import asyncio
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

GROUP = 66          # MGMT_GROUP_ID_PERUSER + 2
CMD_STATS = 0
CMD_ARB = 1
# positional per-link lists (command 0 "link", command 1 "arb")
LINK_FIELDS = ("up", "listen", "skip", "coll", "miss")
ARB_FIELDS = ("gmax", "gus", "gx", "elen", "clip")
SMP_OP_READ = 0
FIELDS = ("up", "idle", "plan", "listen", "skip", "kick", "ev", "miss", "wake", "mv")
COUNTERS = ("idle", "plan", "listen", "skip", "kick", "ev", "miss", "wake")
LINK_COUNTERS = ("listen", "skip", "coll", "miss", "clip")
# arbiter priorities of the "lost" list (5 entries before the STARVING level)
PRIO_NAMES = ("adv", "idle", "active", "starving", "sup", "must")
PRIO_NAMES_5 = ("adv", "idle", "active", "sup", "must")
ADV_COUNTERS = ("ev", "slid", "drop", "cut", "stuck")
FLASH_FIELDS = ("win", "wait", "force", "wmax", "hmax", "pause", "cut", "fkick", "abort", "pskip")
# wmax / hmax are maxima since boot (longest wait, longest window): no deltas
FLASH_COUNTERS = tuple(k for k in FLASH_FIELDS if k not in ("wmax", "hmax"))


# --- pure helpers (unit tested) --------------------------------------------

def build_request(seq, group=GROUP, cmd=CMD_STATS):
    """SMP read request with an empty CBOR map: 8-byte header + 0xA0."""
    return struct.pack(">BBHHBB", SMP_OP_READ, 0, 1, group, seq & 0xFF, cmd) + b"\xa0"


def parse_response(frame):
    """Raw SMP frame (header + CBOR) -> stats dict. Raises on rc != 0."""
    from rainy75_rgb import _cbor_decode
    if len(frame) < 9:
        raise ValueError("short SMP frame")
    body, _ = _cbor_decode(frame[8:])
    if not isinstance(body, dict):
        raise ValueError("SMP body is not a map")
    rc = body.get("rc", 0)
    if rc != 0:
        raise RuntimeError(f"device rc={rc}")
    return body


def normalize(stats, arb=None):
    """Merge a command 0 reply and an optional command 1 reply into the dict
    format_stats() takes: positional per-link lists become dicts (maps of
    older firmware stay as they are), the "arb" lists add gmax, gus, gx, elen,
    clip and lost to the link dicts."""
    links = stats.get("link")
    if isinstance(links, list):
        stats["link"] = [dict(zip(LINK_FIELDS, lk)) if isinstance(lk, list) else lk
                         for lk in links]
    rows = arb.get("arb") if isinstance(arb, dict) else None
    if isinstance(rows, list) and isinstance(stats.get("link"), list):
        for lk, row in zip(stats["link"], rows):
            lk.update(zip(ARB_FIELDS, row))
            lk["lost"] = list(row[len(ARB_FIELDS):])
    return stats


def _sub(new, old, keys):
    return {k: (new[k] - old[k]) & 0xFFFFFFFF for k in keys if k in new and k in old}


def delta(new, old):
    """Per-field difference of two stats dicts; counters wrap at 2**32.
    Per-link ("link") and advertising ("adv") counters are differenced too."""
    d = _sub(new, old, ("up",) + COUNTERS)
    if isinstance(new.get("link"), list) and isinstance(old.get("link"), list):
        d["link"] = [_sub(n, p, LINK_COUNTERS) for n, p in zip(new["link"], old["link"])]
    if isinstance(new.get("adv"), dict) and isinstance(old.get("adv"), dict):
        d["adv"] = _sub(new["adv"], old["adv"], ADV_COUNTERS)
    if isinstance(new.get("flash"), dict) and isinstance(old.get("flash"), dict):
        d["flash"] = _sub(new["flash"], old["flash"], FLASH_COUNTERS)
    return d


def format_stats(s, d=None):
    idle = (f"idle {100.0 * s['idle'] / s['up']:.1f}%  "
            if "idle" in s and s["up"] else "")
    line = (f"up {s['up'] / 1000:.1f}s  {idle}plan {s['plan']}  listen {s['listen']}  "
            f"skip {s['skip']}  kick {s['kick']}  ev {s['ev']}  miss {s['miss']}  "
            f"wake {s['wake']}  batt {s['mv']} mV")
    if "links" in s:
        line += f"  links {s['links']}"
    for i, lk in enumerate(s.get("link") or []):
        if lk.get("up") or lk.get("listen"):
            # counters are cumulative per link id, also after the link ended
            line += (f"\n  link {i}: {'up' if lk.get('up') else 'down'} "
                     f"listen {lk.get('listen', 0)} skip {lk.get('skip', 0)} "
                     f"coll {lk.get('coll', 0)} miss {lk.get('miss', 0)}")
        else:
            line += f"\n  link {i}: down"
        if "gmax" in lk:
            lost = lk.get("lost") or []
            line += (f"\n    gap max {lk.get('gmax', 0)} ev / {lk.get('gus', 0) / 1000:.1f} ms"
                     f" (+{lk.get('gx', 0)} beyond latency)  event max "
                     f"{lk.get('elen', 0)} us  clip {lk.get('clip', 0)}  lost to "
                     + " ".join(f"{n} {v}" for n, v in
                                zip(PRIO_NAMES if len(lost) != 5 else PRIO_NAMES_5, lost)))
    if isinstance(s.get("adv"), dict):
        a = s["adv"]
        line += "\n  adv: " + " ".join(f"{k} {a.get(k, 0)}" for k in ADV_COUNTERS)
    if isinstance(s.get("flash"), dict):
        f = s["flash"]
        line += "\n  flash: " + " ".join(f"{k} {f.get(k, 0)}" for k in FLASH_FIELDS)
    if d and d.get("up"):
        secs = d["up"] / 1000.0
        tot = d.get("listen", 0) + d.get("skip", 0)
        ratio = f"{100.0 * d['skip'] / tot:.0f}% skipped" if tot else "no events"
        if "idle" in d:
            line += f"\n  delta idle {100.0 * d['idle'] / d['up']:.1f}%"
        line += (f"\n  delta {secs:.1f}s: listen {d.get('listen', 0)} "
                 f"skip {d.get('skip', 0)} ({ratio}) wake {d.get('wake', 0)} "
                 f"({d.get('wake', 0) / secs:.1f}/s) miss {d.get('miss', 0)}")
        for i, lk in enumerate(d.get("link") or []):
            line += f"\n  delta link {i}: " + " ".join(
                f"{k} {lk.get(k, 0)}" for k in LINK_COUNTERS)
        if "adv" in d:
            line += "\n  delta adv: " + " ".join(f"{k} {d['adv'].get(k, 0)}"
                                                 for k in ADV_COUNTERS)
        if "flash" in d:
            line += "\n  delta flash: " + " ".join(f"{k} {d['flash'].get(k, 0)}"
                                                   for k in FLASH_COUNTERS)
    return line


# --- serial transport ------------------------------------------------------

def read_serial(port=None, timeout=3.0):
    import rainy75_rgb as r
    kb = r.Rainy75(port)
    try:
        # Reuse its SMP serial framing with our group id.
        old = r.RGB_GROUP
        r.RGB_GROUP = GROUP
        try:
            s = kb._request(SMP_OP_READ, CMD_STATS, [], timeout=timeout)
            try:
                a = kb._request(SMP_OP_READ, CMD_ARB, [], timeout=timeout)
            except RuntimeError:
                a = None   # older firmware: no command 1
            return normalize(s, a)
        finally:
            r.RGB_GROUP = old
    finally:
        kb.close()


# --- BLE transport ---------------------------------------------------------

async def read_ble(address=None, timeout=10.0):
    import rainy75_rgb_ble as b
    kb = b.Rainy75BLE(address=address, timeout=timeout)
    await kb.connect()
    try:
        s = await kb._request(SMP_OP_READ, CMD_STATS, [], group=GROUP)
        try:
            a = await kb._request(SMP_OP_READ, CMD_ARB, [], group=GROUP)
        except RuntimeError:
            a = None   # older firmware: no command 1
        return normalize(s, a)
    finally:
        # Never disconnects a link that was already up (live HID session).
        await kb.disconnect()


def main():
    ap = argparse.ArgumentParser(description="Read open-controller power counters")
    ap.add_argument("--ble", action="store_true", help="use BLE instead of USB serial")
    ap.add_argument("--port", help="serial port (default: auto-detect)")
    ap.add_argument("--address", help="BLE address (default: by name via BlueZ)")
    ap.add_argument("-n", type=int, default=1, help="number of reads (default 1)")
    ap.add_argument("-i", type=float, default=5.0, help="seconds between reads")
    args = ap.parse_args()

    prev = None
    for k in range(args.n):
        if k:
            time.sleep(args.i)
        s = asyncio.run(read_ble(args.address)) if args.ble else read_serial(args.port)
        print(format_stats(s, delta(s, prev) if prev else None), flush=True)
        prev = s


if __name__ == "__main__":
    main()
