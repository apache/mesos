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

#ifndef __ZOOKEEPER_TEST_SERVER_HPP__
#define __ZOOKEEPER_TEST_SERVER_HPP__

#include <string>

#include <glog/logging.h>

#include <jvm/org/apache/zookeeper.hpp>

#include <mesos/zookeeper/zookeeper.hpp>

namespace mesos {
namespace internal {
namespace tests {

// An in-process ZooKeeper server that can be manipulated to control
// repeatable client tests. Sessions can be programmatically expired
// and network partitions can be forced to simulate common failure
// modes.
class ZooKeeperTestServer
{
public:
  ZooKeeperTestServer();
  ~ZooKeeperTestServer();

  // Gets a connection string that can be used to attach a ZooKeeper client to
  // this server.
  std::string connectString() const;

  // Shuts down the network connection to this server.
  void shutdownNetwork();

  // Starts the network connection to this server.  Binds an ephemeral port on
  // the first call and re-uses the port on subsequent calls.
  int startNetwork();

  // Forces the server to expire the given session.
  // Note that there is a delay (~3s) for the corresponding client to receive
  // a session expiration event notification from the ZooKeeper server.
  // TODO(vinod): Fix this so that the notification is immediate.
  // One possible solution is suggested at :
  // http://wiki.apache.org/hadoop/ZooKeeper/FAQ#A4
  // But according to,
  // http://comments.gmane.org/gmane.comp.java.hadoop.zookeeper.user/4489
  // the C binding for ZooKeeper does not yet support multiple
  // clients with the same session id.
  void expireSession(int64_t sessionId);

  // Sets/gets the min/max session timeouts enforced by the server.
  void setMinSessionTimeout(const Duration& min);
  void setMaxSessionTimeout(const Duration& max);
  Duration getMinSessionTimeout() const;
  Duration getMaxSessionTimeout() const;

private:
  org::apache::zookeeper::server::ZooKeeperServer* zooKeeperServer;
  org::apache::zookeeper::server::NIOServerCnxnFactory* connectionFactory;

  int port;
  bool started;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __ZOOKEEPER_TEST_SERVER_HPP__
