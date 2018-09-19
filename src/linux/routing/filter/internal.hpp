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

#ifndef __LINUX_ROUTING_FILTER_INTERNAL_HPP__
#define __LINUX_ROUTING_FILTER_INTERNAL_HPP__

#include <stdint.h>

#include <netlink/cache.h>
#include <netlink/errno.h>
#include <netlink/object.h>
#include <netlink/socket.h>

#include <netlink/route/classifier.h>
#include <netlink/route/link.h>
#include <netlink/route/tc.h>

#include <netlink/route/act/mirred.h>

#include <netlink/route/cls/basic.h>
#include <netlink/route/cls/u32.h>

#include <string>
#include <vector>

#include <process/shared.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/hashset.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "linux/routing/handle.hpp"
#include "linux/routing/internal.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/handle.hpp"
#include "linux/routing/filter/priority.hpp"

#include "linux/routing/link/internal.hpp"

namespace routing {

template <>
inline void cleanup(struct rtnl_cls* cls)
{
  rtnl_cls_put(cls);
}


template <>
inline void cleanup(struct rtnl_act* act)
{
  rtnl_act_put(act);
}

namespace filter {
namespace internal {

/////////////////////////////////////////////////
// Helpers for {en}decoding.
/////////////////////////////////////////////////

// Forward declaration. Each type of classifier needs to implement
// this function to encode itself into the libnl filter (rtnl_cls).
template <typename Classifier>
Try<Nothing> encode(
    const Netlink<struct rtnl_cls>& cls,
    const Classifier& classifier);


// Forward declaration. Each type of classifier needs to implement
// this function to decode itself from the libnl filter (rtnl_cls).
// Returns None if the libnl filter does not match the type of the
// classifier.
template <typename Classifier>
Result<Classifier> decode(const Netlink<struct rtnl_cls>& cls);


// Attaches a redirect action to the libnl filter (rtnl_cls).
inline Try<Nothing> attach(
    const Netlink<struct rtnl_cls>& cls,
    const action::Redirect& redirect)
{
  Result<Netlink<struct rtnl_link>> link =
    link::internal::get(redirect.link);

  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + redirect.link + "' is not found");
  }

  Netlink<struct rtnl_act> act(rtnl_act_alloc());
  if (act.get() == nullptr) {
    return Error("Failed to allocate a libnl action (rtnl_act)");
  }

  // Set the kind of the action to 'mirred'. The kind 'mirred' stands
  // for mirror or redirect actions.
  int error = rtnl_tc_set_kind(TC_CAST(act.get()), "mirred");
  if (error != 0) {
    return Error(
        "Failed to set the kind of the action: " +
        std::string(nl_geterror(error)));
  }

  rtnl_mirred_set_ifindex(act.get(), rtnl_link_get_ifindex(link->get()));
  rtnl_mirred_set_action(act.get(), TCA_EGRESS_REDIR);
  rtnl_mirred_set_policy(act.get(), TC_ACT_STOLEN);

  const std::string kind = rtnl_tc_get_kind(TC_CAST(cls.get()));
  if (kind == "basic") {
    error = rtnl_basic_add_action(cls.get(), act.get());
    if (error != 0) {
      return Error(std::string(nl_geterror(error)));
    }
  } else if (kind == "u32") {
    error = rtnl_u32_add_action(cls.get(), act.get());
    if (error != 0) {
      return Error(std::string(nl_geterror(error)));
    }

    // Automatically set the 'terminal' flag for u32 filters if a
    // redirect action is attached.
    error = rtnl_u32_set_cls_terminal(cls.get());
    if (error != 0) {
      return Error(
          "Failed to set the terminal flag: " +
          std::string(nl_geterror(error)));
    }
  } else {
    return Error("Unsupported classifier kind: " + kind);
  }

