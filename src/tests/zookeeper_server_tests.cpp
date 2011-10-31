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
