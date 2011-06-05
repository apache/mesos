#ifndef __MULTIMAP_HPP__
#define __MULTIMAP_HPP__

#include <iterator>

#include <tr1/unordered_map>
#include <tr1/unordered_set>


namespace mesos { namespace internal {

// Forward declarations of multimap iterators.
template <typename K, typename V>
struct multimap_iterator;

template <typename K, typename V>
struct const_multimap_iterator;


// Multimap implementation using an unordered_map with an
// unordered_set as a value. The multimap implementation is painful to
// use (requires lots of iterator garbage, as well as the use of
// 'equal_range' which makes for cluttered code). Note that this
// implementation doesn't actually provide certain STL operations such
// as 'find' or a version of 'insert' that returns an iterator.
template <typename K, typename V>
class multimap
{
public:
  void insert(const K& key, const V& value);
  void clear();
  bool empty() const;
  size_t size() const;
  size_t erase(const K& key);
  size_t erase(const K& key, const V& value);
  size_t count(const K& key) const;
  size_t count(const K& key, const V& value) const;
  std::tr1::unordered_set<V>& operator [] (const K& key);

  typedef multimap_iterator<K, V> iterator;
  typedef const_multimap_iterator<K, V> const_iterator;

  iterator begin();
  iterator end();

  const_iterator begin() const;
  const_iterator end() const;

private:
  friend class multimap_iterator<K, V>;
  friend class const_multimap_iterator<K, V>;

