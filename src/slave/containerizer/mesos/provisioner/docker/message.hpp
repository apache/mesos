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

#ifndef __MESSAGES_DOCKER_PROVISIONER_HPP__
#define __MESSAGES_DOCKER_PROVISIONER_HPP__

#include <stout/strings.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include "slave/containerizer/mesos/provisioner/docker/message.pb.h"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

// Docker expects the image to be specified on the command line as:
//   [REGISTRY_HOST[:REGISTRY_PORT]/]REPOSITORY[:TAG|@TYPE:DIGEST]
//
// This format is inherently ambiguous when dealing with repository
// names that include forward slashes. To disambiguate, the docker
// code looks for '.', or ':', or 'localhost' to decide if the
// first component is a registry or a respository name. For more
// detail, drill into the implementation of docker pull.
//
// TODO(bmahler): We currently store the digest as a tag, does
// that makes sense?
//
// TODO(bmahler): Validate based on docker's validation logic
// and return a Try here.
inline Image::Name parseImageName(std::string s)
{
  Image::Name name;

  // Extract the digest.
  if (strings::contains(s, "@")) {
    std::vector<std::string> split = strings::split(s, "@");

    s = split[0];
    name.set_tag(split[1]);
  }

  // Remove the tag. We need to watch out for a
  // host:port registry, which also contains ':'.
  if (strings::contains(s, ":")) {
    std::vector<std::string> split = strings::split(s, ":");

    // The tag must be the last component. If a slash is
    // present there is a registry port and no tag.
    if (!strings::contains(split.back(), "/")) {
      name.set_tag(split.back());
      split.pop_back();

      s = strings::join(":", split);
    }
  }

  // Default to the 'latest' tag when omitted.
  if (name.tag().empty()) {
    name.set_tag("latest");
  }

  // Extract the registry and repository. The first component can
  // either be the registry, or the first part of the repository!
  // We resolve this ambiguity using the same hacks used in the
  // docker code ('.', ':', 'localhost' indicate a registry).
  std::vector<std::string> split = strings::split(s, "/", 2);

  if (split.size() == 1) {
    name.set_repository(s);
  } else if (strings::contains(split[0], ".") ||
             strings::contains(split[0], ":") ||
             split[0] == "localhost") {
    name.set_registry(split[0]);
    name.set_repository(split[1]);
  } else {
    name.set_repository(s);
  }

  return name;
}


inline std::ostream& operator<<(
    std::ostream& stream,
    const Image::Name& name)
{
  if (name.has_registry()) {
    return stream << name.registry() << "/" << name.repository() << ":"
                  << name.tag();
  }

  return stream << name.repository() << ":" << name.tag();
}

} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __MESSAGES_DOCKER_PROVISIONER_HPP__
