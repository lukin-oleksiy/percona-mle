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

package com.percona.mle.worker;

import java.io.IOException;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.channels.NonWritableChannelException;
import java.nio.channels.SeekableByteChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.AccessMode;
import java.nio.file.DirectoryStream;
import java.nio.file.LinkOption;
import java.nio.file.NoSuchFileException;
import java.nio.file.OpenOption;
import java.nio.file.Path;
import java.nio.file.ReadOnlyFileSystemException;
import java.nio.file.attribute.FileAttribute;
import java.nio.file.attribute.FileTime;
import java.util.HashMap;
import java.util.Map;
import java.util.Set;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import org.graalvm.polyglot.io.FileSystem;

/**
 * Database libraries (CREATE LIBRARY, Phase 6, 14-phase6-libraries.md §2) as ES modules: Oracle's
 * module paths "/schema/library" are served from mysqld (LIBRARY requests); everything else goes to
 * the worker's read-only file system.
 *
 * <p>"/db/lib" (two segments, no such directory on disk) becomes the virtual
 * "/.mle-libraries/db/lib.mjs"; its real path carries the body's hash
 * ("/.mle-libraries/db/lib.HASH.mjs"), so the language's module cache keeps one module per
 * version: an altered library is a new module at the next import, an unchanged one is evaluated
 * once per worker. mysqld decides which libraries a routine may import.
 */
final class LibraryFileSystem implements FileSystem {
  /** A library of a routine's USING clause. */
  record Import(String schema, String name, String alias) {
    /** The name the routine uses for it. */
    String binding() {
      return alias.isEmpty() ? name : alias;
    }

    String path() {
      return "/" + schema + "/" + name;
    }
  }

  /** mysqld's answer to a LIBRARY request. */
  record Reply(int status, String language, String body, long hash, String message) {}

  /**
   * A library body the worker has; checked: the validation epoch it was last confirmed in.
   * generation and loadset: see {@link #generation}.
   */
  record Library(String language, byte[] body, long hash, long checked, long generation,
      Map<String, Long> loadset) {
    Library confirmed(long epoch) {
      return checked == epoch ? this : new Library(language, body, hash, epoch, generation, loadset);
    }

    /**
     * What the caches of evaluated libraries go by (the module's path, the namespaces kept by
     * Polyglot and PySql): the body's hash, and another value in every generation.
     */
    long stamp() {
      return generation == 0 ? hash : hash ^ (generation * 0x9E3779B97F4A7C15L);
    }
  }

  /**
   * An evaluated library is kept (the language's module map, the namespaces): its imports of other
   * libraries are bound to the modules they were then, and were confirmed with mysqld for the
   * routine that ran then. Later calls confirm the library itself, and would never ask about what
   * it imported: a changed or dropped library would stay behind an unchanged importer, and a
   * routine would get, through an importer evaluated for another routine, a library mysqld
   * refuses it (independent review C17).
   *
   * <p>So each library has a loadset: every library (with its hash) fetched in the top-level
   * request that first evaluated it, from then on, and the loadsets of those: at least what it
   * imports, directly or not. When a library is confirmed for a routine (once per epoch and
   * routine, where mysqld is asked anyway), so is each member of its loadset, for the same
   * routine. If one is refused or has changed, a new generation starts: every library gets a new
   * stamp at its next use, so it is evaluated again, its imports are resolved and confirmed again
   * (a refusal is then the import's error), and its loadset is collected again. Calls that change
   * nothing pay nothing: no request to mysqld, no new evaluation.
   */
  private static long generation;
  /** The loadsets still collecting: of libraries first evaluated in the running top-level request. */
  private static final java.util.List<Map<String, Long>> open = new java.util.ArrayList<>();

  /** A top-level request starts: the libraries evaluated in the last one have their loadsets. */
  static void requestStart() {
    if (!open.isEmpty()) open.clear();
  }

