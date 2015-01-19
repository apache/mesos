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

#include <netlink/errno.h>

#include <netlink/route/qdisc.h>
#include <netlink/route/tc.h>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>

#include "linux/routing/queueing/handle.hpp"
#include "linux/routing/queueing/ingress.hpp"
#include "linux/routing/queueing/internal.hpp"

using std::string;

namespace routing {
namespace queueing {

namespace ingress {

// The ingress queueing discipline is not exposed to the user.
struct Discipline
{
  bool operator == (const Discipline& that) const
  {
    return true;
  }
};

} // namespace ingress {

/////////////////////////////////////////////////
// Type specific {en}decoding functions.
/////////////////////////////////////////////////

namespace internal {

// Encodes an ingress queueing discipline into the libnl queueing
// discipline 'qdisc'. Each type of queueing discipline needs to
// implement this function.
template <>
Try<Nothing> encode<ingress::Discipline>(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const ingress::Discipline& discipline)
{
  int error = rtnl_tc_set_kind(TC_CAST(qdisc.get()), "ingress");
  if (error != 0) {
    return Error(
        "Failed to set the kind of the queueing discipline: " +
        string(nl_geterror(error)));
  }

  rtnl_tc_set_parent(TC_CAST(qdisc.get()), INGRESS_ROOT.get());
  rtnl_tc_set_handle(TC_CAST(qdisc.get()), ingress::HANDLE.get());

  return Nothing();
}


// Decodes the ingress queue discipline from the libnl queueing
// discipline 'qdisc'. Each type of queueing discipline needs to
// implement this function. Returns None if the libnl queueing
// discipline is not an ingress queueing discipline.
template <>
Result<ingress::Discipline> decode<ingress::Discipline>(
    const Netlink<struct rtnl_qdisc>& qdisc)
{
  if (rtnl_tc_get_kind(TC_CAST(qdisc.get())) != string("ingress") ||
      rtnl_tc_get_parent(TC_CAST(qdisc.get())) != INGRESS_ROOT.get() ||
      rtnl_tc_get_handle(TC_CAST(qdisc.get())) != ingress::HANDLE.get()) {
    return None();
  }

  return ingress::Discipline();
}

} // namespace internal {

/////////////////////////////////////////////////
// Public interfaces.
/////////////////////////////////////////////////

namespace ingress {

const Handle HANDLE = Handle(0xffff, 0);


Try<bool> exists(const string& link)
{
  return internal::exists(link, Discipline());
}


Try<bool> create(const string& link)
{
  return internal::create(link, Discipline());
}


Try<bool> remove(const string& link)
{
  return internal::remove(link, Discipline());
}

} // namespace ingress {
} // namespace queueing {
} // namespace routing {
