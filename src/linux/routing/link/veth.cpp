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

#include <unistd.h>

#include <netlink/errno.h>

#include <netlink/route/link/veth.h>

#include <stout/error.hpp>

#include "linux/routing/internal.hpp"

#include "linux/routing/link/veth.hpp"

using std::string;

namespace routing {
namespace link {
namespace veth {

Try<bool> create(
    const string& veth,
    const string& peer,
    const Option<pid_t>& pid)
{
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_link_veth_add(
      socket->get(),
      veth.c_str(),
      peer.c_str(),
      (pid.isNone() ? getpid() : pid.get()));

  if (error != 0) {
    if (error == -NLE_EXIST) {
      return false;
    }
    return Error(nl_geterror(error));
  }

  return true;
}

} // namespace veth {
} // namespace link {
} // namespace routing {
