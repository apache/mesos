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

#ifndef __LINUX_ROUTING_LINK_VETH_HPP__
#define __LINUX_ROUTING_LINK_VETH_HPP__

#include <sys/types.h>

#include <string>

#include <stout/option.hpp>
#include <stout/try.hpp>

namespace routing {
namespace link {
namespace veth {

// Creates a pair of virtual network links. The peer link is put in
// the network namespace represented by 'pid' upon creation if
// specified. If 'pid' is None, the peer link will be put into
// caller's network namespace. Returns false if the virtual network
// links (with the same name) already exist.
Try<bool> create(
    const std::string& veth,
    const std::string& peer,
    const Option<pid_t>& pid);

} // namespace veth {
} // namespace link {
} // namespace routing {

#endif // __LINUX_ROUTING_LINK_VETH_HPP__
