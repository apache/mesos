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

#include "linux/routing/queueing/ingress.hpp"
#include "linux/routing/queueing/internal.hpp"

using std::string;

namespace routing {
namespace queueing {

namespace ingress {

// The ingress queueing discipline configuration is not currently
// exposed to the user.
struct Config {};

} // namespace ingress {

/////////////////////////////////////////////////
// Type specific {en}decoding functions.
/////////////////////////////////////////////////

namespace internal {

// Encodes an ingress queueing discipline configuration into the libnl
// queueing discipline 'qdisc'. Each type of queueing discipline needs
// to implement this function.
template <>
Try<Nothing> encode<ingress::Config>(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const ingress::Config& config)
{
  return Nothing();
}


// Decodes the ingress queue discipline configuration from the libnl
// queueing discipline 'qdisc'. Each type of queueing discipline needs
// to implement this function. Returns None if the libnl queueing
// discipline is not an ingress queueing discipline.
template <>
Result<ingress::Config> decode<ingress::Config>(
    const Netlink<struct rtnl_qdisc>& qdisc)
{
  if (rtnl_tc_get_kind(TC_CAST(qdisc.get())) != ingress::KIND) {
    return None();
  }

  return ingress::Config();
}

} // namespace internal {

/////////////////////////////////////////////////
// Public interfaces.
/////////////////////////////////////////////////

namespace ingress {

Try<bool> exists(const string& link)
{
  return internal::exists(link, INGRESS_ROOT, KIND);
}


Try<bool> create(const string& link)
{
  return internal::create(
      link,
      Discipline<Config>(
          KIND,
          INGRESS_ROOT,
          HANDLE,
          Config()));
}


Try<bool> remove(const string& link)
{
  return internal::remove(link, INGRESS_ROOT, KIND);
}


Result<hashmap<string, uint64_t>> statistics(const string& link)
{
  return internal::statistics(link, INGRESS_ROOT, KIND);
}

} // namespace ingress {
} // namespace queueing {
} // namespace routing {
