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

#ifndef __FUTURE_TRACKER_HPP__
#define __FUTURE_TRACKER_HPP__

#include <list>
#include <map>
#include <string>
#include <vector>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/id.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>

namespace mesos {
namespace internal {

struct FutureMetadata
{
  std::string operation;
  std::string component;
  std::map<std::string, std::string> args;

  inline bool operator==(const FutureMetadata& that) const
  {
    return operation == that.operation &&
           component == that.component &&
           args == that.args;
  }
};


class PendingFutureTrackerProcess
  : public process::Process<PendingFutureTrackerProcess>
{
public:
  PendingFutureTrackerProcess()
    : ProcessBase(process::ID::generate("pending-future-tracker")) {}

  template <typename T>
  void addFuture(const process::Future<T>& future, FutureMetadata&& metadata)
  {
    auto it = pending.emplace(pending.end(), std::move(metadata));

    future
      .onAny(process::defer(
          self(), &PendingFutureTrackerProcess::eraseFuture, it))
      .onAbandoned(process::defer(
          self(), &PendingFutureTrackerProcess::eraseFuture, it));
  }

  void eraseFuture(typename std::list<FutureMetadata>::iterator it)
  {
    pending.erase(it);
  }

  process::Future<std::vector<FutureMetadata>> pendingFutures()
  {
    return std::vector<FutureMetadata>(pending.begin(), pending.end());
  }

private:
  std::list<FutureMetadata> pending;
};


class PendingFutureTracker
{
public:
  static Try<PendingFutureTracker*> create()
  {
    return new PendingFutureTracker(process::Owned<PendingFutureTrackerProcess>(
        new PendingFutureTrackerProcess));
  }

  ~PendingFutureTracker()
  {
    terminate(process.get());
    process::wait(process.get());
  }

  /**
   * This method subscribes on state transitions of the `future` to keep track
   * of pending operations/promises associated with this future.
   *
   * @param operation Operation's name identifies the place in the code related
   * to this future. E.g., "some/isolator::prepare".
   *
   * @param component Component is used to distinguish pending futures
   * related to different components so that they can be exposed by
   * different API endpoints.
   *
   * @param args A list of pairs <argument name, argument value> representing
   * arguments passed to the function that returned the given future.
   *
   * @return The same `future` which is passed as the first argument.
   */
  template <typename T>
  process::Future<T> track(
      const process::Future<T>& future,
      const std::string& operation,
      const std::string& component,
      const std::map<std::string, std::string>& args = {})
  {
    process::dispatch(
        process.get(),
        &PendingFutureTrackerProcess::addFuture<T>,
        future,
        FutureMetadata{operation, component, args});

    return future;
  }

  /**
   * This method returns a list of pending futures represented as objects of
   * `FutureMetadata` class, whose variables are initialized by the arguments
   * passed to the `track` method.
   */
  process::Future<std::vector<FutureMetadata>> pendingFutures()
  {
    return process::dispatch(
        process.get(),
        &PendingFutureTrackerProcess::pendingFutures);
  }

private:
  explicit PendingFutureTracker(
      const process::Owned<PendingFutureTrackerProcess>& _process)
    : process(_process)
  {
    spawn(process.get());
  }

  process::Owned<PendingFutureTrackerProcess> process;
};

} // namespace internal {
} // namespace mesos {

#endif // __FUTURE_TRACKER_HPP__
