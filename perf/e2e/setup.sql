-- Data and routines of the end-to-end benchmark (perf/e2e/e2e.py). Loaded once into its data directory
-- by the mysql client; e2e.py replaces the line "-- @include client/Rerank.java" with that file, so the
-- Java procedures and the external Java application use the same re-ranking code.
--
-- Data (the same generator and seeds as exp notes 43 and 50): items, 100,000 x VECTOR(768) around 100
-- cluster centres, with category, price and stock, an HNSW index (cosine) and an index on category;
-- 10,000 users with 20 viewed items each; promotions on 10 % of the items; the query set (200 query
-- vectors near items, with a preferred category) that every client uses; enriched, where write-back
-- requests upsert their rows (each client its own rows).
--
-- Routines, one per case and runtime: *_java LANGUAGE JAVA (JVM worker), *_py LANGUAGE PYTHON (GraalPy,
-- JVM worker) and *_cpy LANGUAGE CPYTHON (CPython worker), which e2e.py makes from the *_py ones (the same
-- code). Each returns the ids it picked as JSON in its OUT parameter: {"ids": [...]}.

CREATE DATABASE e2e;
USE e2e;
GRANT CREATE_PYTHON_ROUTINE, CREATE_JAVA_ROUTINE, CREATE_CPYTHON_ROUTINE ON *.* TO root@localhost;
CREATE USER app@'%' IDENTIFIED BY 'app';
GRANT ALL ON e2e.* TO app@'%';

-- ------------------------------------------------------------------------------------------------ data

CREATE PROCEDURE gen_items(IN n INT, IN d INT, IN seed INT) MODIFIES SQL DATA LANGUAGE CPYTHON AS $$
    # Synthetic clustered embeddings: 100 unit-length centres, points = centre + Gaussian noise, normalised;
    # metadata random. The random draws in the same order as exp note 43's generator: the same data.
    import numpy as np
    rng = np.random.default_rng(seed)
    centres = rng.standard_normal((100, d)).astype(np.float32)
    centres /= np.linalg.norm(centres, axis=1, keepdims=True)
    session.run_sql(f"CREATE TABLE items (id BIGINT UNSIGNED PRIMARY KEY, category INT NOT NULL, "
                    f"price DOUBLE NOT NULL, stock INT NOT NULL, embedding VECTOR({d}) NOT NULL)")
    for start in range(0, n, 2000):
        m = min(2000, n - start)
        cl = rng.integers(0, 100, m)
        v = centres[cl] + rng.standard_normal((m, d)).astype(np.float32) * np.float32(0.6 / np.sqrt(d))
        v /= np.linalg.norm(v, axis=1, keepdims=True)
        price = np.round(rng.uniform(1, 1000, m), 2)
        stock = np.where(rng.random(m) < 0.2, 0, rng.integers(1, 50, m))
        session.start_transaction()
        session.execute_many("INSERT INTO items VALUES (?, ?, ?, ?, ?)",
                             [(start + i + 1, int(cl[i] % 20), float(price[i]), int(stock[i]), v[i])
                              for i in range(m)])
        session.commit()
    session.run_sql("ALTER TABLE items ADD VECTOR KEY vk (embedding) TYPE hnsw (M = 16, metric = cosine)")
$$;