  return Nothing();
}


// Attaches a mirror action to the libnl filter (rtnl_cls).
inline Try<Nothing> attach(
    const Netlink<struct rtnl_cls>& cls,
    const action::Mirror& mirror)
{
  const std::string kind = rtnl_tc_get_kind(TC_CAST(cls.get()));

  foreach (const std::string& _link, mirror.links) {
    Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
    if (link.isError()) {
      return Error(link.error());
    } else if (link.isNone()) {
      return Error("Link '" + _link + "' is not found");
    }

    Netlink<struct rtnl_act> act(rtnl_act_alloc());
    if (act.get() == nullptr) {
      return Error("Failed to allocate a libnl action (rtnl_act)");
    }

    int error = rtnl_tc_set_kind(TC_CAST(act.get()), "mirred");
    if (error != 0) {
      return Error(
          "Failed to set the kind of the action: " +
          std::string(nl_geterror(error)));
    }

    rtnl_mirred_set_ifindex(act.get(), rtnl_link_get_ifindex(link->get()));
    rtnl_mirred_set_action(act.get(), TCA_EGRESS_MIRROR);
    rtnl_mirred_set_policy(act.get(), TC_ACT_PIPE);

    if (kind == "basic") {
      error = rtnl_basic_add_action(cls.get(), act.get());
      if (error != 0) {
        return Error(std::string(nl_geterror(error)));
      }
    } else if (kind == "u32") {
      error = rtnl_u32_add_action(cls.get(), act.get());
      if (error != 0) {
        return Error(std::string(nl_geterror(error)));
      }
    } else {
      return Error("Unsupported classifier kind: " + kind);
    }
  }

  // Automatically set the 'terminal' flag for u32 filters if a mirror
  // action is attached.
  if (kind == "u32") {
    int error = rtnl_u32_set_cls_terminal(cls.get());
    if (error != 0) {
      return Error(
          "Failed to set the terminal flag: " +
          std::string(nl_geterror(error)));
    }
  }

  return Nothing();
}


// Attaches a terminal action to the libnl filter (rtnl_cls). It will
// stop the packet from being passed to the next filter if a match is
// found. This is only applied to u32 filters (which can match any
// 32-bit value in a packet). This function will return error if the
// user tries to attach a terminal action to a non-u32 filter.
inline Try<Nothing> attach(
    const Netlink<struct rtnl_cls>& cls,
    const action::Terminal& terminal)
{
  const std::string kind = rtnl_tc_get_kind(TC_CAST(cls.get()));
  if (kind != "u32") {
    return Error("Cannot attach terminal action to a non-u32 filter.");
  }

  int error = rtnl_u32_set_cls_terminal(cls.get());
  if (error != 0) {
    return Error(
        "Failed to set the terminal flag: " +
        std::string(nl_geterror(error)));
  }

  return Nothing();
}


// Attaches an action to the libnl filter (rtnl_cls). This function
// essentially delegates the call to the corresponding attach function
// depending on the type of the action.
inline Try<Nothing> attach(
    const Netlink<struct rtnl_cls>& cls,
    const process::Shared<action::Action>& action)
{
  const action::Redirect* redirect =
    dynamic_cast<const action::Redirect*>(action.get());
  if (redirect != nullptr) {
    return attach(cls, *redirect);
  }

  const action::Mirror* mirror =
    dynamic_cast<const action::Mirror*>(action.get());
  if (mirror != nullptr) {
    return attach(cls, *mirror);
  }

  const action::Terminal* terminal =
    dynamic_cast<const action::Terminal*>(action.get());
  if (terminal != nullptr) {
    return attach(cls, *terminal);
  }

  return Error("Unsupported action type");
}


// Generates the handle for the given filter on the link. Returns none
// if we decide to let the kernel choose the handle.
template <typename Classifier>
Result<U32Handle> generateU32Handle(
    const Netlink<struct rtnl_link>& link,
    const Filter<Classifier>& filter)
{
  // If the user does not specify a priority, we have no choice but
  // let the kernel choose the handle because we do not know the
  // 'htid' that is associated with that priority.
  if (filter.priority.isNone()) {
    return None();
  }

  // Scan all the filters attached to the given parent on the link.
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // Dump all the libnl filters (i.e., rtnl_cls) attached to the given
  // parent on the link.
  struct nl_cache* c = nullptr;
  int error = rtnl_cls_alloc_cache(
      socket->get(),
      rtnl_link_get_ifindex(link.get()),
      filter.parent.get(),
      &c);

  if (error != 0) {
    return Error(
        "Failed to get filter info from kernel: " +
        std::string(nl_geterror(error)));
  }

  Netlink<struct nl_cache> cache(c);

  // A map from priority to the corresponding 'htid'.
  hashmap<uint16_t, uint32_t> htids;

  // A map from 'htid' to a set of already used nodes.
  hashmap<uint32_t, hashset<uint32_t>> nodes;

  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != nullptr; o = nl_cache_get_next(o)) {
    struct rtnl_cls* cls = (struct rtnl_cls*) o;

    // Only look at u32 filters. For other type of filters, their
    // handles are generated by the kernel correctly.
    if (rtnl_tc_get_kind(TC_CAST(cls)) == std::string("u32")) {
      U32Handle handle(rtnl_tc_get_handle(TC_CAST(cls)));

      htids[rtnl_cls_get_prio(cls)] = handle.htid();
      nodes[handle.htid()].insert(handle.node());
    }
  }

