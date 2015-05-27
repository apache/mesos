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

#include "linux/routing/handle.hpp"

#include "linux/routing/queueing/fq_codel.hpp"
#include "linux/routing/queueing/internal.hpp"

using std::string;

namespace routing {
namespace queueing {

namespace fq_codel {

// TODO(cwang): The fq_codel queueing discipline is not exposed to the
// user because we use all the default parameters currently.
struct Discipline
{
  bool operator == (const Discipline& that) const
  {
    return true;
  }
};

} // namespace fq_codel {

/////////////////////////////////////////////////
// Type specific {en}decoding functions.
/////////////////////////////////////////////////

namespace internal {

// Encodes an fq_codel queueing discipline into the libnl queueing
// discipline 'qdisc'. Each type of queueing discipline needs to
// implement this function.
template <>
Try<Nothing> encode<fq_codel::Discipline>(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const fq_codel::Discipline& discipline)
{
  int error = rtnl_tc_set_kind(TC_CAST(qdisc.get()), "fq_codel");
  if (error != 0) {
    return Error(
        "Failed to set the kind of the queueing discipline: " +
        string(nl_geterror(error)));
  }

  rtnl_tc_set_parent(TC_CAST(qdisc.get()), EGRESS_ROOT().get());
  rtnl_tc_set_handle(TC_CAST(qdisc.get()), fq_codel::HANDLE.get());

  // We don't set fq_codel parameters here, use the default:
  //   limit 10240p
  //   flows 1024
  //   quantum 1514
  //   target 5.0ms
  //   interval 100.0ms
  //   ecn

  return Nothing();
}


// Decodes the fq_codel queue discipline from the libnl queueing
// discipline 'qdisc'. Each type of queueing discipline needs to
// implement this function. Returns None if the libnl queueing
// discipline is not an fq_codel queueing discipline.
template <>
Result<fq_codel::Discipline> decode<fq_codel::Discipline>(
    const Netlink<struct rtnl_qdisc>& qdisc)
{
  if (rtnl_tc_get_kind(TC_CAST(qdisc.get())) != string("fq_codel") ||
      rtnl_tc_get_parent(TC_CAST(qdisc.get())) != EGRESS_ROOT().get() ||
      rtnl_tc_get_handle(TC_CAST(qdisc.get())) != fq_codel::HANDLE.get()) {
    return None();
  }

  return fq_codel::Discipline();
}

} // namespace internal {

/////////////////////////////////////////////////
// Public interfaces.
/////////////////////////////////////////////////

namespace fq_codel {

// NOTE: Root queueing discipline handle has to be X:0, so handle's
// secondary number has to be 0 here. There can be only one root
// queueing discipline on the egress side of a link.
const Handle HANDLE = Handle(0x1, 0);


const int DEFAULT_FLOWS = 1024;


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

} // namespace fq_codel {
} // namespace queueing {
} // namespace routing {