CREATE PROCEDURE gen_users(IN nusers INT, IN seed INT) MODIFIES SQL DATA LANGUAGE CPYTHON AS $$
    # Users with a preferred category and a price limit, each with 20 viewed items from two favourite
    # categories (newest last); promotions on 10 % of the items; an index on items.category.
    import numpy as np
    rng = np.random.default_rng(seed)
    cols = session.run_sql("SELECT id, category FROM items").fetch_columns()
    ids, cats = np.asarray(cols['id']), np.asarray(cols['category'])
    by_cat = [ids[cats == c] for c in range(20)]
    session.run_sql("CREATE TABLE users (id INT PRIMARY KEY, pref_cat INT NOT NULL, max_price DOUBLE NOT NULL)")
    session.run_sql("CREATE TABLE views (user_id INT NOT NULL, item_id BIGINT UNSIGNED NOT NULL, "
                    "ts INT NOT NULL, PRIMARY KEY (user_id, ts))")
    session.run_sql("CREATE TABLE promos (item_id BIGINT UNSIGNED PRIMARY KEY, discount DOUBLE NOT NULL)")
    users, views = [], []
    for u in range(1, nusers + 1):
        fav = rng.choice(20, 2, replace=False)
        users.append((u, int(fav[0]), float(np.round(rng.uniform(50, 1000), 2))))
        for ts in range(20):
            pool = by_cat[int(fav[ts % 2])]
            views.append((u, int(pool[rng.integers(0, len(pool))]), ts))
    promos = [(int(i), float(np.round(rng.uniform(0.05, 0.3), 3))) for i in rng.choice(ids, len(ids) // 10,
                                                                                         replace=False)]
    for sql, rows in (("INSERT INTO users VALUES (?, ?, ?)", users), ("INSERT INTO views VALUES (?, ?, ?)", views),
                      ("INSERT INTO promos VALUES (?, ?)", promos)):
        for lo in range(0, len(rows), 5000):
            session.start_transaction()
            session.execute_many(sql, rows[lo:lo + 5000])
            session.commit()
    session.run_sql("ALTER TABLE items ADD INDEX cat_idx (category)")
$$;

CREATE PROCEDURE gen_queries(IN nq INT, IN seed INT) MODIFIES SQL DATA LANGUAGE CPYTHON AS $$
    # The query set: nq items, each moved a little (noise, normalised), as text, and a preferred category
    # (the query set of exp note 43).
    import numpy as np
    rng = np.random.default_rng(seed)
    n = int(session.run_sql("SELECT COUNT(*) AS n FROM items").fetch_one().n)
    pick = rng.choice(np.arange(1, n + 1), nq, replace=False)
    cols = session.run_sql("SELECT id, embedding FROM items WHERE id IN (" + ",".join(str(int(i)) for i in pick)
                           + ") ORDER BY id").fetch_columns()
    session.run_sql("CREATE TABLE queries (j INT PRIMARY KEY, cat INT NOT NULL, q TEXT NOT NULL)")
    rows = []
    for j, e in enumerate(cols['embedding']):
        v = e.astype(np.float64)
        v = v + rng.standard_normal(len(v)) * (0.3 / np.sqrt(len(v)))
        v /= np.linalg.norm(v)
        rows.append((j, int(rng.integers(0, 20)), '[' + ','.join('%.8g' % x for x in v) + ']'))
    session.execute_many("INSERT INTO queries VALUES (?, ?, ?)", rows)
$$;

CALL gen_items(100000, 768, 42);
CALL gen_users(10000, 43);
CALL gen_queries(200, 1);
DROP PROCEDURE gen_items;
DROP PROCEDURE gen_users;
DROP PROCEDURE gen_queries;
CREATE TABLE enriched (client INT NOT NULL, slot INT NOT NULL, item_id BIGINT NOT NULL, score DOUBLE NOT NULL,
  emb VECTOR(768), PRIMARY KEY (client, slot));

-- ------------------------------------------------------------------------------------------------ Java

CREATE LIBRARY e2e_rerank LANGUAGE JAVA AS $$
-- @include client/Rerank.java
$$;

-- rescore (pipe 1) and MMR (pipe 2): the C nearest by HNSW, scored; the top K by score or by MMR.
CREATE PROCEDURE e2e_rerank_java(IN q VECTOR(768), IN cat INT, IN c INT, IN k INT, IN pipe INT, OUT r JSON)
READS SQL DATA LANGUAGE JAVA USING (e2e_rerank) AS $$
  String sql = "SELECT id, category, price, stock, DISTANCE(embedding, ?, 'COSINE') AS dist"
      + (pipe == 2 ? ", embedding" : "") + " FROM items ORDER BY DISTANCE(embedding, ?, 'COSINE') LIMIT " + c;
  List<Row> rows = Mle.session().runSql(sql, q, q).fetchAll();
  int n = rows.size();
  long[] ids = new long[n], category = new long[n], stock = new long[n];
  double[] price = new double[n], dist = new double[n];
  float[][] v = pipe == 2 ? new float[n][] : null;
  for (int i = 0; i < n; i++) {
    Row row = rows.get(i);
    ids[i] = ((Number) row.get(0)).longValue();
    category[i] = ((Number) row.get(1)).longValue();
    price[i] = ((Number) row.get(2)).doubleValue();
    stock[i] = ((Number) row.get(3)).longValue();
    dist[i] = ((Number) row.get(4)).doubleValue();
    if (v != null) v[i] = (float[]) row.get(5);
  }
  double[] s = e2e.Rerank.scores(dist, price, stock, category, cat);
  long[] picked = pipe == 1 ? e2e.Rerank.top(ids, s, k) : e2e.Rerank.mmr(ids, s, v, k);
  r[0] = "{\"ids\": " + Arrays.toString(picked) + "}";
