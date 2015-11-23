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

#include <linux/sock_diag.h>

#include <netlink/utils.h>

#include "linux/routing/utils.hpp"

namespace routing {

Try<Nothing> check()
{
  // As advised in libnl, we use numeric values, instead of defined
  // macros (which creates compile time dependency), to check
  // capabilities.

  // Check NL_CAPABILITY_ROUTE_LINK_VETH_GET_PEER_OWN_REFERENCE.
  if (nl_has_capability(2) == 0) {
    return Error(
        "Capability ROUTE_LINK_VETH_GET_PEER_OWN_REFERENCE is not available");
  }

  // Check NL_CAPABILITY_ROUTE_LINK_CLS_ADD_ACT_OWN_REFERENCE.
  if (nl_has_capability(3) == 0) {
    return Error(
        "Capability ROUTE_LINK_CLS_ADD_ACT_OWN_REFERENCE is not available");
  }

  // There is a bug in libnl3-idiag from duplicating a kernel header
  // definition and running certain check logic based on it. The
  // kernel header it copies is from 3.6. Older kernels have a
  // different definition and therefore return error on certain calls.
  // TODO(chzhcn): remove this once the bug in libnl3-idiag is fixed.
  if (SK_MEMINFO_VARS == 7) {
    return Error(
        "libnl3-idiag module has a known bug on kernels older than 3.6");
  }

  return Nothing();
}

} // namespace routing {
