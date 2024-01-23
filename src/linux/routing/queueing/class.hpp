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

#ifndef __LINUX_ROUTING_QUEUEING_CLASS_HPP__
#define __LINUX_ROUTING_QUEUEING_CLASS_HPP__

#include <string>

#include <netlink/route/class.h>

#include <stout/option.hpp>

#include "linux/routing/handle.hpp"
#include "linux/routing/queueing/internal.hpp"

namespace routing {

template <>
inline void cleanup(struct rtnl_class* cls)
{
  rtnl_class_put(cls);
}

namespace queueing {

template <typename Config>
struct Class
{
  Class(
      const std::string& _kind,
      const Handle& _parent,
      const Option<Handle>& _handle,
      const Config& _config)
    : kind(_kind),
      parent(_parent),
      handle(_handle),
      config(_config) {}

  std::string kind;
  Handle parent;
  Option<Handle> handle;
  Config config;
};

} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_CLASS_HPP__
