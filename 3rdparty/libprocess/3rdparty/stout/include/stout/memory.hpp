#ifndef __STOUT_MEMORY_HPP__
#define __STOUT_MEMORY_HPP__

#if __cplusplus >= 201103L
#include <memory>
namespace memory {
using std::shared_ptr;
using std::weak_ptr;
} // namespace memory {
#else // __cplusplus >= 201103L
#include <tr1/memory>
namespace memory {
using std::tr1::shared_ptr;
using std::tr1::weak_ptr;
} // namespace memory {
#endif // __cplusplus >= 201103L

#endif // __STOUT_MEMORY_HPP__
