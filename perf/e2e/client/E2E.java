/* Copyright (c) 2026 Percona LLC and/or its affiliates. All rights
   reserved.

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */
package e2e;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.sql.Connection;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CountDownLatch;

/**
 * The Java client of the end-to-end benchmark (perf/e2e/e2e.py runs it): one measurement of one case and variant.
 *
 * <pre>
 * java --add-modules jdk.incubator.vector -cp 'lib/*:classes' e2e.E2E --host H --port P --variant app|jvm
 *     --pipe 1..6 --c C --k K --clients N --queries Q --warm-min A --warm-max B --batch S
 * </pre>
 *
 * Every client is a thread with one persistent connection (an application's pool). Each warms up until the median
 * of two batches in a row agree within 3 % (at least warm-min, at most warm-max requests), then all wait for each
 * other and run their measured requests at the same time. Prints one JSON line: every latency (microseconds), the
 * wall time of the measured part, the ids of the first measured request of client 0, the warm-up per client.
 *
 * <p>Variants: app, the logic here (Rerank, the same source as the MLE library); jvm, a CALL of the LANGUAGE JAVA
 * procedure. Pipes: 1 rescore, 2 MMR, 3 chatty (3 dependent queries), 4 bulk, 5 write-back, 6 write-back with
 * vectors.
 */
public final class E2E {
  record Query(String text, int cat) {}

  static String url;
  static List<Query> queries;

  static Connection connect() throws Exception {
    return DriverManager.getConnection(url, "app", "app");
  }

  static float[] floats(byte[] b) {
    float[] f = new float[b.length / 4];
    ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer().get(f);
    return f;
  }

  static String text(float[] v) {
    StringBuilder b = new StringBuilder("[");
    for (int i = 0; i < v.length; i++) b.append(i == 0 ? "" : ",").append(String.format(Locale.ROOT, "%.9g", v[i]));
    return b.append(']').toString();
  }

  static int userOf(int j) {
    return 1 + (j * 7919) % 10000;
  }

  static final class Client {
    final int id;
    final Connection c;
    final Map<String, PreparedStatement> statements = new HashMap<>();
    final String variant;
    final int pipe, c0, k;

    Client(int id, String variant, int pipe, int c0, int k) throws Exception {
      this.id = id;
      this.c = connect();
      this.variant = variant;
      this.pipe = pipe;
      this.c0 = c0;
      this.k = k;
    }

    PreparedStatement ps(String sql) throws Exception {
      PreparedStatement p = statements.get(sql);
      if (p == null) statements.put(sql, p = c.prepareStatement(sql));
      return p;
    }

    /** The rows of each result set of a (multi-statement) request. */
    static List<List<Object[]>> sets(PreparedStatement ps) throws Exception {
      List<List<Object[]>> out = new ArrayList<>();
      boolean isResult = ps.execute();
      while (true) {
        if (isResult) {
          List<Object[]> rows = new ArrayList<>();
          try (ResultSet rs = ps.getResultSet()) {
            int columns = rs.getMetaData().getColumnCount();
            while (rs.next()) {
              Object[] r = new Object[columns];
              for (int i = 0; i < columns; i++) r[i] = rs.getObject(i + 1);
              rows.add(r);
            }
          }
          out.add(rows);
        } else if (ps.getUpdateCount() == -1) {
          break;
        }
        isResult = ps.getMoreResults();
      }
      return out;
    }

    static List<Object[]> last(PreparedStatement ps) throws Exception {
      List<List<Object[]>> all = sets(ps);
      return all.get(all.size() - 1);
    }

    /** One request for query j; the ids picked. */
    long[] run(int j) throws Exception {
      Query q = queries.get(j);
      int cat = pipe >= 4 ? j % 20 : q.cat();
      return variant.equals("jvm") ? mle(j, q, cat) : app(j, q, cat);
    }