  /** Library l (key) is used: it belongs to the loadsets being collected, with its own. */
  private static void touch(String key, Library l) {
    for (Map<String, Long> s : open) {
      if (s == l.loadset()) continue;
      s.put(key, l.hash());
      s.putAll(l.loadset());
    }
  }

  /** l as a library of the current generation, to be evaluated again: a new stamp and loadset. */
  private static Library renewed(String key, Library l) {
    Library n = new Library(l.language(), l.body(), l.hash(), l.checked(), generation, new HashMap<>());
    libraries.put(key, n);
    touch(key, n);
    open.add(n.loadset());
    return n;
  }

  /** The libraries evaluated with l are confirmed for the running routine, or a generation starts. */
  private static void confirmLoadset(Library l) {
    for (Map.Entry<String, Long> e : java.util.List.copyOf(l.loadset().entrySet())) {
      int slash = e.getKey().indexOf('/');
      try {
        if (fetch(e.getKey().substring(0, slash), e.getKey().substring(slash + 1)).hash() == e.getValue())
          continue;
      } catch (IOException refused) {
        return;  // dropped, or not for this routine: fetch started a generation
      }
      newGeneration();
      return;
    }
  }

  /**
   * What was evaluated is not to be used as it is. The confirmations go too: a routine confirmed
   * for a library has had that library's loadset confirmed, and the loadsets are collected anew.
   */
  private static void newGeneration() {
    generation++;
    confirmed.clear();
  }

  /**
   * Library schema/name is about to be evaluated (again, after an evaluation that failed): what it
   * imports from here on belongs to its loadset.
   */
  static void evaluating(String schema, String name) {
    Library l = libraries.get(schema + "/" + name);
    if (l == null) return;
    for (Map<String, Long> s : open) if (s == l.loadset()) return;
    open.add(l.loadset());
  }

  /**
   * Library DDL (CREATE/DROP LIBRARY) invalidates the server's stored-program caches, so every
   * routine used after it is set up again and the worker gets a new PREPARE. Each PREPARE starts
   * a new epoch; within an epoch a library confirmed once is used without asking mysqld again. In
   * effect libraries are validated once per statement (and per routine load), not per call.
   */
  private static long epoch = 1;
  /**
   * "routine:schema/name" confirmed by mysqld in this epoch: mysqld decides per importing routine
   * (its schema and USING), so a library one routine may import is not thereby importable by
   * another (an INVOKER routine of another account, say).
   */
  private static final Set<String> confirmed = new java.util.HashSet<>();

  /** A routine was (re)loaded: confirm libraries with mysqld again at their next import. */
  static void revalidate() {
    epoch++;
    confirmed.clear();
  }

  static final String ROOT = "/.mle-libraries";
  private static final Pattern ORACLE_PATH = Pattern.compile("/([^/.]+)/([^/.]+)");
  private static final Pattern VIRTUAL = Pattern.compile(
      Pattern.quote(ROOT) + "/([^/]+)/([^/.]+)(?:\\.([0-9a-f]+))?\\.mjs");

  private final FileSystem delegate;
  /** By "schema/name": the newest body seen (one Context per worker). */
  private static final Map<String, Library> libraries = new HashMap<>();

  LibraryFileSystem(FileSystem delegate) {
    this.delegate = delegate;
  }

  /** A library's current body (Python and Java libraries, cross-language USING). */
  static Library library(String schema, String name) throws IOException {
    return fetch(schema, name);
  }

