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

  /** A library body the worker has. */
  record Library(String language, byte[] body, long hash) {}

  static final String ROOT = "/.mle-libraries";
  private static final Pattern ORACLE_PATH = Pattern.compile("/([^/.]+)/([^/.]+)");
  private static final Pattern VIRTUAL = Pattern.compile(
      Pattern.quote(ROOT) + "/([^/]+)/([^/.]+)(?:\\.([0-9a-f]+))?\\.mjs");

  private final FileSystem delegate;
  /** By "schema/name": the newest body seen. */
  private final Map<String, Library> libraries = new HashMap<>();

  LibraryFileSystem(FileSystem delegate) {
    this.delegate = delegate;
  }

  /** Python and Java libraries need the body itself (see Polyglot). */
  Library library(String schema, String name) throws IOException {
    return fetch(schema, name);
  }

  private Library fetch(String schema, String name) throws IOException {
    Dispatcher d = SqlBridge.dispatcher;
    String key = schema + "/" + name;
    Library known = libraries.get(key);
    if (d == null) throw new NoSuchFileException("/" + key, null, "no database libraries here");
    Reply r = d.libraryRequest(schema, name, known == null ? 0 : known.hash());
    switch (r.status()) {
      case Wire.LIBRARY_UNCHANGED -> {
        return known;
      }
      case Wire.LIBRARY_BODY -> {
        Library l = new Library(r.language(), r.body().getBytes(StandardCharsets.UTF_8), r.hash());
        libraries.put(key, l);
        return l;
      }
      default -> throw new NoSuchFileException("/" + key, null, r.message());
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
    Library l = libraries.get(m.group(1) + "/" + m.group(2));
    // A versioned path names a body the worker has; an import (unversioned) asks mysqld, which
    // answers "unchanged" (no body) or with the new body, or refuses (dropped, not importable).
    if (l == null || m.group(3) == null || !Long.toHexString(l.hash()).equals(m.group(3)))
      l = fetch(m.group(1), m.group(2));
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
    return Path.of(ROOT, m.group(1), m.group(2) + "." + Long.toHexString(l.hash()) + ".mjs");
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
