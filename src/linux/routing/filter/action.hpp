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

#ifndef __LINUX_ROUTING_FILTER_ACTION_HPP__
#define __LINUX_ROUTING_FILTER_ACTION_HPP__

#include <set>
#include <string>

namespace routing {
namespace action {

// Base class for filter actions.
class Action
{
public:
  virtual ~Action() {}

protected:
  // Hide the default constructor.
  Action() {}
};


// Represents an action that redirects a packet to a given link.
// Currently, kernel only supports redirecting to the egress of a
// given link.
struct Redirect : public Action
{
  explicit Redirect(const std::string& _link)
    : link(_link) {}

  // The link to which the packet will be redirected.
  std::string link;
};


// Represents an action that mirrors a packet to a set of links.
// Currently, kernel only supports mirroring to the egress of each
// link.
struct Mirror : public Action
{
  explicit Mirror(const std::set<std::string>& _links)
    : links(_links) {}

  // The set of links to which the packet will be mirrored.
  std::set<std::string> links;
};


// Represents an action that stops the packet from being sent to the
// next filter.
struct Terminal : public Action {};

} // namespace action {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_ACTION_HPP__
