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

#include <netlink/route/tc.h>

#include <netlink/route/cls/basic.h>

#include <stout/error.hpp>
#include <stout/none.hpp>

#include "linux/routing/internal.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/basic.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/internal.hpp"
#include "linux/routing/filter/priority.hpp"

#include "linux/routing/queueing/handle.hpp"

using std::string;

namespace routing {
namespace filter {

/////////////////////////////////////////////////
// Filter specific pack/unpack functions.
/////////////////////////////////////////////////

namespace internal {

// This is a work around. Including <linux/if_ether.h> causes
// duplicated definitions on some platforms with old glibc.
#ifndef ETH_P_ALL
#define ETH_P_ALL 0x0003
#endif


// Encodes the basic classifier into the libnl filter 'cls'. Each type
// of classifier needs to implement this function.
template <>
Try<Nothing> encode<basic::Classifier>(
    const Netlink<struct rtnl_cls>& cls,
    const basic::Classifier& classifier)
{
  rtnl_cls_set_protocol(cls.get(), classifier.protocol());

  int error = rtnl_tc_set_kind(TC_CAST(cls.get()), "basic");
  if (error != 0) {
    return Error(
        "Failed to set the kind of the classifier: " +
        string(nl_geterror(error)));
  }

  return Nothing();
}


// Decodes the basic classifier from the libnl filter 'cls'. Each type
// of classifier needs to implement this function. Returns None if the
// libnl filter is not a basic packet filter.
template <>
Result<basic::Classifier> decode<basic::Classifier>(
    const Netlink<struct rtnl_cls>& cls)
{
  if (rtnl_tc_get_kind(TC_CAST(cls.get())) != string("basic")) {
    return None();
  }

  return basic::Classifier(rtnl_cls_get_protocol(cls.get()));
}

} // namespace internal {


namespace basic {

Try<bool> exists(const string& link, const queueing::Handle& parent)
{
  return internal::exists(link, parent, Classifier(ETH_P_ALL));
}


Try<bool> create(
    const string& link,
    const queueing::Handle& parent,
    const Option<Priority>& priority,
    const Option<queueing::Handle>& classid)
{
  return internal::create(
      link,
      Filter<Classifier>(
          parent,
          Classifier(ETH_P_ALL),
          priority,
          None(),
          classid));
}


Try<bool> remove(const string& link, const queueing::Handle& parent)
{
  return internal::remove(link, parent, Classifier(ETH_P_ALL));
}

} // namespace basic {
} // namespace filter {
} // namespace routing {
