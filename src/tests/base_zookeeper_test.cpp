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

#include <signal.h>

#include <queue>

#include <glog/logging.h>

#include <gtest/gtest.h>

#include <tr1/functional>

#include "common/lock.hpp"

#include "tests/base_zookeeper_test.hpp"
#include "tests/jvm.hpp"
#include "tests/utils.hpp"
#include "tests/zookeeper_server.hpp"

using mesos::internal::test::mesosRoot;
using std::tr1::bind;
using std::tr1::function;

namespace params = std::tr1::placeholders;

namespace mesos {
namespace internal {
namespace test {

const milliseconds BaseZooKeeperTest::NO_TIMEOUT(5000);


static void silenceServerLogs(Jvm* jvm)
{
  Jvm::Attach attach(jvm);

  Jvm::JClass loggerClass = Jvm::JClass::forName("org/apache/log4j/Logger");
  jobject rootLogger = jvm->invokeStatic<jobject>(
      jvm->findStaticMethod(loggerClass.method("getRootLogger")
          .returns(loggerClass)));

  Jvm::JClass levelClass = Jvm::JClass::forName("org/apache/log4j/Level");
  jvm->invoke<void>(rootLogger,
      jvm->findMethod(loggerClass.method("setLevel").parameter(levelClass)
          .returns(jvm->voidClass)),
      jvm->getStaticField<jobject>(jvm->findStaticField(levelClass, "OFF")));
}


static void silenceClientLogs()
{
  // TODO(jsirois): Put this in the C++ API.
  zoo_set_debug_level(ZOO_LOG_LEVEL_ERROR);
}


void BaseZooKeeperTest::SetUpTestCase()
{
  static Jvm* singleton;
  if (singleton == NULL) {
    std::vector<std::string> opts;

    std::string zkHome = mesosRoot + "/third_party/zookeeper-3.3.1";
    std::string classpath = "-Djava.class.path=" +
        zkHome + "/zookeeper-3.3.1.jar:" +
        zkHome + "/lib/log4j-1.2.15.jar";
    LOG(INFO) << "Using classpath setup: " << classpath << std::endl;
    opts.push_back(classpath);
    singleton = new Jvm(opts);

    silenceServerLogs(singleton);
    silenceClientLogs();
  }

  // TODO(John Sirois): Introduce a mechanism to contribute classpath
  // requirements to a singleton Jvm, then access the singleton here.
  jvm = singleton;
}


void BaseZooKeeperTest::SetUp()
{
  zks = new ZooKeeperServer(jvm);
  zks->startNetwork();
};


void BaseZooKeeperTest::TearDown()
{
  zks->shutdownNetwork();
  delete zks;
};


Jvm* BaseZooKeeperTest::jvm = NULL;


BaseZooKeeperTest::TestWatcher::TestWatcher()
{
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  pthread_cond_init(&cond, 0);
}


BaseZooKeeperTest::TestWatcher::~TestWatcher()
{
  pthread_mutex_destroy(&mutex);
  pthread_cond_destroy(&cond);
}


void BaseZooKeeperTest::TestWatcher::process(ZooKeeper* zk,
                                             int type,
                                             int state,
                                             const std::string& path)
{
  Lock lock(&mutex);
  events.push(Event(type, state, path));
  pthread_cond_signal(&cond);
}


static bool isSessionState(const BaseZooKeeperTest::TestWatcher::Event& event,
                           int state)
{
  return event.type == ZOO_SESSION_EVENT && event.state == state;
}


void BaseZooKeeperTest::TestWatcher::awaitSessionEvent(int state)
{
  awaitEvent(bind(&isSessionState, params::_1, state));
}


static bool isCreated(const BaseZooKeeperTest::TestWatcher::Event& event,
                      const std::string& path)
{
  return event.type == ZOO_CHILD_EVENT && event.path == path;
}


void BaseZooKeeperTest::TestWatcher::awaitCreated(const std::string& path)
{
  awaitEvent(bind(&isCreated, params::_1, path));
}


BaseZooKeeperTest::TestWatcher::Event
BaseZooKeeperTest::TestWatcher::awaitEvent()
{
  Lock lock(&mutex);
  while (true) {
    while (events.empty()) {
      pthread_cond_wait(&cond, &mutex);
    }
    Event event = events.front();
    events.pop();
    return event;
  }
}


BaseZooKeeperTest::TestWatcher::Event
BaseZooKeeperTest::TestWatcher::awaitEvent(function<bool(Event)> matches)
{
  while (true) {
    Event event = awaitEvent();
    if (matches(event)) {
      return event;
    }
  }
}

} // namespace test
} // namespace internal
} // namespace mesos

