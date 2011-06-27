#ifndef __HASHSET_HPP__
#define __HASHSET_HPP__

#include <boost/get_pointer.hpp>
#include <boost/unordered_set.hpp>

#include "common/foreach.hpp"


namespace mesos { namespace internal {

// Provides a hash map via Boost's 'unordered_map'. For most intensive
// purposes this could be accomplished with a templated typedef, but
// those don't exist (until C++-0x). Also, doing it this way allows us
// to add functionality, or better naming of existing functionality,
// etc.

template <typename Elem>
class hashset : public boost::unordered_set<Elem>
{
public:
  // Checks whether this map contains a binding for a key.
  bool contains(const Elem& elem) { return count(elem) > 0; }

  // Checks whether there exists a value in this set that returns the
  // a result equal to 'r' when the specified method is invoked.
  template <typename R, typename T>
  bool exists(R (T::*method)(), R r)
  {
    foreach (const Elem& elem, *this) {
      const T* t = boost::get_pointer(elem);
      if (t->*method() == r) {
        return true;
      }
    }
  }

  // Checks whether there exists an element in this set whose
  // specified member is equal to 'r'.
  template <typename R, typename T>
  bool exists(R (T::*member), R r)
  {
    foreach (const Elem& elem, *this) {
      const T* t = boost::get_pointer(elem);
      if (t->*member == r) {
        return true;
      }
    }
  }
};

}} // namespace mesos { namespace internal {

#endif // __HASHMAP_HPP__
