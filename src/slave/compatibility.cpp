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

#include "slave/compatibility.hpp"

#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

#include <mesos/attributes.hpp>
#include <mesos/resources.hpp>
#include <mesos/values.hpp>

#include "mesos/type_utils.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace compatibility {

// TODO(bevers): Compare the SlaveInfo fields individually, in order to be
// able to generate better error messages.
Try<Nothing> equal(
    const SlaveInfo& previous,
    const SlaveInfo& current)
{
  if (previous == current) {
    return Nothing();
  }

  return Error(strings::join(
      "\n",
      "Incompatible agent info detected. ",
      "\n------------------------------------------------------------",
      "Old agent info:\n" + stringify(previous),
      "\n------------------------------------------------------------",
      "New agent info:\n" + stringify(current),
      "\n------------------------------------------------------------"));
}


Try<Nothing> additive(
    const SlaveInfo& previous,
    const SlaveInfo& current)
{
  if (previous.hostname() != current.hostname()) {
    return Error(
        "Configuration change not permitted under `additive` policy: "
        "Hostname changed from " +
        previous.hostname() + " to " + current.hostname());
  }

  if (previous.port() != current.port()) {
    return Error(
        "Configuration change not permitted under `additive` policy: "
        "Port changed from " + stringify(previous.port()) + " to " +
        stringify(current.port()));
  }

  if (previous.has_domain() && !(previous.domain() == current.domain())) {
    return Error(
        "Configuration change not permitted under `additive` policy: "
        "Domain changed from " + stringify(previous.domain()) + " to " +
        stringify(current.domain()));
  }

  // As a side effect, the Resources constructor also normalizes all addable
  // resources, e.g. 'cpus:5;cpus:5' -> cpus:10
  Resources previousResources(previous.resources());
  Resources currentResources(current.resources());

  // TODO(bennoe): We should probably check `resources.size()` and switch to a
  // smarter algorithm for the matching when its bigger than, say, 20.
  for (const Resource& resource : previousResources) {
    Option<Resource> match = currentResources.match(resource);

    if (match.isNone()) {
      return Error(
          "Configuration change not permitted under 'additive' policy. "
          "Resource not found: " + stringify(resource));
    }

    switch (resource.type()) {
      case Value::SCALAR: {
        if (!(resource.scalar() <= match->scalar())) {
          return Error(
              "Configuration change not permitted under 'additive' policy: "
              "Value of scalar resource '" + resource.name() + "' decreased "
              "from " + stringify(resource.scalar()) + " to " +
              stringify(match->scalar()));
        }
        continue;
      }
      case Value::RANGES: {
        if (!(resource.ranges() <= match->ranges())) {
          return Error(
              "Configuration change not permitted under 'additive' policy: "
              "Previous value of range resource '" + resource.name() + "' (" +
              stringify(resource.ranges()) + ") not included in current " +
              stringify(match->ranges()));
        }
        continue;
      }
      case Value::SET: {
        if (!(resource.set() <= match->set())) {
          return Error(
              "Configuration change not permitted under 'additive' policy: "
              "Previous value of set resource '" + resource.name() + "' (" +
              stringify(resource.set()) + ") not included in current " +
              stringify(match->set()));
        }
        continue;
      }
      case Value::TEXT: {
        // Text resources are not supported by Mesos.
        UNREACHABLE();
      }
    }
  }

  const google::protobuf::RepeatedPtrField<Attribute>& currentAttributes =
      current.attributes();

  for (const Attribute& attribute : previous.attributes()) {
    auto match = std::find_if(
      currentAttributes.begin(),
      currentAttributes.end(),
      [&attribute](const Attribute& value) {
        return attribute.name() == value.name();
      });

    if (match == currentAttributes.end()) {
      return Error(
          "Configuration change not permitted under 'additive' policy: "
          "Couldn't find attribute " +  stringify(attribute));
    }

    if (match->type() != attribute.type()) {
      return Error(
          "Configuration change not permitted under 'additive' policy: "
          "Type of attribute '" + attribute.name() + "' changed from " +
          stringify(attribute.type()) + " to " + stringify(match->type()));
    }

    switch (attribute.type()) {
      case Value::SCALAR: {
        if (!(attribute.scalar() == match->scalar())) {
          return Error(
              "Configuration change not permitted under 'additive' policy: "
              "Value of scalar attribute '" + attribute.name() + "' changed "
              "from " + stringify(attribute.scalar()) + " to " +
              stringify(match->scalar()));
        }
        continue;
      }
      case Value::RANGES: {
        if (!(attribute.ranges() <= match->ranges())) {
          return Error(
              "Previous value of ranges resource '" + attribute.name() + "' (" +
              stringify(attribute.ranges()) + ") not included in current " +
              stringify(match->ranges()));
        }
        continue;
      }
      case Value::TEXT: {
        if (!(attribute.text() == match->text())) {
          return Error(
              "Configuration change not permitted under 'additive' policy: "
              "Value of text attribute '" + attribute.name() + "' changed "
              "from " + stringify(attribute.text()) +
              " to " + stringify(match->text()));
        }
        continue;
      }
      case Value::SET: {
        // Set attributes are not supported by Mesos.
        UNREACHABLE();
      }
    }
  }

  return Nothing();
}

} // namespace compatibility {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
