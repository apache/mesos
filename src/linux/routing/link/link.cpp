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



// There seems to be a bug that is occassionally preventing ioctl and libnl
// from setting correct MAC addresses, despite them not returning errors.
//
// Observed scenarios with incorrectly assigned MAC addresses:
//
// 1. After setting the mac address: ioctl returns the correct MAC address, but
//    net::mac returns an incorrect MAC address (different from the original!)
// 2. After setting the mac address: both ioctl and net::mac return the same MAC
//    address, but are both wrong (and different from the original one!)
// 3. After setting the mac address: there are no cases where ioctl or net::mac
//    come back with the same MAC address as before we set the address.
// 4. Before we set the mac address: there is a possibility that ioctl and
//    net::mac results disagree with each other!
// 5. There is a possibility that the MAC address we set ends up overwritten by
//    a garbage value after setMAC has already completed and checked that the
//    mac address was set correctly. Since this error happens after this
//    function has finished, we cannot log nor detect it in setMAC because we
//    have not yet studied at what point this occurs.
//
// Notes:
//
// 1. We have observed this behavior only on CentOS 9 systems at the moment,
//    CentOS 7 systems under various kernels do not seem to have the issue
//    (which is quite strange if this was purely a kernel bug).
// 2. We have tried kernels 5.15.147, 5.15.160, 5.15.161, all of these have
//    this issue on CentOS 9.
//
// To workaround this bug, we check that the MAC address is set correctly
// after the ioctl call, and retry the address setting if necessary. In our
// testing, this workaround appears to workaround scenarios (1), (2), (3),
// and (4) above, but it does not address scenario (5).
//
// See MESOS-10243 for additional details, follow-ups.
Try<Nothing> setMAC(const string& link, const net::MAC& mac)
{
  auto getMacViaIoctl = [](int fd, const string& link) -> Try<ifreq> {
    ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, link.c_str(), IFNAMSIZ);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) == -1) {
      return ErrnoError();
    }
    return ifr;
  };

  // Since loopback interface has sa_family ARPHRD_LOOPBACK, we need
  // to get the MAC address of the link first to decide what value the
  // sa_family should be.
  //
  // TODO(jieyu): We use ioctl to set the MAC address because the
  // interfaces in libnl have some issues with virtual devices.
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd == -1) {
    return ErrnoError();
  }

  Try<ifreq> if_request = getMacViaIoctl(fd, link);
  if (if_request.isError()) {
    os::close(fd);
    return Error(if_request.error());
  }

  // See comment above this function on why we check this and use a loop below.
  Result<net::MAC> net_mac = net::mac(link);
  net::MAC ioctl_mac = net::MAC(if_request->ifr_hwaddr.sa_data);

  if (!(net_mac.isSome() && *net_mac == ioctl_mac)) {
    LOG(WARNING)
      << "MAC mismatch between net::mac = " << (net_mac.isSome()
         ? stringify(*net_mac) : (net_mac.isError() ? net_mac.error() : "None"))
      << " and ioctl = " << stringify(ioctl_mac)
      << " for link " << stringify(link)
      << " before setting to target mac address " << stringify(mac);
  }

  for (int i = 0; i < 10; ++i) {
    if_request->ifr_hwaddr.sa_data[0] = mac[0];
    if_request->ifr_hwaddr.sa_data[1] = mac[1];
    if_request->ifr_hwaddr.sa_data[2] = mac[2];
    if_request->ifr_hwaddr.sa_data[3] = mac[3];
    if_request->ifr_hwaddr.sa_data[4] = mac[4];
    if_request->ifr_hwaddr.sa_data[5] = mac[5];

    if (ioctl(fd, SIOCSIFHWADDR, &(*if_request)) == -1) {
      const string message = os::strerror(errno);
      os::close(fd);
      return Error(message);
    }

    // Prepare another read to check if the MAC address was set correctly.
    if_request = getMacViaIoctl(fd, link);
    if (if_request.isError()) {
      os::close(fd);
      return Error(if_request.error());
    }

    net_mac = net::mac(link);
    ioctl_mac = net::MAC(if_request->ifr_hwaddr.sa_data);

    if (net_mac.isSome() && *net_mac == mac && ioctl_mac == mac) {
      break;
    }

    LOG(WARNING)
      << "MAC mismatch between target mac = " << stringify(mac)
      << " and net::mac = " << (net_mac.isSome()
         ? stringify(*net_mac) : (net_mac.isError() ? net_mac.error() : "None"))
      << " and ioctl: " << stringify(ioctl_mac)
      << " for link " << stringify(link) << "; retrying set operation...";
  }

  os::close(fd);

  if (!(net_mac.isSome() && *net_mac == mac && ioctl_mac == mac)) {
    return Error(
      "net::mac and ioctl did not report the correct address despite multiple"
      " attempts to set target MAC address");
  }
  return Nothing();
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
