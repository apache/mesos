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
* limitations under the License
*/

#ifndef __ZOOKEEPER_AUTHENTICATION_HPP__
#define __ZOOKEEPER_AUTHENTICATION_HPP__

#include <zookeeper.h>

#include <string>

#include "logging/logging.hpp"

namespace zookeeper {

struct Authentication
{
  Authentication(
      const std::string& _scheme,
      const std::string& _credentials)
    : scheme(_scheme),
      credentials(_credentials)
  {
    // TODO(benh): Fix output operator below once this changes.
    CHECK_EQ(scheme, "digest") << "Unsupported authentication scheme";
  }

  const std::string scheme;
  const std::string credentials;
};


// An ACL that ensures we're the only authenticated user to mutate our
// nodes - others are welcome to read.
extern const ACL_vector EVERYONE_READ_CREATOR_ALL;

// An ACL that allows others to create child nodes and read nodes, but
// we're the only authenticated user to mutate our nodes.
extern const ACL_vector EVERYONE_CREATE_AND_READ_CREATOR_ALL;


inline std::ostream& operator<<(
    std::ostream& stream,
    const Authentication& authentication)
{
  // TODO(benh): Fix this once we support more than just 'digest'.
  CHECK_EQ(authentication.scheme, "digest");
  return stream << authentication.credentials;
}


} // namespace zookeeper {

#endif // __ZOOKEEPER_AUTHENTICATION_HPP__
