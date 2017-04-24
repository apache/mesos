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

#include <jvm/jvm.hpp>

#include <jvm/java/io.hpp>
#include <jvm/java/net.hpp>

#include <jvm/org/apache/zookeeper.hpp>

#include <stout/check.hpp>
#include <stout/os.hpp>

#include "logging/logging.hpp"

#include "tests/zookeeper_test_server.hpp"

using org::apache::zookeeper::server::persistence::FileTxnSnapLog;

using org::apache::zookeeper::server::NIOServerCnxnFactory;
using org::apache::zookeeper::server::ZooKeeperServer;

namespace mesos {
namespace internal {
namespace tests {

ZooKeeperTestServer::ZooKeeperTestServer()
  : zooKeeperServer(nullptr),
    connectionFactory(nullptr),
    port(0),
    started(false)
{
  // Create temporary directories for the FileTxnSnapLog.
  Try<std::string> directory = os::mkdtemp();
  CHECK_SOME(directory);
  java::io::File dataDir(directory.get());
  dataDir.deleteOnExit();

  directory = os::mkdtemp();
  CHECK_SOME(directory);
  java::io::File snapDir(directory.get());
  snapDir.deleteOnExit();

  zooKeeperServer = new ZooKeeperServer(
      FileTxnSnapLog(dataDir, snapDir),
      ZooKeeperServer::BasicDataTreeBuilder());
}


ZooKeeperTestServer::~ZooKeeperTestServer()
{
  shutdownNetwork();

  delete zooKeeperServer;
  delete connectionFactory;
}


void ZooKeeperTestServer::expireSession(int64_t sessionId)
{
  zooKeeperServer->closeSession(sessionId);
}


void ZooKeeperTestServer::setMinSessionTimeout(const Duration& min)
{
  // ZooKeeper server uses int representation of milliseconds for
  // session timeouts.
  // See http://zookeeper.apache.org/doc/trunk/zookeeperAdmin.html
  zooKeeperServer->setMinSessionTimeout(static_cast<int>(min.ms()));
}


void ZooKeeperTestServer::setMaxSessionTimeout(const Duration& max)
{
  // See the comment for setMinSessionTimeout.
  zooKeeperServer->setMaxSessionTimeout(static_cast<int>(max.ms()));
}


Duration ZooKeeperTestServer::getMaxSessionTimeout() const
{
  return Milliseconds(zooKeeperServer->getMaxSessionTimeout());
}


Duration ZooKeeperTestServer::getMinSessionTimeout() const
{
  return Milliseconds(zooKeeperServer->getMinSessionTimeout());
}


std::string ZooKeeperTestServer::connectString() const
{
  CHECK(port > 0) << "Illegal state, must call startNetwork first!";
  return "127.0.0.1:" + stringify(port);
}


void ZooKeeperTestServer::shutdownNetwork()
{
  if (connectionFactory != nullptr && started) {
    LOG(INFO) << "Shutting down ZooKeeperTestServer on port " << port;
    connectionFactory->shutdown();
    delete connectionFactory;
    connectionFactory = nullptr;
    started = false;
  }
}


int ZooKeeperTestServer::startNetwork()
{
  if (!started) {
    connectionFactory = new NIOServerCnxnFactory();

    // We use '-1' to allow an unlimited number of connections for this
    // connection factory instance.
    connectionFactory->configure(java::net::InetSocketAddress(port), -1);

    // It's possible that we are restarting the network, but not the
    // server. Unfortunately, we can't just invoke 'startup' on the
    // NIOServerCnxnFactory because of a bug in ZooKeeperServer (see
    // MESOS-670). We remedy this with a giant hack: by setting the
    // 'sessionTracker' variable on ZooKeeperServer to null then a new
    // instance will get created and everything will work out. Until
    // ZooKeeper provides mechanisms for doing in-memory testing this
    // hack (or something like it) will need to exist. This hack is
    // specific to ZooKeeper 3.4.8 and may need to change if up
    // upgrade ZooKeeper.
    zooKeeperServer->sessionTracker = Jvm::Null();

    connectionFactory->startup(*zooKeeperServer);

    if (port == 0) {
      // We save the ephemeral port so if/when we restart the network
      // the clients will reconnect to the same server. Note that this
      // might not actually be kosher because it's possible that another
      // process could bind to our ephemeral port after we unbind.
      port = zooKeeperServer->getClientPort();
    }

    LOG(INFO) << "Started ZooKeeperTestServer on port " << port;
    started = true;
  }

  return port;
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
