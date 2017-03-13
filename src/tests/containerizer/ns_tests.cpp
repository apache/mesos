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

#include <sys/wait.h>

#include <unistd.h>

#include <iostream>
#include <set>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/lambda.hpp>
#include <stout/os.hpp>

#include <process/gtest.hpp>
#include <process/latch.hpp>
#include <process/reap.hpp>
#include <process/subprocess.hpp>

#include "linux/ns.hpp"

#include "tests/flags.hpp"
#include "tests/utils.hpp"

#include "tests/containerizer/setns_test_helper.hpp"

using namespace process;

using std::set;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {


// Test that a child in different namespace(s) can setns back to the
// root namespace. We must fork a child to test this because setns
// doesn't support multi-threaded processes (which gtest is).
TEST(NsTest, ROOT_setns)
{
  // Clone then exec the setns-test-helper into a new namespace for
  // each available namespace.
  set<string> namespaces = ns::namespaces();
  ASSERT_FALSE(namespaces.empty());

  int flags = 0;

  foreach (const string& ns, namespaces) {
    // Skip 'user' namespace because it causes 'clone' to change us
    // from being user 'root' to user 'nobody', but these tests
    // require root. See MESOS-3083.
    if (ns == "user") {
      continue;
    }

    Try<int> nstype = ns::nstype(ns);
    ASSERT_SOME(nstype);

    flags |= nstype.get();
  }

  vector<string> argv;
  argv.push_back("test-helper");
  argv.push_back(SetnsTestHelper::NAME);

  Try<Subprocess> s = subprocess(
      getTestHelperPath("test-helper"),
      argv,
      Subprocess::FD(STDIN_FILENO),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO),
      nullptr,
      None(),
      [=](const lambda::function<int()>& child) {
        return os::clone(child, flags | SIGCHLD);
      });

  // Continue in parent.
  ASSERT_SOME(s);

  // The child should exit 0.
  AWAIT_EXPECT_WEXITSTATUS_EQ(0, s->status());
}


// Test that setns correctly refuses to re-associate to a namespace if
// the caller is multi-threaded.
TEST(NsTest, ROOT_setnsMultipleThreads)
{
  set<string> namespaces = ns::namespaces();
  EXPECT_LT(0u, namespaces.size());

  Latch* latch = new Latch();

  // Do not allow multi-threaded environment.
  std::thread thread([=]() {
    // Wait until the main thread tells us to exit.
    latch->await();
  });

  foreach (const string& ns, namespaces) {
    EXPECT_ERROR(ns::setns(::getpid(), ns));
  }

  // Terminate the thread.
  latch->trigger();

  thread.join();

  delete latch;
}


static int childGetns()
{
  // Sleep until killed.
  while (true) { sleep(1); }

  ABORT("Error, child should be killed before reaching here");
}


// Test that we can get the namespace inodes for a forked child.
TEST(NsTest, ROOT_getns)
{
  set<string> namespaces = ns::namespaces();

  // ns::setns() does not support "pid".
  namespaces.erase("pid");

  // Use the first other namespace available.
  ASSERT_FALSE(namespaces.empty());
  string ns = *(namespaces.begin());

  ASSERT_SOME(ns::getns(::getpid(), ns));

  Try<int> nstype = ns::nstype(ns);
  ASSERT_SOME(nstype);

  pid_t pid = os::clone(childGetns, SIGCHLD | nstype.get());

  ASSERT_NE(-1, pid);

  // Continue in parent.
  Try<ino_t> nsParent = ns::getns(::getpid(), ns);
  ASSERT_SOME(nsParent);

  Try<ino_t> nsChild = ns::getns(pid, ns);
  ASSERT_SOME(nsChild);

  // Child should be in a different namespace.
  EXPECT_NE(nsParent.get(), nsChild.get());

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, reap(pid));
}


// Test we can destroy a pid namespace, i.e., kill all processes.
TEST(NsTest, ROOT_destroy)
{
  set<string> namespaces = ns::namespaces();

  if (namespaces.count("pid") == 0) {
    // Pid namespace is not available.
    return;
  }

  Try<int> nstype = ns::nstype("pid");
  ASSERT_SOME(nstype);

  pid_t pid = os::clone([]() {
    // Fork a bunch of children.
    ::fork();
    ::fork();
    ::fork();

    // Parent and all children sleep.
    while (true) { sleep(1); }

    ABORT("Error, child should be killed before reaching here");

    return -1;
  },
  SIGCHLD | nstype.get());

  ASSERT_NE(-1, pid);

  // NOTE: need to call `reap` here because the `ns::pid::destroy`
  // also calls `reap` and so we won't get the right status if we call
  // `reap` after calling `ns::pid::destroy`.
  Future<Option<int>> status = reap(pid);

  // Ensure the child is in a different pid namespace.
  Try<ino_t> childNs = ns::getns(pid, "pid");
  ASSERT_SOME(childNs);

  Try<ino_t> ourNs = ns::getns(::getpid(), "pid");
  ASSERT_SOME(ourNs);

  ASSERT_NE(ourNs.get(), childNs.get());

  // Kill the child.
  AWAIT_READY(ns::pid::destroy(childNs.get()));

  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, status);

  // Finally, verify that no processes are in the child's pid
  // namespace, i.e., destroy() also killed all descendants.
  Try<set<pid_t>> pids = os::pids();
  ASSERT_SOME(pids);

  foreach (pid_t pid, pids.get()) {
    Try<ino_t> otherNs = ns::getns(pid, "pid");
    // pid may have exited since getting the snapshot of pids so
    // ignore any error.
    if (otherNs.isSome()) {
      ASSERT_SOME_NE(childNs.get(), otherNs);
    }
  }
}


TEST(NsTest, ROOT_clone)
{
  // `ns::clone` does not support user namespaces yet.
  ASSERT_ERROR(ns::clone(getpid(), CLONE_NEWUSER, []() { return -1; }, 0));

  // Determine the namespaces this kernel supports and test them,
  // skipping the user namespace for now because it's not fully
  // supported depending on the filesystem, which we don't check for.
  int nstypes = 0;
  foreach (int nstype, ns::nstypes()) {
    if (nstype != CLONE_NEWUSER) {
      nstypes |= nstype;
    }
  }

  pid_t parent = os::clone([]() {
    while (true) { sleep(1); }
    ABORT("Error, process should be killed before reaching here");
    return -1;
  },
  SIGCHLD | nstypes);

  ASSERT_NE(-1, parent);

  Try<pid_t> child = ns::clone(parent, nstypes, []() {
    while (true) { sleep(1); }
    ABORT("Error, process should be killed before reaching here");
    return -1;
  },
  SIGCHLD);

  ASSERT_SOME(child);

  foreach (const string& ns, ns::namespaces()) {
    // See comment above as to why we're skipping the namespace.
    if (ns == "user") {
      continue;
    }

    Try<ino_t> inode = ns::getns(parent, ns);
    ASSERT_SOME(inode);
    EXPECT_SOME_NE(inode.get(), ns::getns(getpid(), ns));
    EXPECT_SOME_EQ(inode.get(), ns::getns(child.get(), ns));
  }

  // Now kill the parent which should cause the child to exit since
  // it's in the same PID namespace.
  ASSERT_NE(-1, kill(parent, SIGKILL));
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, reap(parent));

  // We can reap the child but we don't expect any status because it's
  // not a direct descendent because `ns::clone` does a fork before
  // clone.
  Future<Option<int>> status = reap(child.get());
  AWAIT_READY(status);
  EXPECT_NONE(status.get());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
