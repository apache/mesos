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

#include <signal.h>
#include <unistd.h>

#include <linux/version.h>

#include <sys/types.h>
#include <sys/wait.h>

#include <gtest/gtest.h>

#include <process/clock.hpp>
#include <process/gtest.hpp>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/hashmap.hpp>
#include <stout/ip.hpp>
#include <stout/mac.hpp>
#include <stout/net.hpp>
#include <stout/stringify.hpp>

#include "linux/routing/route.hpp"
#include "linux/routing/utils.hpp"

#include "linux/routing/diagnosis/diagnosis.hpp"

#include "linux/routing/filter/arp.hpp"
#include "linux/routing/filter/icmp.hpp"
#include "linux/routing/filter/ip.hpp"

#include "linux/routing/link/link.hpp"

#include "linux/routing/queueing/fq_codel.hpp"
#include "linux/routing/queueing/handle.hpp"
#include "linux/routing/queueing/ingress.hpp"

using namespace process;

using namespace routing;
using namespace routing::filter;
using namespace routing::queueing;

using std::endl;
using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


static const string TEST_VETH_LINK = "veth-test";
static const string TEST_PEER_LINK = "veth-peer";


class RoutingTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    ASSERT_SOME(routing::check())
      << "-------------------------------------------------------------\n"
      << "We cannot run any routing tests because either your libnl\n"
      << "library or kernel is not new enough. You can either upgrade,\n"
      << "or disable this test case\n"
      << "-------------------------------------------------------------";
  }
};


// Tests that require setting up virtual ethernet on host.
class RoutingVethTest : public RoutingTest
{
protected:
  virtual void SetUp()
  {
    RoutingTest::SetUp();

    // Clean up the test links, in case it wasn't cleaned up properly
    // from previous tests.
    link::remove(TEST_VETH_LINK);

    ASSERT_SOME_FALSE(link::exists(TEST_VETH_LINK));
    ASSERT_SOME_FALSE(link::exists(TEST_PEER_LINK));
  }

  virtual void TearDown()
  {
    link::remove(TEST_VETH_LINK);
  }
};


TEST_F(RoutingTest, PortRange)
{
  Try<ip::PortRange> ports = ip::PortRange::fromBeginEnd(1, 0);
  EXPECT_ERROR(ports);

  ports = ip::PortRange::fromBeginEnd(4, 11);
  EXPECT_ERROR(ports);

  ports = ip::PortRange::fromBeginEnd(4, 7);
  ASSERT_SOME(ports);
  EXPECT_EQ(4u, ports.get().begin());
  EXPECT_EQ(7u, ports.get().end());
  EXPECT_EQ(0xfffc, ports.get().mask());
  EXPECT_EQ("[4,7]", stringify(ports.get()));

  ports = ip::PortRange::fromBeginEnd(10, 10);
  ASSERT_SOME(ports);
  EXPECT_EQ(10u, ports.get().begin());
  EXPECT_EQ(10u, ports.get().end());
  EXPECT_EQ(0xffff, ports.get().mask());
  EXPECT_EQ("[10,10]", stringify(ports.get()));

  ports = ip::PortRange::fromBeginMask(20, 0xffff);
  ASSERT_SOME(ports);
  EXPECT_EQ(20u, ports.get().begin());
  EXPECT_EQ(20u, ports.get().end());
  EXPECT_EQ(0xffff, ports.get().mask());
  EXPECT_EQ("[20,20]", stringify(ports.get()));

  ports = ip::PortRange::fromBeginMask(1024, 0xfff8);
  ASSERT_SOME(ports);
  EXPECT_EQ(1024u, ports.get().begin());
  EXPECT_EQ(1031u, ports.get().end());
  EXPECT_EQ(0xfff8, ports.get().mask());
  EXPECT_EQ("[1024,1031]", stringify(ports.get()));
}


