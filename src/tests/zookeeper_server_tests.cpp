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

#include <glog/logging.h>

#include <gtest/gtest.h>

#include "tests/base_zookeeper_test.hpp"
#include "tests/zookeeper_server.hpp"

namespace mesos {
namespace internal {
namespace test {

class ZooKeeperServerTest : public BaseZooKeeperTest {};


TEST_F(ZooKeeperServerTest, InProcess)
{
  ZooKeeperServerTest::TestWatcher watcher;
  ZooKeeper zk(zks->connectString(), NO_TIMEOUT, &watcher);

  LOG(INFO) << "awaiting ZOO_CONNECTED_STATE";
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);

  zks->expireSession(zk.getSessionId());
  LOG(INFO) << "awaiting ZOO_EXPIRED_SESSION_STATE";
  watcher.awaitSessionEvent(ZOO_EXPIRED_SESSION_STATE);
}

} // namespace test
} // namespace internal
} // namespace mesos
