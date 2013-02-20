#ifndef __STOUT_HASHMAP_HPP__
#define __STOUT_HASHMAP_HPP__

#include <boost/get_pointer.hpp>
#include <boost/unordered_map.hpp>

#include "hashset.hpp"
#include "foreach.hpp"
#include "none.hpp"
#include "option.hpp"


// Provides a hash map via Boost's 'unordered_map'. For most intensive
// purposes this could be accomplished with a templated typedef, but
// those don't exist (until C++-11). Also, doing it this way allows us
// to add functionality, or better naming of existing functionality,
// etc.

template <typename Key, typename Value>
class hashmap : public boost::unordered_map<Key, Value>
{
public:
  // An explicit default constructor is needed so
  // 'const hashmap<T> map;' is not an error.
  hashmap() {}

  // Checks whether this map contains a binding for a key.
  bool contains(const Key& key) const
  {
    return boost::unordered_map<Key, Value>::count(key) > 0;
  }

  // Checks whether there exists a bound value in this map.
  bool containsValue(const Value& v) const
  {
    foreachvalue (const Value& value, *this) {
      if (value == v) {
        return true;
      }
    }
  }

  // Returns an Option for the binding to the key.
  Option<Value> get(const Key& key) const
  {
    typedef typename boost::unordered_map<Key, Value>::const_iterator
        const_iterator;
    const_iterator it = boost::unordered_map<Key, Value>::find(key);
    if (it == boost::unordered_map<Key, Value>::end()) {
      return None();
    }
    return it->second;
  }

  // Returns the set of keys in this map.
  hashset<Key> keys() const
  {
    hashset<Key> result;
    foreachkey (const Key& key, *this) {
      result.insert(key);
    }
    return result;
  }

  // Returns the set of values in this map.
  hashset<Value> values() const
  {
    hashset<Value> result;
    foreachvalue (const Value& value, *this) {
      result.insert(value);
    }
    return result;
  }

  // Checks whether there exists a value in this map that returns the
  // a result equal to 'r' when the specified method is invoked.
  template <typename R, typename T>
  bool existsValue(R (T::*method)(), R r) const
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
  bool existsValue(R (T::*member), R r) const
  {
    foreachvalue (const Value& value, *this) {
      const T* t = boost::get_pointer(value);
      if (t->*member == r) {
        return true;
      }
    }
  }
};

#endif // __STOUT_HASHMAP_HPP__
