#ifndef __PROCESS_SHARED_HPP__
#define __PROCESS_SHARED_HPP__

#include <boost/shared_ptr.hpp>

namespace process {

// Represents a shared pointer and therefore enforces 'const' access.
template <typename T>
class Shared : public boost::shared_ptr<const T>
{
public:
  Shared(T* t) : boost::shared_ptr<const T>(t) {}

  // TODO(jieyu): Support upgrading from a shared pointer to an owned
  // pointer. The interface is like this: Future<Owned<T> > upgrade().
};

} // namespace process {

#endif // __PROCESS_SHARED_HPP__
