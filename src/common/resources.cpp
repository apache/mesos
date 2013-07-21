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
  if (matches(left, right)) {
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


bool operator != (const Resource& left, const Resource& right)
{
  return !(left == right);
}


bool matches(const Resource& left, const Resource& right)
{
  return left.name() == right.name() &&
    left.type() == right.type() &&
    left.role() == right.role();
}


bool operator <= (const Resource& left, const Resource& right)
{
  if (matches(left, right)) {
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

  if (matches(left, right)) {
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

  if (matches(left, right)) {
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
  if (matches(left, right)) {
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
  if (matches(left, right)) {
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
  stream << resource.name() << "(" << resource.role() << "):";

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

Resources Resources::flatten(const string& role) const
{
  Resources flattened;

  foreach (const Resource& r, resources) {
    Resource toRemove = r;
    toRemove.set_role(role);

    bool found = false;
    for (int i = 0; i < flattened.resources.size(); i++) {
      Resource removed = flattened.resources.Get(i);
      if (toRemove.name() == removed.name() &&
          toRemove.type() == removed.type()) {
        flattened.resources.Mutable(i)->MergeFrom(toRemove + removed);
        found = true;
        break;
      }
    }

    if (!found) {
      flattened.resources.Add()->MergeFrom(toRemove);
    }
  }

  return flattened;
}


Resources Resources::extract(const string& role) const
{
  Resources r;

  foreach (const Resource& resource, resources) {
    if (resource.role() == role) {
      r += resource;
    }
  }

  return r;
}


Try<Resource> Resources::parse(
    const string& name,
    const string& text,
    const string& role)
{
  Resource resource;
  Try<Value> result = values::parse(text);

  if (result.isError()) {
    return Error("Failed to parse resource " + name +
                 " text " + text +
                 " error " + result.error());
  } else{
    Value value = result.get();
    resource.set_name(name);
    resource.set_role(role);

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
      return Error("Bad type for resource " + name +
                   " text " + text +
                   " type " + Value::Type_Name(value.type()));
    }
  }

  return resource;
}


Try<Resources> Resources::parse(const string& s, const string& defaultRole)
{
  Resources resources;

  vector<string> tokens = strings::tokenize(s, ";");

  foreach (const string& token, tokens) {
    vector<string> pair = strings::tokenize(token, ":");
    if (pair.size() != 2) {
      return Error("Bad value for resources, missing or extra ':' in " + token);
    }

    string name;
    string role;
    size_t openParen = pair[0].find("(");
    if (openParen == string::npos) {
      name = strings::trim(pair[0]);
      role = defaultRole;
    } else {
      size_t closeParen = pair[0].find(")");
      if (closeParen == string::npos || closeParen < openParen) {
        return Error("Bad value for resources, mismatched parentheses in " +
                     token);
      }

      name = strings::trim(pair[0].substr(0, openParen));
      role = strings::trim(pair[0].substr(openParen + 1,
                                          closeParen - openParen - 1));
    }

    Try<Resource> resource = Resources::parse(name, pair[1], role);
    if (resource.isError()) {
      return Error(resource.error());
    }
    resources += resource.get();
  }

  return resources;
}

} // namespace internal {
} // namespace mesos {
