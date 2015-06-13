/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __STOUT_SYNCHRONIZED_HPP__
#define __STOUT_SYNCHRONIZED_HPP__

#include <atomic>
#include <mutex>
#include <type_traits>

#include <glog/logging.h>

#include <stout/preprocessor.hpp>

// An RAII class for the 'synchronized(m)' macro.
template <typename T>
class Synchronized
{
public:
  explicit Synchronized(T* t, void (*acquire)(T*), void (*release)(T*))
    : t_(CHECK_NOTNULL(t)), release_(release)
  {
    acquire(t_);
  }

  ~Synchronized() { release_(t_); }

  // NOTE: 'false' being returned here has no significance.
  //       Refer to the NOTE for 'synchronized' at the bottom for why.
  explicit operator bool() const { return false; }

private:
  T* t_;

  void (*release_)(T*);
};


// The generic version handles mutexes which have 'lock' and 'unlock'
// member functions such as 'std::mutex' and 'std::recursive_mutex'.
template <typename T>
Synchronized<T> synchronize(T* t)
{
  return Synchronized<T>(
    t,
    [](T* t) { t->lock(); },
    [](T* t) { t->unlock(); }
  );
}


// An overload of the 'synchronize' function for 'std::atomic_flag'.
inline Synchronized<std::atomic_flag> synchronize(std::atomic_flag* lock)
{
  return Synchronized<std::atomic_flag>(
    lock,
    [](std::atomic_flag* lock) {
      while (lock->test_and_set(std::memory_order_acquire)) {}
    },
    [](std::atomic_flag* lock) {
      lock->clear(std::memory_order_release);
    }
  );
}


// An overload of the 'synchronize' function for 'pthread_mutex_t'.
inline Synchronized<pthread_mutex_t> synchronize(pthread_mutex_t* mutex)
{
  return Synchronized<pthread_mutex_t>(
    mutex,
    [](pthread_mutex_t* mutex) {
      pthread_mutex_lock(mutex);
    },
    [](pthread_mutex_t* mutex) {
      pthread_mutex_unlock(mutex);
    }
  );
}


template <typename T>
T* synchronized_get_pointer(T** t)
{
  return *CHECK_NOTNULL(t);
}


template <typename T>
T* synchronized_get_pointer(T* t)
{
  return t;
}


// Macros to help generate "unique" identifiers for the
// synchronization variable name and label. The line number gets
// embedded which makes it unique enough, but not absolutely unique.
// It shouldn't be a problem however, since it's very unlikely that
// anyone would place multiple 'synchronized' blocks on one line.
#define SYNCHRONIZED_PREFIX CAT(__synchronizer_, __LINE__)
#define SYNCHRONIZED_VAR CAT(SYNCHRONIZED_PREFIX, _var__)
#define SYNCHRONIZED_LABEL CAT(SYNCHRONIZED_PREFIX, _label__)


// A macro for acquiring a scoped 'guard' on any type that can satisfy
// the 'Synchronized' interface. We support 'std::mutex',
// 'std::recursive_mutex' and 'std::atomic_flag' by default.
//
//   Example usage:
//     std::mutex m;
//     synchronized (m) {
//       // Do something under the lock.
//     }
//
// You can easily extend support for your synchronization primitive
// here, by overloading the 'synchronize' function.
//
//   Example overload:
//     inline Synchronized<bufferevent> synchronize(bufferevent* bev)
//     {
//       return Synchronized<bufferevent>(
//         bev,
//         [](bufferevent* bev) { bufferevent_lock(bev); },
//         [](bufferevent* bev) { bufferevent_unlock(bev); },
//       );
//     }
//
// How it works: An instance of the synchronization primitive is
// constructed inside the 'condition' of if statement. This variable
// stays alive for the lifetime of the block. The trick with 'goto',
// 'else' and the label allows the compiler to figure out that the
// synchronized block will always execute. This allows us to return
// within the synchronized block and avoid the
// 'control reaches the end of a non-void function' warning.
// Note that the variable declared inside the 'condition' of an if
// statement is guaranteed to live through the 'else' clause as well.
//
// From Section 6.4.3:
//   A name introduced by a declaration in a condition (either
//   introduced by the decl-specifier-seq or the declarator of the
//   condition) is in scope from its point of declaration until the
//   end of the substatements controlled by the condition.
#define synchronized(m)                                                     \
  if (Synchronized<typename std::remove_pointer<decltype(m)>::type>         \
        SYNCHRONIZED_VAR = ::synchronize(::synchronized_get_pointer(&m))) { \
    goto SYNCHRONIZED_LABEL;                                                \
  } else SYNCHRONIZED_LABEL:

#endif // __STOUT_SYNCHRONIZED_HPP__
