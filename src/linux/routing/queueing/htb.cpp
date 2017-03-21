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

#include <netlink/errno.h>

#include <netlink/route/qdisc.h>
#include <netlink/route/tc.h>

#include <stout/error.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>

#include "linux/routing/handle.hpp"

#include "linux/routing/queueing/htb.hpp"
#include "linux/routing/queueing/internal.hpp"

using std::string;

namespace routing {
namespace queueing {

namespace htb {

// TODO(cwang): The htb queueing discipline configuration is not
// exposed to the user because we use all the default parameters
// currently.
struct Config {};

} // namespace htb {

/////////////////////////////////////////////////
// Type specific {en}decoding functions.
/////////////////////////////////////////////////

namespace internal {

// Encodes an htb queueing discipline configuration into the
// libnl queueing discipline 'qdisc'. Each type of queueing discipline
// needs to implement this function.
template <>
Try<Nothing> encode<htb::Config>(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const htb::Config& config)
{
  return Nothing();
}


// Decodes the htb queue discipline configuration from the libnl
// queueing discipline 'qdisc'. Each type of queueing discipline needs
// to implement this function. Returns None if the libnl queueing
// discipline is not an htb queueing discipline.
template <>
Result<htb::Config> decode<htb::Config>(
    const Netlink<struct rtnl_qdisc>& qdisc)
{
  if (rtnl_tc_get_kind(TC_CAST(qdisc.get())) != htb::KIND) {
    return None();
  }

  return htb::Config();
}

} // namespace internal {

/////////////////////////////////////////////////
// Public interfaces.
/////////////////////////////////////////////////

namespace htb {

Try<bool> exists(const string& link, const Handle& parent)
{
  return internal::exists(link, parent, KIND);
}


Try<bool> create(
    const string& link,
    const Handle& parent,
    const Option<Handle>& handle)
{
  return internal::create(
      link,
      Discipline<Config>(
          KIND,
          parent,
          handle,
          Config()));
}


Try<bool> remove(const string& link, const Handle& parent)
{
  return internal::remove(link, parent, KIND);
}


Result<hashmap<string, uint64_t>> statistics(
    const string& link,
    const Handle& parent)
{
  return internal::statistics(link, parent, KIND);
}


} // namespace htb {
} // namespace queueing {
} // namespace routing {
