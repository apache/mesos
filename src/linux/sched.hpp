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

#ifndef __LINUX_SCHED_HPP__
#define __LINUX_SCHED_HPP__

// This file contains Linux-only OS utilities.
#ifndef __linux__
#error "linux/sched.hpp is only available on Linux systems."
#endif

#include <sched.h>
// Some old distributions, e.g., Redhat 5.5, do not correctly include
// linux/sched.h.
#include <linux/sched.h>


#include <sys/types.h>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace sched {

enum Policy
{
  OTHER = SCHED_OTHER,  // Default policy.
  BATCH = SCHED_BATCH,  // Non-interactive, CPU intensive.
  IDLE  = SCHED_IDLE,   // Very low priority.
  FIFO  = SCHED_FIFO,   // Realtime.
  RR    = SCHED_RR      // Realtime, round-robin.
};


namespace policy {

// Return the current scheduling policy for the specified pid, or for
// the caller if pid is None() or zero.
inline Try<Policy> get(const Option<pid_t>& pid = None())
{
  int status = sched_getscheduler(pid.isSome() ? pid.get() : 0);

  if (status == -1) {
    return ErrnoError("Failed to get scheduler policy");
  }

  return static_cast<Policy>(status);
}


// Set the scheduling policy for the specified pid, or for the caller
// if pid is None() or zero. Only realtime scheduling policies (FIFO
// and RR) accept a priority (within a range, see
// sched_setscheduler(2)).
// TODO(idownes): Add wrappers around sched_get_priority_{min,max}.
inline Try<Nothing> set(
    Policy policy,
    const Option<pid_t>& pid = None(),
    int priority = 0)
{
  if ((policy == OTHER || policy == BATCH || policy == IDLE) &&
      priority != 0) {
    return Error("Non-real-time scheduling policies only support priority = 0");
  }

  sched_param param;
  param.sched_priority = priority;

  if (sched_setscheduler(pid.isSome() ? pid.get() : 0, policy, &param) == -1) {
    return ErrnoError("Failed to set scheduler policy");
  }

  return Nothing();
}

} // namespace policy {
} // namespace sched {

#endif // __LINUX_SCHED_HPP__
