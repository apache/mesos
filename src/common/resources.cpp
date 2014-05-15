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

#include <iostream>
#include <vector>

#include <glog/logging.h>

#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include <stout/foreach.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>


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


Option<Resources> Resources::find(
    const Resources& toFind,
    const string& role) const
{
  Resources foundResources;

  foreach (const Resource& findResource, toFind) {
    Resource remaining = findResource;
    Option<Resources> all = getAll(findResource);
    bool done = false;

    if (all.isSome()) {
      for (int i = 0; i < 3 && !done; i++) {
        foreach (const Resource& potential, all.get()) {
          // Ensures that we take resources first from the specified role,
          // then from the default role, and then from any other role.
          if ((i == 0 && potential.role() == role) ||
              (i == 1 && potential.role() == "*" && potential.role() != role) ||
              (i == 2 && potential.role() != "*" && potential.role() != role)) {
            // The resources must have the same role for <= to work.
            Resource potential_ = potential;
            potential_.set_role(remaining.role());
            if (remaining <= potential_) {
              // We can satisfy the remaining requirements for this
              // resource type.
              Resource found = remaining;
              found.set_role(potential.role());
              foundResources += found;
              done = true;
            } else {
              foundResources += potential;
              remaining -= potential_;
            }
          }
        }
      }
    }

    if (!done) {
      return None();
    }
  }

  return foundResources;
}


Option<Resource> Resources::get(const Resource& r) const
{
  foreach (const Resource& resource, resources) {
    if (matches(resource, r)) {
      return resource;
    }
  }

  return None();
}


Option<Resources> Resources::getAll(const Resource& r) const
{
  Resources total;

  foreach (const Resource& resource, resources) {
    if (r.name() == resource.name() &&
        r.type() == resource.type()) {
      total += resource;
    }
  }

  if (total.size() > 0) {
    return total;
  }

  return None();
}


Option<double> Resources::cpus() const
{
  double total= 0;
  bool found = false;

  foreach (const Resource& resource, resources) {
    if (resource.name() == "cpus" && resource.type() == Value::SCALAR) {
      total += resource.scalar().value();
      found = true;
    }
  }

  if (found) {
    return total;
  }

  return None();
}


Option<Bytes> Resources::mem() const
{
  double total = 0;
  bool found = false;

  foreach (const Resource& resource, resources) {
    if (resource.name() == "mem" &&
        resource.type() == Value::SCALAR) {
      total += resource.scalar().value();
      found = true;
    }
  }

  if (found) {
    return Megabytes(static_cast<uint64_t>(total));
  }

  return None();
}


Option<Bytes> Resources::disk() const
{
  double total = 0;
  bool found = false;

  foreach (const Resource& resource, resources) {
    if (resource.name() == "disk" &&
        resource.type() == Value::SCALAR) {
      total += resource.scalar().value();
      found = true;
    }
  }

  if (found) {
    return Megabytes(static_cast<uint64_t>(total));
  }

  return None();
}


Option<Value::Ranges> Resources::ports() const
{
  Value::Ranges total;
  bool found = false;

  foreach (const Resource& resource, resources) {
    if (resource.name() == "ports" &&
        resource.type() == Value::RANGES) {
      total += resource.ranges();
      found = true;
    }
  }

  if (found) {
    return total;
  }

  return None();
}


Try<Resource> Resources::parse(
    const string& name,
    const string& text,
    const string& role)
{
  Resource resource;
  Try<Value> result = internal::values::parse(text);

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


bool Resources::isValid(const Resource& resource)
{
  if (!resource.has_name() ||
      resource.name() == "" ||
      !resource.has_type() ||
      !Value::Type_IsValid(resource.type())) {
    return false;
  }

  if (resource.type() == Value::SCALAR) {
    return resource.has_scalar();
  } else if (resource.type() == Value::RANGES) {
    return resource.has_ranges();
  } else if (resource.type() == Value::SET) {
    return resource.has_set();
  } else if (resource.type() == Value::TEXT) {
    // Resources doesn't support text.
    return false;
  }

  return false;
}


bool Resources::isAllocatable(const Resource& resource)
{
  if (isValid(resource)) {
    if (resource.type() == Value::SCALAR) {
      if (resource.scalar().value() <= 0) {
        return false;
      }
    } else if (resource.type() == Value::RANGES) {
      if (resource.ranges().range_size() == 0) {
        return false;
      } else {
        for (int i = 0; i < resource.ranges().range_size(); i++) {
          const Value::Range& range = resource.ranges().range(i);

          // Ensure the range make sense (isn't inverted).
          if (range.begin() > range.end()) {
            return false;
          }

          // Ensure ranges don't overlap (but not necessarily coalesced).
          for (int j = i + 1; j < resource.ranges().range_size(); j++) {
            if (range.begin() <= resource.ranges().range(j).begin() &&
                resource.ranges().range(j).begin() <= range.end()) {
              return false;
            }
          }
        }
      }
    } else if (resource.type() == Value::SET) {
      if (resource.set().item_size() == 0) {
        return false;
      } else {
        for (int i = 0; i < resource.set().item_size(); i++) {
          const std::string& item = resource.set().item(i);

          // Ensure no duplicates.
          for (int j = i + 1; j < resource.set().item_size(); j++) {
            if (item == resource.set().item(j)) {
              return false;
            }
          }
        }
      }
    }

    return true;
  }

  return false;
}


bool Resources::isZero(const Resource& resource)
{
  if (resource.type() == Value::SCALAR) {
    return resource.scalar().value() == 0;
  } else if (resource.type() == Value::RANGES) {
    return resource.ranges().range_size() == 0;
  } else if (resource.type() == Value::SET) {
    return resource.set().item_size() == 0;
  }

  return false;
}

} // namespace mesos {
