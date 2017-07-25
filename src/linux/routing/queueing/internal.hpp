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

#include <netlink/route/class.h>
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

#include "linux/routing/queueing/class.hpp"
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
// Helpers for {en}decoding queueing disciplines.
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
// Helpers for {en}decoding queueing classes.
/////////////////////////////////////////////////

namespace cls {
// Forward declaration. Each type of queueing class needs to implement
// this function to encode its type specific configurations into the
// libnl queueing class (rtnl_class).
template <typename Config>
Try<Nothing> encode(
    const Netlink<struct rtnl_class>& cls,
    const Config& config);


// Forward declaration. Each type of queueing class needs to implement
// this function to decode its type specific configurations from the
// libnl queueing class (rtnl_class). Returns None if the libnl
// queueing class does not match the specific queueing class type.
template <typename Config>
Result<Config> decode(const Netlink<struct rtnl_class>& cls);


// Encodes a queueing class (in our representation) to a libnl
// queueing class (rtnl_class). We use templating here so that it works
// for any type of queueing class.
template <typename Config>
Try<Netlink<struct rtnl_class>> encode(
    const Netlink<struct rtnl_link>& link,
    const Class<Config>& config)
{
  struct rtnl_class* c = rtnl_class_alloc();
  if (c == nullptr) {
    return Error("Failed to allocate a libnl class");
  }

  Netlink<struct rtnl_class> cls(c);

  rtnl_tc_set_link(TC_CAST(cls.get()), link.get());
  rtnl_tc_set_parent(TC_CAST(cls.get()), config.parent.get());

  if (config.handle.isSome()) {
    rtnl_tc_set_handle(TC_CAST(cls.get()), config.handle.get().get());
  }

  int error = rtnl_tc_set_kind(TC_CAST(cls.get()), config.kind.c_str());
  if (error != 0) {
    return Error(
        "Failed to set the kind of the queueing class: " +
        std::string(nl_geterror(error)));
  }

  // Perform queueuing class specific encoding.
  Try<Nothing> encoding = encode(cls, config.config);
  if (encoding.isError()) {
    return Error("Failed to encode the queueing class: " + encoding.error());
  }

  return cls;
}

} // namespace cls {

/////////////////////////////////////////////////
// Helpers for internal APIs for queueing disciplines.
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
// Helpers for internal APIs for queueing classes.
/////////////////////////////////////////////////

namespace cls {

// Returns all the libnl queueing classes (rtnl_class) on the link.
inline Try<std::vector<Netlink<struct rtnl_class>>> getClasses(
    const Netlink<struct rtnl_link>& link)
{
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // Dump all the queuing classes from the kernel.
  struct nl_cache* c = nullptr;
  int error = rtnl_class_alloc_cache(
      socket.get().get(),
      rtnl_link_get_ifindex(link.get()),
      &c);

  if (error != 0) {
    return Error(
        "Failed to get queueing class info from kernel: " +
        std::string(nl_geterror(error)));
  }

  Netlink<struct nl_cache> cache(c);

  std::vector<Netlink<struct rtnl_class>> results;

  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != nullptr;
       o = nl_cache_get_next(o)) {
    if (rtnl_tc_get_ifindex(TC_CAST(o)) == rtnl_link_get_ifindex(link.get())) {
      // NOTE: We increment the reference counter here because
      // 'cache' will be freed when this function finishes and we
      // want this object's lifetime to be longer than this
      // function.
      nl_object_get(o);

      results.push_back(Netlink<struct rtnl_class>((struct rtnl_class*) o));
    }
  }

  return results;
}


