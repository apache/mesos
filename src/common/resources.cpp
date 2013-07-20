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

#include <stout/foreach.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "common/resources.hpp"
#include "common/values.hpp"


using std::ostream;
using std::string;
using std::vector;


namespace mesos {

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

  switch (resource.type()) {
    case Value::SCALAR: stream << resource.scalar(); break;
    case Value::RANGES: stream << resource.ranges(); break;
    case Value::SET:    stream << resource.set();    break;
    default:
      LOG(FATAL) << "Unexpected Value type: " << resource.type();
      break;
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
                 << " type " << Value::Type_Name(value.type());
    }
  }

  return resource;
}

Resources Resources::parse(const string& s)
{
  // Tokenize and parse the value of "resources".
  Resources resources;

  vector<string> tokens = strings::tokenize(s, ";\n");

  for (size_t i = 0; i < tokens.size(); i++) {
    const vector<string>& pairs = strings::tokenize(tokens[i], ":");
    if (pairs.size() != 2) {
      LOG(FATAL) << "Bad value for resources, missing ':' within " << pairs[0];
    }

    resources += parse(pairs[0], pairs[1]);
  }

  return resources;
}


} // namespace internal {
} // namespace mesos {
