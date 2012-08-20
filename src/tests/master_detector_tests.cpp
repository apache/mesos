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

#include <gmock/gmock.h>

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <mesos/executor.hpp>
#include <mesos/scheduler.hpp>

#include <stout/os.hpp>
#include <stout/try.hpp>

#include "detector/detector.hpp"

#include "master/master.hpp"

#include "slave/slave.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::test;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using process::PID;
using process::UPID;

using std::map;
using std::vector;

using testing::_;

TEST(MasterDetector, File)
{
  ASSERT_TRUE(GTEST_IS_THREADSAFE);

  TestAllocatorProcess a;
  Master m(&a);
  PID<Master> master = process::spawn(&m);

  map<ExecutorID, Executor*> execs;

  TestingIsolationModule isolationModule(execs);

  Resources resources = Resources::parse("cpus:2;mem:1024");

  Slave s(resources, true, &isolationModule);
  PID<Slave> slave = process::spawn(&s);

  // Write "master" to a file and use the "file://" mechanism to
  // create a master detector for the slave. Still requires a master
  // detector for the master first. TODO(benh): We should really make
  // a utility for creating temporary files/directories.

  BasicMasterDetector detector1(master, vector<UPID>(), true);

  std::string path = "/tmp/mesos-tests_" + UUID::random().toString();
  std::ofstream file(path.c_str());
  ASSERT_TRUE(file.is_open());
  file << master << std::endl;
  file.close();

  Try<MasterDetector*> detector2 =
    MasterDetector::create("file://" + path, slave, false, true);

  os::rm(path);

  ASSERT_TRUE(detector2.isSome());

  MockScheduler sched;
  MesosSchedulerDriver driver(&sched, DEFAULT_FRAMEWORK_INFO, master);

  trigger resourceOffersCall;

  EXPECT_CALL(sched, registered(&driver, _, _))
    .Times(1);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(Trigger(&resourceOffersCall));

  driver.start();

  WAIT_UNTIL(resourceOffersCall);

  driver.stop();
  driver.join();

  process::terminate(slave);
  process::wait(slave);

  process::terminate(master);
  process::wait(master);
}
