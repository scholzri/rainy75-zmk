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
    t.eq(m[1], "default-src 'none'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src data:; base-uri 'none'; form-action 'none'");
    t.ok(m[1].startsWith("default-src 'none'; "), "default-src 'none' first");
    t.ok(!/connect-src/.test(m[1]), "no connect-src: default-src 'none' covers it");
  });

  test("static: the off switch and a focused disabled button stay visible", (C, env) => {
    t.ok(/input\[role="switch"\] \{[^}]*\n\s*background: var\(--muted\);/.test(env.html), "off track in the muted colour");
    t.ok(/button\[aria-disabled="true"\]:focus-visible \{ opacity: 1; \}/.test(env.html), "focus ring at full opacity");
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

  /* Fix 1: the value a set echoed is the truth. The client runs one request
   * at a time, so a set started during page 1 of a full read runs between
   * page 1 and page 2; the old value of page 1 must not replace the echo. */
  function raceSet(C, m, key, value) {
    const handle = m.sim.handle.bind(m.sim);
    const race = { promise: null, seen: [] };
    m.sim.handle = (frame) => {
      const reply = handle(frame);
      const h = C.smpParse(frame);
      if (!race.promise && h.group === 67 && h.cmd === 2 && !h.body.k) race.promise = m.model.set(key, value);
      return reply;
    };
    m.model.on(() => race.seen.push(m.model.values.get(key)));
    return race;
  }

  test("model: a set during a poll's full read keeps its echo", async (C) => {
    const m = simModel(C);
    await m.model.load();
    const race = raceSet(C, m, "rgb.val", 50);
    m.sim.pressFnEnter();
    const before = m.sim.requests.length;
    await m.model.poll();
    await race.promise;
    t.eq(cmds(m.sim).slice(before), ["67/0", "67/2", "67/3", "67/2"], "info, page 1, set, page 2");
    t.eq(m.model.values.get("rgb.val"), 50);
    t.eq(m.sim.values["rgb.val"], 50);
    t.eq(m.model.values.get("rgb.effect"), "rainbow", "the rest of the read is kept");
    t.ok(race.seen.length >= 2 && race.seen.every((v) => v === 50), "no event shows the old value: " + show(race.seen));
    t.eq(m.model.info.rev, 1);
  });

  test("model: a set during a reset's read keeps its echo", async (C) => {
    const m = simModel(C);
    await m.model.load();
    const race = raceSet(C, m, "rgb.val", 50);
    await m.model.reset(["kb.os"]);
    await race.promise;
    t.eq(m.model.values.get("rgb.val"), 50);
    t.ok(race.seen.length >= 2 && race.seen.every((v) => v === 50), "no event shows the old value: " + show(race.seen));
  });

  test("model: a set during a load's read keeps its echo", async (C) => {
    const m = simModel(C);
    await m.model.load();
    const race = raceSet(C, m, "rgb.hue", 77);
    await m.model.load();
    await race.promise;
    t.eq(m.model.values.get("rgb.hue"), 77);
    t.ok(race.seen.length >= 2 && race.seen.every((v) => v === 77), "no event shows the old value: " + show(race.seen));
  });

  test("model: a set stores the echo, not the value it sent", async (C) => {
    const m = simModel(C);
    await m.model.load();
    const request = m.client.request.bind(m.client);
    m.client.request = (op, group, cmd, body, opts) => (cmd === 3 && group === 67
      ? Promise.resolve({ rc: 0, v: 77 }) : request(op, group, cmd, body, opts));
    t.eq(await m.model.set("rgb.val", 120), 77);
    t.eq(m.model.values.get("rgb.val"), 77);
    t.eq(m.events.slice(-1), [{ type: "values", keys: ["rgb.val"] }]);
  });

  test("model: poll reloads everything when n or fx changed", async (C) => {
    const m = simModel(C);
    await m.model.load();
    m.sim.defs.push({ key: "rgb.future", type: "u", min: 0, max: 9, def: 1 });
    m.sim.values["rgb.future"] = 1;
    const before = m.sim.requests.length;
    await m.model.poll();
    t.eq(cmds(m.sim).slice(before, before + 3), ["67/0", "67/0", "67/1"], "poll's info, then load");
    t.eq(cmds(m.sim).slice(-1), ["1/0"]);
    t.eq([m.model.info.n, m.model.entries.size, m.model.values.get("rgb.future")], [22, 22, 1]);
    t.eq(m.events.map((e) => e.type), ["loaded", "loaded"]);
    m.sim.fx.push("fireworks");
    await m.model.poll();
    t.eq(m.model.info.fx.slice(-1), ["fireworks"]);
    t.eq(m.model.entries.get("rgb.effect").a.slice(-1), ["fireworks"]);
    t.eq(m.events.map((e) => e.type), ["loaded", "loaded", "loaded"]);
  });

  test("model: a failed read leaves the state and is tried again", async (C) => {
    const m = simModel(C);
    await m.model.load();
    m.sim.pressFnEnter();
    m.sim._get = () => ({ rc: 7 });
    const e = await t.rejects(m.model.poll(), /rc 7/);
    t.ok(e instanceof C.DeviceError);
    t.eq(m.model.info.rev, 0, "rev unchanged");
    t.eq(m.model.values.get("rgb.effect"), "solid");
    delete m.sim._get;
    await m.model.poll();
    t.eq(m.model.info.rev, 1);
    t.eq(m.model.values.get("rgb.effect"), "rainbow", "read again by the next poll");
    m.sim.defs.push({ key: "rgb.future", type: "u", min: 0, max: 9, def: 1 });
    m.sim.values["rgb.future"] = 1;
    m.sim._get = () => ({ rc: 7 });
    await t.rejects(m.model.poll(), /rc 7/);
    t.eq([m.model.info.n, m.model.entries.size, m.model.values.size], [21, 21, 21], "a failed load changes nothing");
    delete m.sim._get;
    await m.model.poll();
    t.eq([m.model.info.n, m.model.entries.size, m.model.values.size], [22, 22, 22]);
  });

  test("model: a refused reset re-reads and throws on", async (C) => {
    const m = simModel(C);
    await m.model.load();
    m.sim.setOsKeys(1);
    m.sim.values["rgb.val"] = 99;
    const e = await t.rejects(m.model.reset(["kb.os_keys", "rgb.val"]), /rc 11/);
    t.ok(e instanceof C.DeviceError);
    t.eq(m.model.values.get("rgb.val"), 99, "what the keyboard has");
    t.eq(m.model.values.get("kb.os_keys"), 1);
    t.eq(m.events.slice(-1), [{ type: "values" }]);
    m.sim._get = () => ({ rc: 7 });
    const again = await t.rejects(m.model.reset(["kb.os_keys"]), /rc 11/);
    t.ok(again instanceof C.DeviceError, "the refusal, not the failed re-read");
    delete m.sim._get;
  });

  test("model: a lost connection during reset is not read again", async (C) => {
    const m = simModel(C);
    await m.model.load();
    const before = m.sim.requests.length;
    const events = m.events.length;
    m.tr.drop = 2;
    const e = await t.rejects(m.model.reset(), /no answer/);
    t.ok(e instanceof C.TransportError);
    t.eq(cmds(m.sim).slice(before), [], "no second attempt to read");
    t.eq(m.events.length, events);
  });

  // ---- Task 8: labels and help ----

  /* The settings of PRs 1 to 3 (docs/config-protocol.md) and PR 5. */
  const ALL_KEYS = ["rgb.on", "rgb.effect", "rgb.hue", "rgb.sat", "rgb.val", "rgb.speed", "rgb.boot_effect",
    "rgb.cycle", "rgb.val_battery", "rgb.idle_s", "rgb.idle_mode", "ind.caps_style", "ind.caps_color",
    "ind.fn_highlight", "ind.passkey_guide", "ind.bat_low", "kb.os", "kb.gui_lock", "kb.os_keys",
    "kb.sleep_min", "kb.sleep_on_usb"];

  test("labels: every setting of the firmware has a label of its type", async (C) => {
    const m = simModel(C);
    await m.model.load();
    t.eq(Array.from(m.model.entries.keys()), ALL_KEYS);
    for (const e of m.model.entries.values()) t.ok(C.isKnown(e), e.key);
    t.eq(C.SETTINGS.map((s) => s.key).sort(), ALL_KEYS.slice().sort(), "nothing more, nothing twice");
    for (const s of C.SETTINGS) {
      t.ok(s.label && s.help, s.key);
      t.ok(C.SECTIONS.some((x) => x.id === C.sectionOf(s.key)), s.key + " has a section");
    }
  });

  test("labels: unknown keys and changed types are hidden", (C) => {
    t.ok(!C.isKnown({ key: "rgb.future", type: "u" }));
    t.ok(!C.isKnown({ key: "rgb.val", type: "x" }));
    t.eq(C.settingInfo("rgb.val").label, "Brightness");
    t.eq(C.settingInfo("x.y"), null);
    t.eq(C.hiddenNote(1), "1 setting needs a newer page.");
    t.eq(C.hiddenNote(3), "3 settings need a newer page.");
  });

  test("labels: every effect and name, unknown names hidden", (C) => {
    for (const fx of C.SIM_EFFECTS.concat(["walker"])) {
      t.ok(C.nameLabel("rgb.effect", fx) && C.nameHelp("rgb.effect", fx), fx);
      t.eq(C.nameLabel("rgb.cycle", fx), C.nameLabel("rgb.effect", fx));
    }
    t.eq(C.nameLabel("rgb.boot_effect", "last"), "Last used");
    t.eq(C.nameLabel("rgb.boot_effect", "plasma"), "Plasma");
    t.eq(C.nameLabel("kb.os", "mac"), "Mac");
    t.eq(C.nameLabel("ind.caps_style", "tint"), "Tint");
    t.eq(C.nameLabel("rgb.idle_mode", "dim"), "Dim");
    t.eq(C.nameLabel("rgb.effect", "fireworks"), null);
    t.eq(C.nameLabel("kb.os", "linux"), null);
    t.eq(C.nameLabel("rgb.effect", "constructor"), null);
    t.eq(C.nameHelp("kb.os", "win"), null);
    t.eq(C.visibleNames({ key: "rgb.effect", a: ["solid", "fireworks", "plasma"] }), ["solid", "plasma"]);
  });

  test("labels: values as text", (C) => {
    const u = (key) => ({ key, type: "u" });
    t.eq(C.displayValue(u("rgb.idle_s"), 0), "Never");
    t.eq(C.displayValue(u("rgb.idle_s"), 45), "45 s");
    t.eq(C.displayValue(u("rgb.idle_s"), 60), "1 min");
    t.eq(C.displayValue(u("rgb.idle_s"), 90), "1 min 30 s");
    t.eq(C.displayValue(u("ind.bat_low"), 0), "Off");
    t.eq(C.displayValue(u("ind.bat_low"), 20), "20 %");
    t.eq(C.displayValue(u("rgb.val_battery"), 255), "255, no cap");
    t.eq(C.displayValue(u("rgb.val_battery"), 128), "128");
    t.eq(C.displayValue(u("kb.sleep_min"), 0), "Never");
    t.eq(C.displayValue(u("kb.sleep_min"), 15), "15 min");
    t.eq(C.displayValue(u("kb.os_keys"), 2), "2");
    t.eq(C.displayValue({ key: "rgb.on", type: "b" }, false), "Off");
    t.eq(C.displayValue({ key: "ind.caps_color", type: "c" }, 0xff8000), "#FF8000");
    t.eq(C.displayValue({ key: "kb.os", type: "e" }, "win"), "Windows");
    t.eq(C.displayValue({ key: "rgb.cycle", type: "l" }, []), "All effects");
    t.eq(C.displayValue({ key: "rgb.cycle", type: "l" }, ["plasma", "speedcolour"]), "Plasma, Speed colour");
    t.eq(C.displayValue({ key: "rgb.val", type: "u" }, undefined), "");
  });

  test("labels: help texts carry the behaviour notes", (C) => {
    const help = (k) => C.settingInfo(k).help;
    t.ok(/pairing dialog on the computer is the only cue/.test(help("ind.passkey_guide")));
    t.ok(/Fn\+B/.test(help("ind.bat_low")) && /lighting off/.test(help("ind.bat_low")));
    t.ok(/only while the effect is drawn/.test(help("ind.caps_style")));
    t.ok(/USB to sleep/.test(help("rgb.val_battery")) && /USB to sleep/.test(help("ind.bat_low")));
    t.ok(/settings change/.test(help("kb.sleep_min")) && /open page/.test(help("kb.sleep_min")));
    t.ok(/Bluetooth/.test(help("kb.sleep_on_usb")) && /USB to sleep/.test(help("kb.sleep_on_usb")));
    t.ok(/ZMK Studio/.test(C.OS_KEYS_WARNING));
  });

  // ---- Task 9: Web Serial transport ----

  /* A Web Serial port in software. sim: the SimKeyboard behind the console
   * port; null makes it the Studio port, which never answers. Every reply
   * comes after a log line, cut into reads of 7 bytes. stall makes every
   * write hang until the stream is aborted (a writer that does not drain).
   * opts.closeMs: close() takes that long, as a tty that drains its output. */
  class FakePort {
    constructor(C, sim, opts = {}) {
      this.C = C;
      this.sim = sim;
      this.info = opts.info || { usbVendorId: 0x1d50, usbProductId: 0x615e };
      this.failOpen = !!opts.failOpen;
      this.opens = 0;
      this.isOpen = false;
      this.ctrl = null;
      this.stall = false;
      this.writeAborted = false;
      this.aborts = 0;
      this.closeMs = opts.closeMs || 0;
    }

    getInfo() {
      return this.info;
    }

    async open(options) {
      this.opens++;
      if (this.failOpen) throw new Error("Failed to open serial port.");
      if (this.isOpen) throw new Error("The port is already open.");
      this.isOpen = true;
      this.options = options;
      const self = this;
      const decoder = new this.C.SerialDecoder();
      this.readable = new ReadableStream({
        start(ctrl) {
          self.ctrl = ctrl;
          ctrl.enqueue(ascii("*** Booting Zephyr OS ***\r\n[00:00:00.010,000] <inf> zmk: boot\r\n"));
        },
        cancel() {
          self.ctrl = null;
        },
      });
      this.writable = new WritableStream({
        write(chunk, controller) {
          if (self.stall) {
            return new Promise((resolve, reject) => {
              controller.signal.addEventListener("abort", () => {
                self.writeAborted = true;
                reject(new Error("The write was aborted."));
              });
            });
          }
          for (const packet of decoder.push(chunk)) {
            if (!self.sim) continue;
            const reply = joinLines([ascii("[00:00:01.000,000] <inf> cfg_mgmt: log\r\n")].concat(self.C.serialEncode(self.sim.handle(packet))));
            setTimeout(() => {
              for (let i = 0; i < reply.length && self.ctrl; i += 7) self.ctrl.enqueue(reply.slice(i, i + 7));
            }, 2);
          }
        },
        abort() {
          self.aborts++;
        },
      });
    }

    async setSignals(s) {
      this.signals = s;
    }

    async close() {
      if (this.readable.locked || this.writable.locked) throw new Error("Cannot close a locked port.");
      if (this.closeMs) await sleep(this.closeMs);
      this.isOpen = false;
    }

    unplug() {
      this.ctrl.error(new Error("The device has been lost."));
      this.ctrl = null;
    }
  }

  test("usb: the console port answers the probe, through the log", async (C) => {
    const port = new FakePort(C, new C.SimKeyboard());
    const r = await C.probeSerial(port, 200);
    t.ok(r.ok, "console port");
    t.eq(port.options, { baudRate: 115200 });
    t.eq(port.signals, { dataTerminalReady: true, requestToSend: true });
    const model = new C.ConfigModel(r.client);
    await model.load();
    t.eq(model.values.size, 21);
    t.ok(r.transport.decoder.logLines >= 2, "log lines were skipped");
    await r.transport.close();
    t.ok(!port.isOpen, "closed again");
  });

  test("usb: the Studio port stays silent and is closed again", async (C) => {
    const port = new FakePort(C, null);
    const t0 = Date.now();
    const r = await C.probeSerial(port, 200);
    t.eq(r, { ok: false, reason: "silent" });
    t.ok(Date.now() - t0 < 1000, "about the probe time");
    t.ok(!port.isOpen);
    t.eq(C.PROBE_MS, 1000);
  });

  test("usb: a port in use", async (C) => {
    const r = await C.probeSerial(new FakePort(C, null, { failOpen: true }), 200);
    t.eq([r.ok, r.reason, r.error.message], [false, "open", "Failed to open serial port."]);
    t.ok(/Close ZMK Studio/.test(C.portBusyText(r.error)));
    t.ok(/While ZMK Studio is connected over USB, its port is busy: choose the other Rainy 75 port\./.test(C.portBusyText(r.error)));
  });

  test("usb: a firmware without group 67 still is the console", async (C) => {
    const r = await C.probeSerial(new FakePort(C, new C.SimKeyboard({ noConfig: true })), 200);
    t.ok(r.ok);
    await t.rejects(new C.ConfigModel(r.client).load(), /no runtime settings/);
    await r.transport.close();
  });

  test("usb: later visits find the console among the granted ports", async (C) => {
    const other = new FakePort(C, null, { info: { usbVendorId: 0x2341, usbProductId: 0x43 } });
    const studio = new FakePort(C, null);
    const consolePort = new FakePort(C, new C.SimKeyboard());
    const serial = { getPorts: async () => [other, studio, consolePort] };
    const r = await C.findGrantedPort(serial, 200);
    t.ok(r && r.ok);
    t.eq([other.opens, studio.opens, consolePort.opens], [0, 1, 1]);
    t.ok(!studio.isOpen && consolePort.isOpen);
    await r.transport.close();
    t.eq(await C.findGrantedPort({ getPorts: async () => [studio] }, 200), null);
  });

  test("usb: the picker shows only the keyboard", async (C) => {
    let asked = null;
    const serial = {
      requestPort: async (o) => {
        asked = o;
        return "port";
      },
    };
    t.eq(await C.pickPort(serial), "port");
    t.eq(asked, { filters: [{ usbVendorId: 0x1d50, usbProductId: 0x615e }] });
  });

  test("usb: unplugging fails the waiting request and tells the page", async (C) => {
    const sim = new C.SimKeyboard();
    const port = new FakePort(C, sim);
    const r = await C.probeSerial(port, 200);
    let lost = null;
    r.client.onClose = (e) => {
      lost = e.message;
    };
    sim.handle = () => C.smpFrame(9, 9, 9, 9, new Uint8Array(0));
    const p = r.client.request(C.SMP.READ, 67, 0, {});
    await sleep(10);
    port.unplug();
    await t.rejects(p, /closed/);
    t.eq(lost, "The device has been lost.");
  });

  /* The contract of SmpClient: it awaits send() before its timeout can fire,
   * so send() always settles. */

  test("usb: requests of the console client time out after 2 s, once retried", async (C) => {
    const r = await C.probeSerial(new FakePort(C, new C.SimKeyboard()), 200);
    t.eq([r.client.timeoutMs, r.client.retries, C.WRITE_MS], [2000, 1, 2000]);
    await r.transport.close();
  });

  test("usb: a write pending when the port is lost fails", async (C) => {
    const port = new FakePort(C, new C.SimKeyboard());
    const r = await C.probeSerial(port, 200);
    port.stall = true;
    const p = r.transport.send(C.smpFrame(C.SMP.READ, 67, 0, 5, new Uint8Array(0)));
    await sleep(10);
    port.unplug();
    await t.rejects(p, /device has been lost/);
    await t.rejects(r.transport.send(new Uint8Array(8)), /closed/);
    await r.transport.close();
    t.ok(port.writeAborted && !port.isOpen, "the stalled write was aborted, the port closed");
  });

  test("usb: a write pending when the transport closes fails and is aborted", async (C) => {
    const port = new FakePort(C, new C.SimKeyboard());
    const r = await C.probeSerial(port, 200);
    port.stall = true;
    const p = r.transport.send(C.smpFrame(C.SMP.READ, 67, 0, 5, new Uint8Array(0)));
    await sleep(10);
    await r.transport.close();
    await t.rejects(p, /closed/);
    t.ok(port.writeAborted, "the stalled write was aborted");
    t.ok(!port.isOpen, "closed");
  });

  test("usb: a stalled writer fails the request at its deadline", async (C) => {
    const port = new FakePort(C, new C.SimKeyboard());
    const r = await C.probeSerial(port, 200);
    r.transport.writeMs = 40;
    port.stall = true;
    const t0 = Date.now();
    const e = await t.rejects(r.client.request(C.SMP.READ, 67, 0, {}), /sending failed: .*timed out/);
    t.ok(e instanceof C.TransportError);
    t.ok(Date.now() - t0 < 1000, "not the 2 s request timeout");
    await r.transport.close();
    t.ok(port.writeAborted && !port.isOpen, "the stalled write was aborted, the port closed");
  });

  test("usb: after a probe closed the Studio port nothing stays locked", async (C) => {
    const port = new FakePort(C, null);
    t.eq(await C.probeSerial(port, 200), { ok: false, reason: "silent" });
    t.ok(!port.isOpen && !port.readable.locked && !port.writable.locked, "no lock left");
    port.sim = new C.SimKeyboard();
    const r = await C.probeSerial(port, 200);
    t.ok(r.ok, "the same port opens again and its reads reach the decoder");
    t.ok(r.transport.decoder.logLines >= 2);
    t.eq(port.opens, 2);
    await r.transport.close();
    t.ok(!port.readable.locked && !port.writable.locked);
  });

  test("usb: a silent probe drops its output before it closes the port", async (C) => {
    const port = new FakePort(C, null);
    t.eq(await C.probeSerial(port, 200), { ok: false, reason: "silent" });
    t.eq([port.aborts, port.isOpen], [1, false], "the writer was aborted, the port closed");
    t.eq([C.CLOSE_MS, C.REPLUG_WAITS], [3000, [300, 700, 1500]]);
  });

  test("usb: a probe waits at most CLOSE_MS for the close, the next one for the rest", async (C) => {
    const port = new FakePort(C, null, { closeMs: C.CLOSE_MS + 400 });
    const t0 = Date.now();
    t.eq(await C.probeSerial(port, 200), { ok: false, reason: "silent" });
    t.ok(Date.now() - t0 < C.CLOSE_MS + 350, "not the whole close");
    t.ok(port.isOpen, "still closing");
    port.sim = new C.SimKeyboard();
    port.closeMs = 0;
    const r = await C.probeSerial(port, 200);
    t.ok(r.ok, "opened again once the close was done");
    t.eq(port.opens, 2);
    await r.transport.close();
  });

  test("usb: after a replug a silent console is looked at again", async (C) => {
    const consolePort = new FakePort(C, null);
    const busy = new FakePort(C, null, { failOpen: true });
    let looks = 0;
    const serial = {
      getPorts: async () => {
        if (++looks === 3) consolePort.sim = new C.SimKeyboard();
        return [busy, consolePort];
      },
    };
    const r = await C.findGrantedPort(serial, 100, [0, 10, 10, 10]);
    t.ok(r && r.ok, "the third look finds it");
    t.eq([looks, consolePort.opens, busy.opens], [3, 3, 1], "a busy port is not tried again");
    await r.transport.close();
  });

  test("usb: the looks end when no port stays silent, or after the last wait", async (C) => {
    const busy = new FakePort(C, null, { failOpen: true });
    let looks = 0;
    const count = (ports) => ({
      getPorts: async () => {
        looks++;
        return ports;
      },
    });
    t.eq(await C.findGrantedPort(count([busy]), 100, [0, 10, 10]), null);
    t.eq([looks, busy.opens], [1, 1], "busy elsewhere: one look");
    looks = 0;
    t.eq(await C.findGrantedPort(count([]), 100, [0, 10, 10]), null);
    t.eq(looks, 1, "no keyboard port: one look");
    looks = 0;
    const studio = new FakePort(C, null);
    t.eq(await C.findGrantedPort(count([studio]), 100, [0, 10, 10]), null);
    t.eq([looks, studio.opens, studio.isOpen], [3, 3, false], "the Studio port: every look, closed again");
  });

  // ---- Task 10: Web Bluetooth transport ----

  /* A Web Bluetooth device in software: the SMP characteristic joins the
   * writes by the SMP header length, asks the SimKeyboard and notifies the
   * reply in 20-byte pieces. bonded: false fails startNotifications() as an
   * unpaired keyboard does. */
  class FakeChar extends EventTarget {
    constructor(C, dev) {
      super();
      this.C = C;
      this.dev = dev;
      this.properties = { writeWithoutResponse: true, notify: true };
      this.writes = [];
      this.rx = [];
      this.value = null;
    }

    async startNotifications() {
      if (!this.dev.bonded) throw new DOMException("GATT operation not authorized.", "SecurityError");
      return this;
    }

    async stopNotifications() {
      this.dev.stops++;
    }

    async writeValueWithoutResponse(chunk) {
      this.writes.push(chunk.length);
      this.rx.push(...chunk);
      const need = this.rx.length >= 8 ? 8 + ((this.rx[2] << 8) | this.rx[3]) : Infinity;
      if (this.rx.length < need) return;
      const reply = this.dev.sim.handle(Uint8Array.from(this.rx.splice(0, need)));
      setTimeout(() => {
        for (const c of this.C.bleChunks(reply)) {
          this.value = new DataView(c.buffer, c.byteOffset, c.byteLength);
          this.dispatchEvent(new Event("characteristicvaluechanged"));
        }
      }, 2);
    }
  }

  class FakeDevice extends EventTarget {
    constructor(C, sim, opts = {}) {
      super();
      this.name = opts.name || "Rainy 75 Pro";
      this.sim = sim;
      this.bonded = opts.bonded !== false;
      this.stops = 0;
      this.disconnects = 0;
      const dev = this;
      const ch = (this.char = new FakeChar(C, this));
      this.gatt = {
        connected: false,
        async connect() {
          if (opts.hang) await new Promise(() => {});
          if (opts.delay) await sleep(opts.delay);
          this.connected = true;
          return {
            getPrimaryService: async (uuid) => {
              dev.serviceUuid = uuid;
              return {
                getCharacteristic: async (uuid2) => {
                  dev.charUuid = uuid2;
                  return ch;
                },
              };
            },
          };
        },
        disconnect() {
          this.connected = false;
          dev.disconnects++;
        },
      };
    }

    drop() {
      this.gatt.connected = false;
      this.dispatchEvent(new Event("gattserverdisconnected"));
    }
  }

  test("ble: the picker asks by name, with the SMP service allowed", async (C) => {
    let asked = null;
    const bt = {
      requestDevice: async (o) => {
        asked = o;
        return "dev";
      },
    };
    t.eq(await C.pickDevice(bt), "dev");
    t.eq(asked, { filters: [{ name: "Rainy 75 Pro" }], optionalServices: ["8d53dc1d-1db7-4cd3-868b-8a527460aa84"] });
  });

  test("ble: a remembered keyboard without the picker", async (C) => {
    const kb = new FakeDevice(C, null);
    const other = new FakeDevice(C, null, { name: "Mouse" });
    t.eq(await C.rememberedDevice({ getDevices: async () => [other, kb] }), kb);
    t.eq(await C.rememberedDevice({ getDevices: async () => [other] }), null);
    t.eq(await C.rememberedDevice({}), null, "no getDevices in this browser");
  });

  test("ble: settings over 20-byte writes and notifications", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    t.eq([dev.serviceUuid, dev.charUuid], [C.BLE.SERVICE, C.BLE.CHAR]);
    t.eq(r.client.timeoutMs, 5000);
    const model = new C.ConfigModel(r.client);
    await model.load();
    t.eq(model.values.size, 21);
    await model.set("rgb.cycle", ["plasma", "solid", "rain", "wave", "comet"]);
    t.ok(Math.max(...dev.char.writes) === 20, "writes of at most 20 bytes");
    t.eq(dev.sim.values["rgb.cycle"], ["plasma", "solid", "rain", "wave", "comet"]);
    await r.transport.close();
    t.eq([dev.stops, dev.disconnects], [1, 1]);
  });

  test("ble: a keyboard not paired with this computer", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard(), { bonded: false });
    const e = await t.rejects(C.openBle(dev), /not paired with this computer/);
    t.ok(/Fn\+F1, F2 or F3/.test(e.message) && /type the code the computer shows on the keyboard/.test(e.message), "the pairing steps");
    t.ok(/remove that pairing on both sides first/.test(e.message), "an old pairing is removed first");
    t.ok(e instanceof C.NotBondedError);
    t.eq(e.cause.name, "SecurityError");
    t.eq(dev.disconnects, 1, "the link the page opened is closed again");
  });

  test("ble: connecting that takes too long", async (C) => {
    const e = await t.rejects(C.openBle(new FakeDevice(C, null, { hang: true }), 30), /no Bluetooth connection/);
    t.ok(e instanceof C.TransportError);
  });

  test("ble: a dropped link fails the waiting request and tells the page", async (C) => {
    const sim = new C.SimKeyboard();
    const dev = new FakeDevice(C, sim);
    const r = await C.openBle(dev);
    let lost = null;
    r.client.onClose = (err) => {
      lost = err.message;
    };
    sim.handle = () => C.smpFrame(9, 9, 9, 9, new Uint8Array(0));
    const p = r.client.request(C.SMP.READ, 67, 0, {});
    await sleep(10);
    dev.drop();
    await t.rejects(p, /closed/);
    t.eq(lost, "Bluetooth disconnected");
  });

  /* Contracts of BleTransport. SmpClient awaits send() before its timeout can
   * fire, so send() must always settle; the reassembler starts clean with
   * every attempt; the writes of one request never mix with another's; a
   * request fits the firmware's 512-byte mcumgr buffer. */

  /* An open transport whose writes are logged as the first byte of each chunk
   * (a packet filled with one value shows whose chunk it is). */
  async function openLogged(C, ms = 3) {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    const log = [];
    dev.char.writeValueWithoutResponse = async (c) => {
      log.push(c[0]);
      await sleep(ms);
    };
    return { dev, r, log };
  }

  const stuck = () => new Promise(() => {});

  test("ble: a notification may be a view into a bigger buffer", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    const got = [];
    r.transport.onPacket = (p) => got.push(p);
    const frame = C.smpFrame(C.SMP.READ + 1, 67, 0, 7, new Uint8Array(0));
    const heap = new Uint8Array(40).fill(0xee);
    heap.set(frame, 11);
    dev.char.value = new DataView(heap.buffer, 11, frame.length);
    dev.char.dispatchEvent(new Event("characteristicvaluechanged"));
    t.eq(got, [frame]);
    await r.transport.close();
  });

  test("ble: every attempt starts with a clean reassembler", async (C) => {
    const sim = new C.SimKeyboard();
    const dev = new FakeDevice(C, sim);
    const r = await C.openBle(dev);
    r.client.timeoutMs = 40;
    dev.char.value = new DataView(Uint8Array.of(1, 2, 3).buffer);
    dev.char.dispatchEvent(new Event("characteristicvaluechanged"));
    const info = await r.client.request(C.SMP.READ, 67, 0, {});
    t.eq(info.n, 21, "after noise from before the request");
    const handle = sim.handle.bind(sim);
    let calls = 0;
    sim.handle = (f) => {
      const reply = handle(f);
      return ++calls === 1 ? reply.slice(0, 12) : reply;
    };
    t.eq((await r.client.request(C.SMP.READ, 67, 0, {})).n, 21, "the retry after a cut reply");
    t.eq(calls, 2);
    await r.transport.close();
  });

  test("ble: the chunks of one request stay together", async (C) => {
    const { r, log } = await openLogged(C);
    await Promise.all([r.transport.send(new Uint8Array(45).fill(1)), r.transport.send(new Uint8Array(45).fill(2))]);
    t.eq(log, [1, 1, 1, 2, 2, 2]);
    await r.transport.close();
  });

  test("ble: a request over the keyboard's 512 bytes is never sent", async (C) => {
    const { r, log } = await openLogged(C, 0);
    t.eq(C.BLE_SMP_MAX, 512);
    await t.rejects(r.transport.send(new Uint8Array(513)), /513 bytes.*512/);
    t.eq(log, [], "nothing written");
    await r.transport.send(new Uint8Array(512));
    t.eq(log.length, 26, "512 bytes still fit");
    const e = await t.rejects(r.client.request(C.SMP.WRITE, 67, 3, { k: "rgb.cycle", v: "x".repeat(600) }), /sending failed: .*512/);
    t.ok(e instanceof C.TransportError);
    t.eq(log.length, 26, "the client wrote nothing either");
    await r.transport.close();
  });

  test("ble: a stalled write fails the request at its deadline", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    t.ok(C.BLE_WRITE_MS <= r.client.timeoutMs, "no longer than the request timeout");
    r.transport.writeMs = 40;
    dev.char.writeValueWithoutResponse = stuck;
    const t0 = Date.now();
    const e = await t.rejects(r.client.request(C.SMP.READ, 67, 0, {}), /sending failed: .*timed out/);
    t.ok(e instanceof C.TransportError);
    t.ok(Date.now() - t0 < 1000, "not the 5 s request timeout");
    await r.transport.close();
  });

  test("ble: after the deadline the rest of the request is never written", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    r.transport.writeMs = 40;
    const log = [];
    let release = null;
    dev.char.writeValueWithoutResponse = (c) => {
      log.push(c[0]);
      return c[0] === 1 ? new Promise((resolve) => (release = resolve)) : Promise.resolve();
    };
    await t.rejects(r.transport.send(new Uint8Array(45).fill(1)), /timed out/);
    const next = r.transport.send(new Uint8Array(45).fill(2));
    await sleep(10);
    t.eq(log, [1], "the next request waits for the stalled write");
    release();
    await next;
    t.eq(log, [1, 2, 2, 2], "chunks 2 and 3 of the abandoned request stay unwritten");
    await r.transport.close();
  });

  test("ble: a link lost with a write pending fails the request", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    let lost = null;
    r.client.onClose = (err) => {
      lost = err.message;
    };
    dev.char.writeValueWithoutResponse = stuck;
    const p = r.client.request(C.SMP.READ, 67, 0, {});
    await sleep(10);
    dev.drop();
    const e = await t.rejects(p, /sending failed: Bluetooth disconnected/);
    t.ok(e instanceof C.TransportError);
    t.eq(lost, "Bluetooth disconnected");
    await t.rejects(r.transport.send(new Uint8Array(8)), /not connected/);
    await r.transport.close();
  });

  test("ble: closing with a write pending fails it", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    dev.char.writeValueWithoutResponse = stuck;
    const p = r.transport.send(new Uint8Array(45).fill(1));
    await sleep(10);
    await r.transport.close();
    await t.rejects(p, /closed/);
    t.eq(dev.disconnects, 1);
  });

  test("ble: closing does not wait for a notification stop that never ends", async (C) => {
    const dev = new FakeDevice(C, new C.SimKeyboard());
    const r = await C.openBle(dev);
    dev.char.stopNotifications = stuck;
    r.transport.stopMs = 30;
    await r.transport.close();
    t.eq(dev.disconnects, 1, "the link is closed anyway");
  });

  test("ble: giving up on a connect still pending cancels it", async (C) => {
    const dev = new FakeDevice(C, null, { hang: true });
    await t.rejects(C.openBle(dev, 30), /no Bluetooth connection/);
    t.eq(dev.disconnects, 1, "disconnect() is what cancels a connect() in progress");
  });

  test("ble: a connect that finishes after the page gave up is closed again", async (C) => {
    const dev = new FakeDevice(C, null, { delay: 80 });
    await t.rejects(C.openBle(dev, 30), /no Bluetooth connection/);
    await sleep(120);
    t.ok(!dev.gatt.connected, "no link left open");
    t.eq(dev.serviceUuid, undefined, "no service lookup after giving up");
  });

  // ---- Task 11: UI (browser only, on index.html?demo in a frame) ----

  /* index.html?demo[=variant] in a frame of test.html, after it connected:
   * {w, d, sim, row(key), close()}. */
  async function openDemo(env, variant) {
    const f = document.createElement("iframe");
    f.src = "index.html?demo" + (variant ? "=" + variant : "");
    env.frames.append(f);
    await new Promise((resolve) => f.addEventListener("load", resolve, { once: true }));
    const w = f.contentWindow;
    await w.RainyDemo.ready;
    const d = w.document;
    return {
      w, d, sim: w.RainyDemo.sim,
      row: (key) => d.querySelector(`.row[data-key="${key}"]`),
      close: () => f.remove(),
    };
  }
  const fire = (el, type) => el.dispatchEvent(new (el.ownerDocument ? el.ownerDocument.defaultView : el).Event(type, { bubbles: true }));
  async function until(cond, ms = 2000) {
    for (let i = 0; i < ms / 20; i++) {
      if (cond()) return;
      await sleep(20);
    }
    throw new Error("timed out waiting");
  }

  uiTest("ui: the demo shows every setting in its section", async (C, env) => {
    const p = await openDemo(env);
    try {
      t.eq(p.d.getElementById("state").textContent, "Demo: a simulated keyboard");
      t.eq(p.d.getElementById("fw").textContent, "Firmware 0.4.0");
      t.eq(p.d.querySelectorAll("#rows-rgb .row").length, 11);
      t.eq(p.d.querySelectorAll("#rows-ind .row").length, 5);
      t.eq(p.d.querySelectorAll("#rows-kb .row").length, 5);
      t.ok(p.d.getElementById("hidden-note").hidden);
      t.ok(p.d.getElementById("intro").hidden && !p.d.getElementById("settings").hidden);
      t.ok(p.d.getElementById("btn-usb").hidden && p.d.getElementById("btn-disconnect").hidden);
      t.eq(p.d.activeElement.id, "main", "focus moves into the settings when nothing had it");
      for (const el of p.d.querySelectorAll(".row > .control > [id]")) {
        t.ok(p.d.querySelector(`label[for="${el.id}"]`), "a label for " + el.id);
        for (const d of el.getAttribute("aria-describedby").split(" ")) t.ok(p.d.getElementById(d), "help for " + el.id);
      }
    } finally {
      p.close();
    }
  });

  uiTest("ui: a switch and a choice reach the keyboard", async (C, env) => {
    const p = await openDemo(env);
    try {
      const on = p.d.getElementById("set-rgb-on");
      t.eq([on.checked, on.getAttribute("role")], [true, "switch"]);
      on.click();
      await until(() => p.sim.values["rgb.on"] === false);
      const fx = p.d.getElementById("set-rgb-effect");
      t.eq(fx.value, "solid");
      t.eq(fx.options.length, 12);
      fx.value = "plasma";
      fire(fx, "change");
      await until(() => p.sim.values["rgb.effect"] === "plasma");
      t.ok(/^Plasma: Flowing/.test(p.row("rgb.effect").querySelector("p.help:last-child").textContent));
      t.eq(p.d.getElementById("set-kb-os").value, "win");
    } finally {
      p.close();
    }
  });

  uiTest("ui: changes on the keyboard show up (rev)", async (C, env) => {
    const p = await openDemo(env);
    try {
      p.sim.pressFnEnter();
      await until(() => p.d.getElementById("set-rgb-effect").value === "rainbow", 3000);
    } finally {
      p.close();
    }
  });

  uiTest("ui: kb.os_keys 0 warns, re-read on focus without a rev change", async (C, env) => {
    const p = await openDemo(env);
    try {
      const warn = p.row("kb.os_keys").querySelector(".warn");
      t.eq(p.d.getElementById("set-kb-os-keys").textContent, "2");
      t.ok(warn.hidden);
      p.sim.setOsKeys(0);
      fire(p.w, "focus");
      await until(() => !warn.hidden);
      t.eq(p.d.getElementById("set-kb-os-keys").textContent, "0");
    } finally {
      p.close();
    }
  });

  uiTest("ui: a refused change shows the error and the keyboard's value", async (C, env) => {
    const p = await openDemo(env);
    try {
      const sw = p.d.getElementById("set-ind-fn-highlight");
      p.sim.failNextSet = 3;
      sw.click();
      await until(() => !p.d.getElementById("msg").hidden);
      t.eq(p.d.getElementById("msg").textContent, "The keyboard refused: invalid value (rc 3).");
      await until(() => sw.checked === true);
    } finally {
      p.close();
    }
  });

  uiTest("ui: settings and names of a newer firmware are hidden", async (C, env) => {
    const p = await openDemo(env, "future");
    try {
      t.eq(p.d.getElementById("hidden-note").textContent, "1 setting needs a newer page.");
      t.ok(!p.d.getElementById("hidden-note").hidden);
      t.eq(p.row("rgb.future"), null);
      const fx = p.d.getElementById("set-rgb-effect");
      t.eq(fx.options.length, 12, "fireworks is not offered");
      p.sim.values["rgb.effect"] = "fireworks";
      p.sim.rev++;
      await until(() => fx.value === "", 3000);
      t.eq(fx.selectedOptions[0].textContent, "Other (needs a newer page)");
    } finally {
      p.close();
    }
  });

  uiTest("ui: an old firmware and an unconfirmed image", async (C, env) => {
    const old = await openDemo(env, "old");
    try {
      t.ok(/no runtime settings/.test(old.d.getElementById("msg").textContent));
      t.eq(old.d.getElementById("state").textContent, "Not connected");
      t.ok(old.d.getElementById("settings").hidden);
    } finally {
      old.close();
    }
    const test = await openDemo(env, "test");
    try {
      t.eq(test.d.getElementById("fw").textContent, "Firmware 0.4.0, test image (not confirmed)");
    } finally {
      test.close();
    }
  });

  uiTest("ui: a lost connection brings back the connect buttons", async (C, env) => {
    const p = await openDemo(env);
    try {
      p.w.RainyDemo.transport.lose();
      t.eq(p.d.getElementById("state").textContent, "Connection lost");
      t.ok(!p.d.getElementById("btn-usb").hidden && !p.d.getElementById("btn-ble").hidden);
      t.ok(p.d.getElementById("settings").hidden);
      t.eq(p.d.getElementById("msg").textContent, "Connection lost. Connect again.");
    } finally {
      p.close();
    }
  });

  /* Contracts from the reviews of the earlier tasks (see the Task 11 report). */

  uiTest("ui: send() tells the caller whether the value went out", async (C, env) => {
    const p = await openDemo(env);
    try {
      const send = p.w.RainyDemo.send;
      const msg = () => p.d.getElementById("msg");
      t.eq(await send("rgb.hue", 40), true);
      t.eq(p.sim.values["rgb.hue"], 40);
      p.sim.failNextSet = 3;
      t.eq(await send("rgb.hue", 50), false, "refused by the keyboard");
      t.eq(msg().textContent, "The keyboard refused: invalid value (rc 3).");
      t.eq(p.sim.values["rgb.hue"], 40);
      t.eq(await send("rgb.hue", 999), false, "refused by the page's own check");
      t.ok(/outside 0\.\.255/.test(msg().textContent), msg().textContent);
      t.eq(await send("rgb.hue", 60), true);
      t.ok(msg().hidden, "a success clears the error");
      p.w.RainyDemo.transport.send = async () => {
        throw new Error("write failed");
      };
      t.eq(await send("rgb.hue", 70), false, "transport failure");
      t.eq(p.d.getElementById("state").textContent, "Connection lost");
      t.eq(await send("rgb.hue", 80), false, "no connection: nothing is sent");
    } finally {
      p.close();
    }
  });

  uiTest("ui: a transport that stops answering is dropped, closed and not used again", async (C, env) => {
    const p = await openDemo(env);
    try {
      const tr = p.w.RainyDemo.transport;
      tr.drop = 1000000; /* every request is swallowed: the poll times out after its retry */
      await until(() => p.d.getElementById("state").textContent === "Connection lost", 7000);
      t.ok(tr.closed, "the transport is closed");
      t.ok(p.d.getElementById("settings").hidden);
      t.ok(!p.d.getElementById("btn-usb").hidden && !p.d.getElementById("btn-ble").hidden);
      t.eq(p.d.getElementById("msg").textContent, "Connection lost. Connect again.");
      const n = p.sim.requests.length;
      await sleep(1500);
      t.eq(p.sim.requests.length, n, "no request after the drop");
    } finally {
      p.close();
    }
  });

  uiTest("ui: kb.os_keys is re-read when the page becomes visible, not while it is hidden", async (C, env) => {
    const p = await openDemo(env);
    try {
      const warn = p.row("kb.os_keys").querySelector(".warn");
      const visible = (on) => Object.defineProperty(p.d, "hidden", { configurable: true, get: () => !on });
      p.sim.setOsKeys(0);
      visible(false);
      p.d.dispatchEvent(new p.w.Event("visibilitychange"));
      fire(p.w, "focus");
      await sleep(300);
      t.ok(warn.hidden, "a hidden page does not read");
      visible(true);
      p.d.dispatchEvent(new p.w.Event("visibilitychange"));
      await until(() => !warn.hidden);
      t.eq(p.d.getElementById("set-kb-os-keys").textContent, "0");
    } finally {
      p.close();
    }
  });

  uiTest("ui: numbers carry their units", async (C, env) => {
    const p = await openDemo(env);
    try {
      const text = (key) => p.row(key).querySelector("output").textContent;
      t.eq([text("kb.sleep_min"), text("ind.bat_low"), text("rgb.idle_s"), text("rgb.val_battery")],
        ["15 min", "Off", "Never", "255, no cap"]);
      p.sim.values["kb.sleep_min"] = 90;
      p.sim.values["ind.bat_low"] = 20;
      p.sim.values["rgb.idle_s"] = 90;
      p.sim.values["rgb.val_battery"] = 100;
      p.sim.rev++;
      await until(() => text("kb.sleep_min") === "90 min", 3000);
      t.eq([text("ind.bat_low"), text("rgb.idle_s"), text("rgb.val_battery")], ["20 %", "1 min 30 s", "100"]);
    } finally {
      p.close();
    }
  });

  uiTest("ui: the hidden attribute hides an element whatever its class", async (C, env) => {
    const p = await openDemo(env);
    try {
      const conn = p.d.getElementById("conn");
      t.eq(p.w.getComputedStyle(conn).display, "flex");
      conn.hidden = true;
      t.eq(p.w.getComputedStyle(conn).display, "none");
    } finally {
      p.close();
    }
  });

  // ---- Task 12: sliders, colour, effect cycle, reset (browser only) ----

  const setsOf = (sim, key) => sim.requests.filter((r) => r.cmd === 3 && r.body && r.body.k === key);

  uiTest("ui: a dragged slider sends throttled, then the final value", async (C, env) => {
    const p = await openDemo(env);
    try {
      const s = p.d.getElementById("set-rgb-val");
      t.eq([s.type, s.min, s.max, s.value], ["range", "16", "255", "200"]);
      fire(s, "pointerdown");
      for (let v = 100; v < 120; v++) {
        s.value = v;
        fire(s, "input");
        await sleep(5);
      }
      s.value = 120;
      fire(s, "input");
      fire(s, "change");
      fire(s, "pointerup");
      await until(() => p.sim.values["rgb.val"] === 120);
      const n = setsOf(p.sim, "rgb.val").length;
      t.ok(n >= 2 && n <= 5, "throttled: " + n + " sends for 21 moves");
      t.eq(s.getAttribute("aria-valuetext"), "120");
    } finally {
      p.close();
    }
  });

  uiTest("ui: a refused slider value is sent again on release", async (C, env) => {
    const p = await openDemo(env);
    try {
      const s = p.d.getElementById("set-rgb-val");
      p.sim.failNextSet = 3;
      fire(s, "pointerdown");
      s.value = 90;
      fire(s, "input");
      await until(() => !p.d.getElementById("msg").hidden);
      t.eq(p.sim.values["rgb.val"], 200, "the first send was refused");
      fire(s, "change");
      fire(s, "pointerup");
      await until(() => p.sim.values["rgb.val"] === 90);
      await until(() => p.d.getElementById("msg").hidden);
      t.eq(s.value, "90");
    } finally {
      p.close();
    }
  });

  uiTest("ui: a slider whose pointerup never came is not held after change", async (C, env) => {
    const p = await openDemo(env);
    try {
      const s = p.d.getElementById("set-rgb-val");
      fire(s, "pointerdown");
      s.value = 90;
      fire(s, "input");
      fire(s, "change");
      await until(() => p.sim.values["rgb.val"] === 90);
      p.sim.values["rgb.val"] = 60;
      p.sim.rev++;
      await until(() => s.value === "60", 3000);
    } finally {
      p.close();
    }
  });

  uiTest("ui: after release the slider shows what the keyboard stored", async (C, env) => {
    const p = await openDemo(env);
    try {
      const s = p.d.getElementById("set-rgb-val");
      const check = p.sim._check.bind(p.sim);
      p.sim._check = (d, v) => (d.key === "rgb.val" ? Math.min(v, 100) : check(d, v));
      fire(s, "pointerdown");
      s.value = 120;
      fire(s, "input");
      fire(s, "change");
      fire(s, "pointerup");
      await until(() => s.value === "100");
      t.eq(p.sim.values["rgb.val"], 100);
      t.eq(p.row("rgb.val").querySelector("output").textContent, "100");
      t.eq(s.getAttribute("aria-valuetext"), "100");
      /* a refused last value: the slider goes back to the keyboard's */
      p.sim.failNextSet = 3;
      fire(s, "pointerdown");
      s.value = 60;
      fire(s, "input");
      fire(s, "change");
      fire(s, "pointerup");
      await until(() => !p.d.getElementById("msg").hidden);
      await until(() => s.value === "100");
      t.eq(p.row("rgb.val").querySelector("output").textContent, "100");
    } finally {
      p.close();
    }
  });

  uiTest("ui: number formats on sliders", async (C, env) => {
    const p = await openDemo(env);
    try {
      const idle = p.d.getElementById("set-rgb-idle-s");
      t.eq(p.row("rgb.idle_s").querySelector("output").textContent, "Never");
      idle.value = 90;
      fire(idle, "input");
      fire(idle, "change");
      await until(() => p.sim.values["rgb.idle_s"] === 90);
      t.eq(p.row("rgb.idle_s").querySelector("output").textContent, "1 min 30 s");
      t.eq(p.row("kb.sleep_min").querySelector("output").textContent, "15 min");
    } finally {
      p.close();
    }
  });

  uiTest("ui: the colour picker", async (C, env) => {
    const p = await openDemo(env);
    try {
      const c = p.d.getElementById("set-ind-caps-color");
      t.eq(c.value, "#ffffff");
      c.value = "#ff8000";
      fire(c, "input");
      fire(c, "change");
      await until(() => p.sim.values["ind.caps_color"] === 0xff8000);
      t.eq(p.row("ind.caps_color").querySelector("output").textContent, "#FF8000");
    } finally {
      p.close();
    }
  });

  uiTest("ui: a refused colour is sent again when the picker closes", async (C, env) => {
    const p = await openDemo(env);
    try {
      const c = p.d.getElementById("set-ind-caps-color");
      p.sim.failNextSet = 3;
      c.value = "#00ff00";
      fire(c, "input");
      await until(() => !p.d.getElementById("msg").hidden);
      t.eq(p.sim.values["ind.caps_color"], 0xffffff, "the first send was refused");
      fire(c, "change");
      await until(() => p.sim.values["ind.caps_color"] === 0x00ff00);
      await until(() => p.d.getElementById("msg").hidden);
      t.eq(c.value, "#00ff00");
    } finally {
      p.close();
    }
  });

  uiTest("ui: the effect cycle, by checkbox and arrow buttons", async (C, env) => {
    const p = await openDemo(env);
    try {
      const names = () => Array.from(p.d.querySelectorAll("#set-rgb-cycle li"), (li) => li.dataset.name);
      t.eq(names(), C.SIM_EFFECTS);
      p.d.getElementById("set-rgb-cycle-rainbow").click();
      await until(() => p.sim.values["rgb.cycle"].length === 11 && names()[11] === "rainbow");
      t.eq(p.sim.values["rgb.cycle"], C.SIM_EFFECTS.filter((n) => n !== "rainbow"));
      const up = p.d.querySelector('#set-rgb-cycle li[data-name="plasma"] [data-part="up"]');
      t.eq(up.getAttribute("aria-label"), "Move Plasma up");
      up.focus();
      up.click();
      await until(() => p.sim.values["rgb.cycle"][0] === "plasma");
      t.eq(p.sim.values["rgb.cycle"].slice(0, 3), ["plasma", "solid", "twinkle"]);
      t.eq(p.d.activeElement.closest("li").dataset.name, "plasma", "focus stays on the moved row");
      p.sim.values["rgb.cycle"] = ["wave"];
      p.sim.rev++;
      await until(() => names()[0] === "wave", 3000);
      t.ok(p.d.getElementById("set-rgb-cycle-wave").disabled, "the last ticked effect cannot be unticked");
    } finally {
      p.close();
    }
  });

  uiTest("ui: the effect cycle works from the keyboard", async (C, env) => {
    const p = await openDemo(env);
    try {
      const li = (n) => p.d.querySelector(`#set-rgb-cycle li[data-name="${n}"]`);
      const all = p.d.querySelectorAll("#set-rgb-cycle li input, #set-rgb-cycle li button");
      t.eq(all.length, 36, "a checkbox and two buttons per effect");
      t.ok(Array.from(all).every((el) => el.tabIndex === 0 || el.disabled), "all in the tab order");
      const first = li("solid").querySelector('[data-part="up"]');
      const last = li("speedcolour").querySelector('[data-part="down"]');
      t.eq([first.getAttribute("aria-disabled"), last.getAttribute("aria-disabled")], ["true", "true"],
        "no move up on the first row, no move down on the last row");
      t.ok(!first.disabled && !last.disabled, "they stay focusable");
      t.eq(li("rainbow").querySelectorAll("[aria-disabled]").length, 0, "the buttons of the other rows are enabled");
      const down = li("solid").querySelector('[data-part="down"]');
      t.eq(down.getAttribute("aria-label"), "Move Solid down");
      down.focus();
      down.click();
      await until(() => p.sim.values["rgb.cycle"][0] === "rainbow");
      t.eq(p.sim.values["rgb.cycle"].slice(0, 3), ["rainbow", "solid", "plasma"]);
      t.eq([p.d.activeElement.closest("li").dataset.name, p.d.activeElement.dataset.part], ["solid", "down"]);
      const help = p.d.getElementById("set-rgb-cycle-help").textContent;
      t.ok(/Move up/.test(help) && /Move down/.test(help), "the help names the buttons");
    } finally {
      p.close();
    }
  });

  uiTest("ui: Move down pressed again before the keyboard answered still counts", async (C, env) => {
    const p = await openDemo(env);
    try {
      p.w.RainyDemo.transport.latencyMs = 100;
      const names = () => Array.from(p.d.querySelectorAll("#set-rgb-cycle li"), (li) => li.dataset.name);
      const down = () => p.d.querySelector('#set-rgb-cycle li[data-name="plasma"] [data-part="down"]');
      t.eq(names().indexOf("plasma"), 2);
      down().click();
      await sleep(30);
      down().click();
      await sleep(85); /* t = 115 ms: the first answer is in, the second is not */
      down().click();
      await until(() => p.sim.values["rgb.cycle"].indexOf("plasma") === 5, 3000);
      await until(() => names().indexOf("plasma") === 5, 3000);
      t.eq(p.sim.values["rgb.cycle"], names());
    } finally {
      p.close();
    }
  });

  uiTest("ui: the effect cycle keeps effects this page does not know", async (C, env) => {
    const p = await openDemo(env, "future");
    try {
      const names = () => Array.from(p.d.querySelectorAll("#set-rgb-cycle li"), (li) => li.dataset.name);
      t.eq(names(), C.SIM_EFFECTS, "fireworks is not listed");
      p.sim.values["rgb.cycle"] = ["solid", "fireworks", "rainbow"];
      p.sim.rev++;
      await until(() => !p.d.getElementById("set-rgb-cycle-plasma").checked, 3000);
      p.d.getElementById("set-rgb-cycle-rainbow").click();
      await until(() => p.sim.values["rgb.cycle"].length === 2);
      t.eq(p.sim.values["rgb.cycle"], ["solid", "fireworks"]);
      await until(() => p.d.getElementById("set-rgb-cycle-solid").disabled, 3000);
      p.d.getElementById("set-rgb-cycle-wave").click();
      await until(() => p.sim.values["rgb.cycle"].includes("wave"));
      t.eq(p.sim.values["rgb.cycle"], ["solid", "wave", "fireworks"]);
    } finally {
      p.close();
    }
  });

  uiTest("ui: the effect cycle, by drag and drop", async (C, env) => {
    const p = await openDemo(env);
    try {
      const li = (n) => p.d.querySelector(`#set-rgb-cycle li[data-name="${n}"]`);
      const dt = new p.w.DataTransfer();
      li("comet").dispatchEvent(new p.w.DragEvent("dragstart", { bubbles: true, dataTransfer: dt }));
      li("solid").dispatchEvent(new p.w.DragEvent("dragover", { bubbles: true, cancelable: true, dataTransfer: dt }));
      li("solid").dispatchEvent(new p.w.DragEvent("drop", { bubbles: true, cancelable: true, dataTransfer: dt }));
      await until(() => p.sim.values["rgb.cycle"][0] === "comet");
      t.eq(p.sim.values["rgb.cycle"].slice(0, 3), ["comet", "solid", "rainbow"]);
    } finally {
      p.close();
    }
  });

  uiTest("ui: a cycle checkbox is named by its effect and described by the help", async (C, env) => {
    const p = await openDemo(env);
    try {
      const cb = p.d.getElementById("set-rgb-cycle-plasma");
      t.eq(cb.labels.length, 1);
      t.eq(cb.labels[0].textContent, "Plasma", "the name is the effect only");
      const help = p.d.getElementById(cb.getAttribute("aria-describedby"));
      t.ok(help, "aria-describedby points to an element");
      t.ok(!cb.labels[0].contains(help), "the help is not inside the label");
      t.eq(help.textContent.trim(), C.nameHelp("rgb.cycle", "plasma"));
      for (const c of p.d.querySelectorAll("#set-rgb-cycle li input")) {
        t.ok(p.d.getElementById(c.getAttribute("aria-describedby")), "help for " + c.id);
      }
    } finally {
      p.close();
    }
  });

  uiTest("ui: focus stays on a control when the moved row loses a button", async (C, env) => {
    const p = await openDemo(env);
    try {
      p.sim.values["rgb.cycle"] = ["wave"];
      p.sim.rev++;
      await until(() => p.d.getElementById("set-rgb-cycle-wave").disabled, 3000);
      const part = (x) => p.d.querySelector(`#set-rgb-cycle li[data-name="wave"] [data-part="${x}"]`);
      const where = () => [p.d.activeElement.closest("li").dataset.name, p.d.activeElement.dataset.part];
      part("down").focus();
      for (let i = 0; i < 11; i++) part("down").click();
      t.ok(part("down").getAttribute("aria-disabled") === "true" && part("cb").disabled, "last row, only ticked");
      t.eq(where(), ["wave", "down"], "focus stays on the pressed button");
      part("up").focus();
      for (let i = 0; i < 11; i++) part("up").click();
      t.ok(part("up").getAttribute("aria-disabled") === "true" && part("cb").disabled, "first row, only ticked");
      t.eq(where(), ["wave", "up"], "focus stays on the pressed button");
    } finally {
      p.close();
    }
  });

  uiTest("ui: Enter repeated on a move button past the end keeps the row at the end", async (C, env) => {
    const p = await openDemo(env);
    try {
      const names = () => Array.from(p.d.querySelectorAll("#set-rgb-cycle li"), (li) => li.dataset.name);
      const btn = (n, x) => p.d.querySelector(`#set-rgb-cycle li[data-name="${n}"] [data-part="${x}"]`);
      /* Enter on a button clicks it; on a checkbox it does nothing (a script
       * cannot send a trusted key press, so the test applies that rule). */
      const enter = () => {
        const el = p.d.activeElement;
        if (el.tagName === "BUTTON") el.click();
      };
      const settled = (n, i) => names().indexOf(n) === i && p.sim.values["rgb.cycle"].indexOf(n) === i;
      t.eq(names().indexOf("comet"), 4);
      btn("comet", "up").focus();
      for (let i = 0; i < 12; i++) enter();
      await until(() => settled("comet", 0), 3000);
      await sleep(300);
      t.ok(settled("comet", 0), "past the top: comet stays first on the page and on the keyboard");
      t.eq(p.d.activeElement.closest("li").dataset.name, "comet", "focus stays on the row");
      t.eq(names().indexOf("rain"), 9);
      btn("rain", "down").focus();
      for (let i = 0; i < 12; i++) enter();
      await until(() => settled("rain", 11), 3000);
      await sleep(300);
      t.ok(settled("rain", 11), "past the bottom: rain stays last on the page and on the keyboard");
      t.eq(p.d.activeElement.closest("li").dataset.name, "rain", "focus stays on the row");
      btn("rain", "up").focus();
      for (let i = 0; i < 12; i++) enter();
      await until(() => settled("rain", 0), 3000);
      await sleep(300);
      t.ok(settled("rain", 0), "12 presses from the bottom end at the top");
    } finally {
      p.close();
    }
  });

  uiTest("ui: Space repeated past the end changes nothing", async (C, env) => {
    const p = await openDemo(env);
    try {
      const names = () => Array.from(p.d.querySelectorAll("#set-rgb-cycle li"), (li) => li.dataset.name);
      /* Space clicks a button and toggles a checkbox: click() on whatever has focus. */
      const space = () => p.d.activeElement.click();
      t.eq(names().indexOf("comet"), 4);
      p.d.querySelector('#set-rgb-cycle li[data-name="comet"] [data-part="up"]').focus();
      for (let i = 0; i < 6; i++) {
        space();
        await sleep(100); /* separate key presses: the keyboard's answer arrives in between */
      }
      await until(() => names()[0] === "comet" && p.sim.values["rgb.cycle"][0] === "comet", 3000);
      await sleep(300);
      t.eq(names()[0], "comet");
      t.eq(p.sim.values["rgb.cycle"], names(), "the keyboard has the order of the page, all 12 effects");
      t.ok(p.d.getElementById("set-rgb-cycle-comet").checked, "comet is still ticked");
      t.eq(p.d.activeElement.dataset.part, "up", "focus stays on Move up");
    } finally {
      p.close();
    }
  });

  uiTest("ui: an update that arrived during a drag shows after dragend", async (C, env) => {
    const p = await openDemo(env);
    try {
      const names = () => Array.from(p.d.querySelectorAll("#set-rgb-cycle li"), (li) => li.dataset.name);
      const info = () => p.sim.requests.filter((r) => r.group === C.SMP.GROUP_CFG && r.cmd === 0).length;
      const li = p.d.querySelector('#set-rgb-cycle li[data-name="comet"]');
      li.dispatchEvent(new p.w.DragEvent("dragstart", { bubbles: true, dataTransfer: new p.w.DataTransfer() }));
      const polls = info();
      p.sim.values["rgb.cycle"] = ["wave", "rain"];
      p.sim.rev++;
      await sleep(1500);
      t.ok(info() > polls, "the page polled during the drag");
      t.eq(names()[0], "solid", "the update waits while dragging");
      li.dispatchEvent(new p.w.DragEvent("dragend", { bubbles: true }));
      await until(() => names().slice(0, 2).join() === "wave,rain");
    } finally {
      p.close();
    }
  });

  uiTest("ui: dropping an unticked effect elsewhere does not snap it back at dragend", async (C, env) => {
    const p = await openDemo(env);
    try {
      p.sim.values["rgb.cycle"] = ["solid", "rainbow"];
      p.sim.rev++;
      await until(() => !p.d.getElementById("set-rgb-cycle-plasma").checked, 3000);
      const names = () => Array.from(p.d.querySelectorAll("#set-rgb-cycle li"), (li) => li.dataset.name);
      const li = (n) => p.d.querySelector(`#set-rgb-cycle li[data-name="${n}"]`);
      const rain = li("rain");
      const dt = new p.w.DataTransfer();
      t.ok(names().indexOf("wave") < names().indexOf("rain"));
      rain.dispatchEvent(new p.w.DragEvent("dragstart", { bubbles: true, dataTransfer: dt }));
      li("wave").dispatchEvent(new p.w.DragEvent("drop", { bubbles: true, cancelable: true, dataTransfer: dt }));
      t.ok(names().indexOf("rain") < names().indexOf("wave"), "dropped before wave");
      rain.dispatchEvent(new p.w.DragEvent("dragend", { bubbles: true }));
      t.ok(names().indexOf("rain") < names().indexOf("wave"), "still there after dragend");
    } finally {
      p.close();
    }
  });

  uiTest("ui: a colour picker that loses focus without change is not left busy", async (C, env) => {
    const p = await openDemo(env);
    try {
      const c = p.d.getElementById("set-ind-caps-color");
      c.value = "#ff8000";
      fire(c, "input");
      await until(() => p.sim.values["ind.caps_color"] === 0xff8000);
      p.sim.values["ind.caps_color"] = 0x00ff00;
      p.sim.rev++;
      await sleep(1500);
      t.eq(c.value, "#ff8000", "the update waits while the picker is open");
      fire(c, "blur");
      await until(() => c.value === "#00ff00");
      t.eq(p.row("ind.caps_color").querySelector("output").textContent, "#00FF00");
    } finally {
      p.close();
    }
  });

  uiTest("ui: reset to defaults asks first", async (C, env) => {
    const p = await openDemo(env);
    try {
      p.sim.values["rgb.val"] = 50;
      p.sim.values["kb.os"] = "mac";
      p.sim.rev++;
      await until(() => p.d.getElementById("set-rgb-val").value === "50", 3000);
      p.w.confirm = () => false;
      p.d.getElementById("btn-reset").click();
      await sleep(100);
      t.eq(p.sim.values["kb.os"], "mac", "not confirmed: nothing reset");
      p.w.confirm = () => true;
      p.d.getElementById("btn-reset").click();
      await until(() => p.d.getElementById("set-rgb-val").value === "200");
      await until(() => !p.d.getElementById("msg").hidden);
      t.eq([p.sim.values["rgb.val"], p.sim.values["kb.os"]], [200, "win"]);
      t.eq(p.d.getElementById("msg").textContent, "All settings are back to their defaults.");
    } finally {
      p.close();
    }
  });

  /* index.html without ?demo, in a frame whose navigator has a fake Web
   * Serial and a Web Bluetooth that knows no keyboard. The page is written
   * into the frame after the fakes are in, so its first look at the browser
   * finds them. opts: ports (the granted FakePorts), getPorts (replaces the
   * default getPorts), requestDevice (the Bluetooth picker). */
  async function openLive(env, opts = {}) {
    const f = document.createElement("iframe");
    env.frames.append(f);
    const w = f.contentWindow;
    const serial = new EventTarget();
    serial.ports = opts.ports || [];
    serial.getPorts = opts.getPorts || (async () => serial.ports.slice());
    serial.requestPort = async () => {
      throw new DOMException("No port selected by the user.", "NotFoundError");
    };
    const bluetooth = {
      getDevices: async () => [],
      requestDevice: opts.requestDevice || (async () => {
        throw new DOMException("User cancelled the requestDevice() chooser.", "NotFoundError");
      }),
    };
    Object.defineProperty(w.navigator, "serial", { value: serial, configurable: true });
    Object.defineProperty(w.navigator, "bluetooth", { value: bluetooth, configurable: true });
    w.document.open();
    w.document.write(env.html);
    w.document.close();
    const d = w.document;
    return {
      w, d, serial,
      state: () => d.getElementById("state").textContent,
      msg: () => (d.getElementById("msg").hidden ? "" : d.getElementById("msg").textContent),
      close: () => f.remove(),
    };
  }

  uiTest("ui: a keyboard plugged in again clears the old alert, and focus follows the page", async (C, env) => {
    const port = new FakePort(C, new C.SimKeyboard());
    const p = await openLive(env, { ports: [port] });
    try {
      await until(() => p.state() === "Connected over USB", 5000);
      t.eq(p.d.activeElement.id, "main", "focus moves into the settings when nothing had it");
      port.unplug();
      await until(() => p.state() === "Connection lost");
      t.eq(p.msg(), "Connection lost. Connect again.");
      t.eq(p.d.activeElement.id, "btn-usb", "focus was in the settings: Connect USB takes it");
      p.serial.ports = [new FakePort(C, new C.SimKeyboard())];
      p.serial.dispatchEvent(new Event("connect"));
      await until(() => p.state() === "Connected over USB", 5000);
      t.eq(p.msg(), "", "the alert of the lost connection is gone");
      p.d.getElementById("btn-disconnect").focus();
      p.d.getElementById("btn-disconnect").click();
      t.eq(p.state(), "Not connected");
      t.eq(p.d.activeElement.id, "btn-usb", "focus was on Disconnect: Connect USB takes it");
    } finally {
      p.close();
    }
  });

  uiTest("ui: a keyboard that appears while the page is looking is found afterwards", async (C, env) => {
    const kb = new FakePort(C, new C.SimKeyboard());
    let first;
    const scan = new Promise((resolve) => {
      first = resolve;
    });
    let scans = 0;
    const p = await openLive(env, { getPorts: async () => (++scans === 1 ? scan : [kb]) });
    try {
      await until(() => scans === 1);
      t.eq(p.state(), "Looking for the keyboard");
      p.serial.dispatchEvent(new Event("connect"));
      first([]);
      await until(() => p.state() === "Connected over USB", 5000);
      t.eq(scans, 2, "one more look, after the first");
    } finally {
      p.close();
    }
  });

  uiTest("ui: no Bluetooth adapter is shown, a closed picker is not", async (C, env) => {
    let error = new DOMException("Bluetooth adapter not available.", "NotFoundError");
    let asked = 0;
    const p = await openLive(env, {
      requestDevice: async () => {
        asked++;
        throw error;
      },
    });
    try {
      await until(() => p.state() === "Not connected");
      const ble = p.d.getElementById("btn-ble");
      ble.click();
      await until(() => asked === 1 && !ble.disabled);
      t.eq(p.msg(), "Bluetooth adapter not available.");
      error = new DOMException("User cancelled the requestDevice() chooser.", "NotFoundError");
      ble.click();
      await until(() => asked === 2 && !ble.disabled);
      t.eq(p.msg(), "", "a closed picker says nothing");
      t.eq(p.state(), "Not connected");
    } finally {
      p.close();
    }
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
