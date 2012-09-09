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

#ifndef __NETWORK_HPP__
#define __NETWORK_HPP__

// TODO(benh): Eventually move and associate this code with the
// libprocess protobuf code rather than keep it here.

#include <set>
#include <string>

#include <process/collect.hpp>
#include <process/executor.hpp>
#include <process/protobuf.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>

#include "logging/logging.hpp"

#include "zookeeper/group.hpp"

// Forward declaration.
class NetworkProcess;

// A "network" is a collection of protobuf processes (may be local
// and/or remote). A network abstracts away the details of maintaining
// which processes are waiting to receive messages and requests in the
// presence of failures and dynamic reconfiguration.
class Network
{
public:
  Network();
  Network(const std::set<process::UPID>& pids);
  virtual ~Network();

  // Adds a PID to this network.
  void add(const process::UPID& pid);

  // Removes a PID from this network.
  void remove(const process::UPID& pid);

  // Set the PIDs that are part of this network.
  void set(const std::set<process::UPID>& pids);

  // Sends a request to each member of the network and returns a set
  // of futures that represent their responses.
  template <typename Req, typename Res>
  process::Future<std::set<process::Future<Res> > > broadcast(
      const Protocol<Req, Res>& protocol,
      const Req& req,
      const std::set<process::UPID>& filter = std::set<process::UPID>());

  // Sends a message to each member of the network.
  template <typename M>
  void broadcast(
      const M& m,
      const std::set<process::UPID>& filter = std::set<process::UPID>());

private:
  // Not copyable, not assignable.
  Network(const Network&);
  Network& operator = (const Network&);

  NetworkProcess* process;
};


class ZooKeeperNetwork : public Network
{
public:
  ZooKeeperNetwork(zookeeper::Group* group);

private:
  typedef ZooKeeperNetwork This;

  // Helper that sets up a watch on the group.
  void watch(const std::set<zookeeper::Group::Membership>& expected);

  // Invoked when the group memberships have changed.
  void watched();

  // Invoked when group members data has been collected.
  void collected();

  zookeeper::Group* group;
  process::Executor executor;
  process::Future<std::set<zookeeper::Group::Membership> > memberships;
  process::Future<std::list<std::string> > datas;
};


class NetworkProcess : public ProtobufProcess<NetworkProcess>
{
public:
  NetworkProcess() {}

  NetworkProcess(const std::set<process::UPID>& pids)
  {
    set(pids);
  }

  void add(const process::UPID& pid)
  {
    link(pid); // Try and keep a socket open (more efficient).
    pids.insert(pid);
  }

  void remove(const process::UPID& pid)
  {
    // TODO(benh): unlink(pid);
    pids.erase(pid);
  }

  void set(const std::set<process::UPID>& _pids)
  {
    pids.clear();
    foreach (const process::UPID& pid, _pids) {
      add(pid); // Also does a link.
    }
  }

  // Sends a request to each of the groups members and returns a set
  // of futures that represent their responses.
  template <typename Req, typename Res>
  std::set<process::Future<Res> > broadcast(
      const Protocol<Req, Res>& protocol,
      const Req& req,
      const std::set<process::UPID>& filter)
  {
    std::set<process::Future<Res> > futures;
    typename std::set<process::UPID>::const_iterator iterator;
    for (iterator = pids.begin(); iterator != pids.end(); ++iterator) {
      const process::UPID& pid = *iterator;
      if (filter.count(pid) == 0) {
        futures.insert(protocol(pid, req));
      }
    }
    return futures;
  }

  template <typename M>
  void broadcast(
      const M& m,
      const std::set<process::UPID>& filter)
  {
    std::set<process::UPID>::const_iterator iterator;
    for (iterator = pids.begin(); iterator != pids.end(); ++iterator) {
      const process::UPID& pid = *iterator;
      if (filter.count(pid) == 0) {
        process::post(pid, m);
      }
    }
  }

private:
  // Not copyable, not assignable.
  NetworkProcess(const NetworkProcess&);
  NetworkProcess& operator = (const NetworkProcess&);

  std::set<process::UPID> pids;
};


inline Network::Network()
{
  process = new NetworkProcess();
  process::spawn(process);
}


inline Network::Network(const std::set<process::UPID>& pids)
{
  process = new NetworkProcess(pids);
  process::spawn(process);
}


inline Network::~Network()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


inline void Network::add(const process::UPID& pid)
{
  process::dispatch(process, &NetworkProcess::add, pid);
}


inline void Network::remove(const process::UPID& pid)
{
  process::dispatch(process, &NetworkProcess::remove, pid);
}


inline void Network::set(const std::set<process::UPID>& pids)
{
  process::dispatch(process, &NetworkProcess::set, pids);
}


template <typename Req, typename Res>
process::Future<std::set<process::Future<Res> > > Network::broadcast(
    const Protocol<Req, Res>& protocol,
    const Req& req,
    const std::set<process::UPID>& filter)
{
  return process::dispatch(process, &NetworkProcess::broadcast<Req, Res>,
                           protocol, req, filter);
}


template <typename M>
void Network::broadcast(
    const M& m,
    const std::set<process::UPID>& filter)
{
  // Need to disambiguate overloaded function.
  void (NetworkProcess::*broadcast)(const M&, const std::set<process::UPID>&) =
    &NetworkProcess::broadcast<M>;

  process::dispatch(process, broadcast, m, filter);
}


inline ZooKeeperNetwork::ZooKeeperNetwork(zookeeper::Group* _group)
  : group(_group)
{
  watch(std::set<zookeeper::Group::Membership>());
}


inline void ZooKeeperNetwork::watch(
    const std::set<zookeeper::Group::Membership>& expected)
{
  memberships = group->watch(expected);
  memberships.onAny(executor.defer(lambda::bind(&This::watched, this)));
}


inline void ZooKeeperNetwork::watched()
{
  if (memberships.isFailed()) {
    // We can't do much here, we could try creating another Group but
    // that might just continue indifinitely, so we fail early
    // instead. Note that Group handles all retryable/recoverable
    // ZooKeeper errors internally.
    LOG(FATAL) << "Failed to watch ZooKeeper group: " << memberships.failure();
  }

  CHECK(memberships.isReady()); // Not expecting Group to discard futures.

  LOG(INFO) << "ZooKeeper group memberships changed";

  // Get data for each membership in order to convert them to PIDs.
  std::list<process::Future<std::string> > futures;

  foreach (const zookeeper::Group::Membership& membership, memberships.get()) {
    futures.push_back(group->data(membership));
  }

  datas = process::collect(futures, process::Timeout(Seconds(5.0)));
  datas.onAny(executor.defer(lambda::bind(&This::collected, this)));
}


inline void ZooKeeperNetwork::collected()
{
  if (datas.isFailed()) {
    LOG(WARNING) << "Failed to get data for ZooKeeper group members: "
                 << datas.failure();

    // Try again later assuming empty group. Note that this does not
    // remove any of the current group members.
    watch(std::set<zookeeper::Group::Membership>());
    return;
  }

  CHECK(datas.isReady()); // Not expecting collect to discard futures.

  std::set<process::UPID> pids;

  foreach (const std::string& data, datas.get()) {
    process::UPID pid(data);
    CHECK(pid) << "Failed to parse '" << data << "'";
    pids.insert(pid);
  }

  LOG(INFO) << "ZooKeeper group PIDs: " << stringify(pids);

  set(pids); // Update the network.

  watch(memberships.get());
}

#endif // __NETWORK_HPP__
