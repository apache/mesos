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

#ifndef __PROCESS_RUN_QUEUE_HPP__
#define __PROCESS_RUN_QUEUE_HPP__

// At _configuration_ (i.e., build) time you can specify a few
// optimizations:
//
//  (1) --enable-lock-free-run-queue (autotools) or
//      -DENABLE_LOCK_FREE_RUN_QUEUE (cmake) which enables the
//      lock-free run queue implementation (see below for more details).
//
//  (2) --enable-last-in-first-out-fixed-size-semaphore (autotools) or
//      -DENABLE_LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE (cmake) which
//      enables an optimized semaphore implementation (see semaphore.hpp
//      for more details).
//
// By default we use the `LockingRunQueue` and
// `DecomissionableKernelSemaphore`.
//
// We choose to make these _compile-time_ decisions rather than
// _runtime_ decisions because we wanted the run queue implementation
// to be compile-time optimized (e.g., inlined, etc).

#ifdef LOCK_FREE_RUN_QUEUE
#include <concurrentqueue.h>
#endif // LOCK_FREE_RUN_QUEUE

#include <algorithm>
#include <list>

#include <process/process.hpp>

#include <stout/synchronized.hpp>

#include "semaphore.hpp"

namespace process {

#ifndef LOCK_FREE_RUN_QUEUE
class RunQueue
{
public:
  bool extract(ProcessBase* process)
  {
    synchronized (mutex) {
      std::list<ProcessBase*>::iterator it = std::find(
          processes.begin(),
          processes.end(),
          process);

      if (it != processes.end()) {
        processes.erase(it);
        return true;
      }
    }

    return false;
  }

  void wait()
  {
    semaphore.wait();
  }

  void enqueue(ProcessBase* process)
  {
    synchronized (mutex) {
      processes.push_back(process);
    }
    epoch.fetch_add(1);
    semaphore.signal();
  }

  // Precondition: `wait` must get called before `dequeue`!
  ProcessBase* dequeue()
  {
    synchronized (mutex) {
      if (!processes.empty()) {
        ProcessBase* process = processes.front();
        processes.pop_front();
        return process;
      }
    }

    return nullptr;
  }

  // NOTE: this function can't be const because `synchronized (mutex)`
  // is not const ...
  bool empty()
  {
    synchronized (mutex) {
      return processes.empty();
    }
  }

  void decomission()
  {
    semaphore.decomission();
  }

  size_t capacity() const
  {
    return semaphore.capacity();
  }

  // Epoch used to capture changes to the run queue when settling.
  std::atomic_long epoch = ATOMIC_VAR_INIT(0L);

private:
  std::list<ProcessBase*> processes;
  std::mutex mutex;

  // Semaphore used for threads to wait.
#ifndef LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE
  DecomissionableKernelSemaphore semaphore;
#else
  DecomissionableLastInFirstOutFixedSizeSemaphore semaphore;
#endif // LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE
};

#else // LOCK_FREE_RUN_QUEUE

class RunQueue
{
public:
  bool extract(ProcessBase*)
  {
    // NOTE: moodycamel::ConcurrentQueue does not provide a way to
    // implement extract so we simply return false here.
    return false;
  }

  void wait()
  {
    semaphore.wait();
  }

  void enqueue(ProcessBase* process)
  {
    queue.enqueue(process);
    epoch.fetch_add(1);
    semaphore.signal();
  }

  // Precondition: `wait` must get called before `dequeue`!
  ProcessBase* dequeue()
  {
    // NOTE: we loop _forever_ until we actually dequeue a process
    // because the contract for using the run queue is that `wait`
    // must be called first so we know that there is something to be
    // dequeued or the run queue has been decommissioned and we should
    // just return `nullptr`.
    ProcessBase* process = nullptr;
    while (!queue.try_dequeue(process)) {
      if (semaphore.decomissioned()) {
        break;
      }
    }
    return process;
  }

  bool empty() const
  {
    return queue.size_approx() == 0;
  }

  void decomission()
  {
    semaphore.decomission();
  }

  size_t capacity() const
  {
    return semaphore.capacity();
  }

  // Epoch used to capture changes to the run queue when settling.
  std::atomic_long epoch = ATOMIC_VAR_INIT(0L);

private:
  moodycamel::ConcurrentQueue<ProcessBase*> queue;

#ifndef LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE
  DecomissionableKernelSemaphore semaphore;
#else
  DecomissionableLastInFirstOutFixedSizeSemaphore semaphore;
#endif // LAST_IN_FIRST_OUT_FIXED_SIZE_SEMAPHORE
};

#endif // LOCK_FREE_RUN_QUEUE

} // namespace process {

#endif // __PROCESS_RUN_QUEUE_HPP__
