#ifndef __TESTING_ZOO_KEEPER_SERVER_HPP__
#define __TESTING_ZOO_KEEPER_SERVER_HPP__

#include <jni.h>

#include <glog/logging.h>

#include "common/utils.hpp"

#include "tests/jvm.hpp"

#include "zookeeper/zookeeper.hpp"

namespace mesos {
namespace internal {
namespace test {

// An in-process ZooKeeper server that can be manipulated to control repeatable
// client tests.  Sessions can be programmatically expired and network
// partitions can be forced to simulate common failure modes.
class ZooKeeperServer
{
public:
  ZooKeeperServer(Jvm* jvm);
  ~ZooKeeperServer();

  // Gets a connection string that can be used to attach a ZooKeeper client to
  // this server.
  std::string connectString() const;

  // Shuts down the network connection to this server.
  void shutdownNetwork();

  // Starts the network connection to this server.  Binds an ephemeral port on
  // the first call and re-uses the port on subsequent calls.
  int startNetwork();

  // Forces the server to expire the given session immediately.
  void expireSession(int64_t sessionId);

private:
  // TODO(John Sirois): factor out TemporaryDirectory + createTempDir() to utils
  struct TemporaryDirectory
  {
    Jvm* jvm;
    const std::string path;
    const jobject file;

    TemporaryDirectory(Jvm* _jvm,
                       const std::string& _path,
                       const jobject _file) : jvm(_jvm),
                                              path(_path),
                                              file(_file) {}

    ~TemporaryDirectory()
    {
      jvm->deleteGlobalRef(file);
      if (!utils::os::rmdir(path)) {
        LOG(WARNING) << "Failed to delete temp dir: " << path;
      }
    }
  };

  Jvm* jvm;

  Jvm::JConstructor* fileConstructor;
  jobject snapLog;
  jobject dataTreeBuilder;
  jobject zooKeeperServer;
  Jvm::JMethod* getClientPort;
  Jvm::JMethod* closeSession;

  Jvm::JConstructor* inetSocketAddressConstructor;
  jobject inetSocketAddress;
  Jvm::JConstructor* cnxnFactoryConstructor;
  jobject connectionFactory;
  Jvm::JMethod* startup;
  Jvm::JMethod* isAlive;
  Jvm::JMethod* shutdown;

  int port;
  bool started;
  const TemporaryDirectory* dataDir;
  const TemporaryDirectory* snapDir;

  const TemporaryDirectory* createTempDir();
  void checkStarted() const;
};

} // namespace test
} // namespace internal
} // namespace mesos

#endif // __TESTING_ZOO_KEEPER_SERVER_HPP__
