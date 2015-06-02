/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_MULTIHASHMAP_HPP__
#define __STOUT_MULTIHASHMAP_HPP__

#include <algorithm> // For find.
#include <list>
#include <set>
#include <utility>

#include <boost/unordered_map.hpp>


// Implementation of a hash multimap via Boost's 'unordered_multimap'
// but with a better interface. The rationale for creating this is
// that the std::multimap interface is painful to use (requires lots
// of iterator garbage, as well as the use of 'equal_range' which
// makes for cluttered code).
template <typename K, typename V>
class multihashmap : public boost::unordered_multimap<K, V>
{
public:
  void put(const K& key, const V& value);
  std::list<V> get(const K& key) const;
  std::set<K> keys() const;
  bool remove(const K& key);
  bool remove(const K& key, const V& value);
  bool contains(const K& key) const;
  bool contains(const K& key, const V& value) const;
};


template <typename K, typename V>
void multihashmap<K, V>::put(const K& key, const V& value)
{
  boost::unordered_multimap<K, V>::insert(std::pair<K, V>(key, value));
}


template <typename K, typename V>
std::list<V> multihashmap<K, V>::get(const K& key) const
{
  std::list<V> values; // Values to return.

  std::pair<typename boost::unordered_multimap<K, V>::const_iterator,
    typename boost::unordered_multimap<K, V>::const_iterator> range;

  range = boost::unordered_multimap<K, V>::equal_range(key);

  typename boost::unordered_multimap<K, V>::const_iterator i;
  for (i = range.first; i != range.second; ++i) {
    values.push_back(i->second);
  }

  return values;
}


template <typename K, typename V>
std::set<K> multihashmap<K, V>::keys() const
{
  std::set<K> keys;
  foreachkey (const K& key, *this) {
    keys.insert(key);
  }
  return keys;
}


template <typename K, typename V>
bool multihashmap<K, V>::remove(const K& key)
{
  return boost::unordered_multimap<K, V>::erase(key) > 0;
}


template <typename K, typename V>
bool multihashmap<K, V>::remove(const K& key, const V& value)
{
  std::pair<typename boost::unordered_multimap<K, V>::iterator,
    typename boost::unordered_multimap<K, V>::iterator> range;

  range = boost::unordered_multimap<K, V>::equal_range(key);

  typename boost::unordered_multimap<K, V>::iterator i;
  for (i = range.first; i != range.second; ++i) {
    if (i->second == value) {
      boost::unordered_multimap<K, V>::erase(i);
      return true;
    }
  }

  return false;
}


template <typename K, typename V>
bool multihashmap<K, V>::contains(const K& key) const
{
  return multihashmap<K, V>::count(key) > 0;
}


template <typename K, typename V>
bool multihashmap<K, V>::contains(const K& key, const V& value) const
{
  const std::list<V> values = get(key);
  return std::find(values.begin(), values.end(), value) != values.end();
}

#endif // __STOUT_MULTIHASHMAP_HPP__
