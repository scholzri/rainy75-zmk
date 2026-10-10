/*
 * Tests of the config page (index.html): its data-core sections under Node
 * (test-node.mjs) and in the browser (test.html), plus UI tests on
 * index.html?demo that only the browser runs. Each test gets the page's
 * RainyCore object (C) and env: {html, bytes, browser, frames}.
 */
"use strict";
(() => {
  const tests = [];
  const test = (name, fn) => tests.push({ name, fn, ui: false });
  const uiTest = (name, fn) => tests.push({ name, fn, ui: true });
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const settle = () => sleep(0);

  function same(a, b) {
    if (a === b) return true;
    if (ArrayBuffer.isView(a) && ArrayBuffer.isView(b)) {
      return a.length === b.length && Array.prototype.every.call(a, (x, i) => x === b[i]);
    }
    if (Array.isArray(a) || Array.isArray(b)) {
      return Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.every((x, i) => same(x, b[i]));
    }
    if (a && b && typeof a === "object" && typeof b === "object") {
      const ka = Object.keys(a);
      const kb = Object.keys(b);
      return ka.length === kb.length && ka.every((k) => Object.prototype.hasOwnProperty.call(b, k) && same(a[k], b[k]));
    }
    return false;
  }
  const show = (v) => {
    try {
      return JSON.stringify(v, (k, x) => (ArrayBuffer.isView(x) ? Array.from(x) : x));
    } catch (e) {
      return String(v);
    }
  };
  const t = {
    eq(a, b, msg) {
      if (!same(a, b)) throw new Error((msg ? msg + ": " : "") + "got " + show(a) + ", expected " + show(b));
    },
    ok(v, msg) {
      if (!v) throw new Error(msg || "expected a true value");
    },
    throws(fn, re) {
      try {
        fn();
      } catch (e) {
        if (re && !re.test(e.message)) throw new Error("wrong error: " + e.message);
        return e;
      }
      throw new Error("expected an error");
    },
    async rejects(p, re) {
      try {
        await p;
      } catch (e) {
        if (re && !re.test(e.message)) throw new Error("wrong error: " + e.message);
        return e;
      }
      throw new Error("expected a rejection");
    },
  };
  const bytes = (...a) => Uint8Array.from(a.flatMap((x) => (ArrayBuffer.isView(x) ? Array.from(x) : x)));
  const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");
  const fromHex = (s) => Uint8Array.from(s.match(/../g) || [], (x) => parseInt(x, 16));
  const ascii = (s) => Uint8Array.from(s, (ch) => ch.charCodeAt(0));

  // ---- Task 1: the file's rules, CBOR, SMP header ----

  test("static: one file under 100000 bytes", (C, env) => {
    t.ok(env.bytes < 100000, "index.html is " + env.bytes + " bytes");
  });

  test("static: CSP meta blocks the network, first after charset", (C, env) => {
    const m = /<meta charset="utf-8">\s*<meta http-equiv="Content-Security-Policy" content="([^"]+)">/.exec(env.html);
    t.ok(m, "CSP meta tag right after the charset");
    t.ok(m[1].startsWith("default-src 'none'; "), "default-src 'none' first");
    t.ok(!/connect-src/.test(m[1]), "no connect-src: default-src 'none' covers it");
  });

  test("static: nothing loaded from anywhere", (C, env) => {
    t.ok(!/<script[^>]*\ssrc=/i.test(env.html), "no script src");
    t.ok(!/<link[^>]*rel="stylesheet"/i.test(env.html), "no stylesheet link");
    t.ok(!/@import|url\(/.test(env.html), "no CSS import or url()");
    t.ok(!/\bfetch\(|XMLHttpRequest|WebSocket|EventSource|sendBeacon/.test(env.html), "no network API");
    for (const m of env.html.matchAll(/https?:\/\/[^\s"'<>]*/g)) {
      const before = env.html.slice(m.index - 6, m.index);
      t.ok(before === 'href="' && m[0].startsWith("https://github.com/scholzri/rainy75-zmk/"), "only links to the repository: " + m[0]);
    }
  });

  test("static: no em or en dashes", (C, env) => {
    const dashes = new RegExp("[" + String.fromCharCode(0x2013, 0x2014) + "]");
    t.ok(!dashes.test(env.html), "found an em or en dash");
  });

  test("CBOR: encodes scalars as rainy75_cfg.py does", (C) => {
    t.eq(hex(C.cborEncode(true)), "f5");
    t.eq(hex(C.cborEncode(false)), "f4");
    t.eq(hex(C.cborEncode(null)), "f6");
    t.eq(hex(C.cborEncode(5)), "05");
    t.eq(hex(C.cborEncode(24)), "1818");
    t.eq(hex(C.cborEncode(300)), "19012c");
    t.eq(hex(C.cborEncode(0xffffff)), "1a00ffffff");
    t.eq(hex(C.cborEncode(-1)), "20");
    t.eq(hex(C.cborEncode("ab")), "626162");
    t.eq(hex(C.cborEncode(bytes(1, 2))), "420102");
  });

  test("CBOR: encodes lists and maps (definite length)", (C) => {
    t.eq(hex(C.cborEncode(["a", "b"])), "8261616162");
    t.eq(hex(C.cborEncode([])), "80");
    t.eq(hex(C.cborEncode(new Array(24).fill("a")).subarray(0, 2)), "9818");
    t.eq(hex(C.cborEncode({})), "a0");
    t.eq(hex(C.cborEncode({ k: "rgb.on", v: true })), "a2616b667267622e6f6e6176f5");
  });

  test("CBOR: indefinite-length output for the simulator", (C) => {
    t.eq(hex(C.cborEncode({ s: [1] }, { indefinite: true })), "bf61739f01ffff");
  });

  test("CBOR: refuses what the protocol never sends", (C) => {
    t.throws(() => C.cborEncode(1.5), /not an integer/);
    t.throws(() => C.cborEncode(2 ** 32), /out of range/);
    t.throws(() => C.cborEncode(undefined), /cannot encode/);
  });

  test("CBOR: decodes definite and indefinite containers", (C) => {
    t.eq(C.cborDecode(fromHex("a26272630062737681f5")), { rc: 0, sv: [true] });
    t.eq(C.cborDecode(fromHex("bf627263006273769ff5f4ffff")), { rc: 0, sv: [true, false] });
    t.eq(C.cborDecode(fromHex("9f9f01ff9f02ffff")), [[1], [2]]);
    t.eq(C.cborDecode(fromHex("bfff")), {});
  });

  test("CBOR: decodes every integer width, strings, bytes, null", (C) => {
    t.eq(C.cborDecode(fromHex("17")), 23);
    t.eq(C.cborDecode(fromHex("18ff")), 255);
    t.eq(C.cborDecode(fromHex("19ffff")), 65535);
    t.eq(C.cborDecode(fromHex("1affffffff")), 4294967295);
    t.eq(C.cborDecode(fromHex("1b001fffffffffffff")), 9007199254740991);
    t.eq(C.cborDecode(fromHex("38ff")), -256);
    t.eq(C.cborDecode(fromHex("6668c3a46c6c6f")), "h" + String.fromCharCode(0xe4) + "llo");
    t.eq(C.cborDecode(fromHex("43010203")), bytes(1, 2, 3));
    t.eq(C.cborDecode(fromHex("f6")), null);
  });

  test("CBOR: round trip of a list reply", (C) => {
    const v = { rc: 0, s: [["rgb.on", "b", null, null, 0], ["rgb.val", "u", 16, 255, 0]], next: 2 };
    t.eq(C.cborDecode(C.cborEncode(v)), v);
    t.eq(C.cborDecode(C.cborEncode(v, { indefinite: true })), v);
  });

  test("CBOR: rejects broken input", (C) => {
    t.throws(() => C.cborDecode(fromHex("82f5")), /truncated/);
    t.throws(() => C.cborDecode(fromHex("bf6172")), /truncated/);
    t.throws(() => C.cborDecode(fromHex("f5f5")), /trailing/);
    t.throws(() => C.cborDecode(fromHex("c0f5")), /major type 6/);
    t.throws(() => C.cborDecode(fromHex("fa00000000")), /simple value 26/);
    t.throws(() => C.cborDecode(fromHex("7fff")), /indefinite-length string/);
    t.throws(() => C.cborDecode(fromHex("1bffffffffffffffff")), /too large/);
    t.throws(() => C.cborDecode(fromHex("62c328")), /./);
  });

  test("SMP: request header as restore_original.py builds it", (C) => {
    t.eq(hex(C.smpRequest(C.SMP.READ, 67, 0, 0, {})), "0000000100430000a0");
    t.eq(hex(C.smpRequest(C.SMP.WRITE, 67, 3, 255, { k: "rgb.on", v: false }).subarray(0, 8)), "0200000d0043ff03");
  });

  test("SMP: parses a reply frame", (C) => {
    const f = C.smpFrame(3, 67, 3, 7, C.cborEncode({ rc: 0, v: 120 }, { indefinite: true }));
    const m = C.smpParse(f);
    t.eq([m.op, m.group, m.cmd, m.seq, m.len], [3, 67, 3, 7, f.length - 8]);
    t.eq(m.body, { rc: 0, v: 120 });
    t.eq(C.smpParse(C.smpFrame(1, 1, 0, 0, new Uint8Array(0))).body, {});
    t.throws(() => C.smpParse(bytes(1, 0, 0, 5, 0, 67, 0, 0, 0xa0)), /shorter/);
    t.throws(() => C.smpParse(bytes(1, 0)), /too short/);
  });

  // ---- Task 2: SMP client ----

  /* A transport that answers every request with {rc, seq} after delayMs;
   * drop: requests to swallow first; rc: the rc to answer; sent: frames. */
  function echoTransport(C, opts = {}) {
    const tr = {
      sent: [],
      drop: opts.drop || 0,
      onPacket: null,
      onClose: null,
      async send(frame) {
        tr.sent.push(frame);
        if (tr.drop > 0) {
          tr.drop--;
          return;
        }
        const m = C.smpParse(frame);
        const reply = C.smpFrame(m.op + 1, m.group, m.cmd, m.seq, C.cborEncode({ rc: opts.rc || 0, seq: m.seq }));
        setTimeout(() => tr.onPacket(reply), opts.delayMs || 1);
      },
    };
    return tr;
  }

  test("client: request and reply, sequence numbers count up", async (C) => {
    const tr = echoTransport(C);
    const cl = new C.SmpClient(tr, { timeoutMs: 200 });
    t.eq((await cl.request(C.SMP.READ, 67, 0, {})).seq, 0);
    t.eq((await cl.request(C.SMP.READ, 67, 0, {})).seq, 1);
    t.eq(tr.sent.length, 2);
    t.eq(hex(tr.sent[1]), "0000000100430100a0");
  });

  test("client: one request at a time", async (C) => {
    const tr = echoTransport(C, { delayMs: 20 });
    const cl = new C.SmpClient(tr, { timeoutMs: 500 });
    const a = cl.request(C.SMP.READ, 67, 0, {});
    const b = cl.request(C.SMP.READ, 67, 1, {});
    await sleep(5);
    t.eq(tr.sent.length, 1, "second request waits for the first reply");
    t.eq([(await a).seq, (await b).seq], [0, 1]);
  });

  test("client: a lost request is sent once more", async (C) => {
    const tr = echoTransport(C, { drop: 1 });
    const cl = new C.SmpClient(tr, { timeoutMs: 30 });
    t.eq((await cl.request(C.SMP.READ, 67, 0, {})).seq, 1);
    t.eq(tr.sent.length, 2);
  });

  test("client: no answer after the retry is a TransportError", async (C) => {
    const tr = echoTransport(C, { drop: 5 });
    const cl = new C.SmpClient(tr, { timeoutMs: 20 });
    const e = await t.rejects(cl.request(C.SMP.READ, 67, 0, {}), /no answer/);
    t.ok(e instanceof C.TransportError);
    t.eq(tr.sent.length, 2, "two attempts");
  });

  test("client: per-request timeout and retries", async (C) => {
    const tr = echoTransport(C, { drop: 5 });
    const cl = new C.SmpClient(tr, { timeoutMs: 5000 });
    const t0 = Date.now();
    await t.rejects(cl.request(C.SMP.READ, 67, 0, {}, { timeoutMs: 20, retries: 0 }), /no answer/);
    t.ok(Date.now() - t0 < 1000, "used the 20 ms timeout");
    t.eq(tr.sent.length, 1);
  });

  test("client: a late answer to the first attempt counts", async (C) => {
    const tr = echoTransport(C, { delayMs: 40 });
    const cl = new C.SmpClient(tr, { timeoutMs: 25 });
    t.eq((await cl.request(C.SMP.READ, 67, 0, {})).seq, 0, "the reply to seq 0 arrived during attempt 2");
  });

  test("client: replies for another request are ignored", async (C) => {
    const tr = {
      onPacket: null,
      async send(frame) {
        const m = C.smpParse(frame);
        const p = (op, g, c, s) => C.smpFrame(op, g, c, s, C.cborEncode({ rc: 0, n: g + c + s }));
        setTimeout(() => {
          tr.onPacket(p(m.op + 1, 65, m.cmd, m.seq));
          tr.onPacket(p(m.op + 1, m.group, 9, m.seq));
          tr.onPacket(p(m.op + 1, m.group, m.cmd, m.seq + 1));
          tr.onPacket(p(m.op, m.group, m.cmd, m.seq));
          tr.onPacket(bytes(1, 0, 0, 9));
          tr.onPacket(p(m.op + 1, m.group, m.cmd, m.seq));
        }, 1);
      },
    };
    const cl = new C.SmpClient(tr, { timeoutMs: 200 });
    t.eq(await cl.request(C.SMP.READ, 67, 2, {}), { rc: 0, n: 69 });
  });

  test("client: non-zero rc is a DeviceError", async (C) => {
    const tr = echoTransport(C, { rc: 8 });
    const cl = new C.SmpClient(tr, { timeoutMs: 200 });
    const e = await t.rejects(cl.request(C.SMP.READ, 67, 0, {}), /rc 8/);
    t.ok(e instanceof C.DeviceError);
    t.eq(e.rc, 8);
  });

  test("client: a closed link fails the waiting request at once", async (C) => {
    const tr = { onPacket: null, onClose: null, async send() {} };
    const cl = new C.SmpClient(tr, { timeoutMs: 5000 });
    let told = null;
    cl.onClose = (err) => {
      told = err.message;
    };
    const p = cl.request(C.SMP.READ, 67, 0, {});
    await sleep(5);
    tr.onClose(new Error("unplugged"));
    const e = await t.rejects(p, /closed/);
    t.ok(e instanceof C.TransportError);
    t.eq(told, "unplugged");
    await t.rejects(cl.request(C.SMP.READ, 67, 0, {}), /not connected/);
  });

  test("client: a failing write is a TransportError", async (C) => {
    const tr = {
      onPacket: null,
      async send() {
        throw new Error("device lost");
      },
    };
    const cl = new C.SmpClient(tr, { timeoutMs: 200 });
    const e = await t.rejects(cl.request(C.SMP.READ, 67, 0, {}), /sending failed: device lost/);
    t.ok(e instanceof C.TransportError);
  });

  // ---- Task 3: USB serial framing ----

  const joinLines = (lines) => {
    const all = new Uint8Array(lines.reduce((n, l) => n + l.length, 0));
    let o = 0;
    for (const l of lines) {
      all.set(l, o);
      o += l.length;
    }
    return all;
  };
  const counting = (n) => Uint8Array.from({ length: n }, (_, i) => i & 255);

  test("serial: CRC16-XMODEM check value", (C) => {
    t.eq(C.crc16(ascii("123456789")), 0x31c3);
    t.eq(C.crc16(new Uint8Array(0)), 0);
  });

  test("serial: info request framed byte for byte as restore_original.py", (C) => {
    const lines = C.serialEncode(C.smpRequest(C.SMP.READ, 67, 0, 0, {}));
    t.eq(lines.length, 1);
    t.eq(Array.from(lines[0]), Array.from(bytes(6, 9, ascii("AAsAAAABAEMAAKAFCg=="), 10)));
  });

  test("serial: long packets split into lines of at most 127 bytes", (C) => {
    const lines = C.serialEncode(counting(300));
    t.eq(lines.map((l) => l.length), [127, 127, 127, 39]);
    t.eq([lines[0][0], lines[0][1]], [6, 9]);
    for (const l of lines.slice(1)) t.eq([l[0], l[1]], [4, 20]);
    for (const l of lines) t.eq(l[l.length - 1], 10);
  });

  test("serial: decoder round trip for many sizes", (C) => {
    for (const n of [1, 8, 9, 89, 90, 91, 92, 93, 185, 186, 187, 300, 511]) {
      const d = new C.SerialDecoder();
      t.eq(d.push(joinLines(C.serialEncode(counting(n)))), [counting(n)], "size " + n);
    }
  });

  test("serial: log lines before, between and after frame lines are skipped", (C) => {
    const d = new C.SerialDecoder();
    const lines = C.serialEncode(counting(200));
    const log = ascii("[00:00:01.234,000] <inf> zmk: something\r\n");
    t.eq(lines.length, 3);
    const out = d.push(joinLines([log, lines[0], log, lines[1], log, lines[2], ascii("\r\n"), log]));
    t.eq(out, [counting(200)]);
    t.eq(d.logLines, 5);
    t.eq(d.badFrames, 0);
  });

  test("serial: frames split at any byte arrive whole", (C) => {
    const d = new C.SerialDecoder();
    const all = joinLines([ascii("boot\r\n")].concat(C.serialEncode(counting(150)), C.serialEncode(counting(3))));
    const got = [];
    for (let i = 0; i < all.length; i += 7) got.push(...d.push(all.subarray(i, i + 7)));
    t.eq(got, [counting(150), counting(3)]);
  });

  test("serial: CRLF line ends are fine", (C) => {
    const d = new C.SerialDecoder();
    const lines = C.serialEncode(counting(130)).map((l) => bytes(Array.from(l.subarray(0, l.length - 1)), 13, 10));
    t.eq(d.push(joinLines(lines)), [counting(130)]);
  });

  test("serial: a bad CRC drops the frame, the next one decodes", (C) => {
    const d = new C.SerialDecoder();
    const broken = C.serialEncode(counting(10))[0];
    broken[5] = broken[5] === 65 ? 66 : 65;
    t.eq(d.push(joinLines([broken].concat(C.serialEncode(counting(4))))), [counting(4)]);
    t.eq(d.badFrames, 1);
  });

  test("serial: bad base64 and stray continuation lines are dropped", (C) => {
    const d = new C.SerialDecoder();
    const cont = C.serialEncode(counting(200))[1];
    t.eq(d.push(joinLines([cont, bytes(6, 9, ascii("!!!!"), 10)].concat(C.serialEncode(counting(2))))), [counting(2)]);
    t.eq(d.badFrames, 2);
  });

  test("serial: a new first line abandons a half frame", (C) => {
    const d = new C.SerialDecoder();
    const half = C.serialEncode(counting(200))[0];
    t.eq(d.push(joinLines([half].concat(C.serialEncode(counting(5))))), [counting(5)]);
  });

  // ---- Task 4: Bluetooth reassembly ----

  const replyFrame = (C, body) => C.smpFrame(1, 67, 1, 0, C.cborEncode(body, { indefinite: true }));

  test("ble: names and UUIDs of the SMP service", (C) => {
    t.eq(C.BLE.NAME, "Rainy 75 Pro");
    t.eq(C.BLE.SERVICE, "8d53dc1d-1db7-4cd3-868b-8a527460aa84");
    t.eq(C.BLE.CHAR, "da2e7828-fbce-4e01-ae9e-261174997c48");
  });

  test("ble: requests go out in 20-byte writes", (C) => {
    t.eq(C.bleChunks(counting(45)).map((c) => c.length), [20, 20, 5]);
    t.eq(C.bleChunks(counting(20)).map((c) => c.length), [20]);
    t.eq(joinLines(C.bleChunks(counting(45))), counting(45));
  });

  test("ble: a reply over several notifications", (C) => {
    const f = replyFrame(C, { rc: 0, s: new Array(10).fill(["rgb.on", "b", null, null, 0]) });
    t.ok(f.length > 100);
    const r = new C.BleReassembler();
    const got = [];
    for (const c of C.bleChunks(f)) got.push(...r.push(c));
    t.eq(got, [f]);
  });

  test("ble: the header itself split, and two frames in one notification", (C) => {
    const a = replyFrame(C, { rc: 0 });
    const b = replyFrame(C, { rc: 0, next: 3 });
    const r = new C.BleReassembler();
    t.eq(r.push(a.subarray(0, 3)), []);
    t.eq(r.push(bytes(Array.from(a.subarray(3)), Array.from(b))), [a, b]);
  });

  test("ble: reset drops a half frame, nonsense lengths start over", (C) => {
    const a = replyFrame(C, { rc: 0, v: 1 });
    const r = new C.BleReassembler();
    r.push(a.subarray(0, 10));
    r.reset();
    t.eq(r.push(a), [a]);
    t.eq(r.push(bytes(1, 0, 0xff, 0xff, 0, 67, 0, 0)), []);
    t.eq(r.push(a), [a], "after a bogus length");
  });

  // ---- Task 5: values and the slider throttle ----

  const FX3 = ["solid", "rainbow", "plasma"];
  const E = {
    on: { key: "rgb.on", type: "b", a: null, b: null, ro: false },
    val: { key: "rgb.val", type: "u", a: 16, b: 255, ro: false },
    effect: { key: "rgb.effect", type: "e", a: FX3, b: null, ro: false },
    color: { key: "ind.caps_color", type: "c", a: null, b: null, ro: false },
    cycle: { key: "rgb.cycle", type: "l", a: FX3, b: null, ro: false },
  };

  test("values: bool and number checks name the setting", (C) => {
    t.eq(C.checkValue(E.on, true), true);
    t.throws(() => C.checkValue(E.on, 1), /^rgb\.on: expected on or off$/);
    t.eq(C.checkValue(E.val, 120), 120);
    t.throws(() => C.checkValue(E.val, 300), /^rgb\.val: 300 is outside 16\.\.255$/);
    t.throws(() => C.checkValue(E.val, 15), /outside/);
    t.throws(() => C.checkValue(E.val, 1.5), /^rgb\.val: 1\.5 is not a number$/);
    t.throws(() => C.checkValue({ key: "x.new", type: "x" }, 1), /unknown type 'x'/);
  });

  test("values: names", (C) => {
    t.eq(C.checkValue(E.effect, "plasma"), "plasma");
    t.throws(() => C.checkValue(E.effect, "fire"), /'fire' is not one of solid, rainbow, plasma/);
  });

  test("values: colours", (C) => {
    t.eq(C.checkValue(E.color, 0xff8000), 0xff8000);
    t.throws(() => C.checkValue(E.color, 0x1000000), /colour/);
    t.eq(C.parseColor("#FF8000"), 0xff8000);
    t.eq(C.parseColor("0x0000ff"), 0xff);
    t.eq(C.parseColor("FF8000"), 0xff8000);
    for (const bad of ["#FFF", "FFF", "#FF80001", "0x123", "GG0000", ""]) t.throws(() => C.parseColor(bad), /six hex digits/);
    t.eq(C.formatColor(0xff8000), "#FF8000");
    t.eq(C.colorInput(0xff), "#0000ff");
  });

  test("values: lists drop duplicates before the 16-entry limit", (C) => {
    t.eq(C.checkValue(E.cycle, ["plasma", "solid", "plasma"]), ["plasma", "solid"]);
    t.eq(C.checkValue(E.cycle, []), []);
    t.throws(() => C.checkValue(E.cycle, ["solid", "fire"]), /unknown fire/);
    const names = Array.from({ length: 17 }, (_, i) => "fx" + i);
    const big = { key: "rgb.cycle", type: "l", a: names };
    t.eq(C.checkValue(big, names.slice(0, 16).concat(names.slice(0, 4))), names.slice(0, 16));
    t.throws(() => C.checkValue(big, names), /at most 16/);
  });

  test("values: shown as the CLI shows them", (C) => {
    t.eq(C.formatValue(E.on, true), "on");
    t.eq(C.formatValue(E.on, false), "off");
    t.eq(C.formatValue(E.color, 0xff8000), "#FF8000");
    t.eq(C.formatValue(E.cycle, ["a", "b"]), "a,b");
    t.eq(C.formatValue(E.val, 120), "120");
    t.ok(C.sameValue(["a"], ["a"]) && !C.sameValue(["a"], ["a", "b"]) && C.sameValue(3, 3));
  });

  test("cycle: rows from a list value", (C) => {
    const all = () => true;
    t.eq(C.cycleRows(FX3, [], all), [
      { name: "solid", checked: true }, { name: "rainbow", checked: true }, { name: "plasma", checked: true }]);
    t.eq(C.cycleRows(FX3, ["plasma", "solid"], all), [
      { name: "plasma", checked: true }, { name: "solid", checked: true }, { name: "rainbow", checked: false }]);
    const noRainbow = (n) => n !== "rainbow";
    t.eq(C.cycleRows(FX3, ["rainbow", "plasma"], noRainbow), [
      { name: "plasma", checked: true }, { name: "solid", checked: false }]);
  });

  test("cycle: rows back to a list, hidden names kept", (C) => {
    const rows = [{ name: "plasma", checked: true }, { name: "solid", checked: false }];
    t.eq(C.cycleFromRows(rows, [], () => true), ["plasma"]);
    t.eq(C.cycleFromRows(rows, ["rainbow", "solid"], (n) => n !== "rainbow"), ["plasma", "rainbow"]);
  });

  /* A clock the tests move by hand; advance() runs the timers that fall due. */
  function fakeClock() {
    const c = {
      t: 0,
      timers: [],
      now: () => c.t,
      setTimeout(f, ms) {
        const id = { f, at: c.t + ms };
        c.timers.push(id);
        return id;
      },
      clearTimeout(id) {
        c.timers = c.timers.filter((x) => x !== id);
      },
      async advance(ms) {
        const end = c.t + ms;
        for (;;) {
          c.timers.sort((a, b) => a.at - b.at);
          const next = c.timers[0];
          if (!next || next.at > end) break;
          c.timers.shift();
          c.t = next.at;
          next.f();
          await settle();
        }
        c.t = end;
        await settle();
      },
    };
    return c;
  }

  test("throttle: at most one send per interval, the newest value", async (C) => {
    const clock = fakeClock();
    const sent = [];
    const th = C.makeThrottle(50, (v) => sent.push([clock.t, v]), clock);
    th.push(0);
    await settle();
    for (const v of [10, 20, 30, 40]) {
      await clock.advance(10);
      th.push(v);
    }
    await clock.advance(10);
    t.eq(sent, [[0, 0], [50, 40]]);
    await clock.advance(5);
    th.push(55);
    await clock.advance(10);
    th.flush(65);
    await settle();
    t.eq(sent, [[0, 0], [50, 40]], "flush waits for the end of the window");
    await clock.advance(35);
    t.eq(sent, [[0, 0], [50, 40], [100, 65]], "the pending 55 is replaced by the release value");
    await clock.advance(200);
    t.eq(sent.length, 3);
  });

  test("throttle: one send at a time", async (C) => {
    const clock = fakeClock();
    const sent = [];
    let done;
    const th = C.makeThrottle(50, (v) => {
      sent.push(v);
      return new Promise((resolve) => {
        done = resolve;
      });
    }, clock);
    th.push(1);
    await settle();
    th.push(2);
    await clock.advance(200);
    t.eq(sent, [1], "waits for the reply");
    t.ok(th.pending());
    th.flush(3);
    await settle();
    t.eq(sent, [1]);
    done();
    await settle();
    t.eq(sent, [1, 3]);
    done();
    await settle();
    t.ok(!th.pending());
  });

  test("throttle: flush of the value just sent sends nothing", async (C) => {
    const clock = fakeClock();
    const sent = [];
    const th = C.makeThrottle(50, (v) => sent.push(v), clock);
    th.push(7);
    await settle();
    th.flush(7);
    await settle();
    t.eq(sent, [7]);
    th.flush(8);
    await settle();
    t.eq(sent, [7], "8 waits for the end of the window");
    await clock.advance(50);
    t.eq(sent, [7, 8]);
  });

  test("throttle: input then change on every step still sends once per window", async (C) => {
    const clock = fakeClock();
    const sent = [];
    const th = C.makeThrottle(150, (v) => sent.push([clock.t, v]), clock);
    for (let i = 0; i < 12; i++) {
      await clock.advance(33);
      th.push(i);
      th.flush(i); /* a keyboard step on a range input: input, then change */
      await settle();
    }
    const released = clock.t;
    await clock.advance(150);
    t.eq(sent.map((s) => s[1]), [0, 4, 9, 11], "the newest value of each window, then the release value");
    for (let i = 1; i < sent.length; i++) t.ok(sent[i][0] - sent[i - 1][0] >= 150, "gap before send " + i);
    t.ok(sent[sent.length - 1][0] - released <= 150, "the release value goes out within one window");
    t.ok(!th.pending());
  });

  test("throttle: a failed send is sent again by the release value", async (C) => {
    for (const fail of [() => Promise.reject(new Error("no reply")), () => Promise.resolve(false)]) {
      const clock = fakeClock();
      const sent = [];
      let failing = true;
      const th = C.makeThrottle(50, (v) => {
        sent.push(v);
        return failing ? fail() : undefined;
      }, clock);
      th.push(5);
      await settle();
      t.eq(sent, [5]);
      failing = false;
      th.flush(5);
      await clock.advance(50);
      t.eq(sent, [5, 5], "not taken as sent");
      th.flush(5);
      await clock.advance(50);
      t.eq(sent, [5, 5], "sent now, nothing more");
    }
  });

  test("throttle: release value equal to the one still in flight", async (C) => {
    for (const ok of [true, false]) {
      const clock = fakeClock();
      const sent = [];
      let answer;
      const th = C.makeThrottle(50, (v) => {
        sent.push(v);
        return new Promise((resolve, reject) => {
          answer = ok ? resolve : reject;
        });
      }, clock);
      th.push(5);
      th.flush(5);
      await settle();
      t.eq(sent, [5], "input and change send once");
      answer(ok ? undefined : new Error("lost"));
      await clock.advance(100);
      t.eq(sent, ok ? [5] : [5, 5], ok ? "answered: nothing more" : "lost: sent again");
    }
  });

  // ---- Task 6: the simulated keyboard ----

  /* One request to a SimKeyboard -> the decoded reply body. */
  const ask = (C, sim, op, cmd, body, group = 67) => C.smpParse(sim.handle(C.smpRequest(op, group, cmd, 0, body))).body;
  const R = 0;
  const W = 2;

  test("sim: info, and replies in indefinite length", (C) => {
    const sim = new C.SimKeyboard();
    const raw = sim.handle(C.smpRequest(R, 67, 0, 5, {}));
    t.eq([raw[0], raw[6], raw[8]], [1, 5, 0xbf], "read reply, same seq, indefinite map");
    const r = ask(C, sim, R, 0, {});
    t.eq([r.rc, r.v, r.n, r.rev], [0, 1, 21, 0]);
    t.eq(r.fx, C.SIM_EFFECTS);
    t.eq(C.SIM_EFFECTS.length, 12);
  });

  test("sim: list pages like the firmware and covers every setting", (C) => {
    const sim = new C.SimKeyboard();
    const keys = [];
    let i = 0;
    let pages = 0;
    for (;;) {
      const r = ask(C, sim, R, 1, { i });
      pages++;
      for (const e of r.s) keys.push(e[0]);
      if (r.next === undefined) break;
      i = r.next;
    }
    t.ok(pages >= 2, "more than one page");
    t.eq(keys.length, 21);
    t.eq(keys[0], "rgb.on");
    t.eq(keys[20], "kb.sleep_on_usb");
    t.eq(ask(C, sim, R, 1, { i: 18 }).s[0], ["kb.os_keys", "u", 0, 83, 1]);
    t.eq(ask(C, sim, R, 1, { i: 1 }).s[0], ["rgb.effect", "e", C.SIM_EFFECTS, null, 0]);
    t.eq(ask(C, sim, R, 1, { i: 99 }), { rc: 0, s: [] });
  });

  test("sim: get pages, by index and by key", (C) => {
    const sim = new C.SimKeyboard({ budget: 30 });
    const all = {};
    let r = { next: 0 };
    let pages = 0;
    while (r.next !== undefined) {
      r = ask(C, sim, R, 2, { i: r.next });
      Object.assign(all, r.v);
      pages++;
    }
    t.eq(Object.keys(all).length, 21);
    t.ok(pages > 5, "small budget, many pages");
    t.eq([all["rgb.val"], all["ind.caps_color"], all["kb.os"]], [200, 0xffffff, "win"]);
    sim.budget = 20;
    r = ask(C, sim, R, 2, { k: ["rgb.val", "rgb.val", "kb.os"] });
    t.eq(r.v, { "rgb.val": 200 }, "the duplicate is answered once");
    t.eq(r.next, 2, "next is an index into k");
    r = ask(C, sim, R, 2, { k: ["rgb.val", "rgb.val", "kb.os"], i: 2 });
    t.eq(r.v, { "kb.os": "win" });
    t.eq(r.next, undefined);
    t.eq(ask(C, sim, R, 2, { k: [] }), { rc: 0, v: {} });
  });

  test("sim: get errors", (C) => {
    const sim = new C.SimKeyboard();
    t.eq(ask(C, sim, R, 2, { k: ["rgb.val", "nope"] }), { rc: 5 });
    t.eq(ask(C, sim, R, 2, { k: new Array(33).fill("rgb.val") }), { rc: 3 });
    t.eq(ask(C, sim, R, 2, { i: -1 }), { rc: 3 });
  });

  test("sim: set echoes the stored value and counts rev", (C) => {
    const sim = new C.SimKeyboard();
    t.eq(ask(C, sim, W, 3, { k: "rgb.val", v: 120 }), { rc: 0, v: 120 });
    t.eq(ask(C, sim, W, 3, { k: "rgb.cycle", v: ["plasma", "solid", "plasma"] }), { rc: 0, v: ["plasma", "solid"] });
    t.eq(sim.rev, 2);
    t.eq(ask(C, sim, R, 0, {}).rev, 2);
  });

  test("sim: set errors as cfg_mgmt.c", (C) => {
    const sim = new C.SimKeyboard();
    t.eq(ask(C, sim, W, 3, { k: "rgb.val", v: 300 }), { rc: 3 });
    t.eq(ask(C, sim, W, 3, { k: "rgb.on", v: 1 }), { rc: 3 });
    t.eq(ask(C, sim, W, 3, { k: "rgb.effect", v: "fire" }), { rc: 3 });
    t.eq(ask(C, sim, W, 3, { k: "rgb.cycle", v: new Array(17).fill("solid") }), { rc: 3 });
    t.eq(ask(C, sim, W, 3, { k: "nope", v: 1 }), { rc: 5 });
    t.eq(ask(C, sim, W, 3, { k: "kb.os_keys", v: "bad" }), { rc: 11 }, "read-only before the value check");
    t.eq(ask(C, sim, W, 3, { k: "nope", v: null }), { rc: 3 }, "a value the request decoder refuses comes first");
    t.eq(ask(C, sim, W, 3, { k: "kb.os_keys", v: -1 }), { rc: 3 });
    t.eq(ask(C, sim, W, 3, { k: "kb.os_keys", v: new Array(17).fill("a") }), { rc: 3 });
    t.eq(ask(C, sim, W, 3, { k: "rgb.val" }), { rc: 3 });
    sim.failNextSet = 3;
    t.eq(ask(C, sim, W, 3, { k: "rgb.val", v: 100 }), { rc: 3 });
    t.eq(ask(C, sim, W, 3, { k: "rgb.val", v: 100 }), { rc: 0, v: 100 });
  });

  test("sim: reset", (C) => {
    const sim = new C.SimKeyboard();
    ask(C, sim, W, 3, { k: "rgb.val", v: 120 });
    ask(C, sim, W, 3, { k: "kb.os", v: "mac" });
    sim.setOsKeys(1);
    t.eq(ask(C, sim, W, 4, { k: ["kb.os_keys", "rgb.val"] }), { rc: 11 });
    t.eq(sim.values["rgb.val"], 120, "nothing reset when one key fails");
    t.eq(ask(C, sim, W, 4, { k: ["nope"] }), { rc: 5 });
    t.eq(ask(C, sim, W, 4, { k: ["rgb.val"] }), { rc: 0 });
    t.eq([sim.values["rgb.val"], sim.values["kb.os"]], [200, "mac"]);
    t.eq(ask(C, sim, W, 4, {}), { rc: 0 });
    t.eq([sim.values["kb.os"], sim.values["kb.os_keys"]], ["win", 1], "all writable keys, not kb.os_keys");
  });

  test("sim: bad requests, other groups, old firmware", (C) => {
    const sim = new C.SimKeyboard();
    const empty = C.smpFrame(R, 67, 1, 0, new Uint8Array(0));
    t.eq(C.smpParse(sim.handle(empty)).body, { rc: 3 }, "list refuses an empty payload");
    t.eq(ask(C, sim, R, 3, { k: "rgb.val", v: 1 }), { rc: 8 }, "set sent as a read");
    t.eq(ask(C, sim, R, 9, {}), { rc: 8 });
    t.eq(ask(C, sim, R, 0, {}, 65), { rc: 8 });
    const img = ask(C, sim, R, 0, {}, 1).images[0];
    t.eq([img.slot, img.version, img.active, img.confirmed], [0, "0.4.0", true, true]);
    const old = new C.SimKeyboard({ noConfig: true });
    t.eq(ask(C, old, R, 0, {}), { rc: 8 });
    t.eq(ask(C, old, R, 0, {}, 1).images.length, 1);
  });

  test("sim: Fn+Enter follows rgb.cycle, Studio changes kb.os_keys without rev", (C) => {
    const sim = new C.SimKeyboard();
    sim.pressFnEnter();
    t.eq(sim.values["rgb.effect"], "rainbow");
    ask(C, sim, W, 3, { k: "rgb.cycle", v: ["plasma", "wave"] });
    sim.pressFnEnter();
    t.eq(sim.values["rgb.effect"], "plasma", "not listed: the first entry");
    sim.pressFnEnter();
    t.eq(sim.values["rgb.effect"], "wave");
    sim.pressFnEnter();
    t.eq(sim.values["rgb.effect"], "plasma", "wraps");
    const rev = sim.rev;
    sim.setOsKeys(0);
    t.eq(sim.rev, rev);
    t.eq(ask(C, sim, R, 2, { k: ["kb.os_keys"] }).v, { "kb.os_keys": 0 });
  });

  test("sim: as a transport for SmpClient", async (C) => {
    const tr = new C.SimTransport(new C.SimKeyboard(), { latencyMs: 1 });
    const cl = new C.SmpClient(tr, { timeoutMs: 30 });
    t.eq((await cl.request(R, 67, 0, {})).n, 21);
    tr.drop = 1;
    t.eq((await cl.request(R, 67, 2, { k: ["rgb.on"] })).v, { "rgb.on": true }, "after a retry");
    const e = await t.rejects(cl.request(W, 67, 3, { k: "rgb.val", v: 999 }), /rc 3/);
    t.ok(e instanceof C.DeviceError);
    let lost = false;
    cl.onClose = () => {
      lost = true;
    };
    tr.lose();
    t.ok(lost);
    await t.rejects(cl.request(R, 67, 0, {}), /not connected/);
  });

  test("sim: demo variants", (C) => {
    t.eq(C.demoOptions(null), {});
    t.eq(new C.SimKeyboard(C.demoOptions("future")).defs.length, 22);
    t.eq(new C.SimKeyboard(C.demoOptions("future")).fx.slice(-1), ["fireworks"]);
    t.ok(new C.SimKeyboard(C.demoOptions("old")).noConfig);
    t.ok(!new C.SimKeyboard(C.demoOptions("test")).confirmed);
  });

  // ---- Task 7: the config model ----

  /* A model on a SimKeyboard: {sim, tr, client, model, events}. */
  function simModel(C, simOpts, known) {
    const sim = new C.SimKeyboard(simOpts);
    const tr = new C.SimTransport(sim, { latencyMs: 0 });
    const client = new C.SmpClient(tr, { timeoutMs: 50 });
    const model = new C.ConfigModel(client, known);
    const events = [];
    model.on((ev) => events.push(ev));
    return { sim, tr, client, model, events };
  }
  const cmds = (sim) => sim.requests.map((r) => r.group + "/" + r.cmd);

  test("model: load reads info, every list and get page, the firmware", async (C) => {
    const m = simModel(C, { budget: 40 });
    await m.model.load();
    t.eq(m.model.entries.size, 21);
    t.eq(m.model.values.size, 21);
    t.eq(m.model.entries.get("rgb.val"), { key: "rgb.val", type: "u", a: 16, b: 255, ro: false });
    t.eq(m.model.entries.get("kb.os_keys").ro, true);
    t.eq(m.model.values.get("rgb.cycle"), C.SIM_EFFECTS);
    t.eq(m.model.firmware, { version: "0.4.0", confirmed: true });
    t.eq(m.model.info.fx, C.SIM_EFFECTS);
    const list = m.sim.requests.filter((r) => r.cmd === 1 && r.group === 67);
    t.ok(list.length > 5, "budget 40: many list pages");
    t.eq(m.events.map((e) => e.type), ["loaded"]);
    t.eq(cmds(m.sim)[0], "67/0");
    t.eq(cmds(m.sim).slice(-1), ["1/0"]);
  });

  test("model: settings the page cannot show are counted", async (C) => {
    const m = simModel(C, C.demoOptions("future"), (e) => e.key !== "rgb.future");
    await m.model.load();
    t.eq(m.model.hiddenCount(), 1);
  });

  test("model: a firmware without group 67", async (C) => {
    const m = simModel(C, { noConfig: true });
    const e = await t.rejects(m.model.load(), /no runtime settings/);
    t.ok(e instanceof C.NoSettingsError);
    t.ok(/rc 8/.test(C.errorText(new C.DeviceError(8))) && /not supported/.test(C.errorText(new C.DeviceError(8))));
    t.eq(C.errorText(new C.DeviceError(11)), "The keyboard refused: read-only setting (rc 11).");
  });

  test("model: an unconfirmed test image", async (C) => {
    const m = simModel(C, { confirmed: false });
    await m.model.load();
    t.eq(m.model.firmware.confirmed, false);
  });

  test("model: poll re-reads the values only when rev changed", async (C) => {
    const m = simModel(C);
    await m.model.load();
    const before = m.sim.requests.length;
    await m.model.poll();
    t.eq(cmds(m.sim).slice(before), ["67/0"], "rev unchanged: info only");
    m.sim.pressFnEnter();
    m.events.length = 0;
    await m.model.poll();
    t.eq(m.model.values.get("rgb.effect"), "rainbow");
    t.eq(m.events, [{ type: "values" }]);
  });

  test("model: kb.os_keys is re-read without a rev change", async (C) => {
    const m = simModel(C);
    await m.model.load();
    m.sim.setOsKeys(0);
    for (let i = 1; i < C.OS_KEYS_EVERY; i++) await m.model.poll();
    t.eq(m.model.values.get("kb.os_keys"), 2, "not yet");
    await m.model.poll();
    t.eq(m.model.values.get("kb.os_keys"), 0, "every OS_KEYS_EVERY polls");
    m.sim.setOsKeys(2);
    await m.model.refresh(["kb.os_keys", "x.not_listed"]);
    t.eq(m.model.values.get("kb.os_keys"), 2, "at once on refresh");
    t.eq(m.sim.requests.slice(-1)[0].body, { k: ["kb.os_keys"], i: 0 });
  });

  test("model: set keeps the echo", async (C) => {
    const m = simModel(C);
    await m.model.load();
    t.eq(await m.model.set("rgb.cycle", ["plasma", "solid"]), ["plasma", "solid"]);
    t.eq(m.model.values.get("rgb.cycle"), ["plasma", "solid"]);
    t.eq(m.sim.values["rgb.cycle"], ["plasma", "solid"]);
    t.eq(m.events.slice(-1), [{ type: "values", keys: ["rgb.cycle"] }]);
    t.eq(m.sim.requests.slice(-1)[0], { op: 2, group: 67, cmd: 3, body: { k: "rgb.cycle", v: ["plasma", "solid"] } });
  });

  test("model: a refused set re-reads the value", async (C) => {
    const m = simModel(C);
    await m.model.load();
    m.sim.values["rgb.val"] = 99;
    m.sim.failNextSet = 3;
    const e = await t.rejects(m.model.set("rgb.val", 120), /rc 3/);
    t.ok(e instanceof C.DeviceError);
    t.eq(m.model.values.get("rgb.val"), 99, "what the keyboard has");
    t.eq(m.sim.requests.slice(-1)[0].body, { k: ["rgb.val"], i: 0 });
  });

  test("model: invalid values are not sent", async (C) => {
    const m = simModel(C);
    await m.model.load();
    const n = m.sim.requests.length;
    await t.rejects(m.model.set("rgb.val", 999), /outside 16\.\.255/);
    await t.rejects(m.model.set("x.nope", 1), /unknown setting/);
    t.eq(m.sim.requests.length, n);
  });

  test("model: reset all and reset keys", async (C) => {
    const m = simModel(C);
    await m.model.load();
    await m.model.set("rgb.val", 50);
    await m.model.set("kb.os", "mac");
    await m.model.reset(["kb.os"]);
    t.eq([m.model.values.get("rgb.val"), m.model.values.get("kb.os")], [50, "win"]);
    await m.model.reset();
    t.eq(m.model.values.get("rgb.val"), 200);
    t.eq(m.events.slice(-1), [{ type: "values" }]);
  });

  test("model: no answer is a TransportError", async (C) => {
    const m = simModel(C);
    await m.model.load();
    m.tr.drop = 2;
    const e = await t.rejects(m.model.poll(), /no answer/);
    t.ok(e instanceof C.TransportError);
  });

  // end of tests

  async function runOne(fn, C, env) {
    let timer;
    const limit = new Promise((resolve, reject) => {
      timer = setTimeout(() => reject(new Error("timed out after 10 s")), 10000);
    });
    try {
      await Promise.race([Promise.resolve().then(() => fn(C, env)), limit]);
    } finally {
      clearTimeout(timer);
    }
  }

  globalThis.RainyTestRun = async (C, env) => {
    const lines = [];
    let passed = 0;
    let failed = 0;
    let skipped = 0;
    for (const { name, fn, ui } of tests) {
      if (ui && !env.browser) {
        skipped++;
        continue;
      }
      try {
        await runOne(fn, C, env);
        passed++;
        lines.push("ok - " + name);
      } catch (e) {
        failed++;
        lines.push("not ok - " + name + ": " + ((e && e.message) || e));
      }
    }
    return { lines, passed, failed, skipped };
  };
})();
