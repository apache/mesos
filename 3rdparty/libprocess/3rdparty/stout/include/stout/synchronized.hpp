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

// A helper class for the synchronized(m) macro. It is an RAII 'guard'
// for a synchronization primitive 'T'. The general template handles
// cases such as 'std::mutex' and 'std::recursive_mutex'.
template <typename T>
class Synchronized
{
public:
  Synchronized(T* _lock) : lock(CHECK_NOTNULL(_lock)) { lock->lock(); }
  Synchronized(T** _lock) : Synchronized(*CHECK_NOTNULL(_lock)) {}

  ~Synchronized() { lock->unlock(); }

  operator bool() const { return true; }
private:
  T* lock;
};


// A specialization of the Synchronized class for 'std::atomic_flag'.
// This is necessary as the locking functions are different.
template <>
class Synchronized<std::atomic_flag>
{
public:
  Synchronized(std::atomic_flag* _flag) : flag(CHECK_NOTNULL(_flag))
  {
    while (flag->test_and_set(std::memory_order_acquire)) {}
  }
  Synchronized(std::atomic_flag** _flag)
    : Synchronized(*CHECK_NOTNULL(_flag)) {}

  ~Synchronized()
  {
    flag->clear(std::memory_order_release);
  }

  operator bool() const { return true; }
private:
  std::atomic_flag* flag;
};


// A macro for acquiring a scoped 'guard' on any type that can satisfy
// the 'Synchronized' interface. Currently this includes 'std::mutex',
// 'std::recursive_mutex' and 'std::atomic_flag'.
// Example:
//   std::mutex m;
//   synchronized (m) {
//     // Do something under the lock.
//   }
#define synchronized(m)                                                 \
  if (auto __ ## __file__ ## _ ## __line__ ## __lock = Synchronized<typename std::remove_pointer<decltype(m)>::type>(&m)) // NOLINT(whitespace/line_length)

#endif // __STOUT_SYNCHRONIZED_HPP__