TEST_F(RoutingTest, RouteTable)
{
  Try<vector<route::Rule> > table = route::table();
  EXPECT_SOME(table);

  Result<net::IP> gateway = route::defaultGateway();
  EXPECT_FALSE(gateway.isError());
}


TEST_F(RoutingTest, LinkIndex)
{
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);

  foreach (const string& link, links.get()) {
    EXPECT_SOME_NE(0, link::index(link));
  }

  EXPECT_NONE(link::index("not-exist"));
}


TEST_F(RoutingTest, LinkName)
{
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);

  foreach (const string& link, links.get()) {
    EXPECT_SOME_NE(0, link::index(link));
    EXPECT_SOME_EQ(link, link::name(link::index(link).get()));
  }
}


TEST_F(RoutingTest, LinkStatistics)
{
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);

  foreach (const string& link, links.get()) {
    Result<hashmap<string, uint64_t> > statistics = link::statistics(link);

    ASSERT_SOME(statistics);
    EXPECT_TRUE(statistics.get().contains("rx_packets"));
    EXPECT_TRUE(statistics.get().contains("rx_bytes"));
    EXPECT_TRUE(statistics.get().contains("tx_packets"));
    EXPECT_TRUE(statistics.get().contains("tx_bytes"));
  }

  EXPECT_NONE(link::statistics("not-exist"));
}


TEST_F(RoutingTest, LinkExists)
{
  Try<set<string> > links = net::links();
  ASSERT_SOME(links);

  foreach (const string& link, links.get()) {
    EXPECT_SOME_TRUE(link::exists(link));
  }

  EXPECT_SOME_FALSE(link::exists("not-exist"));
}


TEST_F(RoutingTest, Eth0)
{
  Result<string> eth0 = link::eth0();
  EXPECT_FALSE(eth0.isError());

  if (eth0.isSome()) {
    ASSERT_SOME_TRUE(link::exists(eth0.get()));
  }
}


TEST_F(RoutingTest, Lo)
{
  Result<string> lo = link::lo();
  EXPECT_FALSE(lo.isError());

  if (lo.isSome()) {
    ASSERT_SOME_TRUE(link::exists(lo.get()));
  }
}


TEST_F(RoutingTest, INETSockets)
{
  Try<vector<diagnosis::socket::Info> > infos =
    diagnosis::socket::infos(AF_INET, diagnosis::socket::state::ALL);

  EXPECT_SOME(infos);

  foreach (const diagnosis::socket::Info& info, infos.get()) {
    // Both source and destination IPs should be present since
    // 'AF_INET' is asked for.
    EXPECT_SOME(info.sourceIP);
    EXPECT_SOME(info.destinationIP);
  }
}


TEST_F(RoutingVethTest, ROOT_LinkCreate)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  EXPECT_SOME_NE(0, link::index(TEST_VETH_LINK));
  EXPECT_SOME_NE(0, link::index(TEST_PEER_LINK));

  // Test the case where the veth (with the same name) already exists.
  EXPECT_SOME_FALSE(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));
}


TEST_F(RoutingVethTest, ROOT_LinkRemove)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::remove(TEST_VETH_LINK));
  EXPECT_SOME_FALSE(link::remove(TEST_VETH_LINK));
  EXPECT_SOME_FALSE(link::remove(TEST_PEER_LINK));
}


// An old glibc might not have this symbol.
#ifndef CLONE_NEWNET
#define CLONE_NEWNET 0x40000000
#endif


// Entry point of the child process (used in clone()).
static int child(void*)
{
  // Wait to be killed.
  while (true) {
    sleep(1);
  }

  // Should not reach here.
  ABORT("Child process should not reach here");
}


