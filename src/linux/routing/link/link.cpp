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

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/if.h> // Must be included after sys/socket.h.

#include <netlink/errno.h>
#include <netlink/socket.h>

#include <netlink/route/link.h>

#include <set>
#include <string>
#include <vector>

#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/pid.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/error.hpp>
#include <stout/lambda.hpp>
#include <stout/net.hpp>
#include <stout/none.hpp>
#include <stout/os.hpp>

#include "linux/routing/internal.hpp"
#include "linux/routing/route.hpp"

#include "linux/routing/link/internal.hpp"
#include "linux/routing/link/link.hpp"

using namespace process;

using std::set;
using std::string;
using std::vector;

namespace routing {
namespace link {

Result<string> eth0()
{
  Try<vector<route::Rule>> mainRoutingTable = route::table();
  if (mainRoutingTable.isError()) {
    return Error(
        "Failed to retrieve the main routing table on the host: " +
        mainRoutingTable.error());
  }

  foreach (const route::Rule& rule, mainRoutingTable.get()) {
    if (rule.destination.isNone()) {
      // Check if the public interface exists.
      Try<bool> exists = link::exists(rule.link);
      if (exists.isError()) {
        return Error(
            "Failed to check if " + rule.link + " exists: " + exists.error());
      } else if (!exists.get()) {
        return Error(
            rule.link + " is in the routing table but not in the system");
      }

      return rule.link;
    }
  }

  return None();
}


Result<string> lo()
{
  Try<set<string>> links = net::links();
  if (links.isError()) {
    return Error("Failed to get all the links: " + links.error());
  }

  foreach (const string& link, links.get()) {
    Result<bool> test = link::internal::test(link, IFF_LOOPBACK);
    if (test.isError()) {
      return Error("Failed to check the flag on link: " + link);
    } else if (test.isSome() && test.get()) {
      return link;
    }
  }

  return None();
}


Try<bool> exists(const string& _link)
{
  Result<Netlink<struct rtnl_link>> link = internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  }
  return link.isSome();
}


Try<bool> remove(const string& _link)
{
  Result<Netlink<struct rtnl_link>> link = internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_link_delete(socket->get(), link.get().get());
  if (error != 0) {
    if (error == -NLE_OBJ_NOTFOUND || error == -NLE_NODEV) {
      return false;
    }
    return Error(nl_geterror(error));
  }

  return true;
}


namespace internal {

// A process that checks if a link has been removed.
class ExistenceChecker : public Process<ExistenceChecker>
{
public:
  ExistenceChecker(const string& _link)
    : ProcessBase(process::ID::generate("link-existence-checker")),
      link(_link) {}

  virtual ~ExistenceChecker() {}

  // Returns a future which gets set when the link has been removed.
  Future<Nothing> future() { return promise.future(); }

protected:
  virtual void initialize()
  {
    // Stop when no one cares.
    promise.future().onDiscard(lambda::bind(
        static_cast<void (*)(const UPID&, bool)>(terminate), self(), true));

    check();
  }

  virtual void finalize()
  {
    promise.discard();
  }

private:
  void check()
  {
    Try<bool> exists = link::exists(link);
    if (exists.isError()) {
      promise.fail(exists.error());
      terminate(self());
      return;
    } else if (!exists.get()) {
      promise.set(Nothing());
      terminate(self());
      return;
    }

    // Perform the check again.
    delay(Milliseconds(100), self(), &Self::check);
  }

