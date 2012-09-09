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

#include <sys/types.h>
#include <sys/wait.h>

#include <process/delay.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>

#include <stout/foreach.hpp>

#include "slave/reaper.hpp"

using namespace process;

namespace mesos {
namespace internal {
namespace slave {

Reaper::Reaper()
  : ProcessBase(ID::generate("reaper")) {}


Reaper::~Reaper() {}


void Reaper::addProcessExitedListener(
    const PID<ProcessExitedListener>& listener)
{
  listeners.insert(listener);
}


void Reaper::initialize()
{
  delay(Seconds(1.0), self(), &Reaper::reap);
}


void Reaper::reap()
{
  // Check whether any child process has exited.
  pid_t pid;
  int status;
  if ((pid = waitpid((pid_t) -1, &status, WNOHANG)) > 0) {
    // Ignore this if the child process has only stopped.
    if (!WIFSTOPPED(status)) {
      foreach (const PID<ProcessExitedListener>& listener, listeners) {
        dispatch(listener, &ProcessExitedListener::processExited, pid, status);
      }
    }
  }

  delay(Seconds(1.0), self(), &Reaper::reap); // Reap forever!
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {


