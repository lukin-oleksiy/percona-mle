-- Demo 2: polyglot on the JVM worker (as the application account: docker/run.sh sql < 2-demo.sql,
-- after docker/run.sh root --force < 2-setup.sql).

-- A small shop: products with embeddings.
SELECT id, name, category, price, VECTOR_TO_STRING(embedding) AS embedding FROM products;

-- Python with numpy: the similarity of two vectors (a VECTOR arrives as a numpy array).
CREATE FUNCTION py_cosine(a VECTOR(4), b VECTOR(4)) RETURNS DOUBLE LANGUAGE PYTHON AS $$
    import numpy as np
    return float(a @ b / (np.linalg.norm(a) * np.linalg.norm(b)))
$$;

-- JavaScript borrows numpy from Python: one worker, one runtime, no copy through SQL.
CREATE FUNCTION js_norm(v VECTOR(4)) RETURNS DOUBLE LANGUAGE JAVASCRIPT AS $$
    const norm = Polyglot.eval('python', 'import numpy\nlambda xs: float(numpy.linalg.norm(list(xs)))')
    return Math.round(norm(v) * 1000) / 1000
$$;

-- Java: exact money arithmetic, and a JavaScript regular expression for the title.
CREATE FUNCTION java_offer(name VARCHAR(40), price DECIMAL(8,2), qty INT) RETURNS VARCHAR(80) LANGUAGE JAVA AS $$
    BigDecimal total = price.multiply(BigDecimal.valueOf(qty));
    String title = Mle.context().eval("js", "(s) => s.replace(/\\b\\w/g, (c) => c.toUpperCase())")
        .execute(name).asString();
    return qty + " x " + title + " = " + total;
$$;

-- Three languages in one SELECT.
SET @q = STRING_TO_VECTOR('[1.0, 0.1, 0.0, 0.1]');

SELECT name, ROUND(py_cosine(embedding, @q), 3) AS similarity, js_norm(embedding) AS norm,
       java_offer(name, price, 3) AS offer
  FROM products ORDER BY similarity DESC LIMIT 4;

-- Statistics MySQL has no SQL for (a median per group), from Python, with MLE's Java library.
CREATE PROCEDURE py_price_stats(OUT r TEXT) LANGUAGE PYTHON AS $$
    import java
    Stats = java.type('com.percona.mle.api.Stats')
    prices = Stats.load('SELECT category, price FROM products')
    r = str(prices.groupBy('category').median('price'))
$$;

CALL py_price_stats(@r);

SELECT @r AS median_price_per_category;

-- A whole request in one CALL: Python recommends, Java books the order, JavaScript drives both.
CREATE PROCEDURE py_recommend(IN q VECTOR(4), IN k INT) LANGUAGE PYTHON AS $$
    import numpy as np
    c = session.run_sql('SELECT id, embedding FROM products ORDER BY id').fetch_columns()
    m = c['embedding']
    sims = m @ q / (np.linalg.norm(m, axis=1) * np.linalg.norm(q))
    session.execute_many('INSERT INTO recommendations VALUES (?, ?, ?)',
                         [(n + 1, int(c['id'][i]), round(float(sims[i]), 3)) for n, i in enumerate(np.argsort(-sims)[:k])])
$$;

CREATE PROCEDURE java_order(IN qty INT, OUT total DECIMAL(10,2)) LANGUAGE JAVA AS $$
    Session s = Mle.session();
    BigDecimal sum = BigDecimal.ZERO;
    s.startTransaction();
    for (Row r : s.runSql("SELECT p.id, p.price FROM recommendations r JOIN products p ON p.id = r.product_id")) {
      BigDecimal amount = ((BigDecimal) r.get(1)).multiply(BigDecimal.valueOf(qty));
      s.runSql("INSERT INTO order_lines (product_id, qty, amount) VALUES (?, ?, ?)", r.get(0), qty, amount);
      sum = sum.add(amount);
    }
    s.commit();
    total[0] = sum;
$$;

CREATE PROCEDURE js_shop(IN q VECTOR(4), IN k INT, IN qty INT, OUT report TEXT) LANGUAGE JAVASCRIPT AS $$
    session.runSql('DELETE FROM recommendations')
    session.runSql('CALL py_recommend(?, ?)', [q, k])
    session.runSql('CALL java_order(?, @total)', [qty])
    const total = session.runSql('SELECT @total AS t').fetchOne().t
    report = `${k} products recommended and ordered, ${qty} each: total ${total}`
$$;

CALL js_shop(@q, 3, 2, @report);

SELECT @report;

SELECT r.rank_no, p.name, r.similarity, l.qty, l.amount
  FROM recommendations r JOIN products p ON p.id = r.product_id JOIN order_lines l ON l.product_id = p.id
  ORDER BY r.rank_no;

-- All of it ran in one worker process: Python, Java and JavaScript share one runtime.
SHOW GLOBAL STATUS WHERE Variable_name IN ('mle_sessions', 'mle_workers');