  private static Library fetch(String schema, String name) throws IOException {
    Dispatcher d = SqlBridge.dispatcher;
    String key = schema + "/" + name;
    Library known = libraries.get(key);
    String confirmation = (d == null ? 0 : d.routine()) + ":" + key;
    if (known != null && known.checked() == epoch && confirmed.contains(confirmation)) {
      if (known.generation() != generation) return renewed(key, known);
      if (!open.isEmpty()) touch(key, known);
      return known;
    }
    if (d == null) throw new NoSuchFileException("/" + key, null, "no database libraries here");
    Reply r = d.libraryRequest(schema, name, known == null ? 0 : known.hash());
    switch (r.status()) {
      case Wire.LIBRARY_UNCHANGED -> {
        Library l = known.confirmed(epoch);
        libraries.put(key, l);
        confirmed.add(confirmation);
        if (l.generation() == generation) {
          touch(key, l);
          confirmLoadset(l);
        }
        return l.generation() != generation ? renewed(key, l) : l;
      }
      case Wire.LIBRARY_BODY -> {
        Library l = new Library(r.language(), r.body().getBytes(StandardCharsets.UTF_8), r.hash(), epoch,
            generation, new HashMap<>());
        libraries.put(key, l);
        // A changed body: routines confirmed for the old one have its loadset to confirm.
        if (known != null) confirmed.clear();
        confirmed.add(confirmation);
        touch(key, l);
        open.add(l.loadset());  // a new body: evaluated from here on
        return l;
      }
      default -> {
        libraries.remove(key);
        // A library this worker had (evaluated ones may be bound to it, or were evaluated for
        // another routine), or one refused while others are being evaluated (their evaluation
        // fails): everything is evaluated again at its next use. Not for a library never seen
        // (a routine that tries an optional import in every call changes nothing).
        if (known != null || !open.isEmpty()) newGeneration();
        throw new NoSuchFileException("/" + key, null, r.message());
      }
    }
  }

  private static Path virtual(String schema, String name) {
    return Path.of(ROOT, schema, name + ".mjs");
  }

  /** An Oracle module path, if s is one (and not a real directory). */
  private Path oracle(String s) {
    Matcher m = ORACLE_PATH.matcher(s);
    if (!m.matches()) return null;
    try {
      if (delegate.readAttributes(delegate.parsePath("/" + m.group(1)), "isDirectory").get("isDirectory")
          instanceof Boolean b && b)
        return null;
    } catch (IOException | SecurityException | UnsupportedOperationException e) {
      // no such directory: a library
    }
    return virtual(m.group(1), m.group(2));
  }

  /** A library path: virtual, or an Oracle module path made by resolving against another path. */
  private Matcher virtualOf(Path p) {
    Matcher m = VIRTUAL.matcher(p.toString());
    if (m.matches()) return m;
    Path v = oracle(p.toString());
    if (v == null) return null;
    m = VIRTUAL.matcher(v.toString());
    return m.matches() ? m : null;
  }

  private Library body(Path p) throws IOException {
    Matcher m = virtualOf(p);
    if (m == null) return null;
    // Every access, versioned path or not, is confirmed with mysqld once per epoch and importing
    // routine ("unchanged" without the body, a new body, or refused: dropped or not importable).
    return fetch(m.group(1), m.group(2));
  }

  @Override
  public Path parsePath(URI uri) {
    Path p = "file".equals(uri.getScheme()) ? oracle(uri.getPath()) : null;
    return p != null ? p : delegate.parsePath(uri);
  }

  @Override
  public Path parsePath(String path) {
    Path p = oracle(path);
    return p != null ? p : delegate.parsePath(path);
  }

  @Override
  public void checkAccess(Path path, Set<? extends AccessMode> modes, LinkOption... options) throws IOException {
    if (virtualOf(path) == null) {
      delegate.checkAccess(path, modes, options);
      return;
    }
    if (modes.contains(AccessMode.WRITE) || modes.contains(AccessMode.EXECUTE))
      throw new ReadOnlyFileSystemException();
    body(path);
  }

  @Override
  public void createDirectory(Path dir, FileAttribute<?>... attrs) throws IOException {
    if (virtualOf(dir) != null || dir.startsWith(ROOT)) throw new ReadOnlyFileSystemException();
    delegate.createDirectory(dir, attrs);
  }

  @Override
  public void delete(Path path) throws IOException {
    if (path.startsWith(ROOT)) throw new ReadOnlyFileSystemException();
    delegate.delete(path);
  }

