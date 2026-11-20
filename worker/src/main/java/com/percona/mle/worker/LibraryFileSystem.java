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
 * Database libraries (CREATE LIBRARY, Phase 6, 14-phase6-libraries.md §2; protocol §9.10) as ES
 * modules: Oracle's module paths "/schema/library" are served from mysqld (LIBRARY messages);
 * everything else goes to the worker's read-only file system.
 *
 * <p>"/db/lib" (two segments, no such directory on disk) becomes the virtual
 * "/.mle-libraries/db/lib.mjs"; its real path carries the body's version
 * ("/.mle-libraries/db/lib.STAMP.mjs"), so the language's module cache keeps one module per
 * version: an altered library is a new module at the next import, an unchanged one is evaluated
 * once per worker. Every import, also of a module the language has cached, comes through here
 * and is resolved under an answer of mysqld, which decides which libraries a routine may import.
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

  /** mysqld's answer to a LIBRARY message. */
  record Reply(int status, String language, String body, long version, String message) {}

  /**
   * A library body the worker has, with the version mysqld gave it. generation and loadset: see
   * {@link #generation}.
   */
  record Library(String language, byte[] body, long version, long generation, Map<String, Long> loadset) {
    /**
     * What the caches of evaluated libraries go by (the module's path, the namespaces kept by
     * Polyglot and PySql): the body's version, and another value in every generation.
     */
    long stamp() {
      return generation == 0 ? version : version ^ (generation * 0x9E3779B97F4A7C15L);
    }
  }

  /**
   * An answer BODY or UNCHANGED of mysqld (§9.10): it belongs to one routine id and one library
   * and holds for a request of that id with the library epoch it was given under. generation: the
   * generation in which the library's loadset was last confirmed for the routine.
   */
  private static final class Answer {
    final long version;
    final long epoch;
    long generation;

    Answer(long version, long epoch, long generation) {
      this.version = version;
      this.epoch = epoch;
      this.generation = generation;
    }
  }

  /** The answers held, by routine id and "schema/name": at most one per id and library. */
  private static final Map<Long, Map<String, Answer>> answers = new HashMap<>();

  /** A PREPARE of routine id begins, or the id is dropped: its answers are discarded. */
  static void discard(long id) {
    answers.remove(id);
  }

  /** RESET: no answer is held any more. The bodies and what was evaluated stay. */
  static void reset() {
    answers.clear();
  }

  /**
   * An evaluated library is kept (the language's module map, the namespaces): its imports of other
   * libraries are bound to the modules they were then, and were resolved under answers for the
   * routine that ran then. A later request holds or gets an answer for the library itself, and
   * would never ask about what it imported: a changed or dropped library would stay behind an
   * unchanged importer, and a routine would get, through an importer evaluated for another
   * routine, a library mysqld refuses it.
   *
   * <p>So each library has a loadset: every library (with its version) fetched in the top-level
   * request that first evaluated it, from then on, and the loadsets of those: at least what it
   * imports, directly or not. When a routine gets an answer for a library, each member of its
   * loadset is resolved for the same routine and request too (asked for unless an answer holds).
   * If one is refused or has another version, a new generation starts: every library gets a new
   * stamp at its next use, so it is evaluated (linked) again, its imports are resolved again (a
   * refusal is then the import's error), and its loadset is collected again. Requests that change
   * nothing pay nothing: no message to mysqld, no new evaluation.
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
      s.put(key, l.version());
      s.putAll(l.loadset());
    }
  }

  /** l as a library of the current generation, to be evaluated again: a new stamp and loadset. */
  private static Library renewed(String key, Library l) {
    Library n = new Library(l.language(), l.body(), l.version(), generation, new HashMap<>());
    libraries.put(key, n);
    touch(key, n);
    open.add(n.loadset());
    return n;
  }

  /** The libraries evaluated with l are resolved for the running routine, or a generation starts. */
  private static void confirmLoadset(Library l) {
    for (Map.Entry<String, Long> e : java.util.List.copyOf(l.loadset().entrySet())) {
      int slash = e.getKey().indexOf('/');
      try {
        if (fetch(e.getKey().substring(0, slash), e.getKey().substring(slash + 1)).version() == e.getValue())
          continue;
      } catch (IOException refused) {
        // dropped, or not for this routine: l is linked anew, and its import fails then
      }
      generation++;
      return;
    }
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

  static final String ROOT = "/.mle-libraries";
  private static final Pattern ORACLE_PATH = Pattern.compile("/([^/.]+)/([^/.]+)");
  private static final Pattern VIRTUAL = Pattern.compile(
      Pattern.quote(ROOT) + "/([^/]+)/([^/.]+)(?:\\.([0-9a-f]+))?\\.mjs");

  private final FileSystem delegate;
  /** By "schema/name": the most recent body the worker has (one Context per worker). */
  private static final Map<String, Library> libraries = new HashMap<>();

  LibraryFileSystem(FileSystem delegate) {
    this.delegate = delegate;
  }

  /** The library as the request being served may use it (every language's imports end here). */
  static Library library(String schema, String name) throws IOException {
    return fetch(schema, name);
  }

  /** A library's schema or name (§9.10): 1 to 256 bytes of well-formed text without U+0000. */
  private static boolean name(String s) {
    if (s.isEmpty() || s.length() > Wire.MAX_LIBRARY_NAME || s.indexOf('\0') >= 0 || !Text.wellFormed(s)) return false;
    return s.getBytes(StandardCharsets.UTF_8).length <= Wire.MAX_LIBRARY_NAME;
  }

  /**
   * Resolves an import of schema/name for the routine whose request is served (§9.10): under the
   * answer held for that routine id and the request's library epoch, or under a new answer of
   * mysqld when none holds or the worker no longer has that answer's body. A refusal fails the
   * import and changes no answer.
   */
  private static Library fetch(String schema, String name) throws IOException {
    Dispatcher d = SqlBridge.dispatcher;
    String key = schema + "/" + name;
    if (d == null) throw new NoSuchFileException("/" + key, null, "no database libraries here");
    // A name is not repaired or cut: another name would be another library.
    if (!name(schema) || !name(name))
      throw new NoSuchFileException("/" + key, null, "not a library's schema and name");
    long[] scope;
    try {
      scope = d.libraryScope();
    } catch (IllegalStateException e) {
      throw new NoSuchFileException("/" + key, null, e.getMessage());
    }
    long routine = scope[0], epoch = scope[1];
    Library known = libraries.get(key);
    Map<String, Answer> held = answers.get(routine);
    Answer answer = held == null ? null : held.get(key);
    if (known != null && answer != null && answer.epoch == epoch && answer.version == known.version()) {
      if (known.generation() != generation) {
        answer.generation = generation;  // evaluated again: this routine resolves its imports
        return renewed(key, known);
      }
      if (answer.generation != generation) {
        // Evaluated again for another routine since: its imports are resolved for this one.
        answer.generation = generation;
        touch(key, known);
        confirmLoadset(known);
        return known.generation() != generation ? renewed(key, known) : known;
      }
      if (!open.isEmpty()) touch(key, known);
      return known;
    }
    Reply r = d.libraryRequest(schema, name, known == null ? 0 : known.version());
    switch (r.status()) {
      case Wire.LIBRARY_UNCHANGED -> {
        answers.computeIfAbsent(routine, k -> new HashMap<>()).put(key, new Answer(known.version(), epoch, generation));
        if (known.generation() == generation) {
          touch(key, known);
          confirmLoadset(known);
        }
        return known.generation() != generation ? renewed(key, known) : known;
      }
      case Wire.LIBRARY_BODY -> {
        Library l = new Library(r.language(), r.body().getBytes(StandardCharsets.UTF_8), r.version(), generation,
            new HashMap<>());
        libraries.put(key, l);
        answers.computeIfAbsent(routine, k -> new HashMap<>()).put(key, new Answer(l.version(), epoch, generation));
        touch(key, l);
        open.add(l.loadset());  // a new body: evaluated from here on
        return l;
      }
      default -> {
        // Refused while libraries are being evaluated: their evaluation fails, and what the
        // language keeps of them is not to be used: everything is evaluated again at its next use.
        if (!open.isEmpty()) generation++;
        throw new NoSuchFileException("/" + key, null, r.message());
      }
    }
  }

  /**
   * A JavaScript library is a module file "schema/name.mjs" (with its version's stamp before the
   * extension): a '.' or '/' in the names would read as another library's path.
   */
  static void checkJsName(String schema, String name) {
    if (name.isEmpty() || name.indexOf('.') >= 0 || name.indexOf('/') >= 0 || schema.indexOf('/') >= 0)
      throw new IllegalArgumentException("the JavaScript library " + schema + "." + name
          + " cannot be imported: a library's name must not contain '.' or '/'");
  }

  private static Path virtual(String schema, String name) {
    checkJsName(schema, name);
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
    // Every access, versioned path or not, is resolved for the importing routine and its request.
    Library l = fetch(m.group(1), m.group(2));
    if (!l.language().equals("js"))
      throw new NoSuchFileException(p.toString(), null, "the library " + m.group(1) + "." + m.group(2) + " is in the language "
          + l.language() + ": it cannot be imported as a JavaScript module");
    return l;
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
