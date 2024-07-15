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
#include <stout/mac.hpp>
#include <stout/none.hpp>

namespace routing {
namespace link {
namespace veth {

// Creates a pair of virtual network links. The peer link is put in
// the network namespace represented by 'pid' upon creation if
// specified. If 'pid' is None, the peer link will be put into
// caller's network namespace. Returns false if the virtual network
// links (with the same name) already exist.
//
// We provide the ability to assign a specified MAC to the host network
// namespace veth device on creation via veth_mac:
//
// In systems with systemd version above 242, there is a potential data race
// where udev will try to update the MAC address of the device at the same
// time as us if the systemd's MacAddressPolicy is set to 'persistent'.
// To prevent udev from trying to set the veth device's MAC address by itself,
// we *must* set the device MAC address on creation so that addr_assign_type
// will be set to NET_ADDR_SET, which prevents udev from attempting to
// change the MAC address of the veth device.
// see: https://github.com/torvalds/linux/commit/2afb9b533423a9b97f84181e773cf9361d98fed6
// see: https://lore.kernel.org/netdev/CAHXsExy8LKzocBdBzss_vjOpc_TQmyzM87KC192HpmuhMcqasg@mail.gmail.com/T/
// see: https://issues.apache.org/jira/browse/MESOS-10243
Try<bool> create(
    const std::string& veth,
    const std::string& peer,
    const Option<pid_t>& pid = None(),
    const Option<net::MAC>& veth_mac = None(),
    const Option<net::MAC>& peer_mac = None());

} // namespace veth {
} // namespace link {
} // namespace routing {

#endif // __LINUX_ROUTING_LINK_VETH_HPP__