  // If this filter has a new priority, we need to let the kernel
  // decide the handle because we don't know which 'htid' this
  // priority will be associated with.
  if (!htids.contains(filter.priority->get())) {
    return None();
  }

  // NOTE: By default, kernel will choose to use divisor 1, which
  // means all filters will be in hash bucket 0. Also, kernel assigns
  // node id starting from 0x800 by default. Here, we keep the same
  // semantics as kernel.
  uint32_t htid = htids[filter.priority->get()];
  for (uint32_t node = 0x800; node <= 0xfff; node++) {
    if (!nodes[htid].contains(node)) {
      return U32Handle(htid, 0x0, node);
    }
  }

  return Error("No available handle exists");
}


// Encodes a filter (in our representation) to a libnl filter
// (rtnl_cls). We use template here so that it works for any type of
// classifier.
template <typename Classifier>
Try<Netlink<struct rtnl_cls>> encodeFilter(
    const Netlink<struct rtnl_link>& link,
    const Filter<Classifier>& filter)
{
  struct rtnl_cls* c = rtnl_cls_alloc();
  if (c == nullptr) {
    return Error("Failed to allocate a libnl filter (rtnl_cls)");
  }

  Netlink<struct rtnl_cls> cls(c);

  rtnl_tc_set_link(TC_CAST(cls.get()), link.get());
  rtnl_tc_set_parent(TC_CAST(cls.get()), filter.parent.get());

  // Encode the priority.
  if (filter.priority.isSome()) {
    rtnl_cls_set_prio(cls.get(), filter.priority->get());
  }

  // Encode the classifier using the classifier specific function.
  Try<Nothing> encoding = encode(cls, filter.classifier);
  if (encoding.isError()) {
    return Error("Failed to encode the classifier " + encoding.error());
  }

  // Attach actions to the libnl filter.
  foreach (const process::Shared<action::Action>& action, filter.actions) {
    Try<Nothing> attaching = attach(cls, action);
    if (attaching.isError()) {
      return Error("Failed to attach an action " + attaching.error());
    }
  }

  // Encode the handle.
  if (filter.handle.isSome()) {
    rtnl_tc_set_handle(TC_CAST(cls.get()), filter.handle->get());
  } else {
    // NOTE: This is a workaround for MESOS-1617. Normally, if the
    // user does not specify the handle for a filter, the kernel will
    // generate one automatically. However, for u32 filters, the
    // existing kernel is buggy in the sense that it will generate a
    // handle that is already used by some other u32 filter (see the
    // ticket for details). To address that, we explicitly set the
    // handle of the filter by picking an unused handle.
    // TODO(jieyu): Revisit this once the kernel bug is fixed.
    if (rtnl_tc_get_kind(TC_CAST(cls.get())) == std::string("u32")) {
      Result<U32Handle> handle = generateU32Handle(link, filter);
      if (handle.isError()) {
        return Error("Failed to find an unused u32 handle: " + handle.error());
      }

      // If 'handle' is none, let the kernel choose the handle.
      if (handle.isSome()) {
        rtnl_tc_set_handle(TC_CAST(cls.get()), handle->get());
      }
    }
  }

  // Set the classid if needed.
  if (filter.classid.isSome()) {
    if (rtnl_tc_get_kind(TC_CAST(cls.get())) == std::string("u32")) {
      rtnl_u32_set_classid(cls.get(), filter.classid->get());
    } else if (rtnl_tc_get_kind(TC_CAST(cls.get())) == std::string("basic")) {
      rtnl_basic_set_target(cls.get(), filter.classid->get());
    }
  }

  return cls;
}


