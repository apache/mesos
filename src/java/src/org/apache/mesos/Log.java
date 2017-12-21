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
 * read from using a {@link Log.Reader} and written to using a
 * {@link Log.Writer}.
 *
 * <p>Both the <i>Reader</i> and <i>Writer</i> will require a <i>quorum</i>
 * which defines the <i>ratio of active Mesos Masters</i> that need to be
 * available for a successful read or write. The <i>quorum</i> will be satisfied
 * when the number of <i>active Masters</i> is greater than the given
 * <i>number</i>:
 * <pre>{@code
 *  Quorum > (Number of Masters)/2
 * }</pre>
 *
 * <p>If a <i>read</i> or <i>write</i> is executed the operation will wait
 * until their is <i>quorum</i> to succeed.
 */
public class Log {
  static {
    MesosNativeLibrary.load();
  }

  /**
   * An opaque identifier of a log entry's position within the
   * log. Can be used to inidicate {@link Log.Reader#read read} ranges and
   * {@link Log.Writer#truncate truncation} locations.
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
     *
     * @return The identity in bytes.
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
     * @param value The marker for this position in the log.
     */
    public Position(long value) {
      this.value = value;
    }

    private final long value;
  }

  /**
   * Represents an opaque data entry in the {@link Log} with a
   * {@link Log.Position}.
   */
  public static class Entry {
    /**
     * The position of this entry.
     * @see Position
     */
    public final Position position;
    /** The data at the given position.*/
    public final byte[] data;

    /**
     * Creates a log entry.
     * <p>
     * Entries are typically only created by the log implementation. Log
     * users should only ever need to call this constructor in unit tests.
     *
     * @param position  The unique position of this entry within the log.
     * @param data      The content stored in this entry.
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
    /**
     * @param message   The message for this exception.
     */
    public OperationFailedException(String message) {
      super(message);
    }

    /**
     * @param message   The message for this exception.
     * @param cause     The underlying reason this exception was generated.
     */
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
    /**
     * @param message   The message for this exception.
     */
    public WriterFailedException(String message) {
      super(message);
    }

    /**
     * @param message   The message for this exception.
     * @param cause     The underlying reason this exception was generated.
     */
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
    /**
     * Returns an instance of a reader that will access the given instance of
     * the Log.
     * @param log The log that this reader will access.
     */
    public Reader(Log log) {
      this.log = log;
      initialize(log);
    }

    /**
     * Attempts to read from the log between the specified positions
     * (inclusive). If either of the positions are invalid, an
     * {@link OperationFailedException} will get thrown. Unfortunately, this
     * will also get thrown in other circumstances (e.g., disk
     * failure) and therefore it is currently impossible to tell these
     * two cases apart.
     *
     * @param from    Where to start reading.
     * @param to      Where to finish reading.
     * @param timeout Max number of time units to wait before a
     *                {@link TimeoutException}.
     * @param unit    Type of units used for the timeout, e.g. seconds,
     *                minutes, etc.
     *
     * @return        The list of entries fetched from the Log.
     *
     * @throws TimeoutException         If the read doesn't happen before the
     *                                  timeout.
     * @throws OperationFailedException If the read fails due that the read no
     *                                  longer has the ability to perform its
     *                                  operations.
     * @see Position
     * @see TimeUnit
     */
    public native List<Entry> read(Position from,
                                   Position to,
                                   long timeout,
                                   TimeUnit unit)
      throws TimeoutException, OperationFailedException;

    /**
     * Returns the beginning position of the log (might be out of date
     * with respect to another replica).
     *
     * @return The beginning position of the log.
     */
    public native Position beginning();

    /**
     * Returns the ending position of the log (might be out of date
     * with respect to another replica).
     *
     * @return The ending position of the log
     */
    public native Position ending();

    /**
     * Attempts to catch-up positions from the log for reading.
     *
     * @param timeout Max number of time units to wait before a
     *                {@link TimeoutException}.
     * @param unit    Type of time units used for the timeout, e.g. seconds,
     *                minutes, etc.
     *
     * @return The ending position of the caught-up range.
     *
     * @throws TimeoutException         If the catch-up doesn't happen before
     *                                  the timeout.
     * @throws OperationFailedException If the catch-up fails.
     */
    public native Position catchup(long timeout, TimeUnit unit)
      throws TimeoutException, OperationFailedException;

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
    /**
     * Constructs a writer linked the given {@link Log}.
     *
     * @param log     The log that this writer will access.
     * @param timeout Max number of time units to wait before a
     *                {@link TimeoutException}.
     * @param unit    Type of time units used for the timeout, e.g. seconds,
     *                minutes, etc.
     * @param retries Number of retries
     *
     * @see TimeUnit
     */
    public Writer(Log log, long timeout, TimeUnit unit, int retries) {
      this.log = log;
      initialize(log, timeout, unit, retries);
    }

    /**
     * Attempts to append to the log with the specified data returning
     * the new end position of the log if successful.
     *
     * @param data    Data to append to the log.
     * @param timeout Max number of time units to wait before a
     *                {@link TimeoutException}.
     * @param unit    Type of time units used for the timeout, e.g. seconds,
     *                minutes, etc.
     *
     * @return        The new end-position.
     *
     * @throws TimeoutException       If the append doesn't happen before the
     *                                timeout.
     * @throws WriterFailedException  If the append fails due that the writer
     *                                no longer has the ability to perform its
     *                                operations (e.g., because it was
     *                                superseded by another writer).
     * @see TimeUnit
     * @see WriterFailedException
     */
    public native Position append(byte[] data, long timeout, TimeUnit unit)
      throws TimeoutException, WriterFailedException;

    /**
     * Attempts to truncate the log (from the beginning to the
     * specified position exclusive) If the position is invalid, an
     * {@link WriterFailedException} will get thrown. Unfortunately, this will
     * also get thrown in other circumstances (e.g., disk failure) and
     * therefore it is currently impossible to tell these two cases
     * apart.
     *

     * @param to      The log will be truncated up to this point.
     * @param timeout Max number of time units to wait before a
     *                {@link TimeoutException}.
     * @param unit    Type of time units used for the timeout, e.g. seconds,
     *                minutes, etc.
     *
     * @return        The position after the truncation.
     *
     * @throws TimeoutException       If the truncation doesn't happen before
     *                                the timeout.
     * @throws WriterFailedException  If the truncation fails due an invalid
     *                                position or if the writer no longer has
     *                                the ability to perform its operations
     *                                (e.g., because it was superseded by
     *                                another writer).
     */
    // TODO(benh):  Throw both OperationFailedException and WriterFailedException
    //              to differentiate the need for a new writer from a bad
    //              position, or a bad disk, etc.
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

  /**
   * Creates a new replicated log that assumes the specified quorum
   * size, is backed by a file at the specified path, and coordiantes
   * with other replicas via the set of process PIDs.
   *
   * @param quorum  The quorum size.
   * @param path    Path to the file backing this log.
   * @param pids    PIDs of the replicas to coordinate with.
   */
  public Log(int quorum,
             String path,
             Set<String> pids) {
    initialize(quorum, path, pids);
  }

  /**
   * Creates a new replicated log that assumes the specified quorum
   * size, is backed by a file at the specified path, and coordiantes
   * with other replicas associated with the specified ZooKeeper
   * servers, timeout, and znode (or Zookeeper name space).
   *
   * @param quorum  The quorum size.
   * @param path    Path to the file backing this log.
   * @param servers List of ZooKeeper servers (e.g., 'ip1:port1,ip2:port2').
   * @param timeout Max number of time units to wait before a
   *                {@link TimeoutException}.
   * @param unit    Type of time units used for the timeout, e.g. seconds,
   *                minutes, etc.
   * @param znode   Path to znode where "state" should be rooted.
   */
  public Log(int quorum,
             String path,
             String servers,
             long timeout,
             TimeUnit unit,
             String znode) {
    initialize(quorum, path, servers, timeout, unit, znode);
  }

  /**
   * Creates a new replicated log that assumes the specified quorum
   * size, is backed by a file at the specified path, and coordiantes
   * with other replicas associated with the specified ZooKeeper
   * servers, timeout, and znode (or Zookeeper name space).
   *
   * @param quorum      The quorum size.
   * @param path        Path to the file backing this log.
   * @param servers     Zookeper servers/connection string.
   * @param timeout     Max number of time units to wait before a
   *                    {@link TimeoutException}.
   * @param unit        Type of time units used for the timeout, e.g. seconds,
   *                    minutes, etc.
   * @param znode       The Zookeeper name space.
   * @param scheme      Authentication scheme (e.g., "digest").
   * @param credentials Authentication credentials (e.g., "user:pass").
   */
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
   *
   * @param identity    Identity, in bytes, of the position.
   *
   * @return            The position.
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
