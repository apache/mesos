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
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include "linux/routing/internal.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/priority.hpp"

#include "linux/routing/link/internal.hpp"

#include "linux/routing/queueing/handle.hpp"

namespace routing {
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
  Result<Netlink<struct rtnl_link> > link =
    link::internal::get(redirect.link());

  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + redirect.link() + "' is not found");
  }

  // TODO(jieyu): Note that currently, we don't use Netlink for 'act'
  // because libnl has a refcount issue for rtnl_act. Clean this up
  // once the bug is fixed in libnl.
  struct rtnl_act* act = rtnl_act_alloc();
  if (act == NULL) {
    return Error("Failed to allocate a libnl action (rtnl_act)");
  }

  // Set the kind of the action to 'mirred'. The kind 'mirred' stands
  // for mirror or redirect actions.
  int err = rtnl_tc_set_kind(TC_CAST(act), "mirred");
  if (err != 0) {
    rtnl_act_put(act);
    return Error(
        "Failed to set the kind of the action: " +
        std::string(nl_geterror(err)));
  }

  rtnl_mirred_set_ifindex(act, rtnl_link_get_ifindex(link.get().get()));
  rtnl_mirred_set_action(act, TCA_EGRESS_REDIR);
  rtnl_mirred_set_policy(act, TC_ACT_STOLEN);

  const std::string kind = rtnl_tc_get_kind(TC_CAST(cls.get()));
  if (kind == "basic") {
    err = rtnl_basic_add_action(cls.get(), act);
    if (err != 0) {
      rtnl_act_put(act);
      return Error(std::string(nl_geterror(err)));
    }
  } else if (kind == "u32") {
    err = rtnl_u32_add_action(cls.get(), act);
    if (err != 0) {
      rtnl_act_put(act);
      return Error(std::string(nl_geterror(err)));
    }

    // Automatically set the 'terminal' flag for u32 filters if a
    // redirect action is attached.
    err = rtnl_u32_set_cls_terminal(cls.get());
    if (err != 0) {
      return Error(
          "Failed to set the terminal flag: " +
          std::string(nl_geterror(err)));
    }
  } else {
    rtnl_act_put(act);
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

  foreach (const std::string& _link, mirror.links()) {
    Result<Netlink<struct rtnl_link> > link = link::internal::get(_link);
    if (link.isError()) {
      return Error(link.error());
    } else if (link.isNone()) {
      return Error("Link '" + _link + "' is not found");
    }

    // TODO(jieyu): Note that currently, we don't use Netlink for
    // 'act' because libnl has a refcount issue for rtnl_act. Clean
    // this up once libnl fixes the bug.
    struct rtnl_act* act = rtnl_act_alloc();
    if (act == NULL) {
      return Error("Failed to allocate a libnl action (rtnl_act)");
    }

    int err = rtnl_tc_set_kind(TC_CAST(act), "mirred");
    if (err != 0) {
      rtnl_act_put(act);
      return Error(
          "Failed to set the kind of the action: " +
          std::string(nl_geterror(err)));
    }

    rtnl_mirred_set_ifindex(act, rtnl_link_get_ifindex(link.get().get()));
    rtnl_mirred_set_action(act, TCA_EGRESS_MIRROR);
    rtnl_mirred_set_policy(act, TC_ACT_PIPE);

    if (kind == "basic") {
      err = rtnl_basic_add_action(cls.get(), act);
      if (err != 0) {
        rtnl_act_put(act);
        return Error(std::string(nl_geterror(err)));
      }
    } else if (kind == "u32") {
      err = rtnl_u32_add_action(cls.get(), act);
      if (err != 0) {
        rtnl_act_put(act);
        return Error(std::string(nl_geterror(err)));
      }
    } else {
      rtnl_act_put(act);
      return Error("Unsupported classifier kind: " + kind);
    }
  }

  // Automatically set the 'terminal' flag for u32 filters if a mirror
  // action is attached.
  if (kind == "u32") {
    int err = rtnl_u32_set_cls_terminal(cls.get());
    if (err != 0) {
      return Error(
          "Failed to set the terminal flag: " +
          std::string(nl_geterror(err)));
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

  int err = rtnl_u32_set_cls_terminal(cls.get());
  if (err != 0) {
    return Error(
        "Failed to set the terminal flag: " +
        std::string(nl_geterror(err)));
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
  if (redirect != NULL) {
    return attach(cls, *redirect);
  }

  const action::Mirror* mirror =
    dynamic_cast<const action::Mirror*>(action.get());
  if (mirror != NULL) {
    return attach(cls, *mirror);
  }

  const action::Terminal* terminal =
    dynamic_cast<const action::Terminal*>(action.get());
  if (terminal != NULL) {
    return attach(cls, *terminal);
  }

  return Error("Unsupported action type");
}


// Encodes a filter (in our representation) to a libnl filter
// (rtnl_cls). We use template here so that it works for any type of
// classifier.
template <typename Classifier>
Try<Netlink<struct rtnl_cls> > encodeFilter(
    const Netlink<struct rtnl_link>& link,
    const Filter<Classifier>& filter)
{
  struct rtnl_cls* c = rtnl_cls_alloc();
  if (c == NULL) {
    return Error("Failed to allocate a libnl filter (rtnl_cls)");
  }

  Netlink<struct rtnl_cls> cls(c);

  rtnl_tc_set_link(TC_CAST(cls.get()), link.get());
  rtnl_tc_set_parent(TC_CAST(cls.get()), filter.parent().get());

  // Encode the priority.
  if (filter.priority().isSome()) {
    rtnl_cls_set_prio(cls.get(), filter.priority().get().get());
  }

  // Encode the classifier using the classifier specific function.
  Try<Nothing> encoding = encode(cls, filter.classifier());
  if (encoding.isError()) {
    return Error("Failed to encode the classifier " + encoding.error());
  }

  // Attach actions to the libnl filter.
  foreach (const process::Shared<action::Action>& action, filter.actions()) {
    Try<Nothing> attaching = attach(cls, action);
    if (attaching.isError()) {
      return Error("Failed to attach an action " + attaching.error());
    }
  }

  return cls;
}


// Decodes a libnl filter (rtnl_cls) to our filter representation.
// Returns None if the libnl filter does not match the specified
// classifier type. We use template here so that it works for any type
// of classifier.
template <typename Classifier>
Result<Filter<Classifier> > decodeFilter(const Netlink<struct rtnl_cls>& cls)
{
  // If the handle of the libnl filer is 0, it means that it is an
  // internal filter, therefore is definitly not created by us.
  if (rtnl_tc_get_handle(TC_CAST(cls.get())) == 0) {
    return None();
  }

  // Decode the parent.
  queueing::Handle parent(rtnl_tc_get_parent(TC_CAST(cls.get())));

  // Decode the priority. If the priority is not specified by the
  // user, kernel will assign a priority to the filter. So we should
  // always have a valid priority here.
  Priority priority(rtnl_cls_get_prio(cls.get()));

  // Decode the classifier using a classifier specific function.
  Result<Classifier> classifier = decode<Classifier>(cls);
  if (classifier.isError()) {
    return Error("Failed to decode the classifier: " + classifier.error());
  } else if (classifier.isNone()) {
    return None();
  }

  // TODO(jieyu): Decode all the actions attached to the filter.
  // Currently, libnl does not support that (but will support that in
  // the future).
  return Filter<Classifier>(parent, classifier.get(), priority);
}

/////////////////////////////////////////////////
// Helpers for internal APIs.
/////////////////////////////////////////////////

// Returns all the libnl filters (rtnl_cls) attached to the given
// parent on the link.
inline Try<std::vector<Netlink<struct rtnl_cls> > > getClses(
    const Netlink<struct rtnl_link>& link,
    const queueing::Handle& parent)
{
  Try<Netlink<struct nl_sock> > sock = routing::socket();
  if (sock.isError()) {
    return Error(sock.error());
  }

  // Dump all the libnl filters (i.e., rtnl_cls) attached to the given
  // parent on the link.
  struct nl_cache* c = NULL;
  int err = rtnl_cls_alloc_cache(
      sock.get().get(),
      rtnl_link_get_ifindex(link.get()),
      parent.get(),
      &c);

  if (err != 0) {
    return Error(
        "Failed to get filter info from kernel: " +
        std::string(nl_geterror(err)));
  }

  Netlink<struct nl_cache> cache(c);

  std::vector<Netlink<struct rtnl_cls> > results;

  for (struct nl_object* o = nl_cache_get_first(cache.get());
       o != NULL; o = nl_cache_get_next(o)) {
    nl_object_get(o); // Increment the reference counter.
    results.push_back(Netlink<struct rtnl_cls>((struct rtnl_cls*) o));
  }

  return results;
}


// Returns the libnl filter (rtnl_cls) attached to the given parent
// that matches the specified classifier on the link. Returns None if
// no match has been found. We use template here so that it works for
// any type of classifier.
template <typename Classifier>
Result<Netlink<struct rtnl_cls> > getCls(
    const Netlink<struct rtnl_link>& link,
    const queueing::Handle& parent,
    const Classifier& classifier)
{
  Try<std::vector<Netlink<struct rtnl_cls> > > clses = getClses(link, parent);
  if (clses.isError()) {
    return Error(clses.error());
  }

  foreach (const Netlink<struct rtnl_cls>& cls, clses.get()) {
    // The decode function will return None if 'cls' does not match
    // the classifier type. In that case, we just move on to the next
    // libnl filter.
    Result<Filter<Classifier> > filter = decodeFilter<Classifier>(cls);
    if (filter.isError()) {
      return Error("Failed to decode: " + filter.error());
    } else if (filter.isSome() && filter.get().classifier() == classifier) {
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
    const queueing::Handle& parent,
    const Classifier& classifier)
{
  Result<Netlink<struct rtnl_link> > link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_cls> > cls =
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
  Try<bool> _exists = exists(_link, filter.parent(), filter.classifier());
  if (_exists.isError()) {
    return Error("Check filter existence failed: " + _exists.error());
  } else if (_exists.get()) {
    // The filter already exists.
    return false;
  }

  Result<Netlink<struct rtnl_link> > link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return Error("Link '" + _link + "' is not found");
  }

  Try<Netlink<struct rtnl_cls> > cls = encodeFilter(link.get(), filter);
  if (cls.isError()) {
    return Error("Failed to encode the filter: " + cls.error());
  }

  Try<Netlink<struct nl_sock> > sock = routing::socket();
  if (sock.isError()) {
    return Error(sock.error());
  }

  int err = rtnl_cls_add(
      sock.get().get(),
      cls.get().get(),
      NLM_F_CREATE | NLM_F_EXCL);

  if (err != 0) {
    if (err == -NLE_EXIST) {
      return false;
    } else {
      return Error(std::string(nl_geterror(err)));
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
    const queueing::Handle& parent,
    const Classifier& classifier)
{
  Result<Netlink<struct rtnl_link> > link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  Result<Netlink<struct rtnl_cls> > cls =
    getCls(link.get(), parent, classifier);

  if (cls.isError()) {
    return Error(cls.error());
  } else if (cls.isNone()) {
    return false;
  }

  Try<Netlink<struct nl_sock> > sock = routing::socket();
  if (sock.isError()) {
    return Error(sock.error());
  }

  int err = rtnl_cls_delete(sock.get().get(), cls.get().get(), 0);
  if (err != 0) {
    // TODO(jieyu): Interpret the error code and return false if it
    // indicates that the filter is not found.
    return Error(std::string(nl_geterror(err)));
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
  Result<Netlink<struct rtnl_link> > link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return false;
  }

  // Get the old libnl classifier (to-be-updated) from kernel.
  Result<Netlink<struct rtnl_cls> > oldCls =
    getCls(link.get(), filter.parent(), filter.classifier());

  if (oldCls.isError()) {
    return Error(oldCls.error());
  } else if (oldCls.isNone()) {
    return false;
  }

  // The kernel does not allow us to update the priority. So if the
  // user specifies a priority, we will check to make sure they match.
  if (filter.priority().isSome() &&
      filter.priority().get().get() != rtnl_cls_get_prio(oldCls.get().get())) {
    return Error(
        "The priorities do not match. The old priority is " +
        stringify(rtnl_cls_get_prio(oldCls.get().get())) +
        " and the new priority is " +
        stringify(filter.priority().get().get()));
  }

  Try<Netlink<struct rtnl_cls> > newCls = encodeFilter(link.get(), filter);
  if (newCls.isError()) {
    return Error("Failed to encode the new filter: " + newCls.error());
  }

  // Set the handle of the new filter to match that of the old one.
  rtnl_tc_set_handle(
      TC_CAST(newCls.get().get()),
      rtnl_tc_get_handle(TC_CAST(oldCls.get().get())));

  // Set the priority of the new filter to match that of the old one.
  rtnl_cls_set_prio(
      newCls.get().get(),
      rtnl_cls_get_prio(oldCls.get().get()));

  Try<Netlink<struct nl_sock> > sock = routing::socket();
  if (sock.isError()) {
    return Error(sock.error());
  }

  int err = rtnl_cls_change(sock.get().get(), newCls.get().get(), 0);
  if (err != 0) {
    if (err == -NLE_OBJ_NOTFOUND) {
      return false;
    } else {
      return Error(std::string(nl_geterror(err)));
    }
  }

  return true;
}


// Returns all the filters attached to the given parent on the link.
// Returns None if the link or the parent is not found. We use
// template here so that it works for any type of classifier.
template <typename Classifier>
Result<std::vector<Filter<Classifier> > > filters(
    const std::string& _link,
    const queueing::Handle& parent)
{
  Result<Netlink<struct rtnl_link> > link = link::internal::get(_link);
  if (link.isError()) {
    return Error(link.error());
  } else if (link.isNone()) {
    return None();
  }

  Try<std::vector<Netlink<struct rtnl_cls> > > clses =
    getClses(link.get(), parent);

  if (clses.isError()) {
    return Error(clses.error());
  }

  std::vector<Filter<Classifier> > results;

  foreach (const Netlink<struct rtnl_cls>& cls, clses.get()) {
    // The decode function will return None if 'cls' does not match
    // the classifier type. In that case, we just move on to the next
    // libnl filter.
    Result<Filter<Classifier> > filter = decodeFilter<Classifier>(cls);
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
Result<std::vector<Classifier> > classifiers(
    const std::string& link,
    const queueing::Handle& parent)
{
  Result<std::vector<Filter<Classifier> > > _filters =
    filters<Classifier>(link, parent);

  if (_filters.isError()) {
    return Error(_filters.error());
  } else if (_filters.isNone()) {
    return None();
  }

  std::vector<Classifier> results;

  foreach (const Filter<Classifier>& filter, _filters.get()) {
    results.push_back(filter.classifier());
  }

  return results;
}

} // namespace internal {
} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_INTERNAL_HPP__
