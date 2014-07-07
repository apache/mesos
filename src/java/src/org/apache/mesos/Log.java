/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package org.apache.mesos;

import java.io.Closeable;
import java.io.IOException;

import java.util.List;
import java.util.Set;

import java.util.concurrent.TimeoutException;
import java.util.concurrent.TimeUnit;

/**
 * Provides access to a distributed append only log. The log can be
 * read from using a {@link Log.Reader} and written to using a {@link
 * Log.Writer}.
 */
public class Log {
  static {
    MesosNativeLibrary.load();
  }

  /**
   * An opaque identifier of a log entry's position within the
   * log. Can be used to inidicate {@link Reader#read read} ranges and
   * {@link Writer#truncate truncation} locations.
   */
  public static class Position implements Comparable<Position> {
    @Override
    public int compareTo(Position that) {
      return Long.signum(value - that.value);
    }

    @Override
    public boolean equals(Object that) {
      return that instanceof Position && value == ((Position) that).value;
    }

    @Override
    public String toString() {
      return "Position " + value;
    }

    /**
     * Returns an "identity" of this position, useful for serializing
     * to logs or across communication mediums.
     */
    public byte[] identity() {
      byte[] bytes = new byte[8];
      bytes[0] = (byte) (0xff & (value >> 56));
      bytes[1] = (byte) (0xff & (value >> 48));
      bytes[2] = (byte) (0xff & (value >> 40));
      bytes[3] = (byte) (0xff & (value >> 32));
      bytes[4] = (byte) (0xff & (value >> 24));
      bytes[5] = (byte) (0xff & (value >> 16));
      bytes[6] = (byte) (0xff & (value >> 8));
      bytes[7] = (byte) (0xff & value);
      return bytes;
    }

    /**
     * Creates a position identified by an integral {@code value}.
     * <p>
     * Positions are typically only created by the log implementation. Log
     * users should only ever need to call this constructor in unit tests.
     *
     * @param value the marker for this position in the log.
     */
    public Position(long value) {
      this.value = value;
    }

    private final long value;
  }

  /**
   * Represents an opaque data entry in the {@link Log} with a {@link
   * Position}.
   */
  public static class Entry {
    public final Position position;
    public final byte[] data;

    /**
     * Creates a log entry.
     * <p>
     * Entries are typically only created by the log implementation. Log
     * users should only ever need to call this constructor in unit tests.
     *
     * @param position the unique position of this entry within the log.
     * @param data the content stored in this entry.
     */
    public Entry(Position position, byte[] data) {
      this.position = position;
      this.data = data;
    }
  }

  /**
   * An exception that gets thrown when an error occurs while
   * performing a read or write operation.
   */
  public static class OperationFailedException extends Exception {
    public OperationFailedException(String message) {
      super(message);
    }

    public OperationFailedException(String message, Throwable cause) {
      super(message, cause);
    }
  }

  /**
   * An exception that gets thrown when a writer no longer has the
   * ability to perform operations (e.g., because it was superseded by
   * another writer).
   */
  public static class WriterFailedException extends Exception {
    public WriterFailedException(String message) {
      super(message);
    }

    public WriterFailedException(String message, Throwable cause) {
      super(message, cause);
    }
  }

  /**
   * Provides read access to the {@link Log}. This class is safe for
   * use from multiple threads and for the life of the log regardless
   * of any exceptions thrown from its methods.
   */
  public static class Reader {
    public Reader(Log log) {
      this.log = log;
      initialize(log);
    }

    /**
     * Attempts to read from the log between the specified positions
     * (inclusive). If either of the positions are invalid, an
     * OperationFailedException will get thrown. Unfortunately, this
     * will also get thrown in other circumstances (e.g., disk
     * failure) and therefore it is currently impossible to tell these
     * two cases apart.
     */
    public native List<Entry> read(Position from,
                                   Position to,
                                   long timeout,
                                   TimeUnit unit)
      throws TimeoutException, OperationFailedException;

    /**
     * Returns the beginning position of the log (might be out of date
     * with respect to another replica).
     */
    public native Position beginning();

    /**
     * Returns the ending position of the log (might be out of date
     * with respect to another replica).
     */
    public native Position ending();

    protected native void initialize(Log log);

    protected native void finalize();

    private Log log; // Keeps the log from getting garbage collected.
    private long __log;
    private long __reader;
  }

  /**
   * Provides write access to the {@link Log}. This class is not safe
   * for use from multiple threads and instances should be thrown out
   * after any {@link WriterFailedException} is thrown.
   */
  public static class Writer {
    public Writer(Log log, long timeout, TimeUnit unit, int retries) {
      this.log = log;
      initialize(log, timeout, unit, retries);
    }

    /**
     * Attempts to append to the log with the specified data returning
     * the new end position of the log if successful.
     */
    public native Position append(byte[] data, long timeout, TimeUnit unit)
      throws TimeoutException, WriterFailedException;

    /**
     * Attempts to truncate the log (from the beginning to the
     * specified position exclusive) If the position is invalid, an
     * WriterFailedException will get thrown. Unfortunately, this will
     * also get thrown in other circumstances (e.g., disk failure) and
     * therefore it is currently impossible to tell these two cases
     * apart.
     *
     * <p>TODO(benh): Throw both OperationFailedException and
     * WriterFailedException to differentiate the need for a new
     * writer from a bad position, or a bad disk, etc.
     */
    public native Position truncate(Position to, long timeout, TimeUnit unit)
      throws TimeoutException, WriterFailedException;

    protected native void initialize(Log log,
                                     long timeout,
                                     TimeUnit unit,
                                     int retries);

    protected native void finalize();

    private Log log; // Keeps the log from getting garbage collected.
    private long __log;
    private long __writer;
  }

  public Log(int quorum,
             String path,
             Set<String> pids) {
    initialize(quorum, path, pids);
  }

  public Log(int quorum,
             String path,
             String servers,
             long timeout,
             TimeUnit unit,
             String znode) {
    initialize(quorum, path, servers, timeout, unit, znode);
  }

  public Log(int quorum,
             String path,
             String servers,
             long timeout,
             TimeUnit unit,
             String znode,
             String scheme,
             byte[] credentials) {
    initialize(quorum, path, servers, timeout, unit, znode, scheme, credentials);
  }

  /**
   * Returns a position based off of the bytes recovered from
   * Position.identity().
   */
  public Position position(byte[] identity) {
    long value =
      ((long) (identity[0] & 0xff) << 56) |
      ((long) (identity[1] & 0xff) << 48) |
      ((long) (identity[2] & 0xff) << 40) |
      ((long) (identity[3] & 0xff) << 32) |
      ((long) (identity[4] & 0xff) << 24) |
      ((long) (identity[5] & 0xff) << 16) |
      ((long) (identity[6] & 0xff) << 8) |
      ((long) (identity[7] & 0xff));
    return new Position(value);
  }

  protected native void initialize(int quorum,
                                   String path,
                                   Set<String> pids);

  protected native void initialize(int quorum,
                                   String path,
                                   String servers,
                                   long timeout,
                                   TimeUnit unit,
                                   String znode);

  protected native void initialize(int quorum,
                                   String path,
                                   String servers,
                                   long timeout,
                                   TimeUnit unit,
                                   String znode,
                                   String scheme,
                                   byte[] credentials);

  protected native void finalize();

  private long __log;
}
