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

#ifndef __ZOOKEEPER_TEST_HPP__
#define __ZOOKEEPER_TEST_HPP__

#include <pthread.h>

#include <gtest/gtest.h>

#include <queue>

#include <tr1/functional>

#include <stout/duration.hpp>

#include "jvm/jvm.hpp"

#include "tests/zookeeper_test_server.hpp"

namespace mesos {
namespace internal {
namespace tests {

// A fixture for tests that need to interact with a ZooKeeper server
// ensemble. Tests can access the in process ZooKeeperTestServer via
// the variable 'server'. This test fixture ensures the server is
// started before each test and shutdown after it so that each test is
// presented with a ZooKeeper ensemble with no data or watches.
class ZooKeeperTest : public ::testing::Test
{
public:
  // A watcher that is useful to install in a ZooKeeper client for
  // tests. Allows easy blocking on expected events.
  class TestWatcher : public Watcher
  {
  public:

    // Encapsulates all the state of a ZooKeeper watcher event.
    struct Event {
      Event(int _type, int _state, const std::string& _path)
          : type(_type), state(_state), path(_path) {}
      const int type;
      const int state;
      const std::string path;
    };

    TestWatcher();
    virtual ~TestWatcher();

    virtual void process(
        ZooKeeper* zk,
        int type,
        int state,
        const std::string& path);

    // Blocks until the session event of the given state fires.
    void awaitSessionEvent(int state);

    // Blocks until a node appears at the given path.
    void awaitCreated(const std::string& path);

    // Blocks until an event is fired matching the given predicate.
    Event awaitEvent(const std::tr1::function<bool(Event)>& matches);

    // Blocks until an event is fired.
    Event awaitEvent();

  private:
    std::queue<Event> events;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
  };

  ZooKeeperTest() : server(NULL) {}

  static void SetUpTestCase();

protected:
  virtual void SetUp();
  virtual void TearDown();

  // A very long session timeout that simulates no timeout for test cases.
  static const Milliseconds NO_TIMEOUT;

  ZooKeeperTestServer* server;

private:
  static Jvm* jvm;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __ZOOKEEPER_TEST_HPP__
