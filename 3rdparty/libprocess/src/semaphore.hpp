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

#ifndef __PROCESS_SEMAPHORE_HPP__
#define __PROCESS_SEMAPHORE_HPP__

#ifdef __MACH__
#include <mach/mach.h>
#elif __WINDOWS__
#include <stout/windows.hpp>
#else
#include <semaphore.h>
#endif // __MACH__

#include <stout/check.hpp>

// TODO(benh): Introduce a user-level semaphore that _only_ traps into
// the kernel if the thread would actually need to wait.

// TODO(benh): Add tests for these!

#ifdef __MACH__
class KernelSemaphore
{
public:
  KernelSemaphore()
  {
    CHECK_EQ(
        KERN_SUCCESS,
        semaphore_create(mach_task_self(), &semaphore, SYNC_POLICY_FIFO, 0));
  }

  KernelSemaphore(const KernelSemaphore& other) = delete;

  ~KernelSemaphore()
  {
    CHECK_EQ(KERN_SUCCESS, semaphore_destroy(mach_task_self(), semaphore));
  }

  KernelSemaphore& operator=(const KernelSemaphore& other) = delete;

  void wait()
  {
    CHECK_EQ(KERN_SUCCESS, semaphore_wait(semaphore));
  }

  void signal()
  {
    CHECK_EQ(KERN_SUCCESS, semaphore_signal(semaphore));
  }

private:
  semaphore_t semaphore;
};
#elif __WINDOWS__
class KernelSemaphore
{
public:
  KernelSemaphore()
  {
    semaphore = CHECK_NOTNULL(CreateSemaphore(nullptr, 0, LONG_MAX, nullptr));
  }

  KernelSemaphore(const KernelSemaphore& other) = delete;

  ~KernelSemaphore()
  {
    CHECK(CloseHandle(semaphore));
  }

  KernelSemaphore& operator=(const KernelSemaphore& other) = delete;

  void wait()
  {
    CHECK_EQ(WAIT_OBJECT_0, WaitForSingleObject(semaphore, INFINITE));
  }

  void signal()
  {
    CHECK(ReleaseSemaphore(semaphore, 1, nullptr));
  }

private:
  HANDLE semaphore;
};
#else
class KernelSemaphore
{
public:
  KernelSemaphore()
  {
    PCHECK(sem_init(&semaphore, 0, 0) == 0);
  }

  KernelSemaphore(const KernelSemaphore& other) = delete;

  ~KernelSemaphore()
  {
    PCHECK(sem_destroy(&semaphore) == 0);
  }

  KernelSemaphore& operator=(const KernelSemaphore& other) = delete;

  void wait()
  {
    int result = sem_wait(&semaphore);

    while (result != 0 && errno == EINTR) {
      result = sem_wait(&semaphore);
    }

    PCHECK(result == 0);
  }

  void signal()
  {
    PCHECK(sem_post(&semaphore) == 0);
  }

private:
  sem_t semaphore;
};
#endif // __MACH__


// Provides a "decomissionable" kernel semaphore which allows us to
// effectively flush all waiters and keep any future threads from
// waiting. In order to be able to decomission the semaphore we need
// to keep around the number of waiters so we can signal them all.
class DecomissionableKernelSemaphore : public KernelSemaphore
{
public:
  void wait()
  {
    // NOTE: we must check `commissioned` AFTER we have incremented
    // `waiters` otherwise we might race with `decomission()` and fail
    // to properly get signaled.
    waiters.fetch_add(1);

    if (!comissioned.load()) {
      waiters.fetch_sub(1);
      return;
    }

    KernelSemaphore::wait();

    waiters.fetch_sub(1);
  }

  void decomission()
  {
    comissioned.store(false);

    // Now signal all the waiters so they wake up and stop
    // waiting. Note that this may do more `signal()` than necessary
    // but since no future threads will wait that doesn't matter (it
    // would only matter if we cared about the value of the semaphore
    // which in the current implementation we don't).
    for (size_t i = waiters.load(); i > 0; i--) {
      signal();
    }
  }

  bool decomissioned() const
  {
    return !comissioned.load();
  }

private:
  std::atomic<bool> comissioned = ATOMIC_VAR_INIT(true);
  std::atomic<size_t> waiters = ATOMIC_VAR_INIT(0);
};

#endif // __PROCESS_SEMAPHORE_HPP__
