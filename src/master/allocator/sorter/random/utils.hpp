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

#ifndef __MASTER_ALLOCATOR_SORTER_RANDOM_UTILS_HPP__
#define __MASTER_ALLOCATOR_SORTER_RANDOM_UTILS_HPP__

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <vector>

#include <stout/check.hpp>

namespace mesos {
namespace internal {
namespace master {
namespace allocator {

// A weighted variant of std::shuffle. Items with higher weight
// have a higher chance of being towards the front of the list,
// equivalent to weighted random sampling without replacement.
// Code adapted from the following paper:
//
// http://utopia.duth.gr/~pefraimi/research/data/2007EncOfAlg.pdf
// Found from: https://softwareengineering.stackexchange.com/a/344274
//
// This has O(n log n) runtime complexity.
template <class RandomAccessIterator, class URBG>
void weightedShuffle(
    RandomAccessIterator begin,
    RandomAccessIterator end,
    const std::vector<double>& weights,
    URBG&& urbg)
{
  CHECK_EQ(end - begin, (int) weights.size());

  std::vector<double> keys(weights.size());

  for (size_t i = 0; i < weights.size(); ++i) {
    CHECK_GT(weights[i], 0.0);

    // Make the key negative so that we don't have to reverse sort.
    double random = std::uniform_real_distribution<>(0.0, 1.0)(urbg);
    keys[i] = 0.0 - std::pow(random, (1.0 / weights[i]));
  }

  // Sort from smallest to largest keys. We store the sort permutation
  // so that we can apply it to `items`.
  std::vector<size_t> permutation(keys.size());
  std::iota(permutation.begin(), permutation.end(), 0);

  std::sort(permutation.begin(), permutation.end(),
      [&](size_t i, size_t j){ return keys[i] < keys[j]; });

  // Now apply the permutation to `items`.
  std::vector<typename std::iterator_traits<RandomAccessIterator>::value_type>
    shuffled(end - begin);

  std::transform(
      permutation.begin(),
      permutation.end(),
      shuffled.begin(),
      [&](size_t i){ return begin[i]; });

  // Move the shuffled copy back into the `items`.
  std::move(shuffled.begin(), shuffled.end(), begin);
}

} // namespace allocator {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_ALLOCATOR_SORTER_RANDOM_UTILS_HPP__
