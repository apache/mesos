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

#include <unistd.h> // For getpid, getppid.

#include <gmock/gmock.h>

#include <set>

#include <stout/gtest.hpp>
#include <stout/proc.hpp>
#include <stout/try.hpp>

using proc::CPU;
using proc::SystemStatus;
using proc::ProcessStatus;

using std::set;


TEST(ProcTest, pids)
{
  Try<set<pid_t> > pids = proc::pids();

  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().size());
  EXPECT_EQ(1u, pids.get().count(getpid()));
  EXPECT_EQ(1u, pids.get().count(1));
}


TEST(ProcTest, children)
{
  Try<set<pid_t> > children = proc::children(getpid());

  ASSERT_SOME(children);
  EXPECT_EQ(0u, children.get().size());

  // Use pipes to determine the pids of the child and grandchild.
  int childPipes[2];
  int grandchildPipes[2];
  ASSERT_NE(-1, pipe(childPipes));
  ASSERT_NE(-1, pipe(grandchildPipes));

  pid_t childPid;
  pid_t grandchildPid;
  pid_t pid = fork();
  ASSERT_NE(-1, pid);

  if (pid > 0) {
    // In parent process.
    close(childPipes[1]);
    close(grandchildPipes[1]);

    // Get the pids via the pipes.
    ASSERT_NE(
        -1,
        read(childPipes[0], &childPid, sizeof(childPid)));
    ASSERT_NE(
        -1,
        read(grandchildPipes[0], &grandchildPid, sizeof(grandchildPid)));

    close(childPipes[0]);
    close(grandchildPipes[0]);
  } else {
    // In child process.
    close(childPipes[0]);
    close(grandchildPipes[0]);

    // Double fork!
    if ((pid = fork()) == -1) {
      perror("Failed to fork a grand child process");
      abort();
    }

    if (pid > 0) {
      // Still in child process.
      pid = getpid();
      if (write(childPipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
        perror("Failed to write PID on pipe");
        abort();
      }

      close(childPipes[1]);

      while (true); // Keep waiting until we get a signal.
    } else {
      // In grandchild process.
      pid = getpid();
      if (write(grandchildPipes[1], &pid, sizeof(pid)) != sizeof(pid)) {
        perror("Failed to write PID on pipe");
        abort();
      }

      close(grandchildPipes[1]);

      while (true); // Keep waiting until we get a signal.
    }
  }

  // Ensure the non-recursive children does not include the
  // grandchild.
  children = proc::children(getpid(), false);

  ASSERT_SOME(children);
  EXPECT_EQ(1u, children.get().size());
  EXPECT_EQ(1u, children.get().count(childPid));

  children = proc::children(getpid());

  ASSERT_SOME(children);
  EXPECT_EQ(2u, children.get().size());
  EXPECT_EQ(1u, children.get().count(childPid));
  EXPECT_EQ(1u, children.get().count(grandchildPid));

  // Cleanup by killing the descendants.
  EXPECT_EQ(0, kill(grandchildPid, SIGKILL));
  EXPECT_EQ(0, kill(childPid, SIGKILL));
}


TEST(ProcTest, cpus)
{
  Try<std::list<CPU> > cpus = proc::cpus();

  ASSERT_SOME(cpus);
  EXPECT_LE(1u, cpus.get().size());
}


TEST(ProcTest, SystemStatus)
{
  Try<SystemStatus> status = proc::status();

  ASSERT_SOME(status);
  EXPECT_NE(0u, status.get().btime);
}


TEST(ProcTest, ProcessStatus)
{
  Try<ProcessStatus> status = proc::status(getpid());

  ASSERT_SOME(status);
  EXPECT_EQ(getpid(), status.get().pid);
  EXPECT_EQ(getppid(), status.get().ppid);
}