  typedef std::tr1::unordered_map<K, std::tr1::unordered_set<V> > type;
  type map;
};


template <typename K, typename V>
struct multimap_iterator
  : std::iterator_traits<typename std::tr1::unordered_map<K, V>::iterator>
{
  typedef std::forward_iterator_tag iterator_category;

  multimap_iterator()
    : current(NULL),
      outer_iterator(outer_type().end()),
      outer_end(outer_type().end()),
      inner_iterator(inner_type().end()),
      inner_end(inner_type().end()) {}

  explicit multimap_iterator(multimap<K, V>& map)
    : current(NULL),
      outer_iterator(map.map.begin()),
      outer_end(map.map.end()),
      inner_iterator(inner_type().end()),
      inner_end(inner_type().end())
  {
    update();
  }

  multimap_iterator(const multimap_iterator<K, V>& that)
    : current(NULL),
      outer_iterator(that.outer_iterator),
      outer_end(that.outer_end),
      inner_iterator(that.inner_iterator),
      inner_end(that.inner_end)
  {
    if (that.current != NULL) {
      current =
        new std::pair<const K, V>(that.current->first, that.current->second);
    }
  }

  virtual ~multimap_iterator()
  {
    if (current != NULL) {
      delete current;
    }
  }

  multimap_iterator& operator ++ ()
  {
    ++inner_iterator;
    if (inner_iterator == inner_end) {
      ++outer_iterator;
      update();
    } else {
      if (current != NULL) {
        delete current;
        current = NULL;
      }
      current =
        new std::pair<const K, V>(outer_iterator->first, *inner_iterator);
    }

    return *this;
  }

  std::pair<const K, V>& operator * ()
  {
    return *current;
  }

  std::pair<const K, V>* operator -> ()
  {
    return current;
  }

  bool operator == (const multimap_iterator& that) const
  {
    bool this_end = outer_iterator == outer_end;
    bool that_end = that.outer_iterator == that.outer_end;
    if (this_end && that_end) {
      return true;
    } else if (this_end != that_end) {
      return false;
    }

    return outer_iterator == that.outer_iterator 
      && inner_iterator == that.inner_iterator;
  }

  bool operator != (const multimap_iterator& that) const
  {
    return !((*this).operator == (that));
  }

private:
  typedef std::tr1::unordered_map<K, std::tr1::unordered_set<V> > outer_type;
  typedef std::tr1::unordered_set<V> inner_type;

  multimap_iterator& operator = (const multimap_iterator<K, V>&);

  void update()
  {
    while (outer_iterator != outer_end) {
      inner_iterator = outer_iterator->second.begin();
      inner_end = outer_iterator->second.end();
      if (inner_iterator == inner_end) {
        ++outer_iterator;
      } else {
        if (current != NULL) {
          delete current;
          current = NULL;
        }
        current =
          new std::pair<const K, V>(outer_iterator->first, *inner_iterator);
        break;
      }
    }
  }

  typename outer_type::iterator outer_iterator, outer_end;
  typename inner_type::iterator inner_iterator, inner_end;
  std::pair<const K, V>* current;
};


template <typename K, typename V>
struct const_multimap_iterator
  : std::iterator_traits<typename std::tr1::unordered_map<K, V>::const_iterator>
{
  typedef std::forward_iterator_tag iterator_category;

  const_multimap_iterator()
    : current(NULL),
      outer_iterator(outer_type().end()),
      outer_end(outer_type().end()),
      inner_iterator(inner_type().end()),
      inner_end(inner_type().end()) {}

  explicit const_multimap_iterator(const multimap<K, V>& map)
  : current(NULL),
      outer_iterator(map.map.begin()),
      outer_end(map.map.end()),
      inner_iterator(inner_type().end()),
      inner_end(inner_type().end())
  {
    update();
  }

  const_multimap_iterator(const const_multimap_iterator<K, V>& that)
    : current(NULL),
      outer_iterator(that.outer_iterator),
      outer_end(that.outer_end),
      inner_iterator(that.inner_iterator),
      inner_end(that.inner_end)
  {
    if (that.current != NULL) {
      current =
        new std::pair<const K, V>(that.current->first, that.current->second);
    }
  }

  virtual ~const_multimap_iterator()
  {
    if (current != NULL) {
      delete current;
    }
  }

  const_multimap_iterator& operator ++ ()
  {
    ++inner_iterator;
    if (inner_iterator == inner_end) {
      ++outer_iterator;
      update();
    } else {
      if (current != NULL) {
        delete current;
        current = NULL;
      }
      current =
        new std::pair<const K, V>(outer_iterator->first, *inner_iterator);
    }

    return *this;
  }

  const std::pair<const K, V>& operator * () const
  {
    return *current;
  }

  const std::pair<const K, V>* operator -> () const
  {
    return current;
  }

  bool operator == (const const_multimap_iterator& that) const
  {
    bool this_end = outer_iterator == outer_end;
    bool that_end = that.outer_iterator == that.outer_end;
    if (this_end && that_end) {
      return true;
    } else if (this_end != that_end) {
      return false;
    }

    return outer_iterator == that.outer_iterator 
      && inner_iterator == that.inner_iterator;
  }

  bool operator != (const const_multimap_iterator& that) const
  {
    return !((*this).operator == (that));
  }

private:
  typedef std::tr1::unordered_map<K, std::tr1::unordered_set<V> > outer_type;
  typedef std::tr1::unordered_set<V> inner_type;

  const_multimap_iterator& operator = (const const_multimap_iterator<K, V>&);

  void update()
  {
    while (outer_iterator != outer_end) {
      inner_iterator = outer_iterator->second.begin();
      inner_end = outer_iterator->second.end();
      if (inner_iterator == inner_end) {
        ++outer_iterator;
      } else {
        if (current != NULL) {
          delete current;
          current = NULL;
        }
        current =
          new std::pair<const K, V>(outer_iterator->first, *inner_iterator);
        break;
      }
    }
  }

  typename outer_type::const_iterator outer_iterator, outer_end;
  typename inner_type::const_iterator inner_iterator, inner_end;
  std::pair<const K, V>* current;
};


template <typename K, typename V>
void multimap<K, V>::insert(const K& key, const V& value)
{
  map[key].insert(value);
}


template <typename K, typename V>
void multimap<K, V>::clear()
{
  map.clear();
}


template <typename K, typename V>
bool multimap<K, V>::empty() const
{
  return map.empty();
}


template <typename K, typename V>
size_t multimap<K, V>::size() const
{
  return map.size();
}


template <typename K, typename V>
size_t multimap<K, V>::erase(const K& key)
{
  size_t result = count(key);
  map.erase(key);
  return result;
}


template <typename K, typename V>
size_t multimap<K, V>::erase(const K& key, const V& value)
{
  size_t result = count(key, value);
  if (result > 0) {
    map[key].erase(value);
    if (map[key].size() == 0) {
      map.erase(key);
    }
  }
  return result;
}


template <typename K, typename V>
size_t multimap<K, V>::count(const K& key) const
{
  typename type::const_iterator i = map.find(key);
  if (i != map.end()) {
    return i->second.size();
  }

  return 0;
}


template <typename K, typename V>
size_t multimap<K, V>::count(const K& key, const V& value) const
{
  typename type::const_iterator i = map.find(key);
  if (i != map.end()) {
    return i->second.count(value);
  }

  return 0;
}


template <typename K, typename V>
std::tr1::unordered_set<V>& multimap<K, V>::operator [] (const K& key)
{
  return map[key];
}


template <typename K, typename V>
typename multimap<K, V>::iterator multimap<K, V>::begin()
{
  return multimap_iterator<K, V>(*this);
}


template <typename K, typename V>
typename multimap<K, V>::iterator multimap<K, V>::end()
{
  return multimap_iterator<K, V>();
}


template <typename K, typename V>
typename multimap<K, V>::const_iterator multimap<K, V>::begin() const
{
  return const_multimap_iterator<K, V>(*this);
}


template <typename K, typename V>
typename multimap<K, V>::const_iterator multimap<K, V>::end() const
{
  return const_multimap_iterator<K, V>();
}

}} // namespace mesos { namespace internal {

#endif // __MULTIMAP_HPP__
