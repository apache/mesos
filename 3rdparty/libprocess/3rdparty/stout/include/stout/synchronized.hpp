// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_SYNCHRONIZED_HPP__
#define __STOUT_SYNCHRONIZED_HPP__

#include <atomic>
#include <condition_variable>
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


/**
 * Waits on the condition variable associated with 'lock' which has
 * already been synchronized. Currently, the only supported
 * implementation is for 'std::condition_variable' associated with a
 * 'std::mutex'.
 *
 * @param cv The condition variable.
 * @param lock The lock associated with the condition variable.
 *
 * @pre 'lock' is already owned by the calling thread.
 * @post 'lock' is still owned by the calling thread.
 *
 * @return Nothing.
 */
template <typename CV, typename Lock>
void synchronized_wait(CV* cv, Lock* lock);


/**
 * Waits on the 'std::condition_variable' associated with 'std::mutex'
 * which has already been synchronized. We provide this specialization
 * since 'std::condition_variable::wait' only works with
 * 'std::unique_lock<std::mutex>'.
 *
 * @example
 *   std::condition_variable cv;
 *   std::mutex m;
 *   synchronized (m) {  // lock 'm'.
 *     // ...
 *     synchronized_wait(cv, m);  // unlock/lock 'm'.
 *     // ...
 *   }  // unlock 'm'.
 *
 * @param cv The 'std::condition_variable'.
 * @param mutex The 'std::mutex' associated with the
 *     'std::condition_variable'.
 *
 * @pre 'mutex' is already owned by the calling thread.
 * @post 'mutex' is still owned by the calling thread.
 *
 * @return Nothing.
 */
template <>
inline void synchronized_wait(std::condition_variable* cv, std::mutex* mutex)
{
  CHECK_NOTNULL(cv);
  CHECK_NOTNULL(mutex);

  // Since the pre-condition is that 'mutex' is already owned by the
  // calling thread, we use 'std::adopt_lock' to adopt the mutex
  // rather than trying to lock it ourselves.
  std::unique_lock<std::mutex> lock(*mutex, std::adopt_lock);

  // On entrance, 'std::condition_variable::wait' releases 'mutex'.
  // On exit, 'std::condition_variable::wait' re-acquires 'mutex'.
  cv->wait(lock);

  // At this point, the 'mutex' has been acquired. Since the
  // post-condition is that 'mutex' is still owned by the calling
  // thread, we use 'release' in order to prevent 'std::unique_lock'
  // from unlocking the 'mutex'.
  lock.release();
}


/**
 * There is a known bug around 'std::condition_variable_any' in
 * 'libstdc++' distributed 'devtoolset-2', and also general issues
 * around the use of 'std::condition_variable_any' with
 * 'std::recursive_mutex'. Hence, this function is currently deleted
 * until both issues are resolved.
 *
 * @see https://rhn.redhat.com/errata/RHBA-2014-1035.html
 * @see http://stackoverflow.com/questions/11752155/behavior-of-condition-variable-any-when-used-with-a-recursive-mutex
 */
template <typename Lock>
void synchronized_wait(std::condition_variable_any* cv, Lock* lock) = delete;

#endif // __STOUT_SYNCHRONIZED_HPP__
