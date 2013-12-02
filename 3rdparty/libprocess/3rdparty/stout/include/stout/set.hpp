#include <algorithm> // For std::set_intersection.
#include <set>
#include <vector>

template <typename T>
class Set : public std::set<T>
{
public:
  Set() {}

  Set(const T& t1)
  {
    insert(t1);
  }

  Set(const T& t1, const T& t2)
  {
    insert(t1);
    insert(t2);
  }

  Set(const T& t1, const T& t2, const T& t3)
  {
    insert(t1);
    insert(t2);
    insert(t3);
  }

  Set(const T& t1, const T& t2, const T& t3, const T& t4)
  {
    insert(t1);
    insert(t2);
    insert(t3);
    insert(t4);
  }
};


template <typename T>
std::set<T> operator | (const std::set<T>& left, const std::set<T>& right)
{
  // Note, we're not using 'set_union' since it affords us no benefit
  // in efficiency and is more complicated to use given we have sets.
  std::set<T> result = left;
  result.insert(right.begin(), right.end());
  return result;
}


template <typename T>
std::set<T> operator + (const std::set<T>& left, const T& t)
{
  std::set<T> result = left;
  result.insert(t);
  return result;
}


template <typename T>
std::set<T> operator & (const std::set<T>& left, const std::set<T>& right)
{
  std::set<T> result;
  std::set_intersection(
      left.begin(),
      left.end(),
      right.begin(),
      right.end(),
      std::inserter(result, result.begin()));
  return result;
}
