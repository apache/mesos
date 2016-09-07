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

#include <gtest/gtest.h>

#include "linux/sched.hpp"

#include <process/gtest.hpp>
#include <process/reap.hpp>

#include <stout/gtest.hpp>

using sched::Policy;

namespace mesos {
namespace internal {
namespace tests {

// TODO(idownes): Test the priority and preemption behavior for
// running competing SCHED_OTHER and SCHED_IDLE tasks.

TEST(SchedTest, ROOT_PolicySelf)
{
  Try<Policy> original = sched::policy::get();
  ASSERT_SOME(original);

  Policy different = (original.get() == Policy::OTHER ? Policy::IDLE
                                                      : Policy::OTHER);

  // Change our own scheduling policy.
  EXPECT_SOME(sched::policy::set(different));
  EXPECT_SOME_EQ(different, sched::policy::get());

  // Change it back.
  EXPECT_SOME(sched::policy::set(original.get()));
  EXPECT_SOME_EQ(original.get(), sched::policy::get());
}


// Change the scheduling policy of a different process (our child).
TEST(SchedTest, ROOT_PolicyChild)
{
  Try<Policy> original = sched::policy::get();
  ASSERT_SOME(original);

  Policy different = (original.get() == Policy::OTHER ? Policy::IDLE
                                                      : Policy::OTHER);

  pid_t pid = ::fork();
  ASSERT_NE(-1, pid);

  if (pid == 0) {
    // Child.
    sleep(10);

    ABORT("Child process should not reach here");
  }

  // Continue in parent.
  // Check the child has inherited our policy.
  EXPECT_SOME_EQ(original.get(), sched::policy::get(pid));

  // Check we can change the child's policy.
  EXPECT_SOME(sched::policy::set(different, pid));
  EXPECT_SOME_EQ(different, sched::policy::get(pid));

  // Kill the child process.
  ASSERT_NE(-1, ::kill(pid, SIGKILL));

  // Wait for the child process.
  AWAIT_EXPECT_WTERMSIG_EQ(SIGKILL, process::reap(pid));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
