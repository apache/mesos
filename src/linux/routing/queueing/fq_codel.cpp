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

#include "linux/routing/queueing/fq_codel.hpp"
#include "linux/routing/queueing/internal.hpp"

using std::string;

namespace routing {
namespace queueing {

namespace fq_codel {

// TODO(cwang): The fq_codel queueing discipline configuration is not
// exposed to the user because we use all the default parameters
// currently.
struct Config {};

} // namespace fq_codel {

/////////////////////////////////////////////////
// Type specific {en}decoding functions.
/////////////////////////////////////////////////

namespace internal {

// Encodes an fq_codel queueing discipline configuration into the
// libnl queueing discipline 'qdisc'. Each type of queueing discipline
// needs to implement this function.
template <>
Try<Nothing> encode<fq_codel::Config>(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const fq_codel::Config& config)
{
  // We don't set fq_codel parameters here, use the default:
  //   limit 10240p
  //   flows 1024
  //   quantum 1514
  //   target 5.0ms
  //   interval 100.0ms
  //   ecn

  return Nothing();
}


// Decodes the fq_codel queue discipline configuration from the libnl
// queueing discipline 'qdisc'. Each type of queueing discipline needs
// to implement this function. Returns None if the libnl queueing
// discipline is not an fq_codel queueing discipline.
template <>
Result<fq_codel::Config> decode<fq_codel::Config>(
    const Netlink<struct rtnl_qdisc>& qdisc)
{
  if (rtnl_tc_get_kind(TC_CAST(qdisc.get())) != fq_codel::KIND) {
    return None();
  }

  return fq_codel::Config();
}

} // namespace internal {

/////////////////////////////////////////////////
// Public interfaces.
/////////////////////////////////////////////////

namespace fq_codel {

const int DEFAULT_FLOWS = 1024;


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


} // namespace fq_codel {
} // namespace queueing {
} // namespace routing {
