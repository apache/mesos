// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __MESOS_CONTAINERIZER_UTILS_HPP__
#define __MESOS_CONTAINERIZER_UTILS_HPP__

#include <mesos/mesos.hpp>

#include <stout/os.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {

#ifdef __linux__
Try<pid_t> getMountNamespaceTarget(pid_t parent);
#endif // __linux__

} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESOS_CONTAINERIZER_UTILS_HPP__
