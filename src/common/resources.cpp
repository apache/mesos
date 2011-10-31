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


using std::ostream;
using std::string;
using std::vector;


namespace mesos { 

bool operator == (const Resource::Scalar& left, const Resource::Scalar& right)
{
  return left.value() == right.value();
}


bool operator <= (const Resource::Scalar& left, const Resource::Scalar& right)
{
  return left.value() <= right.value();
}


Resource::Scalar operator + (const Resource::Scalar& left, const Resource::Scalar& right)
{
  Resource::Scalar result;
  result.set_value(left.value() + right.value());
  return result;
}

  
Resource::Scalar operator - (const Resource::Scalar& left, const Resource::Scalar& right)
{
  Resource::Scalar result;
  result.set_value(left.value() - right.value());
  return result;
}

  
Resource::Scalar& operator += (Resource::Scalar& left, const Resource::Scalar& right)
{
  left.set_value(left.value() + right.value());
  return left;
}


Resource::Scalar& operator -= (Resource::Scalar& left, const Resource::Scalar& right)
{
  left.set_value(left.value() - right.value());
  return left;
}


static void coalesce(Resource::Ranges* ranges, const Resource::Range& range)
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


static void remove(Resource::Ranges* ranges, const Resource::Range& range)
{
  // Note that we assume that ranges has already been coalesced.

  Resource::Ranges result;

  for (int i = 0; i < ranges->range_size(); i++) {
    int64_t begin = ranges->range(i).begin();
    int64_t end = ranges->range(i).end();

    if (begin == range.begin() && range.end() == end) {
      // Remove range from ranges, but keep everything else.
      for (int j = i; j < ranges->range_size(); j++) {
        result.add_range()->MergeFrom(ranges->range(j));
      }
      break;
    } else if (begin <= range.begin() && range.end() < end) {
      // Shrink range in ranges.
      Resource::Range* temp = result.add_range();
      temp->set_begin(range.end() + 1);
      temp->set_end(end);
      break;
    } else if (begin < range.begin() && range.end() >= end) {
      // Shrink end of range in ranges.
      Resource::Range* temp = result.add_range();
      temp->set_begin(begin);
      temp->set_end(range.begin() - 1);
      break;
    } else if (begin < range.begin() && range.end() < end) {
      // Split range in ranges.
      Resource::Range* temp = result.add_range();
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


bool operator == (const Resource::Ranges& left, const Resource::Ranges& right)
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


bool operator <= (const Resource::Ranges& left, const Resource::Ranges& right)
{
  if (left.range_size() <= right.range_size()) {
    for (int i = 0; i < left.range_size(); i++) {
      // Make sure this range is a subset of every range in right.
      for (int j = 0; j < right.range_size(); j++) {
        if ((left.range(i).begin() <= right.range(j).begin() &&
             left.range(i).end() > right.range(j).end()) ||
            (left.range(i).begin() < right.range(j).begin() &&
             left.range(i).end() >= right.range(j).end())) {
          return false;
        }
      }
    }

    return true;
  }

  return false;
}


Resource::Ranges operator + (const Resource::Ranges& left, const Resource::Ranges& right)
{
  Resource::Ranges result;

  for (int i = 0; i < left.range_size(); i++) {
    coalesce(&result, left.range(i));
  }

  for (int i = 0; i < right.range_size(); i++) {
    coalesce(&result, right.range(i));
  }

  return result;
}

  
Resource::Ranges operator - (const Resource::Ranges& left, const Resource::Ranges& right)
{
  Resource::Ranges result;

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

  
Resource::Ranges& operator += (Resource::Ranges& left, const Resource::Ranges& right)
{
  Resource::Ranges temp;

  for (int i = 0; i < left.range_size(); i++) {
    coalesce(&temp, left.range(i));
  }

  left = temp;

  for (int i = 0; i < right.range_size(); i++) {
    coalesce(&left, right.range(i));
  }

  return left;
}


Resource::Ranges& operator -= (Resource::Ranges& left, const Resource::Ranges& right)
{
  Resource::Ranges temp;

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


bool operator == (const Resource::Set& left, const Resource::Set& right)
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


bool operator <= (const Resource::Set& left, const Resource::Set& right)
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


Resource::Set operator + (const Resource::Set& left, const Resource::Set& right)
{
  Resource::Set result;

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

  
Resource::Set operator - (const Resource::Set& left, const Resource::Set& right)
{
  Resource::Set result;

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

  
Resource::Set& operator += (Resource::Set& left, const Resource::Set& right)
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


Resource::Set& operator -= (Resource::Set& left, const Resource::Set& right)
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
    if (left.type() == Resource::SCALAR) {
      return left.scalar() == right.scalar();
    } else if (left.type() == Resource::RANGES) {
      return left.ranges() == right.ranges();
    } else if (left.type() == Resource::SET) {
      return left.set() == right.set();
    }
  }

  return false;
}


bool operator <= (const Resource& left, const Resource& right)
{
  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Resource::SCALAR) {
      return left.scalar() <= right.scalar();
    } else if (left.type() == Resource::RANGES) {
      return left.ranges() <= right.ranges();
    } else if (left.type() == Resource::SET) {
      return left.set() <= right.set();
    }
  }

  return false;
}


Resource operator + (const Resource& left, const Resource& right)
{
  Resource result = left;

  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Resource::SCALAR) {
      result.mutable_scalar()->MergeFrom(left.scalar() + right.scalar());
    } else if (left.type() == Resource::RANGES) {
      result.mutable_ranges()->Clear();
      result.mutable_ranges()->MergeFrom(left.ranges() + right.ranges());
    } else if (left.type() == Resource::SET) {
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
    if (left.type() == Resource::SCALAR) {
      result.mutable_scalar()->MergeFrom(left.scalar() - right.scalar());
    } else if (left.type() == Resource::RANGES) {
      result.mutable_ranges()->Clear();
      result.mutable_ranges()->MergeFrom(left.ranges() - right.ranges());
    } else if (left.type() == Resource::SET) {
      result.mutable_set()->Clear();
      result.mutable_set()->MergeFrom(left.set() - right.set());
    }
  }
  
  return result;
}

  
Resource& operator += (Resource& left, const Resource& right)
{
  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Resource::SCALAR) {
      left.mutable_scalar()->MergeFrom(left.scalar() + right.scalar());
    } else if (left.type() == Resource::RANGES) {
      left.mutable_ranges()->Clear();
      left.mutable_ranges()->MergeFrom(left.ranges() + right.ranges());
    } else if (left.type() == Resource::SET) {
      left.mutable_set()->Clear();
      left.mutable_set()->MergeFrom(left.set() + right.set());
    }
  }
  
  return left;
}


Resource& operator -= (Resource& left, const Resource& right)
{
  if (left.name() == right.name() && left.type() == right.type()) {
    if (left.type() == Resource::SCALAR) {
      left.mutable_scalar()->MergeFrom(left.scalar() - right.scalar());
    } else if (left.type() == Resource::RANGES) {
      left.mutable_ranges()->Clear();
      left.mutable_ranges()->MergeFrom(left.ranges() - right.ranges());
    } else if (left.type() == Resource::SET) {
      left.mutable_set()->Clear();
      left.mutable_set()->MergeFrom(left.set() - right.set());
    }
  }
  
  return left;
}


ostream& operator << (ostream& stream, const Resource& resource)
{
  stream << resource.name() << "=";
  if (resource.type() == Resource::SCALAR) {
    stream << resource.scalar().value();
  } else if (resource.type() == Resource::RANGES) {
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
  } else if (resource.type() == Resource::SET) {
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

Resource Resources::parse(const string& name, const string& value)
{
  Resource resource;
  resource.set_name(name);

  // Remove any spaces from the value.
  string temp;
  foreach (const char c, value) {
    if (c != ' ') {
      temp += c;
    }
  }

  size_t index = temp.find('[');
  if (index == 0) {
    // This is a ranges.
    Resource::Ranges ranges;
    const vector<string>& tokens = strings::split(temp, "[]-,\n");
    if (tokens.size() % 2 != 0) {
      LOG(FATAL) << "Error parsing value for " << name
                 << ", expecting one or more \"ranges\"";
    } else {
      for (int i = 0; i < tokens.size(); i += 2) {
        Resource::Range *range = ranges.add_range();

        int j = i;
        try {
          range->set_begin(boost::lexical_cast<uint64_t>((tokens[j++])));
          range->set_end(boost::lexical_cast<uint64_t>(tokens[j++]));
        } catch (const boost::bad_lexical_cast&) {
          LOG(FATAL) << "Error parsing value for " << name
                     << ", expecting non-negative integers in '"
                     << tokens[j - 1] << "'";
        }
      }

      resource.set_type(Resource::RANGES);
      resource.mutable_ranges()->MergeFrom(ranges);
    }
  } else if (index == string::npos) {
    size_t index = temp.find('{');
    if (index == 0) {
      // This is a set.
      Resource::Set set;
      const vector<string>& tokens = strings::split(temp, "{},\n");
      for (int i = 0; i < tokens.size(); i++) {
        set.add_item(tokens[i]);
      }

      resource.set_type(Resource::SET);
      resource.mutable_set()->MergeFrom(set);
    } else if (index == string::npos) {
      // This *should* be a scalar.
      Resource::Scalar scalar;
      try {
        scalar.set_value(boost::lexical_cast<double>(temp));
      } catch (const boost::bad_lexical_cast&) {
        LOG(FATAL) << "Error parsing value for " << name
                   << ", expecting a number from '" << temp << "'";
      }

      resource.set_type(Resource::SCALAR);
      resource.mutable_scalar()->MergeFrom(scalar);
    } else {
      LOG(FATAL) << "Error parsing value for " << name
                 << ", bad '{' found";
    }
  } else {
    LOG(FATAL) << "Error parsing value for " << name
               << ", bad '[' found";
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
