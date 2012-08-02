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

import java.util.concurrent.Future;

/**
 * An abstraction of "state" (possibly between multiple distributed
 * components) represented by "variables" (effectively key/value
 * pairs). Variables are versioned such that setting a variable in the
 * state will only succeed if the variable has not changed since last
 * fetched. Varying implementations of state provide varying
 * replicated guarantees.
 *
 * Note that the semantics of 'get' and 'set' provide atomicity. That
 * is, you can not set a variable that has changed since you did the
 * last get. That is, if a set succeeds then no other writes have been
 * performed on the variable since your get.
 *
 * Example:
 *
 *   State state = new ZooKeeperState();
 *   Future<Variable> variable = state.get("machines");
 *   Variable machines = variable.get();
 *   machines.mutate(...);
 *   Future<Boolean> set = state.set(machines);
 */
public interface State {
  /**
   * Returns a "variable" representing the value from the state
   * associated with the specified name.
   */
  Future<Variable> get(String name);

  /**
   * Returns true if successfully updating the specified "variable"
   * in the state.
   */
  Future<Boolean> set(Variable variable);
}
