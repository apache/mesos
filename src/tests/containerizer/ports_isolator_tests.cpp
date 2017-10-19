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

#include <string>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/process.hpp>

#include <stout/gtest.hpp>

#include "slave/containerizer/mesos/isolators/network/ports.hpp"

#include "tests/mesos.hpp"

using process::Future;
using process::Owned;

using mesos::internal::slave::NetworkPortsIsolatorProcess;

using std::string;
using std::vector;

using namespace routing::diagnosis;

namespace mesos {
namespace internal {
namespace tests {

class NetworkPortsIsolatorTest : public MesosTest {};


// This test verifies that we can correctly detect sockets that
// a process is listening on. We take advantage of the fact that
// libprocess always implicitly listens on a socket, so we can
// query our current PID for listening sockets and verify that
// result against the libprocess address.
TEST(NetworkPortsIsolatorUtilityTest, QueryProcessSockets)
{
  Try<hashmap<uint32_t, socket::Info>> listeners =
    NetworkPortsIsolatorProcess::getListeningSockets();

  ASSERT_SOME(listeners);
  EXPECT_GT(listeners->size(), 0u);

  foreachvalue (const socket::Info& info, listeners.get()) {
    EXPECT_SOME(info.sourceIP);
    EXPECT_SOME(info.sourcePort);
  }

  Try<std::vector<uint32_t>> socketInodes =
    NetworkPortsIsolatorProcess::getProcessSockets(getpid());

  ASSERT_SOME(socketInodes);
  EXPECT_GT(socketInodes->size(), 0u);

  vector<socket::Info> socketInfos;

  // Collect the Info for our own listening sockets.
  foreach (uint32_t inode, socketInodes.get()) {
    if (listeners->contains(inode)) {
        socketInfos.push_back(listeners->at(inode));
    }
  }

  // libprocess always listens on a socket, so the fact that we
  // are running implies that we should at least find out about
  // the libprocess socket.
  EXPECT_GT(socketInfos.size(), 0u);

  bool matched = false;
  process::network::inet::Address processAddress = process::address();

  foreach (const auto& info, socketInfos) {
    // We can only match on the port, since libprocess will typically
    // indicate that it is listening on the ANY address (i.e. 0.0.0.0)
    // but the socket diagnostics will publish the actual address of a
    // network interface.
    if (ntohs(info.sourcePort.get()) == processAddress.port) {
      matched = true;
    }
  }

  // Verify that we matched the libprocess address in the set of
  // listening sockets for this process.
  EXPECT_TRUE(matched) << "Unmatched libprocess address "
                       << processAddress;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