TEST_F(RoutingVethTest, ROOT_LinkCreatePid)
{
  // Stack used in the child process.
  unsigned long long stack[32];

  pid_t pid = ::clone(child, &stack[31], CLONE_NEWNET | SIGCHLD, NULL);
  ASSERT_NE(-1, pid);

  // In parent process.
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, pid));
  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));

  // The peer should not exist in parent network namespace.
  EXPECT_SOME_FALSE(link::exists(TEST_PEER_LINK));

  // TODO(jieyu): Enter the child network namespace and make sure that
  // the TEST_PEER_LINK is there.

  EXPECT_SOME_NE(0, link::index(TEST_VETH_LINK));

  // Kill the child process.
  ASSERT_NE(-1, kill(pid, SIGKILL));

  // Wait for the child process.
  int status;
  EXPECT_NE(-1, waitpid((pid_t) -1, &status, 0));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(SIGKILL, WTERMSIG(status));
}


TEST_F(RoutingVethTest, ROOT_LinkWait)
{
  AWAIT_READY(link::removed(TEST_VETH_LINK));

  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  Future<Nothing> removed = link::removed(TEST_VETH_LINK);
  EXPECT_TRUE(removed.isPending());

  ASSERT_SOME_TRUE(link::remove(TEST_VETH_LINK));
  AWAIT_READY(removed);
}


TEST_F(RoutingVethTest, ROOT_LinkSetUp)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  EXPECT_SOME_FALSE(link::isUp(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::setUp(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::isUp(TEST_VETH_LINK));

  EXPECT_SOME_FALSE(link::isUp(TEST_PEER_LINK));
  EXPECT_SOME_TRUE(link::setUp(TEST_PEER_LINK));
  EXPECT_SOME_TRUE(link::isUp(TEST_PEER_LINK));

  EXPECT_NONE(link::isUp("non-exist"));
  EXPECT_SOME_FALSE(link::setUp("non-exist"));
}


TEST_F(RoutingVethTest, ROOT_LinkSetMAC)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  uint8_t bytes[6] = {0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc};

  EXPECT_SOME_TRUE(link::setMAC(TEST_VETH_LINK, net::MAC(bytes)));
  EXPECT_SOME_TRUE(link::setMAC(TEST_PEER_LINK, net::MAC(bytes)));

  Result<net::MAC> mac = net::mac(TEST_VETH_LINK);

  ASSERT_SOME(mac);
  EXPECT_EQ(mac.get(), net::MAC(bytes));

  mac = net::mac(TEST_PEER_LINK);

  ASSERT_SOME(mac);
  EXPECT_EQ(mac.get(), net::MAC(bytes));

  EXPECT_SOME_FALSE(link::setMAC("non-exist", net::MAC(bytes)));

  // Kernel will reject a multicast MAC address.
  uint8_t multicast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

  EXPECT_ERROR(link::setMAC(TEST_VETH_LINK, net::MAC(multicast)));
}


TEST_F(RoutingVethTest, ROOT_LinkMTU)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  EXPECT_SOME_TRUE(link::setMTU(TEST_VETH_LINK, 10000));

  Result<unsigned int> mtu = link::mtu(TEST_VETH_LINK);
  ASSERT_SOME(mtu);
  EXPECT_EQ(10000u, mtu.get());

  EXPECT_NONE(link::mtu("not-exist"));
  EXPECT_SOME_FALSE(link::setMTU("not-exist", 1500));
}