$$;

-- chatty: the user's profile and history, candidates near the history's mean, promotions of the shortlist.
CREATE PROCEDURE e2e_chatty_java(IN uid INT, IN c INT, IN k INT, OUT r JSON) READS SQL DATA
LANGUAGE JAVA USING (e2e_rerank) AS $$
  Session s0 = Mle.session();
  Row prof = s0.runSql("SELECT pref_cat, max_price FROM users WHERE id = ?", uid).fetchOne();
  List<Row> hist = s0.runSql("SELECT i.id, i.embedding FROM views v JOIN items i ON i.id = v.item_id "
      + "WHERE v.user_id = ? ORDER BY v.ts DESC LIMIT 10", uid).fetchAll();
  long[] viewed = new long[hist.size()];
  float[][] hv = new float[hist.size()][];
  for (int i = 0; i < hv.length; i++) {
    viewed[i] = ((Number) hist.get(i).get(0)).longValue();
    hv[i] = (float[]) hist.get(i).get(1);
  }
  float[] u = e2e.Rerank.mean(hv);
  List<Row> rows = s0.runSql("SELECT id, category, price, stock, DISTANCE(embedding, ?, 'COSINE') AS dist FROM items"
      + " ORDER BY DISTANCE(embedding, ?, 'COSINE') LIMIT " + c, u, u).fetchAll();
  int n = rows.size();
  long[] ids = new long[n], category = new long[n], stock = new long[n];
  double[] price = new double[n], dist = new double[n];
  for (int i = 0; i < n; i++) {
    Row row = rows.get(i);
    ids[i] = ((Number) row.get(0)).longValue();
    category[i] = ((Number) row.get(1)).longValue();
    price[i] = ((Number) row.get(2)).doubleValue();
    stock[i] = ((Number) row.get(3)).longValue();
    dist[i] = ((Number) row.get(4)).doubleValue();
  }
  double[] sc = e2e.Rerank.personalScores(ids, dist, price, stock, category, ((Number) prof.get(0)).longValue(),
      ((Number) prof.get(1)).doubleValue(), viewed);
  long[] shortlist = e2e.Rerank.top(ids, sc, 3 * k);
  Map<Long, Integer> at = new HashMap<>();
  for (int i = 0; i < n; i++) at.put(ids[i], i);
  StringBuilder in = new StringBuilder();
  for (long id : shortlist) in.append(in.length() == 0 ? "" : ",").append(id);
  Map<Long, Double> disc = new HashMap<>();
  for (Row row : s0.runSql("SELECT item_id, discount FROM promos WHERE item_id IN (" + in + ")").fetchAll())
    disc.put(((Number) row.get(0)).longValue(), ((Number) row.get(1)).doubleValue());
  double[] shortScores = new double[shortlist.length], d = new double[shortlist.length];
  for (int i = 0; i < shortlist.length; i++) {
    shortScores[i] = sc[at.get(shortlist[i])];
    d[i] = disc.getOrDefault(shortlist[i], 0.0);
  }
  r[0] = "{\"ids\": " + Arrays.toString(e2e.Rerank.promoted(shortlist, shortScores, d, k)) + "}";
$$;

