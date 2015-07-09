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

#ifndef __GATE_HPP__
#define __GATE_HPP__

// TODO(benh): Build implementation directly on-top-of futex's for Linux.

#include <stout/synchronized.hpp>

class Gate
{
public:
  typedef intptr_t state_t;

private:
  int waiters;
  state_t state;
  pthread_mutex_t mutex;
  pthread_cond_t cond;

public:
  Gate() : waiters(0), state(0)
  {
    pthread_mutex_init(&mutex, NULL);
    pthread_cond_init(&cond, NULL);
  }

  ~Gate()
  {
    pthread_cond_destroy(&cond);
    pthread_mutex_destroy(&mutex);
  }

  // Signals the state change of the gate to any (at least one) or
  // all (if 'all' is true) of the threads waiting on it.
  void open(bool all = true)
  {
    synchronized (mutex) {
      state++;
      if (all) {
        pthread_cond_broadcast(&cond);
      } else {
        pthread_cond_signal(&cond);
      }
    }
  }

  // Blocks the current thread until the gate's state changes from
  // the current state.
  void wait()
  {
    synchronized (mutex) {
      waiters++;
      state_t old = state;
      while (old == state) {
        pthread_cond_wait(&cond, &mutex);
      }
      waiters--;
    }
  }

  // Gets the current state of the gate and notifies the gate about
  // the intention to wait for its state change.
  // Call 'leave()' if no longer interested in the state change.
  state_t approach()
  {
    synchronized (mutex) {
      waiters++;
      return state;
    }
  }

  // Blocks the current thread until the gate's state changes from
  // the specified 'old' state. The 'old' state can be obtained by
  // calling 'approach()'.
  void arrive(state_t old)
  {
    synchronized (mutex) {
      while (old == state) {
        pthread_cond_wait(&cond, &mutex);
      }

      waiters--;
    }
  }

  // Notifies the gate that a waiter (the current thread) is no
  // longer waiting for the gate's state change.
  void leave()
  {
    synchronized (mutex) {
      waiters--;
    }
  }

  // Returns true if there is no one waiting on the gate's state
  // change.
  bool empty()
  {
    bool occupied = true;
    synchronized (mutex) {
      occupied = waiters > 0 ? true : false;
    }
    return !occupied;
  }
};

#endif // __GATE_HPP__