// Returns the libnl queueing class (rtnl_class) with the given class
// ID that matches the specified queueing class kind on the link.
// Returns None if no match is found.
inline Result<Netlink<struct rtnl_class>> getClass(
    const Netlink<struct rtnl_link>& link,
    const Handle& classid,
    const std::string& kind)
{
  Try<std::vector<Netlink<struct rtnl_class>>> classes = getClasses(link);
  if (classes.isError()) {
    return Error(classes.error());
  }

  foreach (const Netlink<struct rtnl_class>& cls, classes.get()) {
    if (rtnl_tc_get_handle(TC_CAST(cls.get())) == classid.get() &&
        rtnl_tc_get_kind(TC_CAST(cls.get())) == kind) {
      return cls;
    }
  }

  return None();
}

} // namespace cls {

/////////////////////////////////////////////////
// Internal queueing APIs for queueing disciplines.
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

/////////////////////////////////////////////////
// Internal queueing APIs for queueing classes.
/////////////////////////////////////////////////

namespace cls {

// Returns true if there exists a queueing class attached to the given
// parent that matches the specified queueing class kind on the link.
inline Try<bool> exists(
    const std::string& _link,
    const Handle& classid,
    const std::string& kind)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_class>> cls = getClass(link.get(), classid, kind);
  if (cls.isError()) {
    return Error(cls.error());
  }

  return cls.isSome();
}


// Creates a new queueing class on the link. Returns false if
// a queueing class attached to the same parent with the same
// configuration already exists on the link. We use template here so
// that it works for any type of queueing class.
template <typename Config>
Try<bool> create(
    const std::string& _link,
    const Class<Config>& config)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + _link + "' was not found");
  }

  Try<Netlink<struct rtnl_class>> cls = encode(link.get(), config);
  if (cls.isError()) {
    return Error("Failed to encode the queueing class: " + cls.error());
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // The flag NLM_F_EXCL tells libnl that if the class already exists
  // this function should return error.
  int error = rtnl_class_add(
      socket.get().get(),
      cls.get().get(),
      NLM_F_CREATE | NLM_F_EXCL);

  if (error != 0) {
    if (error == -NLE_EXIST) {
      return false;
    }
    return Error(
        "Failed to add a queueing class to the link: " +
        std::string(nl_geterror(error)));
  }

  return true;
}


// Updates an existing queueing class on the link. Returns false if
// a queueing class attached to the parent does not exist. We use
// template here so that it works for any type of queueing class.
template <typename Config>
Try<bool> update(
    const std::string& _link,
    const Class<Config>& config)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + _link + "' was not found");
  }

  Try<Netlink<struct rtnl_class>> cls = encode(link.get(), config);
  if (cls.isError()) {
    return Error("Failed to encode the queueing class: " + cls.error());
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // No flags are passed so an error will be returned if the class
  // does not exist.
  int error = rtnl_class_add(
      socket.get().get(),
      cls.get().get(),
      0);

  if (error != 0) {
    if (error == -NLE_OBJ_NOTFOUND) {
      return false;
    }
    return Error(
        "Failed to add a queueing class to the link: " +
        std::string(nl_geterror(error)));
  }

  return true;
}

// Returns the class config for a queueing class on the link. Returns
// None() if the link or class does not exist or an error if the
// result cannot be determined.
template <typename Config>
Result<Config> getConfig(
    const std::string& _link,
    const Handle& classid,
    const std::string& kind)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  Result<Netlink<struct rtnl_class>> cls = getClass(link.get(), classid, kind);
  if (cls.isError()) {
    return Error(cls.error());
  } else if (cls.isNone()) {
    return None();
  }

  return decode<Config>(cls.get());
}


// Removes the specified queueing class with the given class ID that
// matches the specified queueing class kind on the link. Returns
// false if such a queueing class is not found.
inline Try<bool> remove(
    const std::string& _link,
    const Handle& classid,
    const std::string& kind)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_class>> cls = getClass(link.get(), classid, kind);
  if (cls.isError()) {
    return Error(cls.error());
  } else if (cls.isNone()) {
    return false;
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_class_delete(socket.get().get(), cls.get().get());
  if (error != 0) {
    return Error(std::string(nl_geterror(error)));
  }

  return true;
}

} // namespace cls {

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
