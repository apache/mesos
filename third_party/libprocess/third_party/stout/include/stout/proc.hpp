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

#ifndef __PROC_HPP__
#define __PROC_HPP__

#include <errno.h>
#include <signal.h>

#include <glog/logging.h>

#include <sys/types.h> // For pid_t.

#include <string>

#include <stout/try.hpp>

namespace proc {

inline Try<bool> alive(pid_t pid)
{
  CHECK(pid > 0);

  if (kill(pid, 0) == 0) {
    return true;
  }

  if (errno == ESRCH) {
    return false;
  }

  return Try<bool>::error(strerror(errno));
}

} // namespace proc {

#endif // __PROC_HPP__
