// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <unistd.h> // For getpid, getppid.

#include <condition_variable>
#include <iostream>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include <gmock/gmock.h>

#include <stout/abort.hpp>
#include <stout/gtest.hpp>
#include <stout/numify.hpp>
#include <stout/os.hpp>
#include <stout/proc.hpp>
#include <stout/synchronized.hpp>
#include <stout/try.hpp>

using proc::CPU;
using proc::SystemStatus;
using proc::ProcessStatus;

using std::set;
using std::string;


TEST(ProcTest, Pids)
{
  Try<set<pid_t>> pids = proc::pids();

  ASSERT_SOME(pids);
  EXPECT_FALSE(pids->empty());
  EXPECT_EQ(1u, pids->count(getpid()));
  EXPECT_EQ(1u, pids->count(1));
}


TEST(ProcTest, Cpus)
{
  Try<std::list<CPU>> cpus = proc::cpus();

  ASSERT_SOME(cpus);
  EXPECT_LE(1u, cpus->size());
}


TEST(ProcTest, SystemStatus)
{
  Try<SystemStatus> status = proc::status();

  ASSERT_SOME(status);
  EXPECT_NE(0u, status->btime);
}


TEST(ProcTest, ProcessStatus)
{
  Result<ProcessStatus> status = proc::status(getpid());

  ASSERT_SOME(status);
  EXPECT_EQ(getpid(), status->pid);
  EXPECT_EQ(getppid(), status->ppid);
}


// NOTE: This test assumes there is a single thread running for the test.
TEST(ProcTest, SingleThread)
{
  // Check we have the expected number of threads.
  Try<set<pid_t>> threads = proc::threads(::getpid());

  ASSERT_SOME(threads);
  EXPECT_EQ(1u, threads->size());
  EXPECT_EQ(1u, threads->count(::getpid()));
}


// NOTE: This test assumes there is only a single thread running for the test.
TEST(ProcTest, MultipleThreads)
{
  const size_t numThreads = 5;

  std::thread* runningThreads[numThreads];

  std::mutex mutex;
  std::condition_variable cond;
  bool stop = false;

  // Create additional threads.
  for (size_t i = 0; i < numThreads; i++) {
    runningThreads[i] = new std::thread([&mutex, &cond, &stop]() {
      // Wait until the main thread tells us to exit.
      synchronized (mutex) {
        while (!stop) {
          synchronized_wait(&cond, &mutex);
        }
      }
    });
  }

  // Check we have the expected number of threads.
  Try<set<pid_t>> threads = proc::threads(::getpid());

  ASSERT_SOME(threads);
  EXPECT_EQ(1u + numThreads, threads->size());
  EXPECT_EQ(1u, threads->count(::getpid()));

  // Terminate the additional threads.
  synchronized (mutex) {
    stop = true;
    cond.notify_all();
  }

  for (size_t i = 0; i < numThreads; i++) {
    runningThreads[i]->join();
    delete runningThreads[i];
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

    if (threads->size() == 1) {
      break;
    }

    if (elapsed > Seconds(1)) {
      FAIL() << "Failed to wait for /proc to update for terminated threads";
    }

    os::sleep(Milliseconds(5));
    elapsed += Milliseconds(5);
  }
}
