#ifndef __STOUT_SET_HPP__
#define __STOUT_SET_HPP__

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
    std::set<T>::insert(t1);
  }

  Set(const T& t1, const T& t2)
  {
    std::set<T>::insert(t1);
    std::set<T>::insert(t2);
  }

  Set(const T& t1, const T& t2, const T& t3)
  {
    std::set<T>::insert(t1);
    std::set<T>::insert(t2);
    std::set<T>::insert(t3);
  }

  Set(const T& t1, const T& t2, const T& t3, const T& t4)
  {
    std::set<T>::insert(t1);
    std::set<T>::insert(t2);
    std::set<T>::insert(t3);
    std::set<T>::insert(t4);
  }
};


template <typename T>
std::set<T> operator|(const std::set<T>& left, const std::set<T>& right)
{
  // Note, we're not using 'set_union' since it affords us no benefit
  // in efficiency and is more complicated to use given we have sets.
  std::set<T> result = left;
  result.insert(right.begin(), right.end());
  return result;
}


template <typename T>
std::set<T> operator+(const std::set<T>& left, const T& t)
{
  std::set<T> result = left;
  result.insert(t);
  return result;
}


template <typename T>
std::set<T> operator&(const std::set<T>& left, const std::set<T>& right)
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

#endif // __STOUT_SET_HPP__
