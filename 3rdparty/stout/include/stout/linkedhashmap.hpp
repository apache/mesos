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

#ifndef __STOUT_LINKEDHASHMAP_HPP__
#define __STOUT_LINKEDHASHMAP_HPP__

#include <list>
#include <utility>

#include <stout/hashmap.hpp>
#include <stout/option.hpp>

// Implementation of a hashmap that maintains the insertion order
// of the keys. Note that re-insertion of a key (i.e., update)
// doesn't update its insertion order.
// TODO(vinod/bmahler): Consider extending from stout::hashmap and/or
// having a compatible API with stout::hashmap.
template <typename Key, typename Value>
class LinkedHashMap
{
public:
  typedef std::list<Key> list;
  typedef hashmap<Key, std::pair<Value, typename list::iterator>> map;

  Value& operator[] (const Key& key)
  {
    if (!values_.contains(key)) {
      // Insert into the list and get the "pointer" into the list.
      typename list::iterator i = keys_.insert(keys_.end(), key);
      values_[key] = std::make_pair(Value(), i); // Store default value.
    }
    return values_[key].first;
  }

  Option<Value> get(const Key& key) const
  {
    if (values_.contains(key)) {
      return values_.at(key).first;
    }
    return None();
  }

  Value& at(const Key& key)
  {
    return values_.at(key).first;
  }

  const Value& at(const Key& key) const
  {
    return values_.at(key).first;
  }

  bool contains(const Key& key) const
  {
    return values_.contains(key);
  }

  size_t erase(const Key& key)
  {
    if (values_.contains(key)) {
      // Get the "pointer" into the list.
      typename list::iterator i = values_[key].second;
      keys_.erase(i);
      return values_.erase(key);
    }
    return 0;
  }

  std::list<Key> keys() const
  {
    return keys_;
  }

  std::list<Value> values() const
  {
    std::list<Value> result;
    foreach (const Key& key, keys_) {
      result.push_back(values_.at(key).first);
    }
    return result;
  }

  size_t size() const
  {
    return keys_.size();
  }

  bool empty() const
  {
    return keys_.empty();
  }

  void clear()
  {
    values_.clear();
    keys_.clear();
  }

private:
  list keys_;  // Keys ordered by the insertion order.
  map values_;  // Map of values and "pointers" to the linked list.
};


#endif // __STOUT_LINKEDHASHMAP_HPP__