-- bulk: all items of a category (~5,000 vectors): the K most typical (nearest the centroid) and the K outliers.
CREATE PROCEDURE e2e_bulk_java(IN cat INT, IN k INT, OUT r JSON) READS SQL DATA LANGUAGE JAVA USING (e2e_rerank) AS $$
  List<Row> rows = Mle.session().runSql("SELECT id, embedding FROM items WHERE category = ?", cat).fetchAll();
  long[] ids = new long[rows.size()];
  float[][] v = new float[rows.size()][];
  for (int i = 0; i < ids.length; i++) {
    ids[i] = ((Number) rows.get(i).get(0)).longValue();
    v[i] = (float[]) rows.get(i).get(1);
  }
  r[0] = "{\"ids\": " + Arrays.toString(e2e.Rerank.typical(ids, v, k)) + "}";
$$;

-- write-back: C items of a category, enriched (score; with vec = 1 also a VECTOR(768)), upserted into enriched
-- in one transaction with executeMany.
CREATE PROCEDURE e2e_wb_java(IN cat INT, IN c INT, IN k INT, IN cl INT, IN vec INT, OUT r JSON) MODIFIES SQL DATA
LANGUAGE JAVA USING (e2e_rerank) AS $$
  Session s = Mle.session();
  List<Row> rows = s.runSql("SELECT id, embedding FROM items WHERE category = ? ORDER BY id LIMIT " + c, cat)
      .fetchAll();
  int n = rows.size();
  long[] ids = new long[n];
  float[][] v = new float[n][];
  for (int i = 0; i < n; i++) {
    ids[i] = ((Number) rows.get(i).get(0)).longValue();
    v[i] = (float[]) rows.get(i).get(1);
  }
  float[][] w = vec == 1 ? new float[n][] : null;
  double[] sim = e2e.Rerank.enrich(v, w);
  long[] picked = e2e.Rerank.top(ids, sim, k);
  List<Object[]> batch = new ArrayList<>(n);
  for (int i = 0; i < n; i++)
    batch.add(vec == 1 ? new Object[] {cl, i, ids[i], sim[i], w[i]} : new Object[] {cl, i, ids[i], sim[i]});
  s.startTransaction();
  s.executeMany(vec == 1
      ? "INSERT INTO enriched (client, slot, item_id, score, emb) VALUES (?, ?, ?, ?, ?)"
        + " AS x ON DUPLICATE KEY UPDATE item_id = x.item_id, score = x.score, emb = x.emb"
      : "INSERT INTO enriched (client, slot, item_id, score) VALUES (?, ?, ?, ?)"
        + " AS x ON DUPLICATE KEY UPDATE item_id = x.item_id, score = x.score", batch);
  s.commit();
  r[0] = "{\"ids\": " + Arrays.toString(picked) + "}";
$$;

-- ------------------------------------------------------------------------------------------------ Python
-- LANGUAGE PYTHON (GraalPy); e2e.py adds a copy of each as LANGUAGE CPYTHON, named *_cpy (the same code).

CREATE PROCEDURE e2e_rerank_py(IN q VECTOR(768), IN cat INT, IN c INT, IN k INT, IN pipe INT, OUT r JSON)
READS SQL DATA LANGUAGE PYTHON AS $$
    import numpy as np
    sql = ("SELECT id, category, price, stock, DISTANCE(embedding, ?, 'COSINE') AS dist"
           + (", embedding" if pipe == 2 else "") + " FROM items ORDER BY DISTANCE(embedding, ?, 'COSINE') LIMIT "
           + str(int(c)))
    cols = session.run_sql(sql, [q, q]).fetch_columns()
    ids = cols['id']
    s = (0.8 * (1 - cols['dist']) + 0.1 * (1 - cols['price'] / 1000) + 0.05 * (cols['stock'] > 0)
         + 0.05 * (cols['category'] == cat))
    if pipe == 1:
        picked = ids[np.lexsort((ids, -s))[:k]]
    else:
        v = cols['embedding']
        n = len(ids)
        max_sim = np.zeros(n)
        taken = np.zeros(n, dtype=bool)
        picked = []
        for step in range(min(k, n)):
            value = 0.7 * s - (0.3 * max_sim if step else 0)
            value[taken] = -np.inf
            i = int(np.argmax(value))
            taken[i] = True
            picked.append(ids[i])
            sims = v @ v[i]
            max_sim = sims if step == 0 else np.maximum(max_sim, sims)
    r = {'ids': [int(x) for x in picked]}
$$;

