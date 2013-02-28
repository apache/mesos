#ifndef __STOUT_OWNED_HPP__
#define __STOUT_OWNED_HPP__

#include <boost/shared_ptr.hpp>

// Represents a uniquely owned pointer.
//
// TODO(bmahler): For now, Owned only provides shared_ptr semantics.
// When we make the switch to C++11, we will change to provide
// unique_ptr semantics. Consequently, each usage of Owned that
// invoked a copy will have to be adjusted to use move semantics.
template <typename T>
class Owned : public boost::shared_ptr<T>
{
public:
  Owned(T* t) : boost::shared_ptr<T>(t) {}
};


#endif // __STOUT_OWNED_HPP__
