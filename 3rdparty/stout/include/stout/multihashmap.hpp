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

#ifndef __STOUT_MULTIHASHMAP_HPP__
#define __STOUT_MULTIHASHMAP_HPP__

#include <algorithm> // For find.
#include <list>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include <stout/foreach.hpp>

// Implementation of a hash multimap via 'std::unordered_multimap'
// but with a better interface. The rationale for creating this is
// that the std::unordered_multimap interface is painful to use
// (requires lots of iterator garbage, as well as the use of
// 'equal_range' which makes for cluttered code).
template <typename Key,
          typename Value,
          typename Hash = std::hash<Key>,
          typename Equal = std::equal_to<Key>>
class multihashmap : public std::unordered_multimap<Key, Value, Hash, Equal>
{
public:
  multihashmap() {}
  multihashmap(const std::multimap<Key, Value>& multimap);
  multihashmap(std::multimap<Key, Value>&& multimap);
  multihashmap(std::initializer_list<std::pair<const Key, Value>> list);

  void put(const Key& key, const Value& value);
  std::list<Value> get(const Key& key) const;
  std::set<Key> keys() const;
  bool remove(const Key& key);
  bool remove(const Key& key, const Value& value);
  bool contains(const Key& key) const;
  bool contains(const Key& key, const Value& value) const;
};


template <typename Key, typename Value, typename Hash, typename Equal>
multihashmap<Key, Value, Hash, Equal>::multihashmap(
    const std::multimap<Key, Value>& multimap)
{
  std::unordered_multimap<Key, Value, Hash, Equal>::reserve(multimap.size());

  foreachpair (const Key& key, const Value& value, multimap) {
    std::unordered_multimap<Key, Value, Hash, Equal>::emplace(key, value);
  }
}


template <typename Key, typename Value, typename Hash, typename Equal>
multihashmap<Key, Value, Hash, Equal>::multihashmap(
    std::multimap<Key, Value>&& multimap)
{
  std::unordered_multimap<Key, Value, Hash, Equal>::insert(
      std::make_move_iterator(multimap.begin()),
      std::make_move_iterator(multimap.end()));
}


template <typename Key, typename Value, typename Hash, typename Equal>
multihashmap<Key, Value, Hash, Equal>::multihashmap(
    std::initializer_list<std::pair<const Key, Value>> list)
  : std::unordered_multimap<Key, Value, Hash, Equal>(list) {}


template <typename Key, typename Value, typename Hash, typename Equal>
void multihashmap<Key, Value, Hash, Equal>::put(
    const Key& key,
    const Value& value)
{
  std::unordered_multimap<Key, Value, Hash, Equal>::insert({key, value});
}


template <typename Key, typename Value, typename Hash, typename Equal>
std::list<Value> multihashmap<Key, Value, Hash, Equal>::get(
    const Key& key) const
{
  std::list<Value> values; // Values to return.

  auto range =
    std::unordered_multimap<Key, Value, Hash, Equal>::equal_range(key);

  for (auto i = range.first; i != range.second; ++i) {
    values.push_back(i->second);
  }

  return values;
}


template <typename Key, typename Value, typename Hash, typename Equal>
std::set<Key> multihashmap<Key, Value, Hash, Equal>::keys() const
{
  std::set<Key> keys;
  foreachkey (const Key& key, *this) {
    keys.insert(key);
  }
  return keys;
}


template <typename Key, typename Value, typename Hash, typename Equal>
bool multihashmap<Key, Value, Hash, Equal>::remove(const Key& key)
{
  return std::unordered_multimap<Key, Value, Hash, Equal>::erase(key) > 0;
}


template <typename Key, typename Value, typename Hash, typename Equal>
bool multihashmap<Key, Value, Hash, Equal>::remove(
    const Key& key,
    const Value& value)
{
  auto range =
    std::unordered_multimap<Key, Value, Hash, Equal>::equal_range(key);

  for (auto i = range.first; i != range.second; ++i) {
    if (i->second == value) {
      std::unordered_multimap<Key, Value, Hash, Equal>::erase(i);
      return true;
    }
  }

  return false;
}


template <typename Key, typename Value, typename Hash, typename Equal>
bool multihashmap<Key, Value, Hash, Equal>::contains(const Key& key) const
{
  return multihashmap<Key, Value, Hash, Equal>::count(key) > 0;
}


template <typename Key, typename Value, typename Hash, typename Equal>
bool multihashmap<Key, Value, Hash, Equal>::contains(
    const Key& key,
    const Value& value) const
{
  const std::list<Value> values = get(key);
  return std::find(values.begin(), values.end(), value) != values.end();
}

#endif // __STOUT_MULTIHASHMAP_HPP__
