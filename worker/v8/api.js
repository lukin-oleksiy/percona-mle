// The V8 worker's JavaScript side (exp design note 63), compiled into mle_v8_worker: called once
// with the native functions (N), it installs the globals routines see (console; the SQL API comes
// in sql.js) and returns the helpers the C++ core uses. The mechanics (values, rows, the request
// loop, SQL messages) are C++; this is only what is easier to say in JavaScript.
(function (N) {
  'use strict';

  // ---------------------------------------------------------------- console
  // Oracle: console output goes to the session (stdout, stderr); console.clear() clears it.
  const show = (v) => {
    switch (typeof v) {
      case 'string': return v;
      case 'undefined': return 'undefined';
      case 'bigint': return v.toString() + 'n';
      case 'symbol': return v.toString();
      case 'function': return '[Function ' + (v.name || '(anonymous)') + ']';
      case 'object':
        if (v === null) return 'null';
        if (v instanceof Error) return v.stack || String(v);
        if (v instanceof Date) return isNaN(v.getTime()) ? 'Invalid Date' : v.toISOString();
        try {
          const s = JSON.stringify(v, (k, x) => typeof x === 'bigint' ? x.toString() : x);
          return s === undefined ? String(v) : s;
        } catch (e) {
          return String(v);
        }
      default: return String(v);
    }
  };
  const line = (args) => args.map(show).join(' ') + '\n';
  const counts = new Map();
  const timers = new Map();
  const console = {
    log: (...a) => N.write(1, line(a)),
    info: (...a) => N.write(1, line(a)),
    debug: (...a) => N.write(1, line(a)),
    trace: (...a) => N.write(1, line(a)),
    error: (...a) => N.write(2, line(a)),
    warn: (...a) => N.write(2, line(a)),
    assert: (c, ...a) => { if (!c) N.write(2, line(['Assertion failed' + (a.length ? ':' : ''), ...a])); },
    count: (l = 'default') => {
      const n = (counts.get(l) || 0) + 1;
      counts.set(l, n);
      N.write(1, l + ': ' + n + '\n');
    },
    countReset: (l = 'default') => { counts.delete(l); },
    time: (l = 'default') => { timers.set(l, Date.now()); },
    timeEnd: (l = 'default') => {
      if (timers.has(l)) N.write(1, l + ': ' + (Date.now() - timers.get(l)) + 'ms\n');
      timers.delete(l);
    },
    clear: () => N.clear(),
  };
  Object.defineProperty(globalThis, 'console', { value: Object.freeze(console), writable: false, configurable: false });

  return {
    // mle_eval: the value of the snippet as text (JSON for objects and arrays, else String(v)).
    show: (v) => v === undefined || v === null ? null : typeof v === 'string' ? v
        : typeof v === 'object' ? (() => { try { return JSON.stringify(v); } catch (e) { return String(v); } })()
        : String(v),
    // USING: a library's module namespace, from the C++ loader.
    lib: (s, n) => N.library(s, n),
  };
})
