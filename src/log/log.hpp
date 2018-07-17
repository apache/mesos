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

#ifndef __LOG_LOG_HPP__
#define __LOG_LOG_HPP__

#include <stdint.h>

#include <mesos/log/log.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/process.hpp>
#include <process/shared.hpp>

#include <process/metrics/pull_gauge.hpp>

#include <stout/nothing.hpp>

#include "log/coordinator.hpp"
#include "log/metrics.hpp"
#include "log/network.hpp"
#include "log/recover.hpp"
#include "log/replica.hpp"

namespace mesos {
namespace internal {
namespace log {

class LogProcess : public process::Process<LogProcess>
{
public:
  LogProcess(
      size_t _quorum,
      const std::string& path,
      const std::set<process::UPID>& pids,
      bool _autoInitialize,
      const Option<std::string>& metricsPrefix);

  LogProcess(
      size_t _quorum,
      const std::string& path,
      const std::string& servers,
      const Duration& timeout,
      const std::string& znode,
      const Option<zookeeper::Authentication>& auth,
      bool _autoInitialize,
      const Option<std::string>& metricsPrefix);

  // Recovers the log by catching up if needed. Returns a shared
  // pointer to the local replica if the recovery succeeds.
  process::Future<process::Shared<Replica>> recover();

protected:
  void initialize() override;
  void finalize() override;

private:
  friend class LogReaderProcess;
  friend class LogWriterProcess;

  // Continuations.
  void _recover();

  // Return true if the log has finished recovery.
  double _recovered();

  // TODO(benh): Factor this out into "membership renewer".
  void watch(
      const process::UPID& pid,
      const std::set<zookeeper::Group::Membership>& memberships);

  void failed(const std::string& message);
  void discarded();

  const size_t quorum;
  process::Shared<Replica> replica;
  process::Shared<Network> network;
  const bool autoInitialize;

  // For replica recovery.
  Option<process::Future<process::Owned<Replica>>> recovering;
  process::Promise<Nothing> recovered;
  std::list<process::Promise<process::Shared<Replica>>*> promises;

  // For renewing membership. We store a Group instance in order to
  // continually renew the replicas membership (when using ZooKeeper).
  zookeeper::Group* group;
  process::Future<zookeeper::Group::Membership> membership;

  friend Metrics;
  Metrics metrics;

  // The size of the network. We use "ensemble" because it as a metric
  // name more intuitively means the "replica set".
  process::Future<double> _ensemble_size()
  {
    // Watching for any value different than 0 should give us the
    // current value.
    return network->watch(0u)
      .then([](size_t size) -> double { return size; });
  }
};


class LogReaderProcess : public process::Process<LogReaderProcess>
{
public:
  explicit LogReaderProcess(mesos::log::Log* log);

  process::Future<mesos::log::Log::Position> beginning();
  process::Future<mesos::log::Log::Position> ending();

  process::Future<std::list<mesos::log::Log::Entry>> read(
      const mesos::log::Log::Position& from,
      const mesos::log::Log::Position& to);

  process::Future<mesos::log::Log::Position> catchup();

protected:
  void initialize() override;
  void finalize() override;

private:
  // Returns a position from a raw value.
  static mesos::log::Log::Position position(uint64_t value);

  // Returns a future which gets set when the log recovery has
  // finished (either succeeded or failed).
  process::Future<Nothing> recover();

  // Continuations.
  void _recover();

  process::Future<mesos::log::Log::Position> _beginning();
  process::Future<mesos::log::Log::Position> _ending();

  process::Future<std::list<mesos::log::Log::Entry>> _read(
      const mesos::log::Log::Position& from,
      const mesos::log::Log::Position& to);

  process::Future<std::list<mesos::log::Log::Entry>> __read(
      const mesos::log::Log::Position& from,
      const mesos::log::Log::Position& to,
      const std::list<Action>& actions);

  process::Future<mesos::log::Log::Position> _catchup();

  const size_t quorum;
  const process::Shared<Network> network;

  process::Future<process::Shared<Replica>> recovering;
  std::list<process::Promise<Nothing>*> promises;
};


class LogWriterProcess : public process::Process<LogWriterProcess>
{
public:
  explicit LogWriterProcess(mesos::log::Log* log);

  process::Future<Option<mesos::log::Log::Position>> start();
  process::Future<Option<mesos::log::Log::Position>> append(
      const std::string& bytes);
  process::Future<Option<mesos::log::Log::Position>> truncate(
      const mesos::log::Log::Position& to);

protected:
  void initialize() override;
  void finalize() override;

private:
  // Helper for converting an optional position returned from the
  // coordinator into a Log::Position.
  static Option<mesos::log::Log::Position> position(
      const Option<uint64_t>& position);

  // Returns a future which gets set when the log recovery has
  // finished (either succeeded or failed).
  process::Future<Nothing> recover();

  // Continuations.
  void _recover();

  process::Future<Option<mesos::log::Log::Position>> _start();
  Option<mesos::log::Log::Position> __start(const Option<uint64_t>& position);

  void failed(const std::string& message, const std::string& reason);

  const size_t quorum;
  const process::Shared<Network> network;

  process::Future<process::Shared<Replica>> recovering;
  std::list<process::Promise<Nothing>*> promises;

  Coordinator* coordinator;
  Option<std::string> error;
};

} // namespace log {
} // namespace internal {
} // namespace mesos {

#endif // __LOG_LOG_HPP__