  const string link;
  Promise<Nothing> promise;
};

} // namespace internal {


Future<Nothing> removed(const string& link)
{
  internal::ExistenceChecker* checker = new internal::ExistenceChecker(link);
  Future<Nothing> future = checker->future();
  spawn(checker, true);
  return future;
}


Result<int> index(const string& _link)
{
  Result<Netlink<struct rtnl_link>> link = internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  return rtnl_link_get_ifindex(link->get());
}


Result<string> name(int index)
{
  Result<Netlink<struct rtnl_link>> link = internal::get(index);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  return rtnl_link_get_name(link->get());
}


Result<bool> isUp(const string& link)
{
  return internal::test(link, IFF_UP);
}


Try<bool> setUp(const string& link)
{
  return internal::set(link, IFF_UP);
}


Try<bool> setMAC(const string& link, const net::MAC& mac)
{
  // TODO(jieyu): We use ioctl to set the MAC address because the
  // interfaces in libnl have some issues with virtual devices.
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));

  strncpy(ifr.ifr_name, link.c_str(), IFNAMSIZ);

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    return ErrnoError();
  }

  // Since loopback interface has sa_family ARPHRD_LOOPBACK, we need
  // to get the MAC address of the link first to decide what value the
  // sa_family should be.
  if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
    if (errno == ENODEV) {
      os::close(fd);
      return false;
    } else {
      // Save the error string as os::close may overwrite errno.
      const string message = os::strerror(errno);
      os::close(fd);
      return Error(message);
    }
  }

  ifr.ifr_hwaddr.sa_data[0] = mac[0];
  ifr.ifr_hwaddr.sa_data[1] = mac[1];
  ifr.ifr_hwaddr.sa_data[2] = mac[2];
  ifr.ifr_hwaddr.sa_data[3] = mac[3];
  ifr.ifr_hwaddr.sa_data[4] = mac[4];
  ifr.ifr_hwaddr.sa_data[5] = mac[5];

  if (ioctl(fd, SIOCSIFHWADDR, &ifr) == -1) {
    if (errno == ENODEV) {
      os::close(fd);
      return false;
    } else {
      // Save the error string as os::close may overwrite errno.
      const string message = os::strerror(errno);
      os::close(fd);
      return Error(message);
    }
  }

  os::close(fd);
  return true;
}


Result<unsigned int> mtu(const string& _link)
{
  Result<Netlink<struct rtnl_link>> link = internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  return rtnl_link_get_mtu(link->get());
}


Try<bool> setMTU(const string& _link, unsigned int mtu)
{
  Result<Netlink<struct rtnl_link>> link = internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  // TODO(jieyu): We use ioctl to set the MTU because libnl has some
  // issues with rtnl_link_change.
  struct ifreq ifr;
  memset(&ifr, 0, sizeof(ifr));

  strncpy(ifr.ifr_name, _link.c_str(), IFNAMSIZ);
  ifr.ifr_mtu = mtu;

  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    return ErrnoError();
  }

  if (ioctl(fd, SIOCSIFMTU, &ifr) == -1) {
    if (errno == ENODEV) {
      os::close(fd);
      return false;
    }

    // Save the error string as os::close may overwrite errno.
    const string message = os::strerror(errno);
    os::close(fd);
    return Error(message);
  }

  os::close(fd);
  return true;
}


Result<hashmap<string, uint64_t>> statistics(const string& _link)
{
  Result<Netlink<struct rtnl_link>> link = internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  rtnl_link_stat_id_t stats[] = {
    // Statistics related to receiving.
    RTNL_LINK_RX_PACKETS,
    RTNL_LINK_RX_BYTES,
    RTNL_LINK_RX_ERRORS,
    RTNL_LINK_RX_DROPPED,
    RTNL_LINK_RX_COMPRESSED,
    RTNL_LINK_RX_FIFO_ERR,
    RTNL_LINK_RX_LEN_ERR,
    RTNL_LINK_RX_OVER_ERR,
    RTNL_LINK_RX_CRC_ERR,
    RTNL_LINK_RX_FRAME_ERR,
    RTNL_LINK_RX_MISSED_ERR,
    RTNL_LINK_MULTICAST,

    // Statistics related to sending.
    RTNL_LINK_TX_PACKETS,
    RTNL_LINK_TX_BYTES,
    RTNL_LINK_TX_ERRORS,
    RTNL_LINK_TX_DROPPED,
    RTNL_LINK_TX_COMPRESSED,
    RTNL_LINK_TX_FIFO_ERR,
    RTNL_LINK_TX_ABORT_ERR,
    RTNL_LINK_TX_CARRIER_ERR,
    RTNL_LINK_TX_HBEAT_ERR,
    RTNL_LINK_TX_WIN_ERR,
    RTNL_LINK_COLLISIONS,
  };

  hashmap<string, uint64_t> results;

  char buf[32];
  size_t size = sizeof(stats) / sizeof(stats[0]);

  for (size_t i = 0; i < size; i++) {
    rtnl_link_stat2str(stats[i], buf, 32);
    results[buf] = rtnl_link_get_stat(link->get(), stats[i]);
  }

  return results;
}

} // namespace link {
} // namespace routing {