    long[] mle(int j, Query q, int cat) throws Exception {
      if (!c.getAutoCommit()) c.setAutoCommit(true);
      PreparedStatement p;
      switch (pipe) {
        case 1, 2 -> {
          p = ps("CALL e2e_rerank_java(STRING_TO_VECTOR(?), ?, ?, ?, ?, @r); SELECT @r");
          p.setString(1, q.text());
          p.setInt(2, cat);
          p.setInt(3, c0);
          p.setInt(4, k);
          p.setInt(5, pipe);
        }
        case 3 -> {
          p = ps("CALL e2e_chatty_java(?, ?, ?, @r); SELECT @r");
          p.setInt(1, userOf(j));
          p.setInt(2, c0);
          p.setInt(3, k);
        }
        case 4 -> {
          p = ps("CALL e2e_bulk_java(?, ?, @r); SELECT @r");
          p.setInt(1, cat);
          p.setInt(2, k);
        }
        default -> {
          p = ps("CALL e2e_wb_java(?, ?, ?, ?, ?, @r); SELECT @r");
          p.setInt(1, cat);
          p.setInt(2, c0);
          p.setInt(3, k);
          p.setInt(4, id);
          p.setInt(5, pipe == 6 ? 1 : 0);
        }
      }
      String json = String.valueOf(last(p).get(0)[0]);
      String list = json.substring(json.indexOf('[') + 1, json.indexOf(']'));
      return Arrays.stream(list.split(",")).map(String::trim).mapToLong(Long::parseLong).toArray();
    }

    long[] app(int j, Query q, int cat) throws Exception {
      if (pipe >= 5) return writeBack(cat, pipe == 6);
      if (!c.getAutoCommit()) c.setAutoCommit(true);
      if (pipe == 3) return chatty(userOf(j));
      if (pipe == 4) {
        PreparedStatement p = ps("SELECT id, embedding FROM items WHERE category = ?");
        p.setInt(1, cat);
        List<Long> il = new ArrayList<>();
        List<float[]> vl = new ArrayList<>();
        try (ResultSet rs = p.executeQuery()) {
          while (rs.next()) {
            il.add(rs.getLong(1));
            vl.add(floats(rs.getBytes(2)));
          }
        }
        return Rerank.typical(il.stream().mapToLong(Long::longValue).toArray(), vl.toArray(new float[0][]), k);
      }
      // The query vector is parsed once (SET @q): STRING_TO_VECTOR(?) in the select list is evaluated per row.
      PreparedStatement p = ps("SET @q = STRING_TO_VECTOR(?); SELECT id, category, price, stock, "
          + "DISTANCE(embedding, @q, 'COSINE') AS dist" + (pipe == 2 ? ", embedding" : "") + " FROM items"
          + " ORDER BY DISTANCE(embedding, @q, 'COSINE') LIMIT " + c0);
      p.setString(1, q.text());
      List<Object[]> rows = last(p);
      int n = rows.size();
      long[] ids = new long[n], category = new long[n], stock = new long[n];
      double[] price = new double[n], dist = new double[n];
      float[][] v = pipe == 2 ? new float[n][] : null;
      for (int i = 0; i < n; i++) {
        Object[] r = rows.get(i);
        ids[i] = ((Number) r[0]).longValue();
        category[i] = ((Number) r[1]).longValue();
        price[i] = ((Number) r[2]).doubleValue();
        stock[i] = ((Number) r[3]).longValue();
        dist[i] = ((Number) r[4]).doubleValue();
        if (v != null) v[i] = floats((byte[]) r[5]);
      }
      double[] s = Rerank.scores(dist, price, stock, category, cat);
      return pipe == 1 ? Rerank.top(ids, s, k) : Rerank.mmr(ids, s, v, k);
    }

