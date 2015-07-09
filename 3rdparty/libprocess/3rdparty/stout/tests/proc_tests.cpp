/**
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License
*/

#include <pthread.h>
#include <unistd.h> // For getpid, getppid.

#include <iostream>
#include <list>
#include <set>
#include <string>

#include <gmock/gmock.h>

#include <stout/abort.hpp>
#include <stout/gtest.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/proc.hpp>
#include <stout/try.hpp>

using proc::CPU;
using proc::SystemStatus;
using proc::ProcessStatus;

using std::set;
using std::string;


TEST(ProcTest, Pids)
{
  Try<set<pid_t> > pids = proc::pids();

  ASSERT_SOME(pids);
  EXPECT_NE(0u, pids.get().size());
  EXPECT_EQ(1u, pids.get().count(getpid()));
  EXPECT_EQ(1u, pids.get().count(1));
}


TEST(ProcTest, Cpus)
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
  Result<ProcessStatus> status = proc::status(getpid());

  ASSERT_SOME(status);
  EXPECT_EQ(getpid(), status.get().pid);
  EXPECT_EQ(getppid(), status.get().ppid);
}


// NOTE: This test assumes there is a single thread running for the test.
TEST(ProcTest, SingleThread)
{
  // Check we have the expected number of threads.
  Try<set<pid_t> > threads = proc::threads(::getpid());

  ASSERT_SOME(threads);
  EXPECT_EQ(1u, threads.get().size());
  EXPECT_EQ(1u, threads.get().count(::getpid()));
}


void* cancelFunction(void*)
{
  // Newly created threads have PTHREAD_CANCEL_ENABLE and
  // PTHREAD_CANCEL_DEFERRED so they can be cancelled from the main
  // thread.
  while (true) {
    // Use pthread_testcancel() as opposed to sleep() because we've
    // seen sleep() hang on certain linux machines even though sleep
    // should be a cancellation point.
    pthread_testcancel();
  }

  return NULL;
}


// NOTE: This test assumes there is only a single thread running for the test.
TEST(ProcTest, MultipleThreads)
{
  size_t numThreads = 5;

  pthread_t pthreads[numThreads];

  // Create additional threads.
  for (size_t i = 0; i < numThreads; i++)
  {
    EXPECT_EQ(0, pthread_create(&pthreads[i], NULL, cancelFunction, NULL));
  }

  // Check we have the expected number of threads.
  Try<set<pid_t> > threads = proc::threads(::getpid());

  ASSERT_SOME(threads);
  EXPECT_EQ(1u + numThreads, threads.get().size());
  EXPECT_EQ(1u, threads.get().count(::getpid()));

  // Terminate the threads.
  for (size_t i = 0; i < numThreads; i++)
  {
    EXPECT_EQ(0, pthread_cancel(pthreads[i]));
    EXPECT_EQ(0, pthread_join(pthreads[i], NULL));
  }

  // There is some delay before /proc updates after the threads have
  // terminated. We wait until this occurs before completing the test to ensure
  // a call to proc::threads in a subsequent test will not return these
  // threads, e.g., if tests are shuffled and ProcTest.SingleThread occurs
  // after this test.
  Duration elapsed = Duration::zero();
  while (true) {
    threads = proc::threads(::getpid());
    ASSERT_SOME(threads);

    if (threads.get().size() == 1) {
      break;
    }

    if (elapsed > Seconds(1)) {
      FAIL() << "Failed to wait for /proc to update for terminated threads";
    }

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  }
}
