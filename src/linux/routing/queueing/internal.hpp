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

#ifndef __LINUX_ROUTING_QUEUEING_INTERNAL_HPP__
#define __LINUX_ROUTING_QUEUEING_INTERNAL_HPP__

#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/object.h>
#include <netlink/socket.h>

#include <netlink/route/link.h>
#include <netlink/route/qdisc.h>
#include <netlink/route/tc.h>

#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "linux/routing/internal.hpp"

#include "linux/routing/link/internal.hpp"

#include "linux/routing/queueing/handle.hpp"

namespace routing {
namespace queueing {
namespace internal {

/////////////////////////////////////////////////
// Helpers for {en}decoding.
/////////////////////////////////////////////////

// Forward declaration. Each type of queueing discipline needs to
// implement this function to encode itself into the libnl queueing
// discipline (rtnl_qdisc).
template <typename Discipline>
Try<Nothing> encode(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const Discipline& discipline);


// Forward declaration. Each type of queueing discipline needs to
// implement this function to decode itself from the libnl queueing
// discipline (rtnl_qdisc). Returns None if the libnl queueing
// discipline does not match the specified queueing discipline type.
template <typename Discipline>
Result<Discipline> decode(const Netlink<struct rtnl_qdisc>& qdisc);


// Encodes a queueing discipline (in our representation) to a libnl
// queueing discipline (rtnl_qdisc). We use template here so that it
// works for any type of queueing discipline.
template <typename Discipline>
Try<Netlink<struct rtnl_qdisc>> encode(
    const Netlink<struct rtnl_link>& link,
    const Discipline& discipline)
{
  struct rtnl_qdisc* q = rtnl_qdisc_alloc();
  if (q == NULL) {
    return Error("Failed to allocate a libnl qdisc");
  }

  Netlink<struct rtnl_qdisc> qdisc(q);

  rtnl_tc_set_link(TC_CAST(qdisc.get()), link.get());

  // Perform queue discipline specific encoding.
  Try<Nothing> encoding = encode(qdisc, discipline);
  if (encoding.isError()) {
    return Error(
        "Failed to encode the queueing discipline: " +
        encoding.error());
  }

  return qdisc;
}

/////////////////////////////////////////////////
// Helpers for internal APIs.
/////////////////////////////////////////////////

// Returns all the libnl queue discipline (rtnl_qdisc) on the link.
inline Try<std::vector<Netlink<struct rtnl_qdisc>>> getQdiscs(
    const Netlink<struct rtnl_link>& link)
{
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // Dump all the queueing discipline from kernel.
  struct nl_cache* c = NULL;
  int error = rtnl_qdisc_alloc_cache(socket.get().get(), &c);
  if (error != 0) {
    return Error(
        "Failed to get queueing discipline info from kernel: " +
        std::string(nl_geterror(error)));
  }

  Netlink<struct nl_cache> cache(c);

  std::vector<Netlink<struct rtnl_qdisc>> results;

  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != NULL; o = nl_cache_get_next(o)) {
    nl_object_get(o); // Increment the reference counter.
    results.push_back(Netlink<struct rtnl_qdisc>((struct rtnl_qdisc*) o));
  }

  return results;
}


// Returns the libnl queueing discipline (rtnl_qdisc) that matches the
// specified queueing discipline on the link. Return None if no match
// has been found. We use template here so that it works for any type
// of queueing discipline.
template <typename Discipline>
Result<Netlink<struct rtnl_qdisc>> getQdisc(
    const Netlink<struct rtnl_link>& link,
    const Discipline& discipline)
{
  Try<std::vector<Netlink<struct rtnl_qdisc>>> qdiscs = getQdiscs(link);
  if (qdiscs.isError()) {
    return Error(qdiscs.error());
  }

  foreach (const Netlink<struct rtnl_qdisc>& qdisc, qdiscs.get()) {
    // The decode function will return None if 'qdisc' does not match
    // the specified queueing discipline. In that case, we just move
    // on to the next libnl queueing discipline.
    Result<Discipline> result = decode<Discipline>(qdisc);
    if (result.isError()) {
      return Error("Failed to decode: " + result.error());
    } else if (result.isSome() && result.get() == discipline) {
      return qdisc;
    }
  }

  return None();
}

/////////////////////////////////////////////////
// Internal queueing APIs.
/////////////////////////////////////////////////

// Returns true if the specified queueing discipline exists on the
// link. We use template here so that it works for any type of
// queueing discipline.
template <typename Discipline>
Try<bool> exists(const std::string& _link, const Discipline& discipline)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_qdisc>> qdisc = getQdisc(link.get(), discipline);
  if (qdisc.isError()) {
    return Error(qdisc.error());
  }
  return qdisc.isSome();
}


// Creates a new queueing discipline on the link. Returns false if the
// same queueing discipline already exists on the link. We use
// template here so that it works for any type of queueing discipline.
template <typename Discipline>
Try<bool> create(const std::string& _link, const Discipline& discipline)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + _link + "' is not found");
  }

  Try<Netlink<struct rtnl_qdisc>> qdisc = encode(link.get(), discipline);
  if (qdisc.isError()) {
    return Error("Failed to encode the queueing discipline: " + qdisc.error());
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // The flag NLM_F_EXCL tells libnl that if the qdisc already exists,
  // this function should return error.
  int error = rtnl_qdisc_add(
      socket.get().get(),
      qdisc.get().get(),
      NLM_F_CREATE | NLM_F_EXCL);

  if (error != 0) {
    if (error == -NLE_EXIST) {
      return false;
    }
    return Error(
        "Failed to add a queueing discipline to the link: " +
        std::string(nl_geterror(error)));
  }

  return true;
}


// Removes the specified queueing discipline on the link. Return false
// if the queueing discipline is not found. We use template here so
// that it works for any type of queueing discipline.
template <typename Discipline>
Try<bool> remove(const std::string& _link, const Discipline& discipline)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_qdisc>> qdisc = getQdisc(link.get(), discipline);
  if (qdisc.isError()) {
    return Error(qdisc.error());
  } else if (qdisc.isNone()) {
    return false;
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_qdisc_delete(socket.get().get(), qdisc.get().get());
  if (error != 0) {
    // TODO(jieyu): Interpret the error code and return false if it
    // indicates that the queueing discipline is not found.
    return Error(std::string(nl_geterror(error)));
  }

  return true;
}

} // namespace internal {
} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_INTERNAL_HPP__
