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

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

#include <stout/multihashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
class ReaperProcess;


// TODO(vinod): Pull reaper into common or libprocess.
class Reaper
{
public:
  Reaper();
  virtual ~Reaper();

  // Monitor the given process and notify the caller if it terminates
  // via a Future of the exit status.
  //
  // The exit status of 'pid' can only be correctly captured if the
  // calling process is the parent of 'pid' and the process hasn't
  // been reaped yet, otherwise 'None' is returned.
  // Note that an invalid pid does not cause a failed Future, but an
  // empty result ('None').
  process::Future<Option<int> > monitor(pid_t pid);

private:
  ReaperProcess* process;
};


// Reaper implementation.
class ReaperProcess : public process::Process<ReaperProcess>
{
public:
  ReaperProcess();

  process::Future<Option<int> > monitor(pid_t pid);

protected:
  virtual void initialize();

  void reap();

  // The notification is sent only if the pid is explicitly registered
  // via the monitor() call.
  void notify(pid_t pid, Option<int> status);

private:
  // Mapping from the monitored pid to all promises the pid exit
  // status should be sent to.
  multihashmap<
    pid_t, process::Owned<process::Promise<Option<int> > > > promises;
};


} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __REAPER_HPP__
