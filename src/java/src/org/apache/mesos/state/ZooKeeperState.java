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

package org.apache.mesos.state;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.TimeUnit;

import org.apache.mesos.MesosNativeLibrary;

/**
 * Implementation of State that uses ZooKeeper to store
 * variables/values. Note that this means the values associated with
 * variables can not be more than 1 MB (actually slightly less since
 * we store some bookkeeping information).
 */
class ZooKeeperState implements State {
  static {
    MesosNativeLibrary.load();
  }

  /**
   * Constructs a new instance of ZooKeeperState.
   *
   * @param servers List of ZooKeeper servers, e.g., 'ip1:port1,ip2:port2'.
   * @param timeout ZooKeeper session timeout.
   * @param unit Unit for session timeout.
   * @param znode Path to znode where "state" should be rooted.
   */
  public ZooKeeperState(String servers,
                        long timeout,
                        TimeUnit unit,
                        String znode) {
    initialize(servers, timeout, unit, znode);
  }

  /**
   * Constructs a new instance of ZooKeeperState.
   *
   * @param servers List of ZooKeeper servers (e.g., 'ip1:port1,ip2:port2').
   * @param timeout ZooKeeper session timeout.
   * @param unit Unit for session timeout.
   * @param znode Path to znode where "state" should be rooted.
   * @param scheme Authentication scheme (e.g., "digest").
   * @param scheme Authentication credentials (e.g., "user:pass").
   */
  public ZooKeeperState(String servers,
                        long timeout,
                        TimeUnit unit,
                        String znode,
                        String scheme,
                        byte[] credentials) {
    initialize(servers, timeout, unit, znode, scheme, credentials);
  }

  @Override
  public Future<Variable> get(final String name) {
    final long future = __get(name); // Asynchronously start the operation.
    return new Future<Variable>() {
      @Override
      public boolean cancel(boolean mayInterruptIfRunning) {
        if (mayInterruptIfRunning) {
          return __get_cancel(future);
        }
        return false; // Should not interrupt and already running (or finished).
      }

      @Override
      public boolean isCancelled() {
        return __get_is_cancelled(future);
      }

      @Override
      public boolean isDone() {
        return __get_is_done(future);
      }

      @Override
      public Variable get() throws InterruptedException, ExecutionException {
        return __get_await(future);
      }

      @Override
      public Variable get(long timeout, TimeUnit unit)
        throws InterruptedException, ExecutionException, TimeoutException {
        return __get_await_timeout(future, timeout, unit);
      }

      @Override
      protected void finalize() {
        __get_finalize(future);
      }
    };
  }

  @Override
  public Future<Variable> set(Variable variable) {
    final long pair = __set(variable); // Asynchronously start the operation.
    return new Future<Variable>() {
      @Override
      public boolean cancel(boolean mayInterruptIfRunning) {
        if (mayInterruptIfRunning) {
          return __set_cancel(pair);
        }
        return false; // Should not interrupt and already running (or finished).
      }

      @Override
      public boolean isCancelled() {
        return __set_is_cancelled(pair);
      }

      @Override
      public boolean isDone() {
        return __set_is_done(pair);
      }

      @Override
      public Variable get() throws InterruptedException, ExecutionException {
        return __set_await(pair);
      }

      @Override
      public Variable get(long timeout, TimeUnit unit)
        throws InterruptedException, ExecutionException, TimeoutException {
        return __set_await_timeout(pair, timeout, unit);
      }

      @Override
      protected void finalize() {
        __set_finalize(pair);
      }
    };
  }

  protected native void initialize(String servers,
                                   long timeout,
                                   TimeUnit unit,
                                   String znode);

  protected native void initialize(String servers,
                                   long timeout,
                                   TimeUnit unit,
                                   String znode,
                                   String scheme,
                                   byte[] credentials);

  protected native void finalize();

  // Native implementations of get/set.
  private native long __get(String name);
  private native boolean __get_cancel(long future);
  private native boolean __get_is_cancelled(long future);
  private native boolean __get_is_done(long future);
  private native Variable __get_await(long future);
  private native Variable __get_await_timeout(
      long future, long timeout, TimeUnit unit);
  private native void __get_finalize(long future);

  private native long __set(Variable variable);
  private native boolean __set_cancel(long pair);
  private native boolean __set_is_cancelled(long pair);
  private native boolean __set_is_done(long pair);
  private native Variable __set_await(long pair);
  private native Variable __set_await_timeout(
      long pair, long timeout, TimeUnit unit);
  private native void __set_finalize(long pair);

  private long __state;
};