TEST_F(RoutingVethTest, ROOT_FqCodelCreate)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(fq_codel::create(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(fq_codel::exists(TEST_VETH_LINK));
}


TEST_F(RoutingVethTest, ROOT_FqCodelClassifier)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(fq_codel::create(TEST_VETH_LINK));

  EXPECT_SOME_TRUE(arp::create(
      TEST_VETH_LINK,
      fq_codel::HANDLE,
      None(),
      queueing::Handle(fq_codel::HANDLE, 0)));

  // There is a kernel bug which could cause this test fail. Please
  // make sure your kernel, if newer than 3.14, has commit:
  // b057df24a7536cce6c372efe9d0e3d1558afedf4
  // (https://git.kernel.org/cgit/linux/kernel/git/davem/net.git).
  // Please fix your kernel if you see this failure.
  EXPECT_SOME_TRUE(arp::exists(TEST_VETH_LINK, fq_codel::HANDLE));

  EXPECT_SOME_TRUE(icmp::create(
      TEST_VETH_LINK,
      fq_codel::HANDLE,
      icmp::Classifier(None()),
      None(),
      queueing::Handle(fq_codel::HANDLE, 0)));

  EXPECT_SOME_TRUE(icmp::exists(
      TEST_VETH_LINK,
      fq_codel::HANDLE,
      icmp::Classifier(None())));

  Result<net::MAC> mac = net::mac(TEST_VETH_LINK);
  ASSERT_SOME(mac);

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  Try<ip::PortRange> sourcePorts =
    ip::PortRange::fromBeginEnd(1024, 1027);
  ASSERT_SOME(sourcePorts);

  Try<ip::PortRange> destinationPorts =
    ip::PortRange::fromBeginEnd(2000, 2000);
  ASSERT_SOME(destinationPorts);

  ip::Classifier classifier =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts.get(),
        destinationPorts.get());

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      fq_codel::HANDLE,
      classifier,
      None(),
      queueing::Handle(fq_codel::HANDLE, 1)));

  EXPECT_SOME_TRUE(ip::exists(TEST_VETH_LINK, fq_codel::HANDLE, classifier));
}


TEST_F(RoutingVethTest, ROOT_ARPFilterCreate)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  EXPECT_SOME_TRUE(arp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(arp::exists(TEST_VETH_LINK, ingress::HANDLE));
}


TEST_F(RoutingVethTest, ROOT_ARPFilterCreateDuplicated)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  set<string> links;
  links.insert(TEST_PEER_LINK);

  EXPECT_SOME_TRUE(arp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      None(),
      action::Mirror(links)));

  EXPECT_SOME_TRUE(arp::exists(TEST_VETH_LINK, ingress::HANDLE));

  EXPECT_SOME_FALSE(arp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      None(),
      action::Redirect(TEST_PEER_LINK)));
}


TEST_F(RoutingVethTest, ROOT_ARPFilterRemove)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  set<string> links;
  links.insert(TEST_PEER_LINK);

  EXPECT_SOME_TRUE(arp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      None(),
      action::Mirror(links)));

  EXPECT_SOME_TRUE(arp::exists(TEST_VETH_LINK, ingress::HANDLE));
  EXPECT_SOME_TRUE(arp::remove(TEST_VETH_LINK, ingress::HANDLE));
  EXPECT_SOME_FALSE(arp::exists(TEST_VETH_LINK, ingress::HANDLE));
}


TEST_F(RoutingVethTest, ROOT_ARPFilterUpdate)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  set<string> links;
  links.insert(TEST_PEER_LINK);

  EXPECT_SOME_FALSE(arp::update(
      TEST_VETH_LINK,
      ingress::HANDLE,
      action::Mirror(links)));

  EXPECT_SOME_TRUE(arp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(arp::exists(TEST_VETH_LINK, ingress::HANDLE));

  EXPECT_SOME_TRUE(arp::update(
      TEST_VETH_LINK,
      ingress::HANDLE,
      action::Mirror(links)));

  EXPECT_SOME_TRUE(arp::exists(TEST_VETH_LINK, ingress::HANDLE));
}


TEST_F(RoutingVethTest, ROOT_ICMPFilterCreate)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  EXPECT_SOME_TRUE(icmp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(ip),
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(icmp::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(ip)));

  Result<vector<icmp::Classifier> > classifiers =
    icmp::classifiers(TEST_VETH_LINK, ingress::HANDLE);

  ASSERT_SOME(classifiers);
  ASSERT_EQ(1u, classifiers.get().size());
  EXPECT_SOME_EQ(ip, classifiers.get().front().destinationIP());
}


