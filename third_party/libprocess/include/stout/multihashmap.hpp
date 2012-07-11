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

#ifndef __STOUT_MULTIHASHMAP_HPP__
#define __STOUT_MULTIHASHMAP_HPP__

#include <utility>

#include <boost/unordered_map.hpp>

#include "hashset.hpp"


// Implementation of a hash multimap via Boost's
// 'unordered_multimap'. The rationale for creating this is that the
// std::multimap implementation is painful to use (requires lots of
// iterator garbage, as well as the use of 'equal_range' which makes
// for cluttered code).
template <typename K, typename V>
class multihashmap : public boost::unordered_multimap<K, V>
{
public:
  void put(const K& key, const V& value);
  hashset<V> get(const K& key) const;
  bool remove(const K& key);
  bool remove(const K& key, const V& value);
  bool contains(const K& key) const;
  bool contains(const K& key, const V& value) const;
};


template <typename K, typename V>
void multihashmap<K, V>::put(const K& key, const V& value)
{
  insert(std::pair<K, V>(key, value));
}


template <typename K, typename V>
hashset<V> multihashmap<K, V>::get(const K& key) const
{
  hashset<V> values; // Values to return.

  std::pair<typename boost::unordered_multimap<K, V>::const_iterator,
    typename boost::unordered_multimap<K, V>::const_iterator> range;

  range = equal_range(key);

  typename boost::unordered_multimap<K, V>::const_iterator i;
  for (i = range.first; i != range.second; ++i) {
    values.insert((*i).second);
  }

  return values;
}


template <typename K, typename V>
bool multihashmap<K, V>::remove(const K& key)
{
  return erase(key) > 0;
}


template <typename K, typename V>
bool multihashmap<K, V>::remove(const K& key, const V& value)
{
  typename boost::unordered_multimap<K, V>::iterator i;
  for (i = find(key); i != boost::unordered_multimap<K, V>::end(); ++i) {
    if ((*i).second == value) {
      erase(i);
      return true;
    }
  }

  return false;
}


template <typename K, typename V>
bool multihashmap<K, V>::contains(const K& key) const
{
  return count(key) > 0;
}


template <typename K, typename V>
bool multihashmap<K, V>::contains(const K& key, const V& value) const
{
  typename boost::unordered_multimap<K, V>::const_iterator i;
  for (i = find(key); i != boost::unordered_multimap<K, V>::end(); ++i) {
    if ((*i).second == value) {
      return true;
    }
  }

  return false;
}

#endif // __STOUT_MULTIHASHMAP_HPP__
