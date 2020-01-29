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

#include <netlink/route/link/veth.h>

#include <netlink/route/qdisc/htb.h>

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

/////////////////////////////////////////////////
// Type specific {en}decoding functions for disciplines and classes.
/////////////////////////////////////////////////

namespace internal {

// Encodes an htb queueing discipline configuration into the
// libnl queueing discipline 'qdisc'. Each type of queueing discipline
// needs to implement this function.
template <>
Try<Nothing> encode<htb::DisciplineConfig>(
    const Netlink<struct rtnl_qdisc>& qdisc,
    const htb::DisciplineConfig& config)
{
  int error = rtnl_htb_set_defcls(qdisc.get(), config.defcls);
  if (error != 0) {
    return Error(string(nl_geterror(error)));
  }
  return Nothing();
}


// Decodes the htb queue discipline configuration from the libnl
// queueing discipline 'qdisc'. Each type of queueing discipline needs
// to implement this function. Returns None if the libnl queueing
// discipline is not an htb queueing discipline.
template <>
Result<htb::DisciplineConfig> decode<htb::DisciplineConfig>(
    const Netlink<struct rtnl_qdisc>& qdisc)
{
  if (rtnl_tc_get_kind(TC_CAST(qdisc.get())) != string(htb::KIND)) {
    return None();
  }

  return htb::DisciplineConfig();
}

namespace cls {

// Encodes an htb queueing class configuration into the libnl queueing
// class 'cls'. Each type of queueing class needs to implement this
// function.
template<>
Try<Nothing> encode<htb::cls::Config>(
    const Netlink<struct rtnl_class>& cls,
    const htb::cls::Config& config)
{
  int error = rtnl_htb_set_rate64(cls.get(), config.rate);
  if (error != 0) {
    return Error(string(nl_geterror(error)));
  }

  if (config.ceil.isSome()) {
    error = rtnl_htb_set_ceil64(cls.get(), config.ceil.get());
    if (error != 0) {
      return Error(string(nl_geterror(error)));
    }
  }

  if (config.burst.isSome()) {
    // NOTE: The libnl documentation is incorrect/confusing. The
    // correct buffer for sending at the ceil rate is rbuffer, *not*
    // the cbuffer.
    // https://www.infradead.org/~tgr/libnl/doc/api/group__qdisc__htb.html
    // https://linux.die.net/man/8/tc-htb
    error = rtnl_htb_set_rbuffer(cls.get(), config.burst.get());
    if (error != 0) {
      return Error(string(nl_geterror(error)));
    }
  }

  return Nothing();
}


// Decodes the htb queueing class configuration from the libnl
// queueing class 'cls'. Each type of queueing class needs to
// implement this function. Returns None if the libnl queueing class
// is not an htb queueing class.
template<>
Result<htb::cls::Config> decode<htb::cls::Config>(
    const Netlink<struct rtnl_class>& cls)
{
  if (rtnl_tc_get_kind(TC_CAST(cls.get())) != string(htb::KIND)) {
    return None();
  }

  htb::cls::Config config;

  // With 32bit rates this function used to return 0 rate and ceil in
  // case of an error. We keep the same behavior even though 64bit
  // getters from libnl are capable of returning an error when class
  // data cannot be recorded or an attribute is not present.
  uint64_t rate = 0;
  uint64_t ceil = 0;
  rtnl_htb_get_rate64(cls.get(), &rate);
  rtnl_htb_get_ceil64(cls.get(), &ceil);

  // NOTE: The libnl documentation is incorrect/confusing. The
  // correct buffer for sending at the ceil rate is rbuffer, *not*
  // the cbuffer.
  // https://www.infradead.org/~tgr/libnl/doc/api/group__qdisc__htb.html
  // https://linux.die.net/man/8/tc-htb
  uint32_t burst = rtnl_htb_get_rbuffer(cls.get());

  return htb::cls::Config(
      rate,
      (ceil > 0) ? Option<uint64_t>(ceil) : None(),
      (burst > 0) ? Option<uint32_t>(burst) : None());
}

} // namespace cls {
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
    const Option<Handle>& handle,
    const Option<DisciplineConfig>& config)
{
  return internal::create(
      link,
      Discipline<DisciplineConfig>(
          KIND,
          parent,
          handle,
          config.getOrElse(DisciplineConfig())));
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


namespace cls {

void json(JSON::ObjectWriter* writer, const Config& config)
{
  writer->field("rate", config.rate);
  if (config.ceil.isSome()) {
    writer->field("ceil", config.ceil.get());
  }
  if (config.burst.isSome()) {
    writer->field("burst", config.burst.get());
  }
}

Try<bool> exists(const string& link, const Handle& classid)
{
  return internal::cls::exists(link, classid, KIND);
}


Try<bool> create(
    const string& link,
    const Handle& parent,
    const Option<Handle>& handle,
    const Config& config)
{
  return internal::cls::create(
      link,
      Class<Config>(
        KIND,
        parent,
        handle,
        config));
}


Try<bool> update(
    const std::string& link,
    const Handle& classid,
    const Config& config)
{
  return internal::cls::update(
      link,
      Class<Config>(
        KIND,
        Handle(classid.primary(), 0),
        classid,
        config));
}


Result<Config> getConfig(
    const std::string& link,
    const Handle& classid)
{
  return internal::cls::getConfig<Config>(
      link,
      classid,
      KIND);
}


Try<bool> remove(const string& link, const Handle& classid)
{
  return internal::cls::remove(link, classid, KIND);
}


} // namespace cls {
} // namespace htb {
} // namespace queueing {
} // namespace routing {
