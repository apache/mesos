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

import java.util.concurrent.Future;

/**
 * An abstraction of "state" (possibly between multiple distributed
 * components) represented by "variables" (effectively key/value
 * pairs). Variables are versioned such that setting a variable in the
 * state will only succeed if the variable has not changed since last
 * fetched. Varying implementations of state provide varying
 * replicated guarantees.
 * <p>
 * Note that the semantics of 'fetch' and 'store' provide
 * atomicity. That is, you cannot store a variable that has changed
 * since you did the last fetch. That is, if a store succeeds then no
 * other writes have been performed on the variable since your fetch.
 *
 * Example:
 * <pre>
 * {@code
 *   State state = new ZooKeeperState();
 *   Future<Variable> variable = state.fetch("machines");
 *   Variable machines = variable.get();
 *   machines = machines.mutate(...);
 *   variable = state.store(machines);
 *   machines = variable.get();
 * }
 * </pre>
 */
public interface State {
  /**
   * Returns an immutable "variable" representing the current value
   * from the state associated with the specified name.
   *
   * @param name  The name of the variable.
   *
   * @return      A future of the variable.
   *
   * @see Variable
   */
  Future<Variable> fetch(String name);

  /**
   * Returns an immutable "variable" representing the current value in
   * the state if updating the specified variable in the state was
   * successful, otherwise returns null.
   *
   * @param variable  The variable to be stored.
   *
   * @return          A future of a variable with the new value on success,
   *                  or null on failure.
   *
   * @see Variable
   */
  Future<Variable> store(Variable variable);

  /**
   * Returns true if successfully expunged the variable from the state
   * or false if the variable did not exist or was no longer valid.
   *
   * @param variable  The variable to be expunged.
   *
   * @return          A future of true on success, false on failure.
   *
   * @see Variable
   */
  Future<Boolean> expunge(Variable variable);

  /**
   * Returns an iterator of variable names in the state.
   *
   * @return A future of an iterator over all variable names in the state.
   */
  Future<Iterator<String>> names();
}
