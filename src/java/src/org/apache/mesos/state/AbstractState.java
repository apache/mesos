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

import java.util.Iterator;

import java.util.concurrent.ExecutionException;
import java.util.concurrent.Future;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.TimeUnit;

import org.apache.mesos.MesosNativeLibrary;

/**
 * Abstract implementation of State that provides operations on
 * futures to make concrete classes easier to create.
 */
public abstract class AbstractState implements State {
  static {
    MesosNativeLibrary.load();
  }

  @Override
  public Future<Variable> fetch(final String name) {
    final long future = __fetch(name); // Asynchronously start the operation.
    return new Future<Variable>() {
      @Override
      public boolean cancel(boolean mayInterruptIfRunning) {
        if (mayInterruptIfRunning) {
          return __fetch_cancel(future);
        }
        return false; // Should not interrupt and already running (or finished).
      }

      @Override
      public boolean isCancelled() {
        return __fetch_is_cancelled(future);
      }

      @Override
      public boolean isDone() {
        return __fetch_is_done(future);
      }

      @Override
      public Variable get() throws InterruptedException, ExecutionException {
        return __fetch_get(future);
      }

      @Override
      public Variable get(long timeout, TimeUnit unit)
        throws InterruptedException, ExecutionException, TimeoutException {
        return __fetch_get_timeout(future, timeout, unit);
      }

      @Override
      protected void finalize() {
        __fetch_finalize(future);
      }
    };
  }

  @Override
  public Future<Variable> store(Variable variable) {
    final long future = __store(variable); // Asynchronously start the operation.
    return new Future<Variable>() {
      @Override
      public boolean cancel(boolean mayInterruptIfRunning) {
        if (mayInterruptIfRunning) {
          return __store_cancel(future);
        }
        return false; // Should not interrupt and already running (or finished).
      }

      @Override
      public boolean isCancelled() {
        return __store_is_cancelled(future);
      }

      @Override
      public boolean isDone() {
        return __store_is_done(future);
      }

      @Override
      public Variable get() throws InterruptedException, ExecutionException {
        return __store_get(future);
      }

      @Override
      public Variable get(long timeout, TimeUnit unit)
        throws InterruptedException, ExecutionException, TimeoutException {
        return __store_get_timeout(future, timeout, unit);
      }

      @Override
      protected void finalize() {
        __store_finalize(future);
      }
    };
  }

  @Override
  public Future<Boolean> expunge(Variable variable) {
    final long future = __expunge(variable); // Asynchronously start the operation.
    return new Future<Boolean>() {
      @Override
      public boolean cancel(boolean mayInterruptIfRunning) {
        if (mayInterruptIfRunning) {
          return __expunge_cancel(future);
        }
        return false; // Should not interrupt and already running (or finished).
      }

      @Override
      public boolean isCancelled() {
        return __expunge_is_cancelled(future);
      }

      @Override
      public boolean isDone() {
        return __expunge_is_done(future);
      }

      @Override
      public Boolean get() throws InterruptedException, ExecutionException {
        return __expunge_get(future);
      }

      @Override
      public Boolean get(long timeout, TimeUnit unit)
        throws InterruptedException, ExecutionException, TimeoutException {
        return __expunge_get_timeout(future, timeout, unit);
      }

      @Override
      protected void finalize() {
        __expunge_finalize(future);
      }
    };
  }

  public Future<Iterator<String>> names() {
    final long future = __names(); // Asynchronously start the operation.
    return new Future<Iterator<String>>() {
      @Override
      public boolean cancel(boolean mayInterruptIfRunning) {
        if (mayInterruptIfRunning) {
          return __names_cancel(future);
        }
        return false; // Should not interrupt and already running (or finished).
      }

      @Override
      public boolean isCancelled() {
        return __names_is_cancelled(future);
      }

      @Override
      public boolean isDone() {
        return __names_is_done(future);
      }

      @Override
      public Iterator<String> get() throws  InterruptedException,
                                            ExecutionException {
        return __names_get(future);
      }

      @Override
      public Iterator<String> get(long timeout, TimeUnit unit)
        throws InterruptedException, ExecutionException, TimeoutException {
        return __names_get_timeout(future, timeout, unit);
      }

      @Override
      protected void finalize() {
        __names_finalize(future);
      }
    };
  }

  protected native void finalize();

  // Native implementations of 'fetch', 'store', 'expunge', and 'names'.
  private native long __fetch(String name);
  private native boolean __fetch_cancel(long future);
  private native boolean __fetch_is_cancelled(long future);
  private native boolean __fetch_is_done(long future);
  private native Variable __fetch_get(long future);
  private native Variable __fetch_get_timeout(
      long future, long timeout, TimeUnit unit);
  private native void __fetch_finalize(long future);

  private native long __store(Variable variable);
  private native boolean __store_cancel(long future);
  private native boolean __store_is_cancelled(long future);
  private native boolean __store_is_done(long future);
  private native Variable __store_get(long future);
  private native Variable __store_get_timeout(
      long future, long timeout, TimeUnit unit);
  private native void __store_finalize(long future);

  private native long __expunge(Variable variable);
  private native boolean __expunge_cancel(long future);
  private native boolean __expunge_is_cancelled(long future);
  private native boolean __expunge_is_done(long future);
  private native Boolean __expunge_get(long future);
  private native Boolean __expunge_get_timeout(
      long future, long timeout, TimeUnit unit);
  private native void __expunge_finalize(long future);

  private native long __names();
  private native boolean __names_cancel(long future);
  private native boolean __names_is_cancelled(long future);
  private native boolean __names_is_done(long future);
  private native Iterator<String> __names_get(long future);
  private native Iterator<String> __names_get_timeout(
      long future, long timeout, TimeUnit unit);
  private native void __names_finalize(long future);

  private long __storage;
  private long __state;
}
