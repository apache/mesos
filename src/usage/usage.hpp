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

#ifndef __USAGE_HPP__
#define __USAGE_HPP__

#include <unistd.h> // For pid_t.

#include "mesos/mesos.hpp"

namespace mesos {
namespace internal {

// Collects resource usage of a process tree rooted at 'pid'. Only
// collects the 'mem_*' values if 'mem' is true and the 'cpus_*'
// values if 'cpus' is true.
Try<ResourceStatistics> usage(pid_t pid, bool mem = true, bool cpus = true);

} // namespace internal {
} // namespace mesos {

#endif // __USAGE_HPP__
