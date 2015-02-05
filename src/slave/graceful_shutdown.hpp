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

#ifndef __SLAVE_GRACEFUL_SHUTDOWN_HPP__
#define __SLAVE_GRACEFUL_SHUTDOWN_HPP__

namespace mesos {
namespace slave {

// Slave           Exec          Executor
//  +               +               +
//  |               |               |
//  |               |               |
//  |   shutdown()  |               |
//  +-^------------->               |
//  | |             |   shutdown()  |
//  | |             +-^-------------> shutdown()
//  | |             | |             | ^
//  | |             | |             | |
//  | timeout       | timeout       | | timeout  <-- flags.
//  | level 2       | level 1       | | level 0        shutdown_grace_period
//  | |             | |             | v
//  | |             | |             | escalated()
//  | |             | v             |
//  | |             | ShutdownProcess
//  | |             | ::kill()      |
//  | v             |               |
//  | shutdownExecutorTimeout()     |
//  |               |               |
//  v               v               v
//  Containerizer->destroy()


// Returns the shutdown grace period for containerizer. We assume it
// is the 2nd and the last level in the shutdown chain.
Duration getContainerizerGracePeriod(const Duration& baseShutdownTimeout);


// Returns the shutdown grace period for ExecutorProcess. We assume it
// is the 1st level in the shutdown chain.
Duration getExecGracePeriod(const Duration& baseShutdownTimeout);


// Returns the shutdown grace period for an executor (e.g.
// CommandExecutorProcess). It is the 0 level in the shutdown chain.
Duration getExecutorGracePeriod(const Duration& baseShutdownTimeout);

} // namespace slave {
} // namespace mesos {

#endif // __SLAVE_GRACEFUL_SHUTDOWN_HPP__
