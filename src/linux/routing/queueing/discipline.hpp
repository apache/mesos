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

#ifndef __LINUX_ROUTING_QUEUEING_DISCIPLINE_HPP__
#define __LINUX_ROUTING_QUEUEING_DISCIPLINE_HPP__

#include <string>

#include <stout/option.hpp>

#include "linux/routing/handle.hpp"

namespace routing {
namespace queueing {

template <typename Config>
class Discipline
{
public:
  Discipline(
      const std::string& _kind,
      const Handle& _parent,
      const Option<Handle>& _handle,
      const Config& _config)
    : kind_(_kind),
      parent_(_parent),
      handle_(_handle),
      config_(_config) {}

  const std::string& kind() const { return kind_; }
  const Handle& parent() const { return parent_; }
  const Option<Handle>& handle() const { return handle_; }
  const Config& config() const { return config_; }

private:
  std::string kind_;
  Handle parent_;
  Option<Handle> handle_;

  // TODO(jieyu): Consider making it optional.
  Config config_;
};

} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_DISCIPLINE_HPP__