TEST_F(RoutingVethTest, ROOT_ICMPFilterCreateDuplicated)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  set<string> links;
  links.insert(TEST_PEER_LINK);

  EXPECT_SOME_TRUE(icmp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None()),
      None(),
      action::Mirror(links)));

  EXPECT_SOME_TRUE(icmp::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None())));

  EXPECT_SOME_FALSE(icmp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None()),
      None(),
      action::Mirror(links)));
}


TEST_F(RoutingVethTest, ROOT_ICMPFilterCreateMultiple)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  net::IP ip1 = net::IP(0x01020304); // 1.2.3.4
  net::IP ip2 = net::IP(0x05060708); // 5.6.7.8

  EXPECT_SOME_TRUE(icmp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(ip1),
      Priority(1, 1),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(icmp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(ip2),
      Priority(1, 2),
      action::Redirect(TEST_PEER_LINK)));

  Result<vector<icmp::Classifier> > classifiers =
    icmp::classifiers(TEST_VETH_LINK, ingress::HANDLE);

  ASSERT_SOME(classifiers);
  ASSERT_EQ(2u, classifiers.get().size());
  EXPECT_SOME_EQ(ip1, classifiers.get().front().destinationIP());
  EXPECT_SOME_EQ(ip2, classifiers.get().back().destinationIP());
}


TEST_F(RoutingVethTest, ROOT_ICMPFilterRemove)
{
  ASSERT_SOME(link::create(
      TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  EXPECT_SOME_TRUE(icmp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None()),
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(icmp::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None())));

  EXPECT_SOME_TRUE(icmp::remove(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None())));

  EXPECT_SOME_FALSE(icmp::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None())));
}


TEST_F(RoutingVethTest, ROOT_ICMPFilterUpdate)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  set<string> links;
  links.insert(TEST_PEER_LINK);

  EXPECT_SOME_FALSE(icmp::update(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None()),
      action::Mirror(links)));

  EXPECT_SOME_TRUE(icmp::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None()),
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(icmp::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None())));

  EXPECT_SOME_FALSE(icmp::update(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(ip),
      action::Mirror(links)));

  EXPECT_SOME_TRUE(icmp::update(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None()),
      action::Mirror(links)));

  EXPECT_SOME_TRUE(icmp::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(None())));

  EXPECT_SOME_FALSE(icmp::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      icmp::Classifier(ip)));
}


TEST_F(RoutingVethTest, ROOT_IPFilterCreate)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  Result<net::MAC> mac = net::mac(TEST_VETH_LINK);
  ASSERT_SOME(mac);

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  Try<ip::PortRange> sourcePorts =
    ip::PortRange::fromBeginEnd(1024, 1027);

  ASSERT_SOME(sourcePorts);

  Try<ip::PortRange> destinationPorts =
    ip::PortRange::fromBeginEnd(2000, 2000);

  ASSERT_SOME(destinationPorts);

  ip::Classifier classifier =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts.get(),
        destinationPorts.get());

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier,
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(ip::exists(TEST_VETH_LINK, ingress::HANDLE, classifier));

  Result<vector<ip::Classifier> > classifiers =
    ip::classifiers(TEST_VETH_LINK, ingress::HANDLE);

  ASSERT_SOME(classifiers);
  ASSERT_EQ(1u, classifiers.get().size());
  EXPECT_SOME_EQ(mac.get(), classifiers.get().front().destinationMAC());
  EXPECT_SOME_EQ(ip, classifiers.get().front().destinationIP());

  EXPECT_SOME_EQ(
      sourcePorts.get(),
      classifiers.get().front().sourcePorts());

  EXPECT_SOME_EQ(
      destinationPorts.get(),
      classifiers.get().front().destinationPorts());
}


