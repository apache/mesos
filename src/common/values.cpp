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

#include <iostream>
#include <vector>

#include <glog/logging.h>

#include <boost/lexical_cast.hpp>

#include "common/foreach.hpp"
#include "common/resources.hpp"
#include "common/strings.hpp"
#include "common/values.hpp"

using std::ostream;
using std::string;
using std::vector;

namespace mesos {

namespace internal {
namespace values {

Try<Value> parse(const std::string& text) {
  Value value;

  // Remove any spaces from the text.
  string temp;
  foreach (const char c, text) {
    if (c != ' ') {
      temp += c;
    }
  }

  if (temp.length() == 0) {
    return Try<Value>::error(
      "Error parsing value, expecting non-empty string");
  }

  // TODO(ynie): Find a better way to check brackets.
  if (!strings::checkBracketsMatching(temp, '{', '}') ||
      !strings::checkBracketsMatching(temp, '[', ']') ||
      !strings::checkBracketsMatching(temp, '(', ')')) {
    return Try<Value>::error(
      "Error parsing value, brackets doesn't match");
  }

  size_t index = temp.find('[');
  if (index == 0) {
    // This is a ranges.
    Value::Ranges ranges;
    const vector<string>& tokens = strings::split(temp, "[]-,\n");
    if (tokens.size() % 2 != 0) {
      return Try<Value>::error("Error parsing value: " + text +
                               ", expect one or more \"ranges \"");
    } else {
      for (int i = 0; i < tokens.size(); i += 2) {
        Value::Range *range = ranges.add_range();

        int j = i;
        try {
          range->set_begin(boost::lexical_cast<uint64_t>((tokens[j++])));
          range->set_end(boost::lexical_cast<uint64_t>(tokens[j++]));
        } catch (const boost::bad_lexical_cast&) {
          return Try<Value>::error(
            "Error parsing value " + text +
            ", expecting non-negative integers in '" + tokens[j - 1] + "'");
        }
      }

      value.set_type(Value::RANGES);
      value.mutable_ranges()->MergeFrom(ranges);
      return Try<Value>::some(value);
    }
  } else if (index == string::npos) {
    size_t index = temp.find('{');
    if (index == 0) {
      // This is a set.
      Value::Set set;
      const vector<string>& tokens = strings::split(temp, "{},\n");
      for (int i = 0; i < tokens.size(); i++) {
        set.add_item(tokens[i]);
      }

      value.set_type(Value::SET);
      value.mutable_set()->MergeFrom(set);
      return Try<Value>::some(value);
    } else if (index == string::npos) {
      try {
        Value::Scalar scalar;
        scalar.set_value(boost::lexical_cast<double>(temp));
        // This is a Scalar.
        value.set_type(Value::SCALAR);
        value.mutable_scalar()->MergeFrom(scalar);
        return Try<Value>::some(value);
      } catch (const boost::bad_lexical_cast&) {
        // This is a Text.
        Value::Text text;
        text.set_value(temp);
        value.set_type(Value::TEXT);
        value.mutable_text()->MergeFrom(text);
        return Try<Value>::some(value);
      }
    } else {
      return Try<Value>::error(
        "Error parsing value " + text + ", bad '{' found");
    }
  }

  return Try<Value>::error(
    "Error parsing value " + text + ", bad '[' found");
}

} // namespace values {
} // namespace internal {


bool operator == (const Value::Scalar& left, const Value::Scalar& right)
{
  return left.value() == right.value();
}


bool operator <= (const Value::Scalar& left, const Value::Scalar& right)
{
  return left.value() <= right.value();
}


Value::Scalar operator + (const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result;
  result.set_value(left.value() + right.value());
  return result;
}


Value::Scalar operator - (const Value::Scalar& left, const Value::Scalar& right)
{
  Value::Scalar result;
  result.set_value(left.value() - right.value());
  return result;
}


Value::Scalar& operator += (Value::Scalar& left, const Value::Scalar& right)
{
  left.set_value(left.value() + right.value());
  return left;
}


Value::Scalar& operator -= (Value::Scalar& left, const Value::Scalar& right)
{
  left.set_value(left.value() - right.value());
  return left;
}


static void coalesce(Value::Ranges* ranges, const Value::Range& range)
{
  // Note that we assume that ranges has already been coalesced.

  bool coalesced = false;

  for (int i = 0; i < ranges->range_size(); i++) {
    int64_t begin = ranges->range(i).begin();
    int64_t end = ranges->range(i).end();

    if (begin <= range.begin() && range.end() <= end) {
      // Ignore range since it is subsumed by a range in ranges.
      coalesced = true;
      break;
    } else if (begin <= range.begin() && end < range.end()) {
      // Grow the end of the range in ranges.
      ranges->mutable_range(i)->set_end(range.end());
      coalesced = true;
      break;
    } else if (range.begin() < begin && range.end() <= end) {
      // Grow the beginning of the range in ranges.
      ranges->mutable_range(i)->set_begin(range.begin());
      coalesced = true;
      break;
    } else if (range.begin() < begin && end < range.end()) {
      // Replace (grow both the beginning and the end) of the range in ranges.
      ranges->mutable_range(i)->set_begin(range.begin());
      ranges->mutable_range(i)->set_end(range.end());
      coalesced = true;
      break;
    }
  }

  if (!coalesced) {
    ranges->add_range()->MergeFrom(range);
  }
}


static void remove(Value::Ranges* ranges, const Value::Range& range)
{
  // Note that we assume that ranges has already been coalesced.

  Value::Ranges result;

  for (int i = 0; i < ranges->range_size(); i++) {
    int64_t begin = ranges->range(i).begin();
    int64_t end = ranges->range(i).end();

    if (begin == range.begin() && range.end() == end) {
      // Remove range from ranges, but keep everything else.
      for (int j = i + 1; j < ranges->range_size(); j++) {
        result.add_range()->MergeFrom(ranges->range(j));
      }
      break;
    } else if (begin <= range.begin() && range.end() < end) {
      // Shrink range in ranges.
      Value::Range* temp = result.add_range();
      temp->set_begin(range.end() + 1);
      temp->set_end(end);
      break;
    } else if (begin < range.begin() && range.end() >= end) {
      // Shrink end of range in ranges.
      Value::Range* temp = result.add_range();
      temp->set_begin(begin);
      temp->set_end(range.begin() - 1);
      break;
    } else if (begin < range.begin() && range.end() < end) {
      // Split range in ranges.
      Value::Range* temp = result.add_range();
      temp->set_begin(begin);
      temp->set_end(range.begin() - 1);
      temp = result.add_range();
      temp->set_begin(range.end() + 1);
      temp->set_end(end);
      break;
    }
  }

  *ranges = result;
}


bool operator == (const Value::Ranges& left, const Value::Ranges& right)
{
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


bool operator <= (const Value::Ranges& left, const Value::Ranges& right)
{
   for (int i = 0; i < left.range_size(); i++) {
      // Make sure this range is a subset of a range in right.
      bool matched = false;
      for (int j = 0; j < right.range_size(); j++) {
        if ((left.range(i).begin() >= right.range(j).begin() &&
             left.range(i).end() <= right.range(j).end())) {
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


Value::Ranges operator + (const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;

  for (int i = 0; i < left.range_size(); i++) {
    coalesce(&result, left.range(i));
  }

  for (int i = 0; i < right.range_size(); i++) {
    coalesce(&result, right.range(i));
  }

  return result;
}


Value::Ranges operator - (const Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges result;

  for (int i = 0; i < left.range_size(); i++) {
    coalesce(&result, left.range(i));
  }

  for (int i = 0; i < right.range_size(); i++) {
    coalesce(&result, right.range(i));
  }

  for (int i = 0; i < right.range_size(); i++) {
    remove(&result, right.range(i));
  }

  return result;
}


Value::Ranges& operator += (Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges temp;

  for (int i = 0; i < left.range_size(); i++) {
    coalesce(&temp, left.range(i));
  }

  left = temp;

  for (int i = 0; i < right.range_size(); i++) {
    coalesce(&left, right.range(i));
  }

  return left;
}


Value::Ranges& operator -= (Value::Ranges& left, const Value::Ranges& right)
{
  Value::Ranges temp;

  for (int i = 0; i < left.range_size(); i++) {
    coalesce(&temp, left.range(i));
  }

  for (int i = 0; i < right.range_size(); i++) {
    coalesce(&temp, right.range(i));
  }

  left = temp;

  for (int i = 0; i < right.range_size(); i++) {
    remove(&left, right.range(i));
  }

  return left;
}


bool operator == (const Value::Set& left, const Value::Set& right)
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


bool operator <= (const Value::Set& left, const Value::Set& right)
{
  if (left.item_size() <= right.item_size()) {
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


Value::Set operator + (const Value::Set& left, const Value::Set& right)
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


Value::Set operator - (const Value::Set& left, const Value::Set& right)
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


Value::Set& operator += (Value::Set& left, const Value::Set& right)
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


Value::Set& operator -= (Value::Set& left, const Value::Set& right)
{
  // For each item in right check if it's in left and add it if not.
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

bool operator == (const Value::Text& left, const Value::Text& right)
{
  return left.value() == right.value();
}

} // namespace mesos
