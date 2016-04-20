// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_MULTIMAP_HPP__
#define __STOUT_MULTIMAP_HPP__

#include <algorithm>
#include <list>
#include <map>
#include <set>
#include <utility>

#include <stout/foreach.hpp>

// Implementation of a multimap via std::multimap but with a better
// interface. The rationale for creating this is that the
// std::multimap interface is painful to use (requires lots of
// iterator garbage, as well as the use of 'equal_range' which makes
// for cluttered code).
template <typename K, typename V>
class Multimap : public std::multimap<K, V>
{
public:
  Multimap() {}
  Multimap(std::initializer_list<std::pair<const K, V>> list);

  void put(const K& key, const V& value);
  std::list<V> get(const K& key) const;
  std::set<K> keys() const;
  bool remove(const K& key);
  bool remove(const K& key, const V& value);
  bool contains(const K& key) const;
  bool contains(const K& key, const V& value) const;
};


template <typename K, typename V>
Multimap<K, V>::Multimap(std::initializer_list<std::pair<const K, V>> list)
  : std::multimap<K, V>(list)
{}


template <typename K, typename V>
void Multimap<K, V>::put(const K& key, const V& value)
{
  std::multimap<K, V>::insert(std::pair<K, V>(key, value));
}


template <typename K, typename V>
std::list<V> Multimap<K, V>::get(const K& key) const
{
  std::list<V> values; // Values to return.

  std::pair<typename std::multimap<K, V>::const_iterator,
    typename std::multimap<K, V>::const_iterator> range;

  range = std::multimap<K, V>::equal_range(key);

  typename std::multimap<K, V>::const_iterator i;
  for (i = range.first; i != range.second; ++i) {
    values.push_back(i->second);
  }

  return values;
}


template <typename K, typename V>
std::set<K> Multimap<K, V>::keys() const
{
  std::set<K> keys;
  foreachkey (const K& key, *this) {
    keys.insert(key);
  }
  return keys;
}


template <typename K, typename V>
bool Multimap<K, V>::remove(const K& key)
{
  return std::multimap<K, V>::erase(key) > 0;
}


template <typename K, typename V>
bool Multimap<K, V>::remove(const K& key, const V& value)
{
  std::pair<typename std::multimap<K, V>::iterator,
    typename std::multimap<K, V>::iterator> range;

  range = std::multimap<K, V>::equal_range(key);

  typename std::multimap<K, V>::iterator i;
  for (i = range.first; i != range.second; ++i) {
    if (i->second == value) {
      std::multimap<K, V>::erase(i);
      return true;
    }
  }

  return false;
}


template <typename K, typename V>
bool Multimap<K, V>::contains(const K& key) const
{
  return std::multimap<K, V>::count(key) > 0;
}


template <typename K, typename V>
bool Multimap<K, V>::contains(const K& key, const V& value) const
{
  const std::list<V> values = get(key);
  return std::find(values.begin(), values.end(), value) != values.end();
}

#endif // __STOUT_MULTIMAP_HPP__
