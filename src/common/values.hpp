// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __COMMON_VALUES_HPP__
#define __COMMON_VALUES_HPP__

#include <limits>
#include <type_traits>
#include <vector>

#include <mesos/mesos.hpp>

#include <stout/foreach.hpp>
#include <stout/interval.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace values {

// Convert Ranges value to IntervalSet value.
template <typename T>
Try<IntervalSet<T>> rangesToIntervalSet(const Value::Ranges& ranges)
{
  IntervalSet<T> set;

  static_assert(
      std::is_integral<T>::value,
      "IntervalSet<T> must use an integral type");

  foreach (const Value::Range& range, ranges.range()) {
    if (range.begin() < std::numeric_limits<T>::min() ||
        range.end() > std::numeric_limits<T>::max()) {
      return Error("Range is out of bounds");
    }

    set += (Bound<T>::closed(range.begin()),
            Bound<T>::closed(range.end()));
  }

  return set;
}


template <typename T>
Try<std::vector<T>> rangesToVector(const Value::Ranges& ranges)
{
  std::vector<T> result;

  static_assert(
      std::is_integral<T>::value,
      "vector<T> must use an integral type");

  foreach (const Value::Range& range, ranges.range()) {
    if (range.begin() < std::numeric_limits<T>::min() ||
        range.end() > std::numeric_limits<T>::max()) {
      return Error("Range is out of bounds");
    }

    for (T value = range.begin(); value <= range.end(); value++) {
      result.push_back(value);
    }
  }

  return result;
}


// Convert IntervalSet value to Ranges value.
template <typename T>
Value::Ranges intervalSetToRanges(const IntervalSet<T>& set)
{
  Value::Ranges ranges;

  static_assert(
      std::is_integral<T>::value,
      "IntervalSet<T> must use an integral type");

  foreach (const Interval<T>& interval, set) {
    Value::Range* range = ranges.add_range();
    range->set_begin(interval.lower());
    range->set_end(interval.upper() - 1);
  }

  return ranges;
}

} // namespace values {
} // namespace internal {
} // namespace mesos {

#endif //  __COMMON_VALUES_HPP__
