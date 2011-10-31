#include <jni.h>
#include <stdarg.h>

#include <glog/logging.h>

#include <sstream>
#include <vector>

#include "common/utils.hpp"
#include "common/uuid.hpp"

#include "tests/jvm.hpp"
#include "tests/zookeeper_server.hpp"

namespace mesos {
namespace internal {
namespace test {

ZooKeeperServer::ZooKeeperServer(Jvm* _jvm) : jvm(_jvm), port(0), started(false)
{
  Jvm::Attach attach(jvm);

  Jvm::JClass fileClass = Jvm::JClass::forName("java/io/File");
  fileConstructor =
      new Jvm::JConstructor(
          jvm->findConstructor(
              fileClass.constructor().parameter(jvm->stringClass)));

  Jvm::JClass inetSocketAddressClass =
      Jvm::JClass::forName("java/net/InetSocketAddress");
  inetSocketAddressConstructor =
      new Jvm::JConstructor(
          jvm->findConstructor(
              inetSocketAddressClass.constructor().parameter(jvm->intClass)));

  Jvm::JClass cnxnFactoryClass =
      Jvm::JClass::forName("org/apache/zookeeper/server/NIOServerCnxn$Factory");
  cnxnFactoryConstructor =
      new Jvm::JConstructor(
          jvm->findConstructor(cnxnFactoryClass.constructor()
              .parameter(inetSocketAddressClass)));

  Jvm::JClass zkServerClass =
      Jvm::JClass::forName("org/apache/zookeeper/server/ZooKeeperServer");
  startup =
      new Jvm::JMethod(
          jvm->findMethod(cnxnFactoryClass.method("startup")
              .parameter(zkServerClass)
              .returns(jvm->voidClass)));

  isAlive =
      new Jvm::JMethod(
          jvm->findMethod(cnxnFactoryClass.method("isAlive")
              .returns(jvm->booleanClass)));
  shutdown =
      new Jvm::JMethod(
          jvm->findMethod(cnxnFactoryClass.method("shutdown")
              .returns(jvm->voidClass)));

  dataDir = createTempDir();
  snapDir = createTempDir();
  Jvm::JClass snapLogClass =
      Jvm::JClass::forName("org/apache/zookeeper/server/"
                           "persistence/FileTxnSnapLog");

  snapLog =
      jvm->newGlobalRef(
          jvm->invoke(
              jvm->findConstructor(snapLogClass.constructor()
                  .parameter(fileClass).parameter(fileClass)),
              dataDir->file,
              snapDir->file));

  dataTreeBuilder =
      jvm->newGlobalRef(
          jvm->invoke(
              jvm->findConstructor(
                  Jvm::JClass::forName("org/apache/zookeeper/server/"
                                       "ZooKeeperServer$BasicDataTreeBuilder")
                                       .constructor())));

  Jvm::JClass dataTreeBuilderClass(
      Jvm::JClass::forName("org/apache/zookeeper/server/"
                           "ZooKeeperServer$DataTreeBuilder"));

  zooKeeperServer =
      jvm->newGlobalRef(
          jvm->invoke(
              jvm->findConstructor(zkServerClass.constructor()
                  .parameter(snapLogClass).parameter(dataTreeBuilderClass)),
              snapLog,
              dataTreeBuilder));

  getClientPort =
      new Jvm::JMethod(
          jvm->findMethod(zkServerClass.method("getClientPort")
              .returns(jvm->intClass)));
  closeSession =
      new Jvm::JMethod(
          jvm->findMethod(zkServerClass.method("closeSession")
              .parameter(jvm->longClass).returns(jvm->voidClass)));
}


const ZooKeeperServer::TemporaryDirectory* ZooKeeperServer::createTempDir()
{
  std::string tmpdir = "/tmp/zks-" + UUID::random().toString();
  jobject file =
      jvm->newGlobalRef(jvm->invoke(*fileConstructor, jvm->string(tmpdir)));
  return new TemporaryDirectory(jvm, tmpdir, file);
}


ZooKeeperServer::~ZooKeeperServer()
{
  shutdownNetwork();

  Jvm::Attach attach(jvm);

  jvm->deleteGlobalRefSafe(inetSocketAddress);
  jvm->deleteGlobalRefSafe(connectionFactory);
  jvm->deleteGlobalRefSafe(snapLog);
  jvm->deleteGlobalRefSafe(dataTreeBuilder);
  jvm->deleteGlobalRefSafe(zooKeeperServer);

  delete fileConstructor;
  delete getClientPort;
  delete closeSession;

  delete inetSocketAddressConstructor;
  delete cnxnFactoryConstructor;

  delete startup;
  delete isAlive;
  delete shutdown;

  delete dataDir;
  delete snapDir;
}


void ZooKeeperServer::expireSession(int64_t sessionId)
{
  Jvm::Attach attach(jvm);

  jvm->invoke<void>(zooKeeperServer, *closeSession, sessionId);
}


std::string ZooKeeperServer::connectString() const
{
  checkStarted();
  return "127.0.0.1:" + utils::stringify(port);
}


void ZooKeeperServer::shutdownNetwork()
{
  Jvm::Attach attach(jvm);

  if (started && jvm->invoke<bool>(connectionFactory, *isAlive)) {
    jvm->invoke<void>(connectionFactory, *shutdown);
    LOG(INFO) << "Shutdown ZooKeeperServer on port " << port << std::endl;
  }
}


int ZooKeeperServer::startNetwork()
{
  Jvm::Attach attach(jvm);

  inetSocketAddress =
      jvm->newGlobalRef(jvm->invoke(*inetSocketAddressConstructor, port));
  connectionFactory =
      jvm->newGlobalRef(
          jvm->invoke(*cnxnFactoryConstructor, inetSocketAddress));

  jvm->invoke<void>(connectionFactory, *startup, zooKeeperServer);
  port = jvm->invoke<int>(zooKeeperServer, *getClientPort);
  LOG(INFO) << "Started ZooKeeperServer on port " << port;
  started = true;
  return port;
}


void ZooKeeperServer::checkStarted() const
{
  CHECK(port > 0) << "Illegal state, must call startNetwork first!";
}

} // namespace test
} // namespace internal
} // namespace mesos

