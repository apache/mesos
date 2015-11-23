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

#ifndef __EXAMPLES_UTILS_HPP__
#define __EXAMPLES_UTILS_HPP__

#include <string>

#include <mesos/mesos.hpp>

namespace mesos {

inline double getScalarResource(const Offer& offer, const std::string& name)
{
  double value = 0.0;

  for (int i = 0; i < offer.resources_size(); i++) {
    const Resource& resource = offer.resources(i);
    if (resource.name() == name &&
        resource.type() == Value::SCALAR) {
      value = resource.scalar().value();
    }
  }

  return value;
}

} // namespace mesos {

#endif // __EXAMPLES_UTILS_HPP__
