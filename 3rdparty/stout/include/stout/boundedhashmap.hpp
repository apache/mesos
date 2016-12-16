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
#ifndef __STOUT_BOUNDEDHASHMAP_HPP__
#define __STOUT_BOUNDEDHASHMAP_HPP__

#include <list>
#include <utility>

#include <stout/check.hpp>
#include <stout/hashmap.hpp>
#include <stout/option.hpp>

// A hashmap that contains a fixed number of entries at most. Entries
// are evicted in FIFO order -- i.e., when the capacity of the map is
// reached, the next insertion results in removing the oldest entry.
// Updating an entry does not change insertion order.
template <typename Key, typename Value>
class BoundedHashMap
{
public:
  typedef std::pair<Key, Value> entry;
  typedef std::list<entry> list;
  typedef hashmap<Key, typename list::iterator> map;

  BoundedHashMap(size_t capacity) : capacity_(capacity) {}

  // NOTE: We don't provide `operator[]`, unlike LinkedHashMap,
  // because it would be difficult to implement correctly for bounded
  // maps with zero capacity.
  void set(const Key& key, const Value& value)
  {
    if (capacity_ == 0) {
      return;
    }

    if (!keys_.contains(key)) {
      // Insert a new list entry and get a "pointer" to its location.
      typename list::iterator iter =
        entries_.insert(entries_.end(), std::make_pair(key, value));

      keys_[key] = iter;

      // If the map now exceeds its capacity, remove the oldest entry.
      // Note that removal from both std::list and hashmap does not
      // invalidate iterators that reference other entries.
      if (keys_.size() > capacity_) {
        typename list::iterator firstEntry = entries_.begin();
        keys_.erase(firstEntry->first);
        entries_.erase(firstEntry);

        CHECK(keys_.size() == capacity_);
      }
    } else {
      keys_[key]->second = value;
    }
  }

  Option<Value> get(const Key& key) const
  {
    if (keys_.contains(key)) {
      return keys_.at(key)->second;
    }
    return None();
  }

  Value& at(const Key& key)
  {
    return keys_.at(key)->second;
  }

  const Value& at(const Key& key) const
  {
    return keys_.at(key)->second;
  }

  bool contains(const Key& key) const
  {
    return keys_.contains(key);
  }

  size_t erase(const Key& key)
  {
    if (keys_.contains(key)) {
      typename list::iterator entry = keys_[key];
      keys_.erase(key);
      entries_.erase(entry);

      return 1;
    }
    return 0;
  }

  // Returns the keys in the map in insertion order.
  std::list<Key> keys() const
  {
    std::list<Key> result;

    foreach (const entry& entry, entries_) {
      result.push_back(entry.first);
    }

    return result;
  }

  // Returns the values in the map in insertion order.
  std::list<Value> values() const
  {
    std::list<Value> result;

    foreach (const entry& entry, entries_) {
      result.push_back(entry.second);
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
    entries_.clear();
    keys_.clear();
  }

  // Support for iteration; this allows using `foreachpair` and
  // related constructs. Note that these iterate over the map in
  // insertion order.
  typename list::iterator begin() { return entries_.begin(); }
  typename list::iterator end() { return entries_.end(); }

  typename list::const_iterator begin() const { return entries_.cbegin(); }
  typename list::const_iterator end() const { return entries_.cend(); }

private:
  const size_t capacity_;
  list entries_; // Key-value pairs ordered by insertion order.
  map keys_; // Map from key to "pointer" to key's location in list.
};

#endif // __STOUT_BOUNDEDHASHMAP_HPP__
