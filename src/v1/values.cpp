/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#include <mesos/v1/resources.hpp>
#include <mesos/v1/values.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/strings.hpp>

using std::max;
using std::min;
using std::ostream;
using std::string;
using std::vector;

namespace mesos {
namespace v1 {

namespace internal {
namespace values {

Try<Value> parse(const std::string& text)
{
  Value value;

  // Remove any spaces from the text.
  string temp;
  foreach (const char c, text) {
    if (c != ' ') {
      temp += c;
    }
  }

  if (temp.length() == 0) {
    return Error("Expecting non-empty string");
  }

  // TODO(ynie): Find a better way to check brackets.
  if (!strings::checkBracketsMatching(temp, '{', '}') ||
      !strings::checkBracketsMatching(temp, '[', ']') ||
      !strings::checkBracketsMatching(temp, '(', ')')) {
    return Error("Mismatched brackets");
  }

  size_t index = temp.find('[');
  if (index == 0) {
    // This is a ranges.
    Value::Ranges ranges;
    const vector<string>& tokens = strings::tokenize(temp, "[]-,\n");
    if (tokens.size() % 2 != 0) {
      return Error("Expecting one or more \"ranges\"");
    } else {
      for (size_t i = 0; i < tokens.size(); i += 2) {
        Value::Range *range = ranges.add_range();

        int j = i;
        try {
          range->set_begin(boost::lexical_cast<uint64_t>((tokens[j++])));
          range->set_end(boost::lexical_cast<uint64_t>(tokens[j++]));
        } catch (const boost::bad_lexical_cast&) {
          return Error(
              "Expecting non-negative integers in '" + tokens[j - 1] + "'");
        }
      }

      value.set_type(Value::RANGES);
      value.mutable_ranges()->MergeFrom(ranges);
      return value;
    }
  } else if (index == string::npos) {
    size_t index = temp.find('{');
    if (index == 0) {
      // This is a set.
      Value::Set set;
      const vector<string>& tokens = strings::tokenize(temp, "{},\n");
      for (size_t i = 0; i < tokens.size(); i++) {
        set.add_item(tokens[i]);
      }

      value.set_type(Value::SET);
      value.mutable_set()->MergeFrom(set);
      return value;
    } else if (index == string::npos) {
      try {
        Value::Scalar scalar;
        scalar.set_value(boost::lexical_cast<double>(temp));
        // This is a Scalar.
        value.set_type(Value::SCALAR);
        value.mutable_scalar()->MergeFrom(scalar);
        return value;
      } catch (const boost::bad_lexical_cast&) {
        // This is a Text.
        Value::Text text;
        text.set_value(temp);
        value.set_type(Value::TEXT);
        value.mutable_text()->MergeFrom(text);
        return value;
      }
    } else {
      return Error("Unexpected '{' found");
    }
  }

  return Error("Unexpected '[' found");
}

} // namespace values {
} // namespace internal {


// We manipulate scalar values by converting them from floating point to a
// fixed point representation, doing a calculation, and then converting
// the result back to floating point. We deliberately only preserve three
// decimal digits of precision in the fixed point representation. This
// ensures that client applications see predictable numerical behavior, at
// the expense of sacrificing some precision.

static long long convertToFixed(double floatValue)
{
  return std::llround(floatValue * 1000);
}


static double convertToFloating(long long fixedValue)
{
  // NOTE: We do the conversion from fixed point via integer division
  // and then modulus, rather than a single floating point division.
  // This ensures that we only apply floating point division to inputs
  // in the range [0,999], which is easier to check for correctness.
  double quotient = static_cast<double>(fixedValue / 1000);
  double remainder = static_cast<double>(fixedValue % 1000) / 1000.0;

  return quotient + remainder;
}


ostream& operator<<(ostream& stream, const Value::Scalar& scalar)
{
  // We discard any additional precision from scalar resources before
  // writing them to an ostream. This is redundant when the scalar is
  // obtained from one of the operators below, but user-specified
  // resource values might contain additional precision.
  return stream << convertToFloating(convertToFixed(scalar.value()));
}


bool operator==(const Value::Scalar& left, const Value::Scalar& right)
{
  return convertToFixed(left.value()) == convertToFixed(right.value());
}


bool operator<=(const Value::Scalar& left, const Value::Scalar& right)
{
  return convertToFixed(left.value()) <= convertToFixed(right.value());
}


Value::Scalar operator+(const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result = left;
  result += right;
  return result;
}


Value::Scalar operator-(const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result = left;
  result -= right;
  return result;
}


Value::Scalar& operator+=(Value::Scalar& left, const Value::Scalar& right)
{
  long long sum = convertToFixed(left.value()) + convertToFixed(right.value());
  left.set_value(convertToFloating(sum));
  return left;
}


Value::Scalar& operator-=(Value::Scalar& left, const Value::Scalar& right)
{
  long long diff = convertToFixed(left.value()) - convertToFixed(right.value());
  left.set_value(convertToFloating(diff));
  return left;
}

namespace ranges {

static void add(Value::Ranges* result, int64_t begin, int64_t end)
{
  if (begin > end) {
    return;
  }
  Value::Range* range = result->add_range();
  range->set_begin(begin);
  range->set_end(end);
}

} // namespace ranges {


// Coalesce the given 'range' into already coalesced 'ranges'.
static void coalesce(Value::Ranges* ranges, const Value::Range& range)
{
  // Note that we assume that ranges has already been coalesced.

  Value::Ranges result;
  Value::Range temp = range;

  for (int i = 0; i < ranges->range_size(); i++) {
    const Value::Range& current = ranges->range(i);

    // Check if current and range overlap. Note, we only need to
    // compare with range and not with temp to check for overlap
    // because we expect ranges to be coalesced to begin with!
    if (current.begin() <= range.end() + 1 &&
        current.end() >= range.begin() - 1) {
      // current:   |   |
      // range:       |   |
      // range:   |   |
      // Update temp with new boundaries.
      temp.set_begin(min(temp.begin(), min(range.begin(), current.begin())));
      temp.set_end(max(temp.end(), max(range.end(), current.end())));
    } else { // No overlap.
      result.add_range()->MergeFrom(current);
    }
  }

  result.add_range()->MergeFrom(temp);
  *ranges = result;
}


// Coalesce the given un-coalesced 'uranges' into already coalesced 'ranges'.
static void coalesce(Value::Ranges* ranges, const Value::Ranges& uranges)
{
  // Note that we assume that ranges has already been coalesced.

  for (int i = 0; i < uranges.range_size(); i++) {
    coalesce(ranges, uranges.range(i));
  }
}


static void remove(Value::Ranges* ranges, const Value::Range& range)
{
  // Note that we assume that ranges has already been coalesced.

  Value::Ranges result;

  for (int i = 0; i < ranges->range_size(); i++) {
    const Value::Range& current = ranges->range(i);

    // Note that these if/else if conditionals are in a particular
    // order. In particular, the last two assume that the "subsumes"
    // checks have already occurred.
    if (range.begin() <= current.begin() && range.end() >= current.end()) {
      // Range subsumes current.
      // current:  |     |
      // range:  |         |
      // range:  |       |
      // range:    |       |
      // range:    |     |
    } else if (range.begin() >= current.begin() &&
               range.end() <= current.end()) {
      // Range is subsumed by current.
      // current:  |     |
      // range:      | |
      // range:    |   |
      // range:      |   |
      ranges::add(&result, current.begin(), range.begin() - 1);
      ranges::add(&result, range.end() + 1, current.end());
    } else if (range.begin() <= current.begin() &&
               range.end() >= current.begin()) {
      // Range overlaps to the left.
      // current:  |     |
      // range:  |     |
      // range:  | |
      ranges::add(&result, range.end() + 1, current.end());
    } else if (range.begin() <= current.end() && range.end() >= current.end()) {
      // Range overlaps to the right.
      // current:  |     |
      // range:      |      |
      // range:          |  |
      ranges::add(&result, current.begin(), range.begin() - 1);
    } else {
      // Range doesn't overlap current.
      // current:        |   |
      // range:   |   |
      // range:                |   |
      ranges::add(&result, current.begin(), current.end());
    }
  }

  *ranges = result;
}


ostream& operator<<(ostream& stream, const Value::Ranges& ranges)
{
  stream << "[";
  for (int i = 0; i < ranges.range_size(); i++) {
    stream << ranges.range(i).begin() << "-" << ranges.range(i).end();
    if (i + 1 < ranges.range_size()) {
      stream << ", ";
    }
  }
  stream << "]";
  return stream;
}


bool operator==(const Value::Ranges& _left, const Value::Ranges& _right)
{
  Value::Ranges left;
  coalesce(&left, _left);

  Value::Ranges right;
  coalesce(&right, _right);

  if (left.range_size() == right.range_size()) {
    for (int i = 0; i < left.range_size(); i++) {
      // Make sure this range is equal to a range in the right.
      bool found = false;
      for (int j = 0; j < right.range_size(); j++) {
        if (left.range(i).begin() == right.range(j).begin() &&
            left.range(i).end() == right.range(j).end()) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


bool operator<=(const Value::Ranges& _left, const Value::Ranges& _right)
{
  Value::Ranges left;
  coalesce(&left, _left);

  Value::Ranges right;
  coalesce(&right, _right);

  for (int i = 0; i < left.range_size(); i++) {
    // Make sure this range is a subset of a range in right.
    bool matched = false;
    for (int j = 0; j < right.range_size(); j++) {
      if (left.range(i).begin() >= right.range(j).begin() &&
          left.range(i).end() <= right.range(j).end()) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return false;
    }
  }

  return true;
}


Value::Ranges operator+(const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;

  coalesce(&result, left);
  coalesce(&result, right);

  return result;
}


Value::Ranges operator-(const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;

  coalesce(&result, left);
  coalesce(&result, right);

  for (int i = 0; i < right.range_size(); i++) {
    remove(&result, right.range(i));
  }

  return result;
}


Value::Ranges& operator+=(Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges temp;

  coalesce(&temp, left);

  left = temp;

  coalesce(&left, right);

  return left;
}


Value::Ranges& operator-=(Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges temp;

  coalesce(&temp, left);
  coalesce(&temp, right);

  left = temp;

  for (int i = 0; i < right.range_size(); i++) {
    remove(&left, right.range(i));
  }

  return left;
}


ostream& operator<<(ostream& stream, const Value::Set& set)
{
  stream << "{";
  for (int i = 0; i < set.item_size(); i++) {
    stream << set.item(i);
    if (i + 1 < set.item_size()) {
      stream << ", ";
    }
  }
  stream << "}";
  return stream;
}


bool operator==(const Value::Set& left, const Value::Set& right)
{
  if (left.item_size() == right.item_size()) {
    for (int i = 0; i < left.item_size(); i++) {
      // Make sure this item is equal to an item in the right.
      bool found = false;
      for (int j = 0; j < right.item_size(); j++) {
        if (left.item(i) == right.item(i)) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


bool operator<=(const Value::Set& left, const Value::Set& right)
{
  if (left.item_size() <= right.item_size()) {
    for (int i = 0; i < left.item_size(); i++) {
      // Make sure this item is equal to an item in the right.
      bool found = false;
      for (int j = 0; j < right.item_size(); j++) {
        if (left.item(i) == right.item(j)) {
          found = true;
          break;
        }
      }

      if (!found) {
        return false;
      }
    }

    return true;
  }

  return false;
}


Value::Set operator+(const Value::Set& left, const Value::Set& right)
{
  Value::Set result;

  for (int i = 0; i < left.item_size(); i++) {
    result.add_item(left.item(i));
  }

  // A little bit of extra logic to avoid adding duplicates from right.
  for (int i = 0; i < right.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < result.item_size(); j++) {
      if (right.item(i) == result.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      result.add_item(right.item(i));
    }
  }

  return result;
}


Value::Set operator-(const Value::Set& left, const Value::Set& right)
{
  Value::Set result;

  // Look for the same item in right as we add left to result.
  for (int i = 0; i < left.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < right.item_size(); j++) {
      if (left.item(i) == right.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      result.add_item(left.item(i));
    }
  }

  return result;
}


Value::Set& operator+=(Value::Set& left, const Value::Set& right)
{
  // A little bit of extra logic to avoid adding duplicates from right.
  for (int i = 0; i < right.item_size(); i++) {
    bool found = false;
    for (int j = 0; j < left.item_size(); j++) {
      if (right.item(i) == left.item(j)) {
        found = true;
        break;
      }
    }

    if (!found) {
      left.add_item(right.item(i));
    }
  }

  return left;
}


Value::Set& operator-=(Value::Set& left, const Value::Set& right)
{
  // For each item in right, remove it if it's in left.
  for (int i = 0; i < right.item_size(); i++) {
    for (int j = 0; j < left.item_size(); j++) {
      if (right.item(i) == left.item(j)) {
        left.mutable_item()->DeleteSubrange(j, 1);
        break;
      }
    }
  }

  return left;
}


ostream& operator<<(ostream& stream, const Value::Text& value)
{
  return stream << value.value();
}


bool operator==(const Value::Text& left, const Value::Text& right)
{
  return left.value() == right.value();
}

} // namespace v1 {
} // namespace mesos {