// Decodes a libnl filter (rtnl_cls) to our filter representation.
// Returns None if the libnl filter does not match the specified
// classifier type. We use template here so that it works for any type
// of classifier.
template <typename Classifier>
Result<Filter<Classifier>> decodeFilter(const Netlink<struct rtnl_cls>& cls)
{
  // If the handle of the libnl filer is 0, it means that it is an
  // internal filter, therefore is definitely not created by us.
  if (rtnl_tc_get_handle(TC_CAST(cls.get())) == 0) {
    return None();
  }

  // Decode the parent.
  Handle parent(rtnl_tc_get_parent(TC_CAST(cls.get())));

  // Decode the priority. If the priority is not specified by the
  // user, kernel will assign a priority to the filter. So we should
  // always have a valid priority here.
  Priority priority(rtnl_cls_get_prio(cls.get()));

  // Decode the handle. If the handle is not specified by the user,
  // kernel will assign a handle to the filter. So we should always
  // have a valid handle here.
  Handle handle(rtnl_tc_get_handle(TC_CAST(cls.get())));

  // Decode the classifier using a classifier specific function.
  Result<Classifier> classifier = decode<Classifier>(cls);
  if (classifier.isError()) {
    return Error("Failed to decode the classifier: " + classifier.error());
  } else if (classifier.isNone()) {
    return None();
  }

  Option<Handle> classid;
  if (rtnl_tc_get_kind(TC_CAST(cls.get())) == std::string("u32")) {
    uint32_t _classid;
    if (rtnl_u32_get_classid(cls.get(), &_classid) == 0) {
      classid = _classid;
    }
  } else if (rtnl_tc_get_kind(TC_CAST(cls.get())) == std::string("basic")) {
    classid = rtnl_basic_get_target(cls.get());
  }

  // TODO(jieyu): Decode all the actions attached to the filter.
  // Currently, libnl does not support that (but will support that in
  // the future).
  return Filter<Classifier>(parent,
                            classifier.get(),
                            priority,
                            handle,
                            classid);
}

/////////////////////////////////////////////////
// Helpers for internal APIs.
/////////////////////////////////////////////////

// Returns all the libnl filters (rtnl_cls) attached to the given
// parent on the link.
inline Try<std::vector<Netlink<struct rtnl_cls>>> getClses(
    const Netlink<struct rtnl_link>& link,
    const Handle& parent)
{
  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  // Dump all the libnl filters (i.e., rtnl_cls) attached to the given
  // parent on the link.
  struct nl_cache* c = nullptr;
  int error = rtnl_cls_alloc_cache(
      socket->get(),
      rtnl_link_get_ifindex(link.get()),
      parent.get(),
      &c);

  if (error != 0) {
    return Error(
        "Failed to get filter info from kernel: " +
        std::string(nl_geterror(error)));
  }

  Netlink<struct nl_cache> cache(c);

  std::vector<Netlink<struct rtnl_cls>> results;

  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != nullptr; o = nl_cache_get_next(o)) {
    // NOTE: We increment the reference counter here because 'cache'
    // will be freed when this function finishes and we want this
    // object's life to be longer than this function.
    nl_object_get(o);

    results.push_back(Netlink<struct rtnl_cls>((struct rtnl_cls*) o));
  }

  return results;
}


