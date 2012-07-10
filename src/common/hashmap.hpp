/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __HASHMAP_HPP__
#define __HASHMAP_HPP__

#include <boost/get_pointer.hpp>
#include <boost/unordered_map.hpp>

#include "common/hashset.hpp"
#include "common/foreach.hpp"
#include "common/option.hpp"


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
  bool contains(const Key& key) const { return count(key) > 0; }

  // Checks whether there exists a bound value in this map.
  bool containsValue(const Value& v) const
  {
    foreachvalue (const Value& value, *this) {
      if (value == v) {
        return true;
      }
    }
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

#endif // __HASHMAP_HPP__
