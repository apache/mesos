#ifndef __HASHMAP_HPP__
#define __HASHMAP_HPP__

#include <boost/get_pointer.hpp>
#include <boost/unordered_map.hpp>

#include "common/foreach.hpp"


namespace mesos { namespace internal {

// Provides a hash map via Boost's 'unordered_map'. For most intensive
// purposes this could be accomplished with a templated typedef, but
// those don't exist (until C++-0x). Also, doing it this way allows us
// to add functionality, or better naming of existing functionality,
// etc.

template <typename Key, typename Value>
class hashmap : public boost::unordered_map<Key, Value>
{
public:
  // Checks whether this map contains a binding for a key.
  bool contains(const Key& key) { return count(key) > 0; }

  // Checks whether there exists a bound value in this map.
  bool containsValue(const Value& v)
  {
    foreachvalue (const Value& value, *this) {
      if (value == v) {
        return true;
      }
    }
  }

  // Checks whether there exists a value in this map that returns the
  // a result equal to 'r' when the specified method is invoked.
  template <typename R, typename T>
  bool existsValue(R (T::*method)(), R r)
  {
    foreachvalue (const Value& value, *this) {
      const T* t = boost::get_pointer(value);
      if (t->*method() == r) {
        return true;
      }
    }
  }

  // Checks whether there exists a value in this map whose specified
  // member is equal to 'r'.
  template <typename R, typename T>
  bool existsValue(R (T::*member), R r)
  {
    foreachvalue (const Value& value, *this) {
      const T* t = boost::get_pointer(value);
      if (t->*member == r) {
        return true;
      }
    }
  }
};

}} // namespace mesos { namespace internal {

#endif // __HASHMAP_HPP__