TEST_F(RoutingVethTest, ROOT_IPFilterCreate2)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  net::IP ip(0x12345678);

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      ip::Classifier(None(), ip, None(), None()),
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(ip::exists(
      TEST_VETH_LINK,
      ingress::HANDLE,
      ip::Classifier(None(), ip, None(), None())));

  Result<vector<ip::Classifier> > classifiers =
    ip::classifiers(TEST_VETH_LINK, ingress::HANDLE);

  ASSERT_SOME(classifiers);
  ASSERT_EQ(1u, classifiers.get().size());
  EXPECT_NONE(classifiers.get().front().destinationMAC());
  EXPECT_SOME_EQ(ip, classifiers.get().front().destinationIP());
  EXPECT_NONE(classifiers.get().front().sourcePorts());
  EXPECT_NONE(classifiers.get().front().destinationPorts());
}


TEST_F(RoutingVethTest, ROOT_IPFilterCreateDuplicated)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  Result<net::MAC> mac = net::mac(TEST_VETH_LINK);
  ASSERT_SOME(mac);

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  Try<ip::PortRange> sourcePorts =
    ip::PortRange::fromBeginEnd(1024, 1027);

  ASSERT_SOME(sourcePorts);

  Try<ip::PortRange> destinationPorts =
    ip::PortRange::fromBeginEnd(2000, 2000);

  ASSERT_SOME(destinationPorts);

  ip::Classifier classifier =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts.get(),
        destinationPorts.get());

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier,
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(ip::exists(TEST_VETH_LINK, ingress::HANDLE, classifier));

  EXPECT_SOME_FALSE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier,
      None(),
      action::Redirect(TEST_PEER_LINK)));
}


TEST_F(RoutingVethTest, ROOT_IPFilterCreateMultiple)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  Result<net::MAC> mac = net::mac(TEST_VETH_LINK);
  ASSERT_SOME(mac);

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  Try<ip::PortRange> sourcePorts1 =
    ip::PortRange::fromBeginEnd(1024, 1027);

  ASSERT_SOME(sourcePorts1);

  Try<ip::PortRange> destinationPorts1 =
    ip::PortRange::fromBeginEnd(2000, 2000);

  ASSERT_SOME(destinationPorts1);

  Try<ip::PortRange> sourcePorts2 =
    ip::PortRange::fromBeginEnd(3024, 3025);

  ASSERT_SOME(sourcePorts2);

  Try<ip::PortRange> destinationPorts2 =
    ip::PortRange::fromBeginEnd(4000, 4003);

  ASSERT_SOME(destinationPorts2);

  ip::Classifier classifier1 =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts1.get(),
        destinationPorts1.get());

  ip::Classifier classifier2 =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts2.get(),
        destinationPorts2.get());

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier1,
      Priority(2, 1),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier2,
      Priority(2, 2),
      action::Redirect(TEST_PEER_LINK)));

  Result<vector<ip::Classifier> > classifiers =
    ip::classifiers(TEST_VETH_LINK, ingress::HANDLE);

  ASSERT_SOME(classifiers);
  ASSERT_EQ(2u, classifiers.get().size());

  EXPECT_SOME_EQ(mac.get(), classifiers.get().front().destinationMAC());
  EXPECT_SOME_EQ(ip, classifiers.get().front().destinationIP());

  EXPECT_SOME_EQ(
      sourcePorts1.get(),
      classifiers.get().front().sourcePorts());

  EXPECT_SOME_EQ(
      destinationPorts1.get(),
      classifiers.get().front().destinationPorts());

  EXPECT_SOME_EQ(mac.get(), classifiers.get().back().destinationMAC());
  EXPECT_SOME_EQ(ip, classifiers.get().back().destinationIP());

  EXPECT_SOME_EQ(
      sourcePorts2.get(),
      classifiers.get().back().sourcePorts());

  EXPECT_SOME_EQ(
      destinationPorts2.get(),
      classifiers.get().back().destinationPorts());
}