// Returns the libnl filter (rtnl_cls) attached to the given parent
// that matches the specified classifier on the link. Returns None if
// no match has been found. We use template here so that it works for
// any type of classifier.
template <typename Classifier>
Result<Netlink<struct rtnl_cls>> getCls(
    const Netlink<struct rtnl_link>& link,
    const Handle& parent,
    const Classifier& classifier)
{
  Try<std::vector<Netlink<struct rtnl_cls>>> clses = getClses(link, parent);
  if (clses.isError()) {
    return Error(clses.error());
  }

  foreach (const Netlink<struct rtnl_cls>& cls, clses.get()) {
    // The decode function will return None if 'cls' does not match
    // the classifier type. In that case, we just move on to the next
    // libnl filter.
    Result<Filter<Classifier>> filter = decodeFilter<Classifier>(cls);
    if (filter.isError()) {
      return Error("Failed to decode: " + filter.error());
    } else if (filter.isSome() && filter->classifier == classifier) {
      return cls;
    }
  }

  return None();
}

/////////////////////////////////////////////////
// Internal filter APIs.
/////////////////////////////////////////////////

// Returns true if there exists a filter attached to the given parent
// that matches the specified classifier on the link. We use template
// here so that it works for any type of classifier.
template <typename Classifier>
Try<bool> exists(
    const std::string& _link,
    const Handle& parent,
    const Classifier& classifier)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_cls>> cls =
    getCls(link.get(), parent, classifier);

  if (cls.isError()) {
    return Error(cls.error());
  }
  return cls.isSome();
}


// Creates a new filter on the link. Returns false if a filter
// attached to the same parent with the same classifier already
// exists. We use template here so that it works for any type of
// classifier.
template <typename Classifier>
Try<bool> create(const std::string& _link, const Filter<Classifier>& filter)
{
  // TODO(jieyu): Currently, we're not able to guarantee the atomicity
  // between the existence check and the following add operation. So
  // if two threads try to create the same filter, both of them may
  // succeed and end up with two filters in the kernel.
  Try<bool> _exists = exists(_link, filter.parent, filter.classifier);
  if (_exists.isError()) {
    return Error("Check filter existence failed: " + _exists.error());
  } else if (_exists.get()) {
    // The filter already exists.
    return false;
  }

  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + _link + "' is not found");
  }

  Try<Netlink<struct rtnl_cls>> cls = encodeFilter(link.get(), filter);
  if (cls.isError()) {
    return Error("Failed to encode the filter: " + cls.error());
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_cls_add(
      socket->get(),
      cls->get(),
      NLM_F_CREATE | NLM_F_EXCL);

  if (error != 0) {
    if (error == -NLE_EXIST) {
      return false;
    } else {
      return Error(std::string(nl_geterror(error)));
    }
  }

  return true;
}


// Removes the filter attached to the given parent that matches the
// specified classifier from the link. Returns false if such a filter
// is not found. We use template here so that it works for any type of
// classifier.
template <typename Classifier>
Try<bool> remove(
    const std::string& _link,
    const Handle& parent,
    const Classifier& classifier)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_cls>> cls =
    getCls(link.get(), parent, classifier);

  if (cls.isError()) {
    return Error(cls.error());
  } else if (cls.isNone()) {
    return false;
  }

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_cls_delete(socket->get(), cls.get().get(), 0);
  if (error != 0) {
    // TODO(jieyu): Interpret the error code and return false if it
    // indicates that the filter is not found.
    return Error(std::string(nl_geterror(error)));
  }

  return true;
}