  @Override
  public SeekableByteChannel newByteChannel(Path path, Set<? extends OpenOption> options,
      FileAttribute<?>... attrs) throws IOException {
    Library l = body(path);
    if (l == null) return delegate.newByteChannel(path, options, attrs);
    return new BytesChannel(l.body());
  }

  @Override
  public DirectoryStream<Path> newDirectoryStream(Path dir, DirectoryStream.Filter<? super Path> filter)
      throws IOException {
    if (dir.startsWith(ROOT) || virtualOf(dir) != null) throw new NoSuchFileException(dir.toString());
    return delegate.newDirectoryStream(dir, filter);
  }

  @Override
  public Path toAbsolutePath(Path path) {
    return path.startsWith(ROOT) || virtualOf(path) != null ? path : delegate.toAbsolutePath(path);
  }

  @Override
  public Path toRealPath(Path path, LinkOption... options) throws IOException {
    Matcher m = virtualOf(path);
    if (m == null) return delegate.toRealPath(path, options);
    Library l = body(path);
    return Path.of(ROOT, m.group(1), m.group(2) + "." + Long.toHexString(l.stamp()) + ".mjs");
  }

  @Override
  public Map<String, Object> readAttributes(Path path, String attributes, LinkOption... options)
      throws IOException {
    Library l = body(path);
    if (l == null) return delegate.readAttributes(path, attributes, options);
    Map<String, Object> a = new HashMap<>();
    FileTime t = FileTime.fromMillis(0);
    a.put("isRegularFile", true);
    a.put("isDirectory", false);
    a.put("isSymbolicLink", false);
    a.put("isOther", false);
    a.put("size", (long) l.body().length);
    a.put("lastModifiedTime", t);
    a.put("lastAccessTime", t);
    a.put("creationTime", t);
    a.put("fileKey", null);
    return a;
  }

  @Override
  public String getSeparator() {
    return delegate.getSeparator();
  }

  @Override
  public String getPathSeparator() {
    return delegate.getPathSeparator();
  }

  @Override
  public void setCurrentWorkingDirectory(Path currentWorkingDirectory) {
    delegate.setCurrentWorkingDirectory(currentWorkingDirectory);
  }

  @Override
  public Path getTempDirectory() {
    return delegate.getTempDirectory();
  }

  @Override
  public boolean isSameFile(Path path1, Path path2, LinkOption... options) throws IOException {
    if (path1.startsWith(ROOT) || path2.startsWith(ROOT)) return toRealPath(path1).equals(toRealPath(path2));
    return delegate.isSameFile(path1, path2, options);
  }

  @Override
  public String getMimeType(Path path) {
    return virtualOf(path) != null ? "application/javascript+module" : delegate.getMimeType(path);
  }

  @Override
  public java.nio.charset.Charset getEncoding(Path path) {
    return virtualOf(path) != null ? StandardCharsets.UTF_8 : delegate.getEncoding(path);
  }

  /** A read-only channel over a library body. */
  private static final class BytesChannel implements SeekableByteChannel {
    private final byte[] data;
    private int position;
    private boolean open = true;

    BytesChannel(byte[] data) {
      this.data = data;
    }

    @Override
    public int read(ByteBuffer dst) {
      if (position >= data.length) return -1;
      int n = Math.min(dst.remaining(), data.length - position);
      dst.put(data, position, n);
      position += n;
      return n;
    }

    @Override
    public int write(ByteBuffer src) {
      throw new NonWritableChannelException();
    }

    @Override
    public long position() {
      return position;
    }

    @Override
    public SeekableByteChannel position(long p) {
      position = (int) Math.min(p, data.length);
      return this;
    }

    @Override
    public long size() {
      return data.length;
    }

    @Override
    public SeekableByteChannel truncate(long size) {
      throw new NonWritableChannelException();
    }

    @Override
    public boolean isOpen() {
      return open;
    }

    @Override
    public void close() {
      open = false;
    }
  }
}
