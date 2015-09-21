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
#include "slave/containerizer/provisioner/docker/message.pb.h"

namespace mesos {
namespace internal {
namespace slave {
namespace docker {

inline Image::Name parseName(const std::string& value)
{
  Image::Name imageName;
  Option<std::string> registry = None();
  std::vector<std::string> components = strings::split(value, "/");
  if (components.size() > 2) {
    imageName.set_registry(value.substr(0, value.find_last_of("/")));
  }

  std::size_t found = components.back().find_last_of(':');
  if (found == std::string::npos) {
    imageName.set_repository(components.back());
    imageName.set_tag("latest");
  } else {
    imageName.set_repository(components.back().substr(0, found));
    imageName.set_tag(components.back().substr(found + 1));
  }

  return imageName;
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
