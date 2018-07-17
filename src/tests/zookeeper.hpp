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

#ifndef __TESTS_ZOOKEEPER_HPP__
#define __TESTS_ZOOKEEPER_HPP__

#include <stdint.h>

#include <condition_variable>
#include <mutex>
#include <queue>

#include <gtest/gtest.h>

#include <stout/duration.hpp>

#include "tests/zookeeper_test_server.hpp"

namespace mesos {
namespace internal {
namespace tests {

// Helper for invoking ZooKeeper::get(path, ...) in order to check the
// data stored at a specified znode path.
inline ::testing::AssertionResult AssertZKGet(
    const char* expectedExpr,
    const char* zkExpr,
    const char* pathExpr,
    const std::string& expected,
    ZooKeeper* zk,
    const std::string& path)
{
  std::string result;
  int code = zk->get(path, false, &result, nullptr);
  if (code == ZOK) {
    if (expected == result) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure()
        << "Expected data at znode '" << pathExpr << "' "
        << "to be '" << expected << "', but actually '" << result << "'";
    }
  } else {
    return ::testing::AssertionFailure()
      << "(" << zkExpr << ").get(" << pathExpr << ", ...): "
      << zk->message(code);
  }
}

#define ASSERT_ZK_GET(expected, zk, path)                               \
  ASSERT_PRED_FORMAT3(mesos::internal::tests::AssertZKGet, expected, zk, path)


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

    TestWatcher() = default;
    ~TestWatcher() override = default;

    void process(
        int type,
        int state,
        int64_t sessionId,
        const std::string& path) override;

    // Blocks until the session event of the given state fires.
    void awaitSessionEvent(int state);

    // Blocks until a node appears at the given path.
    void awaitCreated(const std::string& path);

    // Blocks until an event is fired matching the given predicate.
    Event awaitEvent(const lambda::function<bool(Event)>& matches);

    // Blocks until an event is fired.
    Event awaitEvent();

  private:
    std::queue<Event> events;
    std::mutex mutex;
    std::condition_variable cond;
  };

  static void SetUpTestCase();

protected:
  ZooKeeperTest() : server(new ZooKeeperTestServer()) {}
  ~ZooKeeperTest() override { delete server; }

  void SetUp() override;

  // A very long session timeout that simulates no timeout for test cases.
  static const Duration NO_TIMEOUT;

  // TODO(benh): Share the same ZooKeeperTestServer across all tests?
  ZooKeeperTestServer* server;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __ZOOKEEPER_TEST_HPP__
