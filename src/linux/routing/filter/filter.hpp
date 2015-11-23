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

#ifndef __LINUX_ROUTING_FILTER_FILTER_HPP__
#define __LINUX_ROUTING_FILTER_FILTER_HPP__

#include <vector>

#include <process/shared.hpp>

#include <stout/option.hpp>

#include "linux/routing/handle.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/priority.hpp"

namespace routing {
namespace filter {

// Our representation of a filter. Each filter is attached to a
// 'parent' (either a queueing discipline or queueing class), and
// contains a 'classifier' which defines how packets will be matched,
// a 'priority' which defines the order in which filters will be
// applied, and a series of 'actions' that will be taken when a packet
// satisfies the conditions specified in the classifier. If the
// priority is not specified, the kernel will assign a priority to the
// filter.
// TODO(jieyu): Currently, libnl does not support getting all actions
// associated with a filter. In other words, the list of actions
// obtained from the filter might not be the complete list.
template <typename Classifier>
struct Filter
{
public:
  // Creates a filter with no action.
  Filter(const Handle& _parent,
         const Classifier& _classifier,
         const Option<Priority>& _priority,
         const Option<Handle>& _handle,
         const Option<Handle>& _classid)
    : parent(_parent),
      classifier(_classifier),
      priority(_priority),
      handle(_handle),
      classid(_classid) {}

  // TODO(jieyu): Support arbitrary number of actions.
  template <typename Action>
  Filter(const Handle& _parent,
         const Classifier& _classifier,
         const Option<Priority>& _priority,
         const Option<Handle>& _handle,
         const Option<Handle>& _classid,
         const Action& action)
    : parent(_parent),
      classifier(_classifier),
      priority(_priority),
      handle(_handle),
      classid(_classid)
  {
    attach(action);
  }

  // Attaches an action to this filter.
  template <typename A>
  void attach(const A& action)
  {
    actions.push_back(process::Shared<action::Action>(new A(action)));
  }

  // Each filter is attached to a queueing object (either a queueing
  // discipline or a queueing class).
  Handle parent;

  // The filter specific classifier.
  Classifier classifier;

  // The priority of this filter.
  Option<Priority> priority;

  // The handle of this filter.
  Option<Handle> handle;

  // The classid of this filter.
  //
  // Note: the classid can be used for two purposes:
  //  1) For a classful queueing discipline, set the class id which
  //     refers to a TC class;
  //  2) For a classless queueing discipline, set the flow id which
  //     refers to a flow defined by its parent qdisc.
  // In both cases, the primary portion of a classid refers to its
  // parent queueing discipline, the secondary portion refers to
  // either its child queueing discipline or a flow.
  //
  // Kernel uses classid and flowid interchangeably. However, in our
  // code base, we use classid consistently.
  Option<Handle> classid;

  // The set of actions attached to this filer. Note that we use
  // Shared here to make Filter copyable.
  std::vector<process::Shared<action::Action>> actions;
};

} // namespace filter {
} // namespace routing {

#endif // __LINUX_ROUTING_FILTER_FILTER_HPP__
