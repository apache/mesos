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

#include "common/foreach.hpp"
#include "common/resources.hpp"
#include "common/strings.hpp"
#include "common/try.hpp"
#include "common/values.hpp"


using std::ostream;
using std::string;
using std::vector;


namespace mesos {

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


bool operator == (const Resource& left, const Resource& right)
{
  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Value::SCALAR) {
      return left.scalar() == right.scalar();
    } else if (left.type() == Value::RANGES) {
      return left.ranges() == right.ranges();
    } else if (left.type() == Value::SET) {
      return left.set() == right.set();
    }
  }

  return false;
}


bool operator <= (const Resource& left, const Resource& right)
{
  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Value::SCALAR) {
      return left.scalar() <= right.scalar();
    } else if (left.type() == Value::RANGES) {
      return left.ranges() <= right.ranges();
    } else if (left.type() == Value::SET) {
      return left.set() <= right.set();
    }
  }

  return false;
}


Resource operator + (const Resource& left, const Resource& right)
{
  Resource result = left;

  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Value::SCALAR) {
      result.mutable_scalar()->MergeFrom(left.scalar() + right.scalar());
    } else if (left.type() == Value::RANGES) {
      result.mutable_ranges()->Clear();
      result.mutable_ranges()->MergeFrom(left.ranges() + right.ranges());
    } else if (left.type() == Value::SET) {
      result.mutable_set()->Clear();
      result.mutable_set()->MergeFrom(left.set() + right.set());
    }
  }

  return result;
}


Resource operator - (const Resource& left, const Resource& right)
{
  Resource result = left;

  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Value::SCALAR) {
      result.mutable_scalar()->MergeFrom(left.scalar() - right.scalar());
    } else if (left.type() == Value::RANGES) {
      result.mutable_ranges()->Clear();
      result.mutable_ranges()->MergeFrom(left.ranges() - right.ranges());
    } else if (left.type() == Value::SET) {
      result.mutable_set()->Clear();
      result.mutable_set()->MergeFrom(left.set() - right.set());
    }
  }

  return result;
}


Resource& operator += (Resource& left, const Resource& right)
{
  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Value::SCALAR) {
      left.mutable_scalar()->MergeFrom(left.scalar() + right.scalar());
    } else if (left.type() == Value::RANGES) {
      left.mutable_ranges()->Clear();
      left.mutable_ranges()->MergeFrom(left.ranges() + right.ranges());
    } else if (left.type() == Value::SET) {
      left.mutable_set()->Clear();
      left.mutable_set()->MergeFrom(left.set() + right.set());
    }
  }

  return left;
}


Resource& operator -= (Resource& left, const Resource& right)
{
  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Value::SCALAR) {
      left.mutable_scalar()->MergeFrom(left.scalar() - right.scalar());
    } else if (left.type() == Value::RANGES) {
      left.mutable_ranges()->Clear();
      left.mutable_ranges()->MergeFrom(left.ranges() - right.ranges());
    } else if (left.type() == Value::SET) {
      left.mutable_set()->Clear();
      left.mutable_set()->MergeFrom(left.set() - right.set());
    }
  }

  return left;
}


ostream& operator << (ostream& stream, const Resource& resource)
{
  stream << resource.name() << "=";
  if (resource.type() == Value::SCALAR) {
    stream << resource.scalar().value();
  } else if (resource.type() == Value::RANGES) {
    stream << "[";
    for (int i = 0; i < resource.ranges().range_size(); i++) {
      stream << resource.ranges().range(i).begin()
             << "-"
             << resource.ranges().range(i).end();
      if (i + 1 < resource.ranges().range_size()) {
        stream << ", ";
      }
    }
    stream << "]";
  } else if (resource.type() == Value::SET) {
    stream << "{";
    for (int i = 0; i < resource.set().item_size(); i++) {
      stream << resource.set().item(i);
      if (i + 1 < resource.set().item_size()) {
        stream << ", ";
      }
    }
    stream << "}";
  }

  return stream;
}


namespace internal {

Resource Resources::parse(const std::string& name, const std::string& text)
{
  Resource resource;
  Try<Value> result = values::parse(text);

  if (result.isError()) {
    LOG(FATAL) << "Failed to parse resource " << name
               << " text " << text
               << " error " << result.error();
  } else{
    Value value = result.get();
    resource.set_name(name);

    if (value.type() == Value::RANGES) {
      resource.set_type(Value::RANGES);
      resource.mutable_ranges()->MergeFrom(value.ranges());
    } else if (value.type() == Value::SET) {
      resource.set_type(Value::SET);
      resource.mutable_set()->MergeFrom(value.set());
    } else if (value.type() == Value::SCALAR) {
      resource.set_type(Value::SCALAR);
      resource.mutable_scalar()->MergeFrom(value.scalar());
    } else {
      LOG(FATAL) << "Bad type for resource " << name
                 << " text " << text
                 << " type " << value.type();
    }
  }

  return resource;
}

Resources Resources::parse(const string& s)
{
  // Tokenize and parse the value of "resources".
  Resources resources;

  vector<string> tokens = strings::split(s, ";\n");

  for (int i = 0; i < tokens.size(); i++) {
    const vector<string>& pairs = strings::split(tokens[i], ":");
    if (pairs.size() != 2) {
      LOG(FATAL) << "Bad value for resources, missing ':' within " << pairs[0];
    }

    resources += parse(pairs[0], pairs[1]);
  }

  return resources;
}


} // namespace internal {
} // namespace mesos {
