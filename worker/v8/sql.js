// The SQL API of the V8 worker: Oracle's JavaScript API (session, mysql, SqlError) with the JVM
// worker's behaviour (GuestSql.java, JsSql.java, com.percona.mle.sql; the CPython worker's sql.py
// is the same port in Python). Messages, values, rows and the statement states of the protocol
// (docs/protocol.md §10) are C++ (N.sql, which checks all it is given); this is the API on top.
// Called once with the natives.
(function (N) {
  'use strict';

  const EXECUTE = 0, PREPARE = 1, EXECUTE_PREPARED = 2, FETCH = 3, NEXT_RESULT = 4, CLOSE = 5, EXECUTE_MANY = 6;
  const TO_CLIENT = 1;
  const MAX_VALUES = 65535;
  const freeze = Object.freeze;

  // ---------------------------------------------------------------- errors
  // Oracle: new SqlError(sql_state, message, error_number); thrown, MySQL raises it like SIGNAL.
  // Errors of SQL statements are SqlErrors too (code, sqlState, message).
  class SqlError extends Error {
    constructor(sql_state, message, error_number) {
      super(message === undefined || message === null ? '' : String(message));
      for (const [n, v] of [['sql_state', sql_state], ['error_number', error_number]])
        if (typeof v === 'number' && !Number.isFinite(v))
          throw new TypeError('SqlError: ' + n + ' must be a finite number');
      let state = sql_state === undefined || sql_state === null ? '45000' : String(sql_state);
      if (typeof sql_state === 'number') state = state.padStart(5, '0');
      Object.defineProperty(this, '$mle$sql', { value: true });
      this.sqlState = state;
      this.code = error_number === undefined || error_number === null ? 1644 : Number(error_number);
      this.row = null;  // executeMany: the failing row (exp note 51)
    }
    toString() {
      return 'SqlError: Error code: ' + this.code + ' Error state: ' + this.sqlState + ' Error message: ' + this.message;
    }
  }
  Object.defineProperty(SqlError.prototype, 'name', { value: 'SqlError', writable: true, configurable: true });

  const sqlError = (code, state, message, row) => {
    const e = new SqlError(state, message, code);
    if (row !== undefined && row !== null && row >= 0) e.row = row;
    return e;
  };
  // The error of an SQL_RESULT ERROR t. A routine that ends with this object hands the server's
  // error back by its id (§10.8): N.handBack keeps the id with the error's own number, SQLSTATE
  // and message where routine code cannot change them. An error the worker made itself has id 0.
  const replyError = (t) => {
    const e = sqlError(t[1], t[2], t[3], t[4]);
    if (t[5] !== 0n) N.handBack(e, t[5], t[1], t[2], t[3]);
    return e;
  };
  const bindError = (message, row) => sqlError(1210, 'HY000', message, row);

  // GuestSql's argument rules.
  const arg = (a, i) => (i < a.length && a[i] !== null && a[i] !== undefined ? a[i] : null);
  const str = (a, i) => {
    const v = arg(a, i);
    if (v === null) throw new TypeError('argument ' + (i + 1) + ' is missing');
    return typeof v === 'string' ? v : String(v);
  };
  const isArray = (v) => Array.isArray(v) || (ArrayBuffer.isView(v) && !(v instanceof DataView));
  const elements = (v) => (v === null || v === undefined ? [] : isArray(v) ? Array.from(v) : [v]);
  const rowsOf = (v) => {
    if (v === null || v === undefined || !isArray(v))
      throw new TypeError('executeMany takes a list of rows, each a list of values');
    const rows = Array.from(v);
    rows.forEach((r, i) => {
      if (r === null || r === undefined || !isArray(r))
        throw new TypeError('row ' + i + ' of executeMany is not a list of values');
    });
    return rows;
  };
  const truthy = (v) => {
    if (v === null || v === undefined) return false;
    if (typeof v === 'boolean') return v;
    if (typeof v === 'number') return v !== 0 && !Number.isNaN(v);
    if (typeof v === 'string') return v !== '';
    return true;
  };

  // ---------------------------------------------------------------- options (SqlOptions.java)
  const INTEGER_TYPES = ['UNSAFE_STRING', 'UNSAFE_BIGINT', 'STRING', 'BIGINT'];
  const DECIMAL_TYPES = ['STRING', 'NUMBER'];

  class Options {
    constructor(o = {}) {
      this.passResult = o.passResult ?? null;
      this.charset = o.charset ?? null;
      this.integerType = o.integerType ?? null;
      this.decimalType = o.decimalType ?? null;
      this.batchRewrite = o.batchRewrite ?? null;
    }
    over(base) {
      const b = base || NO_OPTIONS;
      const pick = (k) => (this[k] === null ? b[k] : this[k]);
      return new Options({ passResult: pick('passResult'), charset: pick('charset'), integerType: pick('integerType'),
                           decimalType: pick('decimalType'), batchRewrite: pick('batchRewrite') });
    }
    get toClient() { return this.passResult === true; }
    get rewrite() { return this.batchRewrite !== false; }
    set(name, value) {
      const pick = (v, allowed, what) => {
        const u = String(v).toUpperCase();
        if (!allowed.includes(u))
          throw new TypeError(what + ' must be one of [' + allowed.join(', ') + "], not '" + v + "'");
        return u;
      };
      switch (name) {
        case 'passResultToClient':
          if (typeof value !== 'boolean') throw new TypeError('passResultToClient must be a boolean');
          this.passResult = value;
          break;
        case 'charsetName': {
          const v = String(value);
          if (!['utf8mb4', 'utf8', 'utf8mb3'].includes(v.toLowerCase()))
            throw new TypeError("charsetName: only utf8mb4 is supported, not '" + v + "'");
          this.charset = 'utf8mb4';
          break;
        }
        case 'integerType': this.integerType = pick(value, INTEGER_TYPES, 'integerType'); break;
        case 'decimalType': this.decimalType = pick(value, DECIMAL_TYPES, 'decimalType'); break;
        case 'batchRewrite':
          if (typeof value !== 'boolean') throw new TypeError('batchRewrite must be a boolean');
          this.batchRewrite = value;
          break;
        default: throw new TypeError("unknown option '" + name + "'");
      }
      return this;
    }
    get(name) {
      switch (name) {
        case 'passResultToClient': return this.passResult === true;
        case 'charsetName': return this.charset === null ? 'utf8mb4' : this.charset;
        case 'integerType': return this.integerType;
        case 'decimalType': return this.decimalType;
        case 'batchRewrite': return this.batchRewrite !== false;
      }
      throw new TypeError("unknown option '" + name + "'");
    }
    // The codes N.sql takes.
    get intCode() { return this.integerType === null ? -1 : INTEGER_TYPES.indexOf(this.integerType); }
    get decCode() { return this.decimalType === null ? -1 : DECIMAL_TYPES.indexOf(this.decimalType); }
  }
  const NO_OPTIONS = new Options();

  const optionsOf = (v) => {
    if (v === null || v === undefined) return null;
    if (typeof v !== 'object') throw new TypeError('options must be an object');
    const o = new Options();
    for (const k of Object.keys(v)) {
      const x = v[k];
      if (x === null || x === undefined) continue;
      o.set(k, typeof x === 'boolean' || typeof x === 'string' ? x : String(x));
    }
    return o;
  };

  // ---------------------------------------------------------------- rows
  // GuestSql.GRow: an array of the values (JSON.stringify(row) is the array; its keys are the
  // indexes, then the labels), with the columns by label as properties (a label's first column:
  // SELECT 1 a, 2 a) and getField / getLength / toArray from the prototype. The C++ core sends a
  // reply's values flat (row by row); a factory per label list, compiled once, makes the rows (V8
  // gives all of them one shape). Routine code may replace built-ins in its worker
  // (Array.prototype.join, JSON.parse ...): what makes rows uses only what it took here, at load.
  const objectCreate = Object.create, setProto = Object.setPrototypeOf, defineProp = Object.defineProperty;
  const quote = JSON.stringify, FunctionCtor = Function, arrayFrom = Array.from;
  const RESERVED = objectCreate(null);
  RESERVED.length = RESERVED.__proto__ = RESERVED.getField = RESERVED.getLength = RESERVED.toArray = true;
  // A label that is a plain name (not reserved): a store the compiler knows.
  const plainName = (l) => {
    if (l.length === 0 || RESERVED[l] === true) return false;
    for (let i = 0; i < l.length; i++) {
      const c = l.charCodeAt(i);
      const letter = (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c === 95 || c === 36;
      if (!letter && !(i > 0 && c >= 48 && c <= 57)) return false;
    }
    return true;
  };
  // A label that is an array index ("0", "12"): the row's elements are its values in order.
  const arrayIndex = (l) => {
    if (l.length === 0 || l.length > 10 || (l.length > 1 && l.charCodeAt(0) === 48)) return false;
    for (let i = 0; i < l.length; i++) {
      const c = l.charCodeAt(i);
      if (c < 48 || c > 57) return false;
    }
    return +l < 4294967295;
  };
  const define = (r, k, v) => { defineProp(r, k, { value: v, writable: true, enumerable: true, configurable: true }); };
  let makers = objectCreate(null), makerCount = 0;
  const makerFor = (labels) => {
    const n = labels.length;
    let key = n + ':';  // the count too: a label may hold the separator
    for (let j = 0; j < n; j++) key += (j === 0 ? '' : '\u0000') + labels[j];
    let m = makers[key];
    if (m !== undefined) return m;
    const index = objectCreate(null);  // label -> its first column
    for (let j = n - 1; j >= 0; j--) index[labels[j]] = j;
    const proto = objectCreate(Array.prototype, {
      getField: { value: function (name) {
        if (name === undefined || name === null) throw new TypeError('argument 1 is missing');
        const k = String(name);
        const j = index[k];
        if (j === undefined) throw new TypeError("no column '" + k + "' in the row");
        return this[j];
      } },
      getLength: { value: function () { return this.length; } },
      toArray: { value: function () { return arrayFrom(this); } },
    });
    // (v, i): the row of the values from v[i]. Labels that are not plain names (a prototype's
    // member, "x y") are defined as own properties; "length" and the indexes stay the array's
    // (getField has them).
    let body = 'const r = [';
    for (let j = 0; j < n; j++) body += (j === 0 ? '' : ', ') + 'v[i + ' + j + ']';
    body += '];\nsetProto(r, proto);\n';
    const seen = objectCreate(null);
    for (let j = 0; j < n; j++) {
      const l = labels[j];
      if (seen[l] === true || l === 'length' || arrayIndex(l)) continue;
      seen[l] = true;
      body += plainName(l) ? 'r.' + l + ' = r[' + j + '];\n' : 'define(r, ' + quote(l) + ', r[' + j + ']);\n';
    }
    body += 'return r;';
    const make = new FunctionCtor('setProto', 'proto', 'define', 'return (v, i) => {\n' + body + '\n}')(
      setProto, proto, define);
    m = { n, make };
    if (makerCount >= 1024) { makers = objectCreate(null); makerCount = 0; }
    makers[key] = m; makerCount++;
    return m;
  };
  // The rows of one reply: labels, the values flat.
  const replyRows = (labels, values) => {
    const { n, make } = makerFor(labels);
    const rows = [];
    if (n === 0) return rows;
    for (let k = 0, i = 0; i < values.length; k++, i += n) rows[k] = make(values, i);
    return rows;
  };

  // ---------------------------------------------------------------- columns, warnings
  const ZEROFILL = 64, ENUM = 256, SET = 2048;
  const NUMERIC = new Set(['TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'YEAR', 'BIT', 'FLOAT', 'DOUBLE', 'DECIMAL']);
  const OWN_TYPES = new Set(['TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'BIT', 'FLOAT', 'DOUBLE', 'DECIMAL',
                             'JSON', 'TIME', 'DATE', 'DATETIME', 'TIMESTAMP', 'SET', 'ENUM', 'GEOMETRY', 'VECTOR']);
  const TAG_NAMES = ['NULL', 'BIGINT', 'BIGINT UNSIGNED', 'DOUBLE', 'FLOAT', 'DECIMAL', 'string', 'binary', 'DATE',
                     'TIME', 'DATETIME', 'TIMESTAMP', 'JSON', 'VECTOR'];

  class Column {
    // c: [name, table, tag, unsigned, length, precision, scale, typeName, orgName, schema, orgTable,
    //     charset, collation, flags] (N.sql)
    constructor(c) { Object.defineProperty(this, '$c', { value: c }); }
    get $sql() { return this.$c[7] || TAG_NAMES[this.$c[2]] || '?'; }
    getColumnName() { return this.$c[8] || this.$c[0]; }
    getColumnLabel() { return this.$c[0]; }
    getSchemaName() { return this.$c[9]; }
    getTableName() { return this.$c[10]; }
    getTableLabel() { return this.$c[1]; }
    getType() {
      const sql = this.$sql, flags = this.$c[13];
      if (flags & ENUM) return 'ENUM';
      if (flags & SET) return 'SET';
      if (OWN_TYPES.has(sql)) return sql;
      if (sql === 'YEAR') return 'INT';
      return this.$c[11] === 'binary' ? 'BYTES' : 'STRING';
    }
    getLength() { return this.$c[4]; }
    getFractionalDigits() { return this.$c[6]; }
    getCharacterSetName() { return this.$c[11]; }
    getCollationName() { return this.$c[12]; }
    isNumberSigned() { return NUMERIC.has(this.$sql) && !this.$c[3]; }
    isPadded() { return (this.$c[13] & ZEROFILL) !== 0 || this.$sql === 'CHAR'; }
    toString() { return this.$c[0]; }
  }

  // mysqld's warning levels count from 0 (note, warning, error); Oracle's from 1.
  const warningsOf = (ws) => (ws && ws.length ? ws.map(([l, c, m]) => freeze({ level: l + 1, code: c, message: m })) : []);

  // ---------------------------------------------------------------- the requests
  // [depth, scope, routine schema, integer type, decimal type] of the request being served.
  const state = () => N.state();

  // One SQL message and its reply. A statement is known by the scope of the request that opened
  // it and its id there; EXECUTE and PREPARE open one, and the reply says which. The reply:
  // [0, code, state, message, row, errorId] (thrown as a SqlError) |
  // [1, affected, insertId, warnings, moreResults, paramCount, execUs, scope, id] |
  // [2, labels, columns, total, rows, more, moreResults, warnings, execUs, scope, id]: one batch of
  //    a result set; labels and columns only in the set's first batch (later: null, and the rows
  //    are made with the labels given here); total null if the host does not know it |
  // [3]: no further result (NEXT_RESULT).
  const send = (action, scope, id, text, params, options, flags = 0, width = 0, labels = null) => {
    const o = options || NO_OPTIONS;
    // -1: the session's integerType / decimalType (OPTIONS).
    const t = N.sql(action, scope, id, text, params, flags, width, o.intCode, o.decCode);
    if (t[0] === 0) throw replyError(t);
    if (t[0] === 2) t[4] = replyRows(t[1] !== null ? t[1] : labels, t[4]);
    return t;
  };
  // The statement a reply opened or answered: its scope and id are the reply's last two items.
  const scopeOf = (t) => t[t.length - 2];
  const idOf = (t) => t[t.length - 1];

  // ---------------------------------------------------------------- results
  const REPLACED = 'the result was replaced by a new execution of its prepared statement';
  // A batch of rows onto the rows read so far, in place (concat would copy them all per batch); the
  // rows from at. Index stores, not push or slice, which routine code may have replaced.
  const appendRows = (rows, batch) => {
    for (let i = 0; i < batch.length; i++) rows[rows.length] = batch[i];
  };
  const rowsFrom = (rows, at) => {
    const out = [];
    for (let i = at; i < rows.length; i++) out[out.length] = rows[i];
    return out;
  };

  // A result's own state: one slot under a symbol (not in its keys or JSON; a plain store, where
  // defineProperties per statement cost as much as the rest of the statement's JavaScript).
  const RESULT = Symbol('result');
  class SqlResult {
    constructor(scope, sid, owned, options, reply) {
      this[RESULT] = { scope, id: sid, owned, options, s: { replaced: false, labels: [], columns: [], warnings: [] } };
      this.$load(reply);
    }
    get $scope() { return this[RESULT].scope; }
    get $id() { return this[RESULT].id; }
    get $owned() { return this[RESULT].owned; }
    get $options() { return this[RESULT].options; }
    get $s() { return this[RESULT].s; }
    // A new result of the statement: the first batch of a result set, a row count, or the end.
    $load(t) {
      const s = this.$s;
      s.rows = []; s.at = 0;
      if (t[0] === 2) {
        s.hasData = true; s.labels = t[1]; s.columns = t[2]; s.rows = t[4];
        s.more = t[5]; s.moreResults = t[6]; s.warnings = warningsOf(t[7]); s.execUs = t[8];
        s.affected = 0; s.insertId = 0;
      } else if (t[0] === 3) {
        s.hasData = false; s.labels = []; s.columns = []; s.more = false; s.moreResults = false;
        s.affected = 0; s.insertId = 0; s.warnings = []; s.execUs = 0;
      } else {
        s.hasData = false; s.labels = []; s.columns = []; s.more = false;
        s.affected = t[1]; s.insertId = t[2]; s.warnings = warningsOf(t[3]); s.moreResults = t[4]; s.execUs = t[6];
      }
    }
    // The next batch of the result set (§10.3). Its warnings: a list in a batch that is not the
    // last is empty ("nothing new") or the complete list so far; the last batch's list is complete.
    $fetch() {
      const s = this.$s;
      const t = send(FETCH, this.$scope, this.$id, null, null, this.$options, 0, 0, s.labels);
      s.more = t[5];
      s.moreResults = t[6];
      const ws = warningsOf(t[7]);
      if (!s.more || ws.length) s.warnings = ws;
      return t[4];
    }
    $replace() {
      const s = this.$s;
      s.replaced = true; s.rows = []; s.at = 0; s.more = false; s.moreResults = false;
    }
    $live() { if (this.$s.replaced) throw new Error(REPLACED); }

    *[Symbol.iterator]() {
      for (let r = this.fetchOne(); r !== null; r = this.fetchOne()) yield r;
    }
    fetchOne() {
      this.$live();
      const s = this.$s;
      while (s.at >= s.rows.length) {
        if (!s.more) return null;
        s.rows = this.$fetch(); s.at = 0;
      }
      return s.rows[s.at++];
    }
    fetchAll() {
      this.$live();
      const s = this.$s;
      const rows = s.at === 0 ? s.rows : rowsFrom(s.rows, s.at);
      s.rows = []; s.at = 0;
      while (s.more) appendRows(rows, this.$fetch());
      return rows;
    }
    hasData() { return this.$s.hasData; }
    nextResult() {
      this.$live();
      const s = this.$s;
      if (!s.moreResults && !s.more) {
        s.hasData = false; s.rows = []; s.at = 0;
        return false;
      }
      this.$load(send(NEXT_RESULT, this.$scope, this.$id, null, null, this.$options));
      return s.hasData;
    }
    getColumns() { return this.$s.columns.map((c) => new Column(c)); }
    getColumnNames() { return this.$s.labels.slice(); }
    getColumnCount() { return this.$s.labels.length; }
    getAffectedItemsCount() { return this.$s.affected; }
    getAutoIncrementValue() { return this.$s.insertId; }
    getWarnings() { return this.$s.warnings.slice(); }
    getWarningsCount() { return this.$s.warnings.length; }
    getExecutionTime() { return (this.$s.execUs / 1e6).toFixed(4) + ' sec'; }
    close() {
      const s = this.$s;
      if (this.$owned && (s.more || s.moreResults) && !s.replaced)
        send(CLOSE, this.$scope, this.$id, null, null, this.$options);
      s.rows = []; s.at = 0; s.more = false; s.moreResults = false; s.hasData = false;
    }
    fetchColumns(...names) {
      const labels = this.$s.labels;
      let want = [];
      if (names.length === 0 || (names.length === 1 && (names[0] === null || names[0] === undefined))) {
        want = labels.slice();
      } else {
        for (const v of names) {
          if (isArray(v)) want.push(...Array.from(v, String));
          else want.push(String(v));
        }
      }
      for (const n of want)
        if (!labels.includes(n)) throw new TypeError("no column '" + n + "' in the result");
      const rows = this.fetchAll();
      const out = {};
      for (const n of want) out[n] = rows.map((r) => r[n]);
      return out;
    }
  }

  // ---------------------------------------------------------------- statements
  class SqlExecute {
    constructor(session, sql, options) {
      Object.defineProperties(this, { $session: { value: session }, $sql: { value: sql }, $options: { value: options } });
    }
    execute() { return run(this.$sql, null, this.$options); }
    getOption(...a) { return this.$options.get(str(a, 0)); }
  }

  // PreparedStatement.java: executeMany's limits (exp notes 51, 52).
  const BATCH_MIN_ROWS = 256, REWRITE_MIN_ROWS = 16, REWRITE_MAX_ROWS = 1000;
  // Errors a row's values cause: only after these a failed chunk is replayed row by row.
  const REPLAY = new Set([1022, 1048, 1062, 1169, 1216, 1264, 1265, 1292, 1364, 1365, 1366, 1367, 1406, 1411, 1452,
                          1526, 1586, 1690, 3140, 3141, 3156, 3819, 6136, 6137, 6138]);
  const checkArgs = (values) => {
    for (const v of values)
      if (v instanceof Argument) throw new TypeError('mysql.arg() values are for OUT and INOUT parameters of procedures');
  };

  class PreparedStatement {
    // bound: the values of the last execution. The host keeps no values between executions
    // (§10.2): every EXECUTE_PREPARED carries all of them, and a later execution that binds fewer
    // takes the others from here.
    constructor(scope, sid, sql, options, count) {
      Object.defineProperty(this, '$s', { value: {
        scope, id: sid, sql, options, count, pending: [], bound: null, executed: false, deallocated: false,
        last: null, formChecked: false, form: null, chunks: new Map(),
      } });
    }
    $open() { if (this.$s.deallocated) throw new Error('the prepared statement was deallocated'); }
    $dropLast() {
      const s = this.$s;
      if (s.last !== null) { s.last.$replace(); s.last = null; }
    }
    bind(...values) {
      this.$open();
      const s = this.$s;
      const n = s.pending.length + values.length;
      if (n > s.count)
        throw bindError('Incorrect arguments to bind: ' + n + ' values for ' + s.count + ' parameter markers');
      s.pending.push(...values);
      return this;
    }
    execute() {
      this.$open();
      const s = this.$s;
      if (!s.executed && s.pending.length < s.count)
        throw bindError('Incorrect arguments to EXECUTE: ' + s.pending.length + ' of ' + s.count + ' parameter markers bound');
      this.$dropLast();
      checkArgs(s.pending);
      const params = s.pending;
      if (s.executed && s.bound !== null)
        for (let i = params.length; i < s.count && i < s.bound.length; i++) params[i] = s.bound[i];
      s.pending = [];
      const t = send(EXECUTE_PREPARED, s.scope, s.id, null, params, s.options);
      s.executed = true;
      s.bound = params;
      if (s.options.toClient) return null;
      s.last = new SqlResult(s.scope, s.id, false, s.options, t);
      return s.last;
    }
    executeMany(rows) { return this.$executeMany(rowsOf(rows)); }
    executemany(rows) { return this.executeMany(rows); }
    $executeMany(rows) {
      this.$open();
      const s = this.$s;
      if (s.options.toClient)
        throw bindError('Incorrect arguments to executeMany: it cannot send results to the client (passResultToClient)');
      if (s.count === 0 && rows.length)
        throw bindError('Incorrect arguments to executeMany: the statement has no parameter markers');
      if (rows.length >= BATCH_MIN_ROWS && s.options.rewrite) {
        if (!s.formChecked) { s.formChecked = true; s.form = batchForm(s.sql, s.count); }
        const f = s.form;
        if (f !== null && innodb(f.schema, f.table, false)) return this.$executeRewritten(rows, f);
      }
      return this.$result(...this.$executeRows(rows, 0));
    }
    $result(affected, execUs, warnings) {
      const s = this.$s;
      return new SqlResult(s.scope, s.id, false, s.options, [1, affected, 0, warnings, false, 0, execUs, s.scope, s.id]);
    }
    // The rows in chunks of the multi-row form (exp note 52); a chunk that fails because of a row's
    // values is replayed row by row, which stops at the failing row with its index.
    $executeRewritten(rows, f) {
      const s = this.$s;
      this.$dropLast();
      s.pending = [];
      const pc = s.count;
      const maxRows = Math.min(REWRITE_MAX_ROWS, Math.floor(MAX_VALUES / pc));
      s.executed = false;  // the chunks run on statements of their own
      s.bound = null;
      let affected = 0, execUs = 0, warnings = [];
      for (let i = 0; i < rows.length;) {
        const start = i;
        let k = 0, bad = null;
        const params = [];
        while (i < rows.length && k < maxRows) {
          const row = Array.from(rows[i]);
          checkArgs(row);
          if (row.length !== pc) {
            bad = bindError('Incorrect arguments to executeMany: row ' + i + ' has ' + row.length + ' values for ' + pc +
                            ' parameter markers', i);
            break;
          }
          params.push(...row);
          k++; i++;
        }
        if (k < REWRITE_MIN_ROWS) {
          if (k > 0) {
            const r = this.$executeRows(rows.slice(start, start + k), start);
            affected += r[0]; execUs += r[1]; warnings = r[2];
          }
          if (bad !== null) throw bad;
          continue;
        }
        try {
          const t = this.$chunk(f, k).$executeMapped(params);
          affected += Number(t[1]); execUs += t[6]; warnings = t[3];
        } catch (e) {
          if (!(e instanceof SqlError)) throw e;
          const why = !REPLAY.has(e.code) ? "the error is not one of a row's values" : replayable(f.schema, f.table);
          if (why !== null)
            throw sqlError(e.code, e.sqlState, e.message + ' (executeMany: rows ' + start + ' to ' + (start + k - 1) +
                           ' ran as one statement and are not run again row by row, so the failing row is not known: ' +
                           why + ')', start);
          const r = this.$executeRows(rows.slice(start, start + k), start);
          affected += r[0]; execUs += r[1]; warnings = r[2];
        }
        if (bad !== null) throw bad;
      }
      return this.$result(affected, execUs, warningsOf(warnings).length ? warnings : []);
    }
    $chunk(f, k) {
      const chunks = this.$s.chunks;
      let p = chunks.get(k);
      if (p !== undefined) { chunks.delete(k); chunks.set(k, p); return p; }
      p = prepare(f.statement(k), new Options({ passResult: false }).over(this.$s.options));
      chunks.set(k, p);
      if (chunks.size > 4) {
        const [oldK, old] = chunks.entries().next().value;
        chunks.delete(oldK);
        old.deallocate();
      }
      return p;
    }
    $executeMapped(params) {
      this.$open();
      const s = this.$s;
      this.$dropLast();
      s.pending = [];
      const t = send(EXECUTE_PREPARED, s.scope, s.id, null, params, s.options);
      s.executed = true;
      s.bound = null;  // a chunk's statement is never executed with fewer values
      return t;
    }
    // execute_many row by row on mysqld's side (EXECUTE_MANY) in messages of up to MAX_VALUES values.
    // An ERROR to EXECUTE_MANY says how many rows of its message ran before it (§10.2): the error's
    // row is that count from the first row of the whole batch.
    $executeRows(rows, offset) {
      this.$open();
      const s = this.$s;
      this.$dropLast();
      s.pending = [];
      const pc = s.count;
      let affected = 0, execUs = 0, done = offset, warnings = [];
      let bad = null, lastRow = null;
      const perMessage = Math.max(1, Math.min(Math.floor(MAX_VALUES / Math.max(pc, 1)), 4096));
      for (let i = 0; i < rows.length && bad === null;) {
        const params = [];
        let n = 0;
        while (i < rows.length && n < perMessage) {
          const row = Array.from(rows[i]);
          checkArgs(row);
          if (row.length !== pc) {
            bad = bindError('Incorrect arguments to executeMany: row ' + (offset + i) + ' has ' + row.length +
                            ' values for ' + pc + ' parameter markers', offset + i);
            break;
          }
          params.push(...row);
          lastRow = row;
          n++; i++;
        }
        if (n === 0) break;
        let t;
        try {
          t = send(EXECUTE_MANY, s.scope, s.id, null, params, s.options, 0, pc);
        } catch (e) {
          if (e instanceof SqlError && e.row !== null) e.row += done;
          throw e;
        }
        affected += Number(t[1]); execUs += t[6]; warnings = t[3];
        done += n;
      }
      // Rows ran on this statement: its last row stays bound.
      if (done > offset) { s.executed = true; s.bound = lastRow; }
      if (bad !== null) throw bad;
      return [affected, execUs, warnings];
    }
    deallocate() {
      const s = this.$s;
      if (s.deallocated) return;
      s.deallocated = true;
      for (const p of s.chunks.values()) p.deallocate();
      s.chunks.clear();
      if (s.last !== null) s.last.$replace();
      send(CLOSE, s.scope, s.id, null, null, s.options);
    }
    getParameterCount() { return this.$s.count; }
    getOption(...a) { return this.$s.options.get(str(a, 0)); }
  }

  // ---------------------------------------------------------------- batch rewrite (BatchRewrite.java)
  // An INSERT or REPLACE of exactly one row of values (markers, NULL, DEFAULT, TRUE, FALSE,
  // literals) and no marker outside it, so that the row can be repeated; null otherwise.
  const batchForm = (sql, count) => {
    if (sql === null || count <= 0 || sql.includes('\\')) return null;
    const t = [];
    const n = sql.length;
    const space = (c) => /\s/.test(c);
    const control = (c) => { const o = c.charCodeAt(0); return o <= 0x1f || (o >= 0x7f && o <= 0x9f); };
    const digit = (c) => c >= '0' && c <= '9';
    const word = (c) => /[\p{L}\p{N}_$]/u.test(c) || c.charCodeAt(0) > 127;
    for (let i = 0; i < n;) {
      const c = sql[i];
      if (space(c)) { i++; continue; }
      if (c === '#' || (c === '-' && sql[i + 1] === '-' && (i + 2 === n || space(sql[i + 2]) || control(sql[i + 2])))) {
        const e = sql.indexOf('\n', i);
        i = e < 0 ? n : e + 1;
        continue;
      }
      if (c === '/' && sql[i + 1] === '*') {
        if (sql[i + 2] === '!') return null;
        const e = sql.indexOf('*/', i + 2);
        if (e < 0) return null;
        i = e + 2;
        continue;
      }
      if (c === "'" || c === '"' || c === '`') {
        let j = i + 1;
        for (;;) {
          if (j >= n) return null;
          if (sql[j] === c) { if (sql[j + 1] === c) { j += 2; continue; } break; }
          j++;
        }
        t.push({ k: c === '`' ? 'i' : 's', start: i, end: j + 1, text: sql.slice(i, j + 1) });
        i = j + 1;
        continue;
      }
      if (digit(c) || (c === '.' && i + 1 < n && digit(sql[i + 1]))) {
        let j = i + 1;
        while (j < n && (/[\p{L}\p{N}.]/u.test(sql[j]) || ((sql[j] === '+' || sql[j] === '-') && /[eE]/.test(sql[j - 1])))) j++;
        t.push({ k: 'n', start: i, end: j, text: sql.slice(i, j) });
        i = j;
        continue;
      }
      if (/\p{L}/u.test(c) || c === '_' || c === '$' || c.charCodeAt(0) > 127) {
        let j = i + 1;
        while (j < n && word(sql[j])) j++;
        t.push({ k: 'w', start: i, end: j, text: sql.slice(i, j) });
        i = j;
        continue;
      }
      t.push({ k: c === '?' ? '?' : 'p', start: i, end: i + 1, text: c });
      i++;
    }
    const END = { k: 'e', start: 0, end: 0, text: ' ' };
    const at = (p) => (p < t.length ? t[p] : END);
    const isWord = (x, ...ws) => x.k === 'w' && ws.includes(x.text.toUpperCase());
    const isPunct = (x, c) => x.k === 'p' && x.text === c;
    const ident = (x) => (x.k === 'w' ? x.text : x.k === 'i' ? x.text.slice(1, -1).replace(/``/g, '`') : null);
    const skipParens = (p) => {
      if (!isPunct(at(p), '(')) return -1;
      let depth = 0;
      for (; p < t.length; p++) {
        const x = t[p];
        if (isPunct(x, '(')) depth++;
        else if (isPunct(x, ')')) { if (--depth === 0) return p + 1; }
        else if (x.k === '?') return -1;
      }
      return -1;
    };
    let p = 0;
    const insert = isWord(at(p), 'INSERT'), replace = isWord(at(p), 'REPLACE');
    if (!insert && !replace) return null;
    p++;
    while (isWord(at(p), 'LOW_PRIORITY', 'HIGH_PRIORITY', 'DELAYED', 'IGNORE')) {
      if (replace && isWord(at(p), 'HIGH_PRIORITY', 'IGNORE')) return null;
      p++;
    }
    if (isWord(at(p), 'INTO')) p++;
    let schema = null, table = ident(at(p));
    if (table === null) return null;
    p++;
    if (isPunct(at(p), '.')) {
      schema = table;
      table = ident(at(p + 1));
      if (table === null) return null;
      p += 2;
    }
    if (isWord(at(p), 'PARTITION')) { p = skipParens(p + 1); if (p < 0) return null; }
    if (isPunct(at(p), '(')) { p = skipParens(p); if (p < 0) return null; }
    if (!isWord(at(p), 'VALUES', 'VALUE')) return null;
    p++;
    if (!isPunct(at(p), '(')) return null;
    const rowStart = at(p).start;
    p++;
    let markers = 0;
    for (;;) {
      const x = at(p);
      if (x.k === '?') markers++;
      else if (isPunct(x, '-') && at(p + 1).k === 'n') p++;
      else if (!(x.k === 'n' || x.k === 's' || isWord(x, 'NULL', 'DEFAULT', 'TRUE', 'FALSE'))) return null;
      p++;
      if (isPunct(at(p), ',')) { p++; continue; }
      if (isPunct(at(p), ')')) break;
      return null;
    }
    const rowEnd = at(p).end;
    p++;
    if (isPunct(at(p), ',')) return null;
    if (p < t.length) {
      if (replace || !isWord(at(p), 'AS', 'ON')) return null;
      for (let j = p; j < t.length; j++) if (t[j].k === '?' || isPunct(t[j], ';')) return null;
    }
    if (markers !== count) return null;
    const head = sql.slice(0, rowStart), row = sql.slice(rowStart, rowEnd), tail = sql.slice(rowEnd);
    return { schema, table, statement: (k) => head + new Array(k).fill(row).join(', ') + tail };
  };

  // ---------------------------------------------------------------- session internals
  const quoteName = (name) => '`' + String(name).replace(/`/g, '``') + '`';
  const callOptions = new Map();  // depth -> [scope, options] (independent review C15)
  const callScopeOptions = (st = state()) => {
    const e = callOptions.get(st[0]);
    return e !== undefined && e[0] === st[1] ? e[1] : null;
  };
  // A statement's options: its own, over the routine call's (setOptions), over the defaults. Oracle's
  // setOptions: "Options not specified assume their default values"; the integerType and
  // decimalType defaults are the session's (mle_set_session_state), so getOption gives them too.
  const effective = (o) => {
    const st = state();
    const defaults = new Options({ integerType: INTEGER_TYPES[st[3]] ?? null, decimalType: DECIMAL_TYPES[st[4]] ?? null });
    const base = (callScopeOptions(st) ?? NO_OPTIONS).over(defaults);
    return o === null || o === undefined ? base : o.over(base);
  };
  const internal = () => effective(new Options({ passResult: false }));

  // PASS_RESULT_TO_CLIENT is given where a statement is opened, at EXECUTE or PREPARE, and holds
  // for the life of its id (§10.2).
  const run = (sql, params, o) => {
    if (params) checkArgs(params);
    const t = send(EXECUTE, 0, 0, sql, params, o, o.toClient ? TO_CLIENT : 0);
    return o.toClient ? null : new SqlResult(scopeOf(t), idOf(t), true, o, t);
  };
  const exec = (sql, params = null) => {
    const t = send(EXECUTE, 0, 0, sql, params, internal());
    if ((t[0] === 2 && t[5]) || (t[0] === 2 ? t[6] : t[4])) send(CLOSE, scopeOf(t), idOf(t), null, null, null);
  };
  const column = (sql, ...params) => {
    const o = internal();
    const first = send(EXECUTE, 0, 0, sql, params, o);
    const values = [];
    let t = first;
    for (;;) {
      if (t[0] === 2) for (const r of t[4]) values.push(r[0]);
      if (t[0] !== 2 || !t[5]) break;
      t = send(FETCH, scopeOf(first), idOf(first), null, null, o, 0, 0, first[1]);
    }
    if (t[0] === 2 ? t[6] : t[4]) send(CLOSE, scopeOf(first), idOf(first), null, null, null);
    return values;
  };
  const prepare = (sql, options) => {
    const o = effective(options);
    const t = send(PREPARE, 0, 0, sql, null, o, o.toClient ? TO_CLIENT : 0);
    return new PreparedStatement(scopeOf(t), idOf(t), sql, o, t[5]);
  };
  const innodbCache = new Map();
  const innodb = (schema, table, fresh) => {
    const s = schema !== null ? schema : state()[2];
    const key = s === null ? null : s + '\u0000' + table;
    let b = key === null || fresh ? undefined : innodbCache.get(key);
    if (b === undefined) {
      const e = column("SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA = COALESCE(?, DATABASE()) AND " +
                       "TABLE_NAME = ? AND TABLE_TYPE = 'BASE TABLE'", schema, table);
      b = e.length === 1 && String(e[0]).toLowerCase() === 'innodb';
      if (key !== null) innodbCache.set(key, b);
    }
    return b;
  };
  const replayable = (schema, table) => {
    const name = (schema === null ? '' : quoteName(schema) + '.') + quoteName(table);
    try {
      const t = send(EXECUTE, 0, 0, 'SHOW CREATE TABLE ' + name, null, internal());
      const rows = t[0] === 2 ? t[4] : [];
      if (t[0] === 2 && (t[5] || t[6])) send(CLOSE, scopeOf(t), idOf(t), null, null, null);
      if (!rows.length || String(rows[0][1]).startsWith('CREATE TEMPORARY')) return 'the table is a temporary one';
    } catch (e) {
      if (e instanceof SqlError) return "the table's definition cannot be read";
      throw e;
    }
    if (!innodb(schema, table, true)) return 'the table is not an InnoDB table';
    const n = column('SELECT COUNT(*) FROM information_schema.TRIGGERS WHERE EVENT_OBJECT_SCHEMA = COALESCE(?, DATABASE()) ' +
                     'AND EVENT_OBJECT_TABLE = ?', schema, table);
    if (n.length !== 1 || Number(n[0]) !== 0) return 'the table has triggers';
    return null;
  };

  // ---------------------------------------------------------------- schemas, routines
  class Schema {
    constructor(name) { Object.defineProperty(this, '$name', { value: name }); }
    getName() { return this.$name; }
    getSession() { return session; }
    existsInDatabase() { return column('SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?', this.$name).length > 0; }
    getTable(...a) { return new Table(this, str(a, 0)); }
    getTables() {
      return column('SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? ORDER BY TABLE_NAME',
                    this.$name).map((t) => new Table(this, t));
    }
    getFunction(...a) { return routine(this.$name, str(a, 0), false); }
    getProcedure(...a) { return routine(this.$name, str(a, 0), true); }
    toString() { return this.$name; }
  }
  class Table {
    constructor(schema, name) { Object.defineProperties(this, { $schema: { value: schema }, $name: { value: name } }); }
    getName() { return this.$name; }
    getSchema() { return this.$schema; }
    getSession() { return session; }
    existsInDatabase() {
      return column('SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ?',
                    this.$schema.$name, this.$name).length > 0;
    }
    isView() {
      return column("SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = ? AND TABLE_NAME = ? AND TABLE_TYPE = 'VIEW'",
                    this.$schema.$name, this.$name).length > 0;
    }
    count() { return Number(column('SELECT COUNT(*) FROM ' + quoteName(this.$schema.$name) + '.' + quoteName(this.$name))[0]); }
    toString() { return this.$schema.$name + '.' + this.$name; }
  }
  class Argument {
    constructor(val) { this.val = val === undefined ? null : val; }
  }
  // A stored routine as a function (Routine.java): SELECT f(?, ...) returns the value; CALL p(...)
  // drops its result sets, and its OUT and INOUT arguments are mysql.arg()s.
  const routine = (schema, name, procedure) => {
    const kind = procedure ? 'PROCEDURE' : 'FUNCTION';
    if (!column('SELECT 1 FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = ? AND ROUTINE_NAME = ? AND ROUTINE_TYPE = ?',
                schema, name, kind).length)
      throw sqlError(1305, '42000', kind + ' ' + schema + '.' + name + ' does not exist');
    let closed = false;
    const qualified = quoteName(schema) + '.' + quoteName(name);
    const f = function (...args) {
      if (closed) throw new Error((procedure ? 'procedure ' : 'function ') + name + ' was closed');
      if (!procedure) {
        const r = run('SELECT ' + qualified + '(' + args.map(() => '?').join(', ') + ')', args, internal());
        const v = r.fetchOne()[0];
        r.close();
        return v;
      }
      const marks = [], params = [], outs = [], sets = [], gets = [], initial = [];
      for (const a of args) {
        if (a instanceof Argument) {
          const v = '@_mle_o' + (outs.length + 1);
          marks.push(v); sets.push(v + ' = ?'); gets.push(v); initial.push(a.val); outs.push(a);
        } else {
          marks.push('?'); params.push(a);
        }
      }
      if (outs.length) exec('SET ' + sets.join(', '), initial);
      const r = run('CALL ' + qualified + '(' + marks.join(', ') + ')', params, effective(null));
      if (r !== null) r.close();
      if (outs.length) {
        const row = run('SELECT ' + gets.join(', '), null, internal()).fetchOne();
        outs.forEach((a, i) => { a.val = row[i]; });
      }
      return null;
    };
    Object.defineProperty(f, 'close', { value: () => { if (closed) throw new Error(kind.toLowerCase() + ' ' + name + ' was closed'); closed = true; } });
    return f;
  };

  // ---------------------------------------------------------------- mysql.* (Functions.java)
  const uuidState = { last: 0n, node: null, seq: 0 };
  const randomBits = (n) => {
    let v = 0n;
    for (let i = 0; i < n; i += 16) v = (v << 16n) | BigInt(Math.floor(Math.random() * 65536));
    return v & ((1n << BigInt(n)) - 1n);
  };
  const uuid = () => {
    if (uuidState.node === null) {
      uuidState.node = randomBits(48) | 0x010000000000n;  // multicast bit
      uuidState.seq = Number(randomBits(14));
    }
    let t = BigInt(Date.now()) * 10000n + 0x01B21DD213814000n;
    if (t <= uuidState.last) t = uuidState.last + 1n;
    uuidState.last = t;
    const hi = ((t & 0xFFFFFFFFn) << 32n) | (((t >> 32n) & 0xFFFFn) << 16n) | 0x1000n | ((t >> 48n) & 0x0FFFn);
    const lo = ((0x8000n | BigInt(uuidState.seq)) << 48n) | uuidState.node;
    const h = hi.toString(16).padStart(16, '0') + lo.toString(16).padStart(16, '0');
    return h.slice(0, 8) + '-' + h.slice(8, 12) + '-' + h.slice(12, 16) + '-' + h.slice(16, 20) + '-' + h.slice(20);
  };
  const UUID = /^(?:[0-9a-fA-F]{32}|[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}|\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\})$/;
  const types = {};
  for (const t of ['TINYINT', 'SMALLINT', 'MEDIUMINT', 'INT', 'BIGINT', 'BIT', 'FLOAT', 'DOUBLE', 'DECIMAL', 'JSON', 'STRING',
                   'BYTES', 'TIME', 'DATE', 'DATETIME', 'TIMESTAMP', 'SET', 'ENUM', 'GEOMETRY', 'VECTOR'])
    types[t] = t;
  const mysql = freeze({
    rand: () => Math.random(),
    // SLEEP(): 0; a KILL meanwhile stops the routine (TerminateExecution reaches the loop).
    sleep: (s) => {
      const seconds = Number(s);
      if (!(seconds > 0)) return 0;
      // In naps of at most 10 ms, back in JavaScript between them: a KILL stops the loop there.
      const end = Date.now() + Math.min(seconds, 1e12) * 1000;
      for (let left = end - Date.now(); left > 0; left = end - Date.now()) N.nap(Math.min(left, 10));
      return 0;
    },
    uuid,
    isUUID: (v) => (v === undefined || v === null ? null : UUID.test(String(v)) ? 1 : 0),
    arg: (...v) => new Argument(v.length ? v[0] : null),
    IntegerType: freeze({ BIGINT: 'BIGINT', STRING: 'STRING', UNSAFE_BIGINT: 'UNSAFE_BIGINT', UNSAFE_STRING: 'UNSAFE_STRING' }),
    DecimalType: freeze({ STRING: 'STRING', NUMBER: 'NUMBER' }),
    Type: freeze(types),
    SQLError: SqlError,
    SqlError,
  });

  // ---------------------------------------------------------------- the session
  const savepointName = (n) => {
    if (!n) throw new TypeError('the savepoint name must not be empty');
    return n;
  };
  let savepoints = 0;
  const methods = {
    sql: (...a) => new SqlExecute(session, str(a, 0), effective(optionsOf(arg(a, 1)))),
    runSql: (...a) => {
      const query = str(a, 0);
      const second = arg(a, 1);
      const params = second !== null && isArray(second);
      const o = optionsOf(params || (second === null && a.length > 2) ? arg(a, 2) : second);
      return run(query, params ? elements(second) : null, effective(o));
    },
    prepare: (...a) => prepare(str(a, 0), optionsOf(arg(a, 1))),
    executeMany: (...a) => {
      const sql = str(a, 0);
      const rows = rowsOf(arg(a, 1));
      const p = prepare(sql, new Options({ passResult: false }).over(optionsOf(arg(a, 2))));
      try {
        return p.$executeMany(rows);
      } finally {
        p.deallocate();
      }
    },
    setOptions: (...a) => {
      const o = optionsOf(arg(a, 0));
      if (o !== null) {
        const [depth, scope] = state();
        callOptions.set(depth, [scope, o.over(callScopeOptions())]);
      }
      return null;
    },
    getOption: (...a) => effective(null).get(str(a, 0)),
    getSchema: (...a) => {
      const name = str(a, 0);
      if (!column('SELECT 1 FROM information_schema.SCHEMATA WHERE SCHEMA_NAME = ?', name).length)
        throw sqlError(1049, '42000', "Unknown database '" + name + "'");
      return new Schema(name);
    },
    getSchemas: () => column('SELECT SCHEMA_NAME FROM information_schema.SCHEMATA ORDER BY SCHEMA_NAME').map((n) => new Schema(n)),
    getDefaultSchema: () => {
      const n = column('SELECT DATABASE()')[0];
      return n === null || n === undefined ? null : new Schema(n);
    },
    createSchema: (...a) => { const n = str(a, 0); exec('CREATE DATABASE ' + quoteName(n)); return new Schema(n); },
    dropSchema: (...a) => { exec('DROP DATABASE ' + quoteName(str(a, 0))); return null; },
    quoteName: (...a) => quoteName(str(a, 0)),
    startTransaction: () => { exec('START TRANSACTION'); return null; },
    commit: () => { exec('COMMIT'); return null; },
    rollback: () => { exec('ROLLBACK'); return null; },
    setSavepoint: (...a) => {
      const name = arg(a, 0) === null ? 'TxSp' + (++savepoints) : str(a, 0);
      exec('SAVEPOINT ' + quoteName(savepointName(name)));
      return name;
    },
    rollbackTo: (...a) => { exec('ROLLBACK TO SAVEPOINT ' + quoteName(savepointName(arg(a, 0) === null ? '' : str(a, 0)))); return null; },
    releaseSavepoint: (...a) => { exec('RELEASE SAVEPOINT ' + quoteName(savepointName(arg(a, 0) === null ? '' : str(a, 0)))); return null; },
    autocommit: (...a) => {
      if (a.length === 0) return Number(column('SELECT @@autocommit')[0]);
      exec(truthy(a[0]) ? 'SET autocommit = 1' : 'SET autocommit = 0');
      return null;
    },
  };
  methods.executemany = methods.executeMany;
  const METHODS = new Set(Object.keys(methods));
  // The session object: its methods, and the user variables as other members (x <-> @x).
  const session = new Proxy(Object.freeze(Object.create(null)), {
    get: (t, k) => {
      if (typeof k === 'symbol') return undefined;
      if (METHODS.has(k)) return methods[k];
      if (k.startsWith('__') || k === 'then') return undefined;
      const r = run('SELECT @' + quoteName(k), null, internal());
      return r.fetchOne()[0];
    },
    set: (t, k, v) => {
      if (typeof k === 'symbol' || METHODS.has(k) || k.startsWith('__')) throw new TypeError('cannot set ' + String(k));
      send(EXECUTE, 0, 0, 'SET @' + quoteName(k) + ' = ?', [v], internal());
      return true;
    },
    has: (t, k) => typeof k === 'string' && (METHODS.has(k) || !k.startsWith('__')),
    ownKeys: () => [...METHODS],
    getOwnPropertyDescriptor: (t, k) =>
      METHODS.has(k) ? { value: methods[k], writable: false, enumerable: true, configurable: true } : undefined,
    defineProperty: () => false,
    deleteProperty: () => false,
  });

  for (const [k, v] of [['session', session], ['mysql', mysql], ['SqlError', SqlError]])
    Object.defineProperty(globalThis, k, { value: v, writable: false, configurable: false });

})