    /** Three dependent round trips: profile and history (one request), candidates, promotions. */
    long[] chatty(int uid) throws Exception {
      PreparedStatement p1 = ps("SELECT pref_cat, max_price FROM users WHERE id = ?; "
          + "SELECT i.id, i.embedding FROM views v JOIN items i ON i.id = v.item_id "
          + "WHERE v.user_id = ? ORDER BY v.ts DESC LIMIT 10");
      p1.setInt(1, uid);
      p1.setInt(2, uid);
      List<List<Object[]>> r1 = sets(p1);
      Object[] prof = r1.get(0).get(0);
      List<Object[]> hist = r1.get(1);
      long[] viewed = new long[hist.size()];
      float[][] hv = new float[hist.size()][];
      for (int i = 0; i < hv.length; i++) {
        viewed[i] = ((Number) hist.get(i)[0]).longValue();
        hv[i] = floats((byte[]) hist.get(i)[1]);
      }
      float[] u = Rerank.mean(hv);
      PreparedStatement p2 = ps("SET @q = STRING_TO_VECTOR(?); SELECT id, category, price, stock, "
          + "DISTANCE(embedding, @q, 'COSINE') AS dist FROM items ORDER BY DISTANCE(embedding, @q, 'COSINE') "
          + "LIMIT " + c0);
      p2.setString(1, text(u));
      List<Object[]> rows = last(p2);
      int n = rows.size();
      long[] ids = new long[n], category = new long[n], stock = new long[n];
      double[] price = new double[n], dist = new double[n];
      for (int i = 0; i < n; i++) {
        Object[] r = rows.get(i);
        ids[i] = ((Number) r[0]).longValue();
        category[i] = ((Number) r[1]).longValue();
        price[i] = ((Number) r[2]).doubleValue();
        stock[i] = ((Number) r[3]).longValue();
        dist[i] = ((Number) r[4]).doubleValue();
      }
      double[] sc = Rerank.personalScores(ids, dist, price, stock, category, ((Number) prof[0]).longValue(),
          ((Number) prof[1]).doubleValue(), viewed);
      long[] shortlist = Rerank.top(ids, sc, 3 * k);
      Map<Long, Integer> at = new HashMap<>();
      for (int i = 0; i < n; i++) at.put(ids[i], i);
      StringBuilder in = new StringBuilder();
      for (long id : shortlist) in.append(in.length() == 0 ? "" : ",").append(id);
      Map<Long, Double> disc = new HashMap<>();
      try (Statement st = c.createStatement();
           ResultSet rs = st.executeQuery("SELECT item_id, discount FROM promos WHERE item_id IN (" + in + ")")) {
        while (rs.next()) disc.put(rs.getLong(1), rs.getDouble(2));
      }
      double[] ss = new double[shortlist.length], dd = new double[shortlist.length];
      for (int i = 0; i < shortlist.length; i++) {
        ss[i] = sc[at.get(shortlist[i])];
        dd[i] = disc.getOrDefault(shortlist[i], 0.0);
      }
      return Rerank.promoted(shortlist, ss, dd, k);
    }

    /**
     * Write-back: read C items of a category, enrich them, upsert them in one transaction: one multi-row statement
     * (executeBatch with rewriteBatchedStatements), then COMMIT. Three round trips.
     */
    long[] writeBack(int cat, boolean vec) throws Exception {
      if (c.getAutoCommit()) c.setAutoCommit(false);
      PreparedStatement p = ps("SELECT id, embedding FROM items WHERE category = ? ORDER BY id LIMIT " + c0);
      p.setInt(1, cat);
      List<Long> il = new ArrayList<>();
      List<float[]> vl = new ArrayList<>();
      try (ResultSet rs = p.executeQuery()) {
        while (rs.next()) {
          il.add(rs.getLong(1));
          vl.add(floats(rs.getBytes(2)));
        }
      }
      long[] ids = il.stream().mapToLong(Long::longValue).toArray();
      float[][] v = vl.toArray(new float[0][]);
      float[][] w = vec ? new float[ids.length][] : null;
      double[] sim = Rerank.enrich(v, w);
      long[] picked = Rerank.top(ids, sim, k);
      PreparedStatement u = ps("INSERT INTO enriched" + (vec
          ? " (client, slot, item_id, score, emb) VALUES (?, ?, ?, ?, ?)"
          : " (client, slot, item_id, score) VALUES (?, ?, ?, ?)")
          + " ON DUPLICATE KEY UPDATE item_id = VALUES(item_id), score = VALUES(score)"
          + (vec ? ", emb = VALUES(emb)" : ""));
      for (int i = 0; i < ids.length; i++) {
        u.setInt(1, id);
        u.setInt(2, i);
        u.setLong(3, ids[i]);
        u.setDouble(4, sim[i]);
        if (vec) {
          ByteBuffer b = ByteBuffer.allocate(w[i].length * 4).order(ByteOrder.LITTLE_ENDIAN);
          b.asFloatBuffer().put(w[i]);
          u.setBytes(5, b.array());
        }
        u.addBatch();
      }
      u.executeBatch();
      c.commit();
      return picked;
    }
  }

