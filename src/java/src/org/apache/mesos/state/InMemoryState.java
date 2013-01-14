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
import java.util.UUID;

import java.util.concurrent.Callable;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentMap;
import java.util.concurrent.Future;
import java.util.concurrent.FutureTask;

/**
 * An in-memory implementation of state.
 */
public class InMemoryState implements State {
  @Override
  public Future<Variable> fetch(String name) {
    Entry entry = entries.get(name); // Is null if doesn't exist.

    if (entry == null) {
      entry = new Entry();
      entry.name = name;
      entry.uuid = UUID.randomUUID();
      entry.value = new byte[0];

      // We use 'putIfAbsent' because multiple threads might be
      // attempting to fetch a "new" variable at the same time.
      if (entries.putIfAbsent(name, entry) != null) {
        return fetch(name);
      }
    }

    assert entry != null;

    return futureFrom((Variable) new InMemoryVariable(entry));
  }

  @Override
  public Future<Variable> store(Variable v) {
    InMemoryVariable variable = (InMemoryVariable) v;

    Entry entry = new Entry();
    entry.name = variable.entry.name;
    entry.uuid = UUID.randomUUID();
    entry.value = variable.value;

    if (entries.replace(entry.name, variable.entry, entry)) {
      return futureFrom((Variable) new InMemoryVariable(entry));
    }

    return futureFrom((Variable) null);
  }

  @Override
  public Future<Boolean> expunge(Variable v) {
    InMemoryVariable variable = (InMemoryVariable) v;

    return futureFrom(entries.remove(variable.entry.name, variable.entry));
  }

  @Override
  public Future<Iterator<String>> names() {
    return futureFrom(entries.keySet().iterator());
  }

  private static class InMemoryVariable extends Variable {
    private InMemoryVariable(Entry entry) {
      this(entry, null);
    }

    private InMemoryVariable(Entry entry, byte[] value) {
      this.entry = entry;
      this.value = value;
    }

    @Override
    public byte[] value() {
      if (this.value != null) {
        return this.value;
      } else {
        return this.entry.value;
      }
    }

    @Override
    public Variable mutate(byte[] value) {
      return new InMemoryVariable(entry, value);
    }

    final Entry entry;
    final byte[] value;
  }

  private static class Entry {
    @Override
    public boolean equals(Object that) {
      if (that instanceof Entry) {
        return uuid.equals(((Entry) that).uuid);
      }

      return false;
    }

    @Override
    public int hashCode() {
      return uuid.hashCode();
    }

    String name;
    UUID uuid;
    byte[] value;
  }

  private static <T> Future<T> futureFrom(final T t) {
    FutureTask<T> future = new FutureTask<T>(new Callable<T>() {
        public T call() {
          return t;
        }});
    future.run();
    return future;
  }

  private final ConcurrentMap<String, Entry> entries =
    new ConcurrentHashMap<String, Entry>();
}
