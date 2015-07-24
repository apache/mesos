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

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <stout/foreach.hpp>
#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

#include <process/gtest.hpp>
#include <process/io.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include "mesos/resources.hpp"

#include "slave/containerizer/mesos/launch.hpp"

#include "linux/fs.hpp"

#include "tests/flags.hpp"
#include "tests/utils.hpp"

using namespace process;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

class Chroot
{
public:
  Chroot(const string& _rootfs)
    : rootfs(_rootfs) {}

  virtual ~Chroot() {}

  virtual Try<Subprocess> run(const string& command) = 0;

  const string rootfs;
};


class BasicLinuxChroot : public Chroot
{
public:
  static Try<Owned<Chroot>> create(const string& rootfs)
  {
    if (!os::exists(rootfs)) {
      return Error("rootfs does not exist");
    }

    if (os::system("cp -r /bin " + rootfs + "/") != 0) {
      return ErrnoError("Failed to copy /bin to chroot");
    }

    if (os::system("cp -r /lib " + rootfs + "/") != 0) {
      return ErrnoError("Failed to copy /lib to chroot");
    }

    if (os::system("cp -r /lib64 " + rootfs + "/") != 0) {
      return ErrnoError("Failed to copy /lib64 to chroot");
    }

    vector<string> directories = {"proc", "sys", "dev", "tmp"};
    foreach (const string& directory, directories) {
      Try<Nothing> mkdir = os::mkdir(path::join(rootfs, directory));
      if (mkdir.isError()) {
        return Error("Failed to create /" + directory + " in chroot: " +
                     mkdir.error());
      }
    }

    // We need to bind mount the rootfs so we can pivot on it.
    Try<Nothing> mount =
      fs::mount(rootfs, rootfs, None(), MS_BIND | MS_SLAVE, NULL);

    if (mount.isError()) {
      return Error("Failed to bind mount chroot rootfs: " + mount.error());
    }

    return Owned<Chroot>(new BasicLinuxChroot(rootfs));
  }

  virtual Try<Subprocess> run(const string& _command)
  {
    slave::MesosContainerizerLaunch::Flags launchFlags;

    CommandInfo command;
    command.set_value(_command);

    launchFlags.command = JSON::Protobuf(command);
    launchFlags.directory = "/tmp";
    launchFlags.pipe_read = open("/dev/zero", O_RDONLY);
    launchFlags.pipe_write = open("/dev/null", O_WRONLY);
    launchFlags.rootfs = rootfs;

    vector<string> argv(2);
    argv[0] = "mesos-containerizer";
    argv[1] = slave::MesosContainerizerLaunch::NAME;

    Try<Subprocess> s = subprocess(
        path::join(tests::flags.build_dir, "src", "mesos-containerizer"),
        argv,
        Subprocess::PATH("/dev/null"),
        Subprocess::PIPE(),
        Subprocess::FD(STDERR_FILENO),
        launchFlags,
        None(),
        None(),
        lambda::bind(&clone, lambda::_1));

    if (s.isError()) {
      close(launchFlags.pipe_read.get());
      close(launchFlags.pipe_write.get());
    } else {
      s.get().status().onAny([=]() {
        // Close when the subprocess terminates.
        close(launchFlags.pipe_read.get());
        close(launchFlags.pipe_write.get());
      });
    }

    return s;
  }

private:
  static pid_t clone(const lambda::function<int()>& f)
  {
    static unsigned long long stack[(8*1024*1024)/sizeof(unsigned long long)];

    return ::clone(
        _clone,
        &stack[sizeof(stack)/sizeof(stack[0]) - 1],  // Stack grows down.
        CLONE_NEWNS | SIGCHLD,   // Specify SIGCHLD as child termination signal.
        (void*) &f);
  }

  static int _clone(void* f)
  {
    const lambda::function<int()>* _f =
      static_cast<const lambda::function<int()>*> (f);

    return (*_f)();
  }

  BasicLinuxChroot(const string& rootfs) : Chroot(rootfs) {}

  ~BasicLinuxChroot()
  {
    // Because the test process has the rootfs as its cwd the umount
    // won't actually happen until the
    // TemporaryDirectoryTest::TearDown() changes back to the original
    // directory.
    fs::unmount(rootfs, MNT_DETACH);
  }
};


template <typename T>
class LaunchChrootTest : public TemporaryDirectoryTest {};


// TODO(idownes): Add tests for OSX chroots.
typedef ::testing::Types<BasicLinuxChroot> ChrootTypes;


TYPED_TEST_CASE(LaunchChrootTest, ChrootTypes);


TYPED_TEST(LaunchChrootTest, ROOT_DifferentRoot)
{
  Try<Owned<Chroot>> chroot = TypeParam::create(os::getcwd());
  ASSERT_SOME(chroot);

  // Add /usr/bin/stat into the chroot.
  const string usrbin = path::join(chroot.get()->rootfs, "usr", "bin");
  ASSERT_SOME(os::mkdir(usrbin));
  ASSERT_EQ(0, os::system("cp /usr/bin/stat " + path::join(usrbin, "stat")));

  Clock::pause();

  Try<Subprocess> s = chroot.get()->run(
      "/usr/bin/stat -c %i / >" + path::join("/", "stat.output"));

  CHECK_SOME(s);

  // Advance time until the internal reaper reaps the subprocess.
  while (s.get().status().isPending()) {
    Clock::advance(Seconds(1));
    Clock::settle();
  }

  AWAIT_ASSERT_READY(s.get().status());
  ASSERT_SOME(s.get().status().get());

  int status = s.get().status().get().get();
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(0, WEXITSTATUS(status));

  // Check the chroot has a different root by comparing the inodes.
  Try<ino_t> self = os::stat::inode("/");
  ASSERT_SOME(self);

  Try<string> read = os::read(path::join(chroot.get()->rootfs, "stat.output"));
  CHECK_SOME(read);

  Try<ino_t> other = numify<ino_t>(strings::trim(read.get()));
  ASSERT_SOME(other);

  EXPECT_NE(self.get(), other.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
