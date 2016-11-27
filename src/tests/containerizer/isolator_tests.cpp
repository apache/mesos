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

#include <unistd.h>

#include <string>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include <mesos/mesos.hpp>

#ifdef __linux__
#include "linux/ns.hpp"
#endif

#include "slave/containerizer/fetcher.hpp"

#include "slave/containerizer/mesos/containerizer.hpp"

#include "tests/mesos.hpp"

using std::string;

using process::Future;
using process::Owned;

using mesos::internal::slave::Fetcher;
using mesos::internal::slave::MesosContainerizer;

using mesos::slave::ContainerTermination;

namespace mesos {
namespace internal {
namespace tests {

#ifdef __linux__
class NamespacesPidIsolatorTest : public MesosTest {};


TEST_F(NamespacesPidIsolatorTest, ROOT_PidNamespace)
{
  slave::Flags flags = CreateSlaveFlags();
  flags.isolation = "filesystem/linux,namespaces/pid";

  string directory = os::getcwd(); // We're inside a temporary sandbox.

  Fetcher fetcher;

  Try<MesosContainerizer*> _containerizer =
    MesosContainerizer::create(flags, false, &fetcher);

  ASSERT_SOME(_containerizer);
  Owned<MesosContainerizer> containerizer(_containerizer.get());

  ContainerID containerId;
  containerId.set_value(UUID::random().toString());

  // Write the command's pid namespace inode and init name to files.
  const string command =
    "stat -c %i /proc/self/ns/pid > ns && (cat /proc/1/comm > init)";

  Future<bool> launch = containerizer->launch(
      containerId,
      None(),
      createExecutorInfo("executor", command),
      directory,
      None(),
      SlaveID(),
      std::map<string, string>(),
      false);

  AWAIT_READY(launch);
  ASSERT_TRUE(launch.get());

  // Wait on the container.
  Future<Option<ContainerTermination>> wait = containerizer->wait(containerId);
  AWAIT_READY(wait);
  ASSERT_SOME(wait.get());

  // Check the executor exited correctly.
  EXPECT_TRUE(wait->get().has_status());
  EXPECT_EQ(0, wait->get().status());

  // Check that the command was run in a different pid namespace.
  Try<ino_t> testPidNamespace = ns::getns(::getpid(), "pid");
  ASSERT_SOME(testPidNamespace);

  Try<string> containerPidNamespace = os::read(path::join(directory, "ns"));
  ASSERT_SOME(containerPidNamespace);

  EXPECT_NE(stringify(testPidNamespace.get()),
            strings::trim(containerPidNamespace.get()));

  // Check that the word 'mesos' is the part of the name for the
  // container's 'init' process. This verifies that /proc has been
  // correctly mounted for the container.
  Try<string> init = os::read(path::join(directory, "init"));
  ASSERT_SOME(init);

  EXPECT_TRUE(strings::contains(init.get(), "mesos"));
}
#endif // __linux__

} // namespace tests {
} // namespace internal {
} // namespace mesos {
