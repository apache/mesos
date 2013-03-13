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

#ifndef __REAPER_HPP__
#define __REAPER_HPP__

#include <list>
#include <set>

#include <process/process.hpp>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {

class ProcessExitedListener : public process::Process<ProcessExitedListener>
{
public:
  virtual void processExited(pid_t pid, int status) = 0;
};


// TODO(vinod): Refactor the Reaper into 2 components:
// 1) Reaps the status of child processes.
// 2) Checks the exit status of requested processes.
// Also, use Futures instead of callbacks to notify process exits.
class Reaper : public process::Process<Reaper>
{
public:
  Reaper();
  virtual ~Reaper();

  void addListener(const process::PID<ProcessExitedListener>&);

  // Monitor the given process and notify the listener if it terminates.
  // NOTE: A notification is only sent if the calling process:
  // 1) is the parent of 'pid' or
  // 2) has the same real/effective UID as that of 'pid' or
  // 3) is run as a privileged user.
  Try<Nothing> monitor(pid_t pid);

protected:
  virtual void initialize();

  void reap();

  // TOOD(vinod): Make 'status' an option.
  void notify(pid_t pid, int status);

private:
  std::list<process::PID<ProcessExitedListener> > listeners;
  std::set<pid_t> pids;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __REAPER_HPP__
