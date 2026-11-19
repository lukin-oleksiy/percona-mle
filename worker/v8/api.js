// The V8 worker's JavaScript side (exp design note 63), compiled into mle_v8_worker: called once
// with the native functions (N), it installs the globals routines see (console; the SQL API comes
// in sql.js) and returns the helpers the C++ core uses. The mechanics (values, rows, the request
// loop, SQL messages) are C++; this is only what is easier to say in JavaScript.
(function (N) {
  'use strict';

  // ---------------------------------------------------------------- console
  // Oracle: console output goes to the session (stdout, stderr); console.clear() clears it.
  // As GraalJS's console: each argument as String(v) (arrays "1,2", errors by their toString()).
  const show = (v) => {
    try {
      return typeof v === 'symbol' ? v.toString() : String(v);
    } catch (e) {
      return Object.prototype.toString.call(v);
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

  // ---------------------------------------------------------------- npm packages
  // The DBA's packages (mle_package: <package_dir>/js/node_modules), as on GraalJS: require() of
  // CommonJS, import of ES modules by package name (modules.cc asks resolvePackage). Node's
  // built-in modules (fs, http, ...) are not there, as on GraalJS. Files are read only below the
  // package directory (N.readFile).
  const ROOT = N.packages;
  const normalize = (path) => {
    const out = [];
    for (const part of path.split('/')) {
      if (part === '' || part === '.') continue;
      if (part === '..') out.pop();
      else out.push(part);
    }
    return '/' + out.join('/');
  };
  const dirname = (path) => path.slice(0, path.lastIndexOf('/')) || '/';
  const readJson = (path) => {
    const t = N.readFile(path);
    if (t === null) return null;
    try { return JSON.parse(t); } catch (e) { return null; }
  };
  const asFile = (path) => {
    for (const p of [path, path + '.js', path + '.json', path + '.cjs', path + '.mjs'])
      if (N.readFile(p) !== null) return p;
    return null;
  };
  // A package.json's entry for require ('require') or import ('import').
  const entryOf = (pkg, condition) => {
    const pick = (e) => {
      if (typeof e === 'string') return e;
      if (e === null || typeof e !== 'object' || Array.isArray(e)) return null;
      for (const k of [condition, 'node', 'default']) if (k in e) { const v = pick(e[k]); if (v !== null) return v; }
      return null;
    };
    if (pkg.exports !== undefined) {
      const e = pkg.exports;
      const root = e !== null && typeof e === 'object' && !Array.isArray(e) && '.' in e ? e['.'] : e;
      const v = pick(root);
      if (v !== null) return v;
    }
    if (condition === 'import' && typeof pkg.module === 'string') return pkg.module;
    return typeof pkg.main === 'string' ? pkg.main : 'index.js';
  };
  const asDir = (dir, condition) => {
    if (!N.isDir(dir)) return null;
    const pkg = readJson(dir + '/package.json');
    if (pkg !== null) {
      const f = asFile(normalize(dir + '/' + entryOf(pkg, condition)));
      if (f !== null) return f;
    }
    return asFile(dir + '/index');
  };
  // GraalJS's error for a module that is not there (its CommonJS loader's words).
  const notFound = (spec) => {
    const e = new TypeError("Cannot load module: '" + spec + "'");
    e.code = 'MODULE_NOT_FOUND';
    return e;
  };
  // An absolute path below ROOT, or throws.
  const resolveFile = (spec, from, condition) => {
    if (!ROOT) throw notFound(spec);
    const fromDir = from ? dirname(from) : ROOT;
    if (spec.startsWith('./') || spec.startsWith('../') || spec.startsWith('/')) {
      const p = normalize(spec.startsWith('/') ? spec : fromDir + '/' + spec);
      const f = asFile(p) || asDir(p, condition);
      if (f !== null) return f;
      throw notFound(spec);
    }
    const parts = spec.split('/');
    const name = spec.startsWith('@') ? parts.slice(0, 2).join('/') : parts[0];
    const sub = parts.slice(spec.startsWith('@') ? 2 : 1).join('/');
    for (let dir = fromDir; dir.startsWith(ROOT); dir = dirname(dir)) {
      const base = dir + '/node_modules/' + name;
      if (N.isDir(base)) {
        const f = sub ? (asFile(base + '/' + sub) || asDir(base + '/' + sub, condition)) : asDir(base, condition);
        if (f !== null) return f;
      }
      if (dir === ROOT) break;
    }
    throw notFound(spec);
  };
  const kindOf = (path) => {
    if (path.endsWith('.json')) return 'json';
    if (path.endsWith('.mjs')) return 'esm';
    if (path.endsWith('.cjs')) return 'cjs';
    for (let dir = dirname(path); dir.startsWith(ROOT); dir = dirname(dir)) {
      const pkg = readJson(dir + '/package.json');
      if (pkg !== null) return pkg.type === 'module' ? 'esm' : 'cjs';
      if (dir === ROOT) break;
    }
    return 'cjs';
  };
  const cache = new Map();
  const load = (path) => {
    const hit = cache.get(path);
    if (hit !== undefined) return hit.exports;
    const text = N.readFile(path);
    if (text === null) throw notFound(path);
    const module = { exports: {}, id: path, filename: path, loaded: false };
    cache.set(path, module);
    try {
      if (path.endsWith('.json')) {
        module.exports = JSON.parse(text);
      } else {
        const fn = N.compileFunction(text, path, ['exports', 'require', 'module', '__filename', '__dirname']);
        fn.call(module.exports, module.exports, requireFrom(path), module, path, dirname(path));
      }
    } catch (e) {
      cache.delete(path);
      throw e;
    }
    module.loaded = true;
    return module.exports;
  };
  const requireFrom = (from) => {
    const r = (spec) => {
      if (typeof spec !== 'string') throw new TypeError('require: the module name must be a string');
      const path = resolveFile(spec, from, 'require');
      if (kindOf(path) === 'esm') throw new Error("require() of the ES module '" + spec + "': use import()");
      return load(path);
    };
    r.resolve = (spec) => resolveFile(String(spec), from, 'require');
    r.cache = cache;
    return r;
  };
  if (ROOT && N.isDir(ROOT + '/node_modules'))
    Object.defineProperty(globalThis, 'require', { value: requireFrom(null), writable: false, configurable: false });

  return {
    // modules.cc: an import of a package name: [path, kind, source].
    resolvePackage: (spec, from) => {
      const path = resolveFile(spec, from || null, 'import');
      const kind = kindOf(path);
      return [path, kind, kind === 'esm' ? N.readFile(path) : null];
    },
    // modules.cc: a CommonJS module imported from an ES module (its default export).
    requirePath: (path) => load(path),
    // mle_eval: the value of the snippet as text (JSON for objects and arrays, else String(v)).
    show: (v) => v === undefined || v === null ? null : typeof v === 'string' ? v
        : typeof v === 'object' ? (() => { try { return JSON.stringify(v); } catch (e) { return String(v); } })()
        : String(v),
    // USING: a library's module namespace, from the C++ loader.
    lib: (s, n) => N.library(s, n),
  };
})