// Updates the action of the filter attached to the given parent that
// matches the specified classifier on the link. Returns false if such
// a filter is not found. We use template here so that it works for
// any type of classifier.
template <typename Classifier>
Try<bool> update(const std::string& _link, const Filter<Classifier>& filter)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  // Get the old libnl classifier (to-be-updated) from kernel.
  Result<Netlink<struct rtnl_cls>> oldCls =
    getCls(link.get(), filter.parent, filter.classifier);

  if (oldCls.isError()) {
    return Error(oldCls.error());
  } else if (oldCls.isNone()) {
    return false;
  }

  // The kernel does not allow us to update the priority. So if the
  // user specifies a priority, we will check to make sure they match.
  if (filter.priority.isSome() &&
      filter.priority->get() != rtnl_cls_get_prio(oldCls.get().get())) {
    return Error(
        "The priorities do not match. The old priority is " +
        stringify(rtnl_cls_get_prio(oldCls->get())) +
        " and the new priority is " +
        stringify(filter.priority->get()));
  }

  // The kernel does not allow us to update the handle. So if the user
  // specifies a handle, we will check to make sure they match.
  if (filter.handle.isSome() &&
      filter.handle->get() !=
        rtnl_tc_get_handle(TC_CAST(oldCls->get()))) {
    return Error(
        "The handles do not match. The old handle is " +
        stringify(rtnl_tc_get_handle(TC_CAST(oldCls->get()))) +
        " and the new handle is " +
        stringify(filter.handle->get()));
  }

  Try<Netlink<struct rtnl_cls>> newCls = encodeFilter(link.get(), filter);
  if (newCls.isError()) {
    return Error("Failed to encode the new filter: " + newCls.error());
  }

  // Set the handle of the new filter to match that of the old one.
  rtnl_tc_set_handle(
      TC_CAST(newCls->get()),
      rtnl_tc_get_handle(TC_CAST(oldCls->get())));

  // Set the priority of the new filter to match that of the old one.
  rtnl_cls_set_prio(
      newCls->get(),
      rtnl_cls_get_prio(oldCls->get()));

  Try<Netlink<struct nl_sock>> socket = routing::socket();
  if (socket.isError()) {
    return Error(socket.error());
  }

  int error = rtnl_cls_change(socket->get(), newCls.get().get(), 0);
  if (error != 0) {
    if (error == -NLE_OBJ_NOTFOUND) {
      return false;
    } else {
      return Error(std::string(nl_geterror(error)));
    }
  }

  return true;
}


// Returns all the filters attached to the given parent on the link.
// Returns None if the link or the parent is not found. We use
// template here so that it works for any type of classifier.
template <typename Classifier>
Result<std::vector<Filter<Classifier>>> filters(
    const std::string& _link,
    const Handle& parent)
{
  Result<Netlink<struct rtnl_link>> link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  Try<std::vector<Netlink<struct rtnl_cls>>> clses =
    getClses(link.get(), parent);

  if (clses.isError()) {
    return Error(clses.error());
  }

  std::vector<Filter<Classifier>> results;

  foreach (const Netlink<struct rtnl_cls>& cls, clses.get()) {
    // The decode function will return None if 'cls' does not match
    // the classifier type. In that case, we just move on to the next
    // libnl filter.
    Result<Filter<Classifier>> filter = decodeFilter<Classifier>(cls);
    if (filter.isError()) {
      return Error(filter.error());
    } else if (filter.isSome()) {
      results.push_back(filter.get());
    }
  }

  return results;
}


// Returns all the classifiers attached to the given parent on the
// link. Returns None if the link or the parent is not found. We use
// template here so that it works for any type of classifier.
template <typename Classifier>
Result<std::vector<Classifier>> classifiers(
    const std::string& link,
    const Handle& parent)
{
  Result<std::vector<Filter<Classifier>>> _filters =
    filters<Classifier>(link, parent);

  if (_filters.isError()) {
    return Error(_filters.error());
  } else if (_filters.isNone()) {
    return None();
  }

  std::vector<Classifier> results;

  foreach (const Filter<Classifier>& filter, _filters.get()) {
    results.push_back(filter.classifier);
  }

  return results;
}

} // namespace internal {
} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_INTERNAL_HPP__
