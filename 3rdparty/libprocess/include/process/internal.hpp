#ifndef __PROCESS_INTERNAL_HPP__
#define __PROCESS_INTERNAL_HPP__

#include <stout/check.hpp>

// TODO(benh): Replace usage of 'acquire' and 'release' with
// std::mutex with C++11 and kill this header.

namespace process {
namespace internal {

inline void acquire(int* lock)
{
  while (!__sync_bool_compare_and_swap(lock, 0, 1)) {
#if defined(__i386__) || defined(__x86_64__)
    asm volatile ("pause");
#endif
  }
}


inline void release(int* lock)
{
  // Unlock via a compare-and-swap so we get a memory barrier too.
  bool unlocked = __sync_bool_compare_and_swap(lock, 1, 0);
  CHECK(unlocked);
}

} // namespace internal {
} // namespace process {

#endif // __PROCESS_INTERNAL_HPP__
