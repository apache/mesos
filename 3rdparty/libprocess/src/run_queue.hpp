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

// At _configuration_ (i.e., build) time you can specify
// RUN_QUEUE=... as an environment variable (i.e., just like CC or
// CXXFLAGS) to pick the run queue implementation. If nothing is
// specified we'll default to the LockingRunQueue.
//
// Alternatively we could have made this be a _runtime_ decision but
// for performance reasons we wanted the run queue implementation to
// be compile-time optimized (e.g., inlined, etc).
//
// Note that care should be taken not to reconfigure with a different
// value of RUN_QUEUE when reusing a build directory!
#define RUN_QUEUE LockingRunQueue

#ifdef LOCK_FREE_RUN_QUEUE
#define RUN_QUEUE LockFreeRunQueue
#include <concurrentqueue.h>
#endif // LOCK_FREE_RUN_QUEUE

#include <algorithm>
#include <list>

#include <process/process.hpp>

#include <stout/synchronized.hpp>

#include "semaphore.hpp"

namespace process {

class LockingRunQueue
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

  // Epoch used to capture changes to the run queue when settling.
  std::atomic_long epoch = ATOMIC_VAR_INIT(0L);

private:
  std::list<ProcessBase*> processes;
  std::mutex mutex;

  // Semaphore used for threads to wait.
  DecomissionableKernelSemaphore semaphore;
};


#ifdef LOCK_FREE_RUN_QUEUE
class LockFreeRunQueue
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

  // Epoch used to capture changes to the run queue when settling.
  std::atomic_long epoch = ATOMIC_VAR_INIT(0L);

private:
  moodycamel::ConcurrentQueue<ProcessBase*> queue;

  // Semaphore used for threads to wait for the queue.
  DecomissionableKernelSemaphore semaphore;
};
#endif // LOCK_FREE_RUN_QUEUE

} // namespace process {

#endif // __PROCESS_RUN_QUEUE_HPP__
