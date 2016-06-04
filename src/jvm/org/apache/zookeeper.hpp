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

#ifndef __ORG_APACHE_ZOOKEEPER_HPP__
#define __ORG_APACHE_ZOOKEEPER_HPP__

#include <jvm/jvm.hpp>

#include <jvm/java/io.hpp>
#include <jvm/java/net.hpp>


// Package 'org.apache.zookeeper.server'.

namespace org {
namespace apache {
namespace zookeeper {
namespace server {

// Package 'org.apache.zookeeper.server.persistence'.

namespace persistence {

class FileTxnSnapLog : public Jvm::Object
{
public:
  FileTxnSnapLog(const java::io::File& dataDir,
                 const java::io::File& snapDir)
  {
    static Jvm::Constructor constructor = Jvm::get()->findConstructor(
        Jvm::Class::named(
            "org/apache/zookeeper/server/persistence/FileTxnSnapLog")
        .constructor()
        .parameter(Jvm::Class::named("java/io/File"))
        .parameter(Jvm::Class::named("java/io/File")));

    object = Jvm::get()->invoke(
        constructor, (jobject) dataDir, (jobject) snapDir);
  }
};

} // namespace persistence {


class SessionTracker : public Jvm::Object {};


extern const char ZOOKEEPERSERVER_SESSIONTRACKER_SIGNATURE[];
extern const char ZOOKEEPERSERVER_SESSIONTRACKER[];


class ZooKeeperServer : public Jvm::Object
{
public:
  class DataTreeBuilder : public Jvm::Object {};

  class BasicDataTreeBuilder : public DataTreeBuilder
  {
  public:
    BasicDataTreeBuilder()
    {
      static Jvm::Constructor constructor = Jvm::get()->findConstructor(
          Jvm::Class::named(
              "org/apache/zookeeper/server/ZooKeeperServer$BasicDataTreeBuilder") // NOLINT(whitespace/line_length)
          .constructor());

      object = Jvm::get()->invoke(constructor);
    }
  };

  ZooKeeperServer(const persistence::FileTxnSnapLog& txnLogFactory,
                  const DataTreeBuilder& treeBuilder)
    : sessionTracker(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer"))
  {
    static Jvm::Constructor constructor = Jvm::get()->findConstructor(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer")
        .constructor()
        .parameter(
            Jvm::Class::named(
                "org/apache/zookeeper/server/persistence/FileTxnSnapLog"))
        .parameter(
            Jvm::Class::named(
                "org/apache/zookeeper/server/ZooKeeperServer$DataTreeBuilder"))); // NOLINT(whitespace/line_length)

    object = Jvm::get()->invoke(
        constructor, (jobject) txnLogFactory, (jobject) treeBuilder);

    // We need to "bind" the 'sessionTracker' Variable after we assign
    // 'object' above so that '*this' is a Jvm::Object instance that
    // doesn't point to a nullptr jobject.
    sessionTracker.bind(*this);
  }

  void setMaxSessionTimeout(int max)
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer")
        .method("setMaxSessionTimeout")
        .parameter(Jvm::get()->intClass)
        .returns(Jvm::get()->voidClass));

    Jvm::get()->invoke<void>(object, method, max);
  }

  void setMinSessionTimeout(int min)
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer")
        .method("setMinSessionTimeout")
        .parameter(Jvm::get()->intClass)
        .returns(Jvm::get()->voidClass));

    Jvm::get()->invoke<void>(object, method, min);
  }

  int getMaxSessionTimeout()
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer")
        .method("getMaxSessionTimeout")
        .returns(Jvm::get()->intClass));

    return Jvm::get()->invoke<int>(object, method);
  }

  int getMinSessionTimeout()
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer")
        .method("getMinSessionTimeout")
        .returns(Jvm::get()->intClass));

    return Jvm::get()->invoke<int>(object, method);
  }

  int getClientPort()
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer")
        .method("getClientPort")
        .returns(Jvm::get()->intClass));

    return Jvm::get()->invoke<int>(object, method);
  }

  void closeSession(int64_t sessionId)
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named("org/apache/zookeeper/server/ZooKeeperServer")
        .method("closeSession")
        .parameter(Jvm::get()->longClass)
        .returns(Jvm::get()->voidClass));

    Jvm::get()->invoke<void>(object, method, sessionId);
  }

  Jvm::Variable<SessionTracker,
                ZOOKEEPERSERVER_SESSIONTRACKER,
                ZOOKEEPERSERVER_SESSIONTRACKER_SIGNATURE> sessionTracker;
};


// TODO(benh): Extends ServerCnxnFactory implements Runnable.
class NIOServerCnxnFactory : public Jvm::Object
{
public:
  NIOServerCnxnFactory()
  {
    static Jvm::Constructor constructor = Jvm::get()->findConstructor(
        Jvm::Class::named(
            "org/apache/zookeeper/server/NIOServerCnxnFactory")
        .constructor());

    object = Jvm::get()->invoke(constructor);
  }

  void configure(const java::net::InetSocketAddress& addr, int maxcc)
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named(
            "org/apache/zookeeper/server/NIOServerCnxnFactory")
        .method("configure")
        .parameter(Jvm::Class::named("java/net/InetSocketAddress"))
        .parameter(Jvm::get()->intClass)
        .returns(Jvm::get()->voidClass));


    Jvm::get()->invoke<void>(object, method, (jobject) addr, maxcc);
  }

  void startup(const ZooKeeperServer& zks)
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named(
            "org/apache/zookeeper/server/NIOServerCnxnFactory")
        .method("startup")
        .parameter(Jvm::Class::named(
                       "org/apache/zookeeper/server/ZooKeeperServer"))
        .returns(Jvm::get()->voidClass));

    Jvm::get()->invoke<void>(object, method, (jobject) zks);
  }

  void shutdown()
  {
    static Jvm::Method method = Jvm::get()->findMethod(
        Jvm::Class::named(
            "org/apache/zookeeper/server/NIOServerCnxnFactory")
        .method("shutdown")
        .returns(Jvm::get()->voidClass));

    Jvm::get()->invoke<void>(object, method);
  }
};

} // namespace server {
} // namespace zookeeper {
} // namespace apache {
} // namespace org {

#endif // __ORG_APACHE_ZOOKEEPER_HPP__
