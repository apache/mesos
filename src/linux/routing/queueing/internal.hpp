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

#include "linux/routing/handle.hpp"
#include "linux/routing/internal.hpp"

#include "linux/routing/link/internal.hpp"

#include "linux/routing/queueing/discipline.hpp"
#include "linux/routing/queueing/statistics.hpp"

namespace routing {

template <>
inline void cleanup(struct rtnl_qdisc* qdisc)
{
  rtnl_qdisc_put(qdisc);
}

namespace queueing {
namespace internal {

/////////////////////////////////////////////////
// Helpers for {en}decoding.
/////////////////////////////////////////////////

// Forward declaration. Each type of queueing discipline needs to
// implement this function to encode its type specific configurations
// into the libnl queueing discipline (rtnl_qdisc).
template <typename Config>
Try<Nothing> encode(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const Config& config);


// Forward declaration. Each type of queueing discipline needs to
// implement this function to decode its type specific configurations
// from the libnl queueing discipline (rtnl_qdisc). Returns None if
// the libnl queueing discipline does not match the specified queueing
// discipline type.
template <typename Config>
Result<Config> decode(const Netlink<struct rtnl_qdisc>& qdisc);


// Encodes a queueing discipline (in our representation) to a libnl
// queueing discipline (rtnl_qdisc). We use template here so that it
// works for any type of queueing discipline.
template <typename Config>
Try<Netlink<struct rtnl_qdisc>> encodeDiscipline(
    const Netlink<struct rtnl_link>& link,
    const Discipline<Config>& discipline)
{
  struct rtnl_qdisc* q = rtnl_qdisc_alloc();
  if (q == nullptr) {
    return Error("Failed to allocate a libnl qdisc");
  }

  Netlink<struct rtnl_qdisc> qdisc(q);

  rtnl_tc_set_link(TC_CAST(qdisc.get()), link.get());
  rtnl_tc_set_parent(TC_CAST(qdisc.get()), discipline.parent.get());

  if (discipline.handle.isSome()) {
    rtnl_tc_set_handle(TC_CAST(qdisc.get()), discipline.handle->get());
  }

  int error = rtnl_tc_set_kind(TC_CAST(qdisc.get()), discipline.kind.c_str());
  if (error != 0) {
    return Error(
        "Failed to set the kind of the queueing discipline: " +
        std::string(nl_geterror(error)));
  }

  // Perform queue discipline specific encoding.
  Try<Nothing> encoding = encode(qdisc, discipline.config);
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
  struct nl_cache* c = nullptr;
  int error = rtnl_qdisc_alloc_cache(socket->get(), &c);
  if (error != 0) {
    return Error(
        "Failed to get queueing discipline info from kernel: " +
        std::string(nl_geterror(error)));
  }

  Netlink<struct nl_cache> cache(c);

  std::vector<Netlink<struct rtnl_qdisc>> results;

  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != nullptr; o = nl_cache_get_next(o)) {
    if (rtnl_tc_get_ifindex(TC_CAST(o)) == rtnl_link_get_ifindex(link.get())) {
      // NOTE: We increment the reference counter here because 'cache'
      // will be freed when this function finishes and we want this
      // object's life to be longer than this function.
      nl_object_get(o);

      results.push_back(Netlink<struct rtnl_qdisc>((struct rtnl_qdisc*) o));
    }
  }

  return results;
}


// Returns the libnl queueing discipline (rtnl_qdisc) attached to the
// given parent that matches the specified queueing discipline kind on
// the link. Return None if no match has been found.
inline Result<Netlink<struct rtnl_qdisc>> getQdisc(
    const Netlink<struct rtnl_link>& link,
    const Handle& parent,
    const std::string& kind)
{
  Try<std::vector<Netlink<struct rtnl_qdisc>>> qdiscs = getQdiscs(link);
  if (qdiscs.isError()) {
    return Error(qdiscs.error());
  }

  foreach (const Netlink<struct rtnl_qdisc>& qdisc, qdiscs.get()) {
    if (rtnl_tc_get_parent(TC_CAST(qdisc.get())) == parent.get() &&
        rtnl_tc_get_kind(TC_CAST(qdisc.get())) == kind) {
      return qdisc;
    }
  }

  return None();
}

/////////////////////////////////////////////////
// Internal queueing APIs.
/////////////////////////////////////////////////

// Returns true if there exists a queueing discipline attached to the
// given parent that matches the specified queueing discipline kind on
// the link.
inline Try<bool> exists(
    const std::string& _link,
    const Handle& parent,
    const std::string& kind)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_qdisc>> qdisc = getQdisc(link.get(), parent, kind);
  if (qdisc.isError()) {
    return Error(qdisc.error());
  }

  return qdisc.isSome();
}


// Creates a new queueing discipline on the link. Returns false if a
// queueing discipline attached to the same parent with the same
// configuration already exists on the link. We use template here so
// that it works for any type of queueing discipline.
template <typename Config>
Try<bool> create(
    const std::string& _link,
    const Discipline<Config>& discipline)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + _link + "' is not found");
  }

  Try<Netlink<struct rtnl_qdisc>> qdisc =
    encodeDiscipline(link.get(), discipline);

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
      socket->get(),
      qdisc->get(),
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


// Removes the specified discipline attached to the given parent that
// matches the specified queueing discipline kind on the link. Return
// false if such a queueing discipline is not found.
inline Try<bool> remove(
    const std::string& _link,
    const Handle& parent,
    const std::string& kind)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_qdisc>> qdisc = getQdisc(link.get(), parent, kind);
  if (qdisc.isError()) {
    return Error(qdisc.error());
  } else if (qdisc.isNone()) {
    return false;
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_qdisc_delete(socket->get(), qdisc.get().get());
  if (error != 0) {
    // TODO(jieyu): Interpret the error code and return false if it
    // indicates that the queueing discipline is not found.
    return Error(std::string(nl_geterror(error)));
  }

  return true;
}


// Returns the set of common Traffic Control statistics for the
// queueing discipline on the link, None() if the link or qdisc does
// not exist or an error if we cannot cannot determine the result.
inline Result<hashmap<std::string, uint64_t>> statistics(
    const std::string& _link,
    const Handle& parent,
    const std::string& kind)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  Result<Netlink<struct rtnl_qdisc>> qdisc = getQdisc(link.get(), parent, kind);
  if (qdisc.isError()) {
    return Error(qdisc.error());
  } else if (qdisc.isNone()) {
    return None();
  }

  hashmap<std::string, uint64_t> results;
  char name[32];

  // NOTE: We use '<=' here because RTNL_TC_STATS_MAX is set to be the
  // value of the last enum entry.
  for (size_t i = 0; i <= static_cast<size_t>(RTNL_TC_STATS_MAX); i++) {
    if (rtnl_tc_stat2str(static_cast<rtnl_tc_stat>(i), name, sizeof(name))) {
      results[name] = rtnl_tc_get_stat(
          TC_CAST(qdisc->get()),
          static_cast<rtnl_tc_stat>(i));
    }
  }
  return results;
}

} // namespace internal {
} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_INTERNAL_HPP__
