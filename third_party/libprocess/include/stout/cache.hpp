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

#ifndef __STOUT_CACHE_HPP__
#define __STOUT_CACHE_HPP__

#include <functional>
#include <iostream>
#include <list>
#include <map>

#include <tr1/functional>
#include <tr1/unordered_map>

#include "option.hpp"

// Forward declaration.
template <typename Key, typename Value>
class cache;

// Outputs the key/value pairs from least to most-recently used.
template <typename Key, typename Value>
std::ostream& operator << (
    std::ostream& stream,
    const cache<Key, Value>& c);


// Provides a least-recently used (LRU) cache of some predefined
// capacity. A "write" and a "read" both count as uses.
template <typename Key, typename Value>
class cache
{
public:
  typedef std::list<Key> list;
  typedef std::tr1::unordered_map<
    Key, std::pair<Value, typename list::iterator> > map;

  explicit cache(int _capacity) : capacity(_capacity) {}

  void put(const Key& key, const Value& value)
  {
    typename map::iterator i = values.find(key);
    if (i == values.end()) {
      insert(key, value);
    } else {
      (*i).second.first = value;
      use(i);
    }
  }

  Option<Value> get(const Key& key)
  {
    typename map::iterator i = values.find(key);

    if (i != values.end()) {
      use(i);
      return (*i).second.first;
    }

    return Option<Value>::none();
  }

private:
  // Not copyable, not assignable.
  cache(const cache&);
  cache& operator = (const cache&);

  // Give the operator access to our internals.
  friend std::ostream& operator << <>(
      std::ostream& stream,
      const cache<Key, Value>& c);

  // Insert key/value into the cache.
  void insert(const Key& key, const Value& value)
  {
    if (keys.size() == capacity) {
      evict();
    }

    // Get a "pointer" into the lru list for efficient update.
    typename list::iterator i = keys.insert(keys.end(), key);

    // Save key/value and "pointer" into lru list.
    values.insert(std::make_pair(key, std::make_pair(value, i)));
  }

  // Updates the LRU ordering in the cache for the given iterator.
  void use(const typename map::iterator& i)
  {
    // Move the "pointer" to the end of the lru list.
    keys.splice(keys.end(), keys, (*i).second.second);

    // Now update the "pointer" so we can do this again.
    (*i).second.second = --keys.end();
  }

  // Evict the least-recently used element from the cache.
  void evict()
  {
    const typename map::iterator& i = values.find(keys.front());
    CHECK(i != values.end());
    values.erase(i);
    keys.pop_front();
  }

  // Size of the cache.
  int capacity;

  // Cache of values and "pointers" into the least-recently used list.
  map values;

  // Keys ordered by least-recently used.
  list keys;
};


template <typename Key, typename Value>
std::ostream& operator << (
    std::ostream& stream,
    const cache<Key, Value>& c)
{
  typename cache<Key, Value>::list::const_iterator i1;
  for (i1 = c.keys.begin(); i1 != c.keys.end(); i1++) {
    stream << *i1 << ": ";
    typename cache<Key, Value>::map::const_iterator i2;
    i2 = c.values.find(*i1);
    CHECK(i2 != c.values.end());
    stream << *i2 << std::endl;
  }
}

#endif // __STOUT_CACHE_HPP__