  static double median(double[] x, int n) {
    double[] s = Arrays.copyOf(x, n);
    Arrays.sort(s);
    return n % 2 == 1 ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
  }

  public static void main(String[] args) throws Exception {
    Map<String, String> a = new HashMap<>();
    for (int i = 0; i + 1 < args.length; i += 2) a.put(args[i].substring(2), args[i + 1]);
    url = "jdbc:mysql://" + a.get("host") + ":" + a.get("port") + "/e2e?allowMultiQueries=true&useSSL=false"
        + "&allowPublicKeyRetrieval=true&socketTimeout=600000&rewriteBatchedStatements=true";
    String variant = a.get("variant");
    int pipe = Integer.parseInt(a.get("pipe")), c0 = Integer.parseInt(a.get("c")), k = Integer.parseInt(a.get("k"));
    int conc = Integer.parseInt(a.get("clients")), nq = Integer.parseInt(a.get("queries"));
    int warmMin = Integer.parseInt(a.get("warm-min")), warmMax = Integer.parseInt(a.get("warm-max"));
    int batch = Integer.parseInt(a.get("batch"));
    queries = new ArrayList<>();
    try (Connection c = connect(); Statement s = c.createStatement();
         ResultSet rs = s.executeQuery("SELECT cat, q FROM queries ORDER BY j")) {
      while (rs.next()) queries.add(new Query(rs.getString(2), rs.getInt(1)));
    }
    int nset = queries.size();
    Client[] clients = new Client[conc];
    for (int i = 0; i < conc; i++) clients[i] = new Client(i + 1, variant, pipe, c0, k);
    double[][] lat = new double[conc][nq];
    int[] warmed = new int[conc];
    boolean[] converged = new boolean[conc];
    long[][] first = new long[conc][];
    // latches, not barriers: a client that fails still counts down, so nobody waits forever
    CountDownLatch ready = new CountDownLatch(conc), go = new CountDownLatch(1), done = new CountDownLatch(conc);
    Exception[] failure = new Exception[1];
    for (int t = 0; t < conc; t++) {
      final int ti = t;
      Thread th = new Thread(() -> {
        boolean counted = false;
        try {
          Client cl = clients[ti];
          double[] b = new double[batch];
          double prev = -1;
          int n = 0;
          while (n < warmMax && failure[0] == null) {
            for (int i = 0; i < batch; i++, n++) {
              long t0 = System.nanoTime();
              cl.run((ti * 37 + 100 + n) % nset);
              b[i] = System.nanoTime() - t0;
            }
            double m = median(b, batch);
            if (n >= warmMin && prev > 0 && Math.abs(m - prev) <= 0.03 * Math.min(m, prev)) {
              converged[ti] = true;
              break;
            }
            prev = m;
          }
          warmed[ti] = n;
          counted = true;
          ready.countDown();
          go.await();
          for (int i = 0; i < nq && failure[0] == null; i++) {
            int j = (ti * 37 + i) % nset;
            long t0 = System.nanoTime();
            long[] ids = cl.run(j);
            lat[ti][i] = (System.nanoTime() - t0) / 1e3;
            if (i == 0) first[ti] = ids;
          }
        } catch (Exception e) {
          failure[0] = e;
        } finally {
          if (!counted) ready.countDown();
          done.countDown();
        }
      });
      th.setDaemon(true);
      th.start();
    }
    ready.await();
    long t0 = System.nanoTime();
    go.countDown();
    done.await();
    double wall = (System.nanoTime() - t0) / 1e9;
    if (failure[0] != null) {
      failure[0].printStackTrace();
      System.exit(1);
    }
    StringBuilder out = new StringBuilder("{\"wall_s\": ").append(wall).append(", \"lat_us\": [");
    for (int t = 0; t < conc; t++)
      for (int i = 0; i < nq; i++) out.append(t + i == 0 ? "" : ", ").append(Math.round(lat[t][i]));
    out.append("], \"ids\": ").append(Arrays.toString(first[0])).append(", \"warm\": ").append(Arrays.toString(warmed))
        .append(", \"converged\": ").append(Arrays.toString(converged)).append(", \"dot\": \"")
        .append(Rerank.dotKind()).append("\"}");
    System.out.println(out);
    for (Client cl : clients) cl.c.close();
  }
}