TEST_F(RoutingVethTest, ROOT_IPFilterRemove)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  Result<net::MAC> mac = net::mac(TEST_VETH_LINK);
  ASSERT_SOME(mac);

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  Try<ip::PortRange> sourcePorts1 =
    ip::PortRange::fromBeginEnd(1024, 1027);

  ASSERT_SOME(sourcePorts1);

  Try<ip::PortRange> destinationPorts1 =
    ip::PortRange::fromBeginEnd(2000, 2000);

  ASSERT_SOME(destinationPorts1);

  Try<ip::PortRange> sourcePorts2 =
    ip::PortRange::fromBeginEnd(3024, 3025);

  ASSERT_SOME(sourcePorts2);

  Try<ip::PortRange> destinationPorts2 =
    ip::PortRange::fromBeginEnd(4000, 4003);

  ASSERT_SOME(destinationPorts2);

  ip::Classifier classifier1 =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts1.get(),
        destinationPorts1.get());

  ip::Classifier classifier2 =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts2.get(),
        destinationPorts2.get());

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier1,
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier2,
      None(),
      action::Redirect(TEST_PEER_LINK)));

  EXPECT_SOME_TRUE(ip::remove(TEST_VETH_LINK, ingress::HANDLE, classifier1));
  EXPECT_SOME_FALSE(ip::exists(TEST_VETH_LINK, ingress::HANDLE, classifier1));

  EXPECT_SOME_TRUE(ip::remove(TEST_VETH_LINK, ingress::HANDLE, classifier2));
  EXPECT_SOME_FALSE(ip::exists(TEST_VETH_LINK, ingress::HANDLE, classifier2));

  Result<vector<ip::Classifier> > classifiers =
    ip::classifiers(TEST_VETH_LINK, ingress::HANDLE);

  ASSERT_SOME(classifiers);
  EXPECT_EQ(0u, classifiers.get().size());
}


// Test the workaround introduced for MESOS-1617.
TEST_F(RoutingVethTest, ROOT_HandleGeneration)
{
  ASSERT_SOME(link::create(TEST_VETH_LINK, TEST_PEER_LINK, None()));

  EXPECT_SOME_TRUE(link::exists(TEST_VETH_LINK));
  EXPECT_SOME_TRUE(link::exists(TEST_PEER_LINK));

  ASSERT_SOME_TRUE(ingress::create(TEST_VETH_LINK));

  Result<net::MAC> mac = net::mac(TEST_VETH_LINK);
  ASSERT_SOME(mac);

  net::IP ip = net::IP(0x01020304); // 1.2.3.4

  Try<ip::PortRange> sourcePorts1 =
    ip::PortRange::fromBeginEnd(1024, 1027);

  ASSERT_SOME(sourcePorts1);

  Try<ip::PortRange> destinationPorts1 =
    ip::PortRange::fromBeginEnd(2000, 2000);

  ASSERT_SOME(destinationPorts1);

  Try<ip::PortRange> sourcePorts2 =
    ip::PortRange::fromBeginEnd(3024, 3025);

  ASSERT_SOME(sourcePorts2);

  Try<ip::PortRange> destinationPorts2 =
    ip::PortRange::fromBeginEnd(4000, 4003);

  ASSERT_SOME(destinationPorts2);

  ip::Classifier classifier1 =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts1.get(),
        destinationPorts1.get());

  ip::Classifier classifier2 =
    ip::Classifier(
        mac.get(),
        ip,
        sourcePorts2.get(),
        destinationPorts2.get());

  // Use handle 800:00:fff for the first filter.
  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier1,
      Priority(2, 1),
      U32Handle(0x800, 0x0, 0xfff),
      action::Redirect(TEST_PEER_LINK)));

  // With the workaround, this filter should be assigned a handle
  // different than 800:00:fff.
  EXPECT_SOME_TRUE(ip::create(
      TEST_VETH_LINK,
      ingress::HANDLE,
      classifier2,
      Priority(2, 1),
      action::Redirect(TEST_PEER_LINK)));

  // Try to remove the second filter. If we don't have the workaround,
  // removing the second filter will return false since the kernel
  // will find the handle matches the first filter.
  EXPECT_SOME_TRUE(ip::remove(TEST_VETH_LINK, ingress::HANDLE, classifier2));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
