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

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <stout/tests/utils.hpp>

#include <process/gtest.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include "slave/containerizer/mesos/launch.hpp"

#include "tests/flags.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/rootfs.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// TODO(jieyu): Move this test to mesos_containerizer_tests.cpp once
// we have a filesystem isolator that supports changing rootfs.

class MesosContainerizerLaunchTest : public TemporaryDirectoryTest
{
public:
  Try<Subprocess> run(
      const string& _command,
      const Option<string>& rootfs = None())
  {
    slave::MesosContainerizerLaunch::Flags launchFlags;

    CommandInfo command;
    command.set_value(_command);

    launchFlags.command = JSON::protobuf(command);
    launchFlags.working_directory = "/tmp";
    launchFlags.pipe_read = open("/dev/zero", O_RDONLY);
    launchFlags.pipe_write = open("/dev/null", O_WRONLY);
    launchFlags.rootfs = rootfs;

    vector<string> argv(2);
    argv[0] = "mesos-containerizer";
    argv[1] = slave::MesosContainerizerLaunch::NAME;

    Try<Subprocess> s = subprocess(
        path::join(getLauncherDir(), "mesos-containerizer"),
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::FD(STDOUT_FILENO),
        Subprocess::FD(STDERR_FILENO),
        NO_SETSID,
        launchFlags,
        None(),
        lambda::bind(&os::clone, lambda::_1, CLONE_NEWNS | SIGCHLD));

    close(launchFlags.pipe_read.get());
    close(launchFlags.pipe_write.get());

    return s;
  }
};


TEST_F(MesosContainerizerLaunchTest, ROOT_ChangeRootfs)
{
  Try<Owned<Rootfs>> rootfs =
    LinuxRootfs::create(path::join(os::getcwd(), "rootfs"));

  ASSERT_SOME(rootfs);

  // Add /usr/bin/stat into the rootfs.
  ASSERT_SOME(rootfs.get()->add("/usr/bin/stat"));

  Clock::pause();

  Try<Subprocess> s = run(
      "/usr/bin/stat -c %i / >" + path::join("/", "stat.output"),
      rootfs.get()->root);

  ASSERT_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(process::MAX_REAP_INTERVAL());
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  // Check the rootfs has a different root by comparing the inodes.
  Try<ino_t> self = os::stat::inode("/");
  ASSERT_SOME(self);

  Try<string> read = os::read(path::join(rootfs.get()->root, "stat.output"));
  ASSERT_SOME(read);

  Try<ino_t> other = numify<ino_t>(strings::trim(read.get()));
  ASSERT_SOME(other);

  EXPECT_NE(self.get(), other.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
