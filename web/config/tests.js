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