CREATE PROCEDURE e2e_chatty_py(IN uid INT, IN c INT, IN k INT, OUT r JSON) READS SQL DATA LANGUAGE PYTHON AS $$
    import numpy as np
    prof = session.run_sql("SELECT pref_cat, max_price FROM users WHERE id = ?", [uid]).fetch_one()
    hist = session.run_sql("SELECT i.id, i.embedding FROM views v JOIN items i ON i.id = v.item_id "
                           "WHERE v.user_id = ? ORDER BY v.ts DESC LIMIT 10", [uid]).fetch_columns()
    u = hist['embedding'].astype(np.float64).sum(axis=0)
    u = (u / np.linalg.norm(u)).astype(np.float32)
    cand = session.run_sql("SELECT id, category, price, stock, DISTANCE(embedding, ?, 'COSINE') AS dist FROM items"
                           " ORDER BY DISTANCE(embedding, ?, 'COSINE') LIMIT " + str(int(c)), [u, u]).fetch_columns()
    ids = np.asarray(cand['id'])
    s = (0.8 * (1 - cand['dist']) + 0.05 * (cand['category'] == prof.pref_cat)
         + 0.05 * (cand['price'] <= prof.max_price) + 0.05 * (cand['stock'] > 0))
    s[np.isin(ids, np.asarray(hist['id']))] = -np.inf
    order = np.lexsort((ids, -s))[:3 * k]
    short, sc = ids[order], s[order]
    disc = dict((int(row.item_id), row.discount) for row in session.run_sql(
        "SELECT item_id, discount FROM promos WHERE item_id IN (" + ",".join(str(int(x)) for x in short) + ")"
        ).fetch_all())
    final = sc + 0.2 * np.array([disc.get(int(x), 0.0) for x in short])
    r = {'ids': [int(x) for x in short[np.lexsort((short, -final))[:k]]]}
$$;

CREATE PROCEDURE e2e_bulk_py(IN cat INT, IN k INT, OUT r JSON) READS SQL DATA LANGUAGE PYTHON AS $$
    import numpy as np
    cols = session.run_sql("SELECT id, embedding FROM items WHERE category = ?", [cat]).fetch_columns()
    ids, v = np.asarray(cols['id']), cols['embedding'].astype(np.float64)
    c = v.mean(axis=0)
    c /= np.linalg.norm(c)
    sim = v @ c
    r = {'ids': [int(x) for x in list(ids[np.lexsort((ids, -sim))[:k]]) + list(ids[np.lexsort((ids, sim))[:k]])]}
$$;

CREATE PROCEDURE e2e_wb_py(IN cat INT, IN c INT, IN k INT, IN cl INT, IN vec INT, OUT r JSON) MODIFIES SQL DATA
LANGUAGE PYTHON AS $$
    import numpy as np
    cols = session.run_sql("SELECT id, embedding FROM items WHERE category = ? ORDER BY id LIMIT " + str(int(c)),
                           [cat]).fetch_columns()
    ids, v = np.asarray(cols['id']), cols['embedding'].astype(np.float64)
    cen = v.mean(axis=0)
    cen /= np.linalg.norm(cen)
    sim = v @ cen
    picked = [int(x) for x in ids[np.lexsort((ids, -sim))[:k]]]
    idl, siml = ids.tolist(), sim.tolist()
    if vec:
        w = v - cen
        w = (w / np.linalg.norm(w, axis=1, keepdims=True)).astype(np.float32)
        sql = ("INSERT INTO enriched (client, slot, item_id, score, emb) VALUES (?, ?, ?, ?, ?)"
               " AS x ON DUPLICATE KEY UPDATE item_id = x.item_id, score = x.score, emb = x.emb")
        rows = [[cl, i, idl[i], siml[i], w[i]] for i in range(len(idl))]
    else:
        sql = ("INSERT INTO enriched (client, slot, item_id, score) VALUES (?, ?, ?, ?)"
               " AS x ON DUPLICATE KEY UPDATE item_id = x.item_id, score = x.score")
        rows = [[cl, i, idl[i], siml[i]] for i in range(len(idl))]
    session.start_transaction()
    session.execute_many(sql, rows)
    session.commit()
    r = {'ids': picked}
$$;
