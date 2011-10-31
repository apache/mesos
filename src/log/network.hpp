#ifndef __NETWORK_HPP__
#define __NETWORK_HPP__

// TODO(benh): Eventually move and associate this code with the
// libprocess protobuf code rather than keep it here.

#include <set>
#include <string>

#include <process/async.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>
#include <process/timeout.hpp>

#include "config/config.hpp"

#include "common/foreach.hpp"
#include "common/lambda.hpp"
#include "common/option.hpp"
#include "common/seconds.hpp"
#include "common/utils.hpp"

#ifdef WITH_ZOOKEEPER
#include "zookeeper/group.hpp"
#endif

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


#ifdef WITH_ZOOKEEPER
class ZooKeeperNetwork : public Network
{
public:
  ZooKeeperNetwork(zookeeper::Group* group);

private:
  // Helper that sets up a watch on the group.
  void watch(const std::set<zookeeper::Group::Membership>& memberships =
             std::set<zookeeper::Group::Membership>());

  // Invoked when the group has updated.
  void ready(const std::set<zookeeper::Group::Membership>& memberships);

  // Invoked if watching the group fails.
  void failed(const std::string& message) const;

  // Invoked if we were unable to watch the group.
  void discarded() const;

  zookeeper::Group* group;

  async::Dispatch dispatch;
};
#endif // WITH_ZOOKEEPER


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
  void (NetworkProcess::*broadcast) (
      const M&, const std::set<process::UPID>&) =
    &NetworkProcess::broadcast<M>;

  process::dispatch(process, broadcast, m, filter);
}

#ifdef WITH_ZOOKEEPER
inline ZooKeeperNetwork::ZooKeeperNetwork(zookeeper::Group* _group)
  : group(_group)
{
  watch();
}


inline void ZooKeeperNetwork::watch(
    const std::set<zookeeper::Group::Membership>& memberships)
{
  group->watch(memberships)
    .onReady(dispatch(lambda::bind(&ZooKeeperNetwork::ready,
                                   this, lambda::_1)))
    .onFailed(dispatch(lambda::bind(&ZooKeeperNetwork::failed,
                                    this, lambda::_1)))
    .onDiscarded(dispatch(lambda::bind(&ZooKeeperNetwork::discarded, this)));
}


inline void ZooKeeperNetwork::ready(
    const std::set<zookeeper::Group::Membership>& memberships)
{
  LOG(INFO) << "ZooKeeper group memberships changed";

  // Get infos for each membership in order to convert them to PIDs.
  std::set<process::Future<std::string> > futures;

  foreach (const zookeeper::Group::Membership& membership, memberships) {
    futures.insert(group->info(membership));
  }

  std::set<process::UPID> pids;

  Option<process::Future<std::string> > option;

  process::Timeout timeout = 5.0;

  while (!futures.empty()) {
    option = select(futures, timeout.remaining());
    if (option.isSome()) {
      CHECK(option.get().isReady());
      process::UPID pid(option.get().get());
      CHECK(pid) << "Failed to parse '" << option.get().get() << "'";
      pids.insert(pid);
      futures.erase(option.get());
    } else {
      watch(); // Try again later assuming empty group.
      return;
    }
  }

  LOG(INFO) << "ZooKeeper group PIDs: "
            << mesos::internal::utils::stringify(pids);

  set(pids); // Update the network.

  watch(memberships);
}


inline void ZooKeeperNetwork::failed(const std::string& message) const
{
  LOG(FATAL) << "Failed to watch ZooKeeper group: "<< message;
}


inline void ZooKeeperNetwork::discarded() const
{
  LOG(FATAL) << "Unexpected discarded future while watching ZooKeeper group";
}
#endif // WITH_ZOOKEEPER

#endif // __NETWORK_HPP__
