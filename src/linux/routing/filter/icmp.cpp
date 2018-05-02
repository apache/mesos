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

#include <stdint.h>

#include <arpa/inet.h>

#include <netlink/errno.h>

#include <netlink/route/tc.h>

#include <netlink/route/cls/u32.h>

#include <stout/error.hpp>
#include <stout/none.hpp>

#include "linux/routing/handle.hpp"
#include "linux/routing/internal.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/icmp.hpp"
#include "linux/routing/filter/internal.hpp"
#include "linux/routing/filter/priority.hpp"

using std::string;
using std::vector;

namespace routing {
namespace filter {

/////////////////////////////////////////////////
// Classifier specific {en}decoding functions.
/////////////////////////////////////////////////

namespace internal {

// This is a work around. Including <linux/if_ether.h> causes
// duplicated definitions on some platforms with old glibc.
#ifndef ETH_P_IP
#define ETH_P_IP  0x0800
#endif


// Encodes the ICMP classifier into the libnl filter 'cls'. Each type
// of classifier needs to implement this function.
template <>
Try<Nothing> encode<icmp::Classifier>(
    const Netlink<struct rtnl_cls>& cls,
    const icmp::Classifier& classifier)
{
  // ICMP packets are one type of IP packets.
  rtnl_cls_set_protocol(cls.get(), ETH_P_IP);

  int error = rtnl_tc_set_kind(TC_CAST(cls.get()), "u32");
  if (error != 0) {
    return Error(
        "Failed to set the kind of the classifier: " +
        string(nl_geterror(error)));
  }

  // To avoid confusion, we only use u32 selectors which are used to
  // match arbitrary 32-bit content in a packet.

  // Format of an IP packet at offset 8. The IP protocol field is at
  // offset 9. ICMP has protocol = 1.
  //        +--------+--------+--------+--------+
  //        |   X    | Proto. |   X    |   X    |
  //        +--------+--------+--------+--------+
  // Offset:    8        9        10       11
  uint32_t protocol = 0x00010000;
  uint32_t mask = 0x00ff0000; // Ignore offset 8, 10, 11.

  // To match ICMP packets (protocol = 1).
  error = rtnl_u32_add_key(
      cls.get(),
      htonl(protocol),
      htonl(mask),
      8, // Offset from which to start matching.
      0);

  if (error != 0) {
    return Error(
        "Failed to add selector for IP protocol: " +
        string(nl_geterror(error)));
  }

  if (classifier.destinationIP.isSome()) {
    Try<struct in_addr> in = classifier.destinationIP->in();
    if (in.isError()) {
      return Error("Destination IP is not an IPv4 address");
    }

    // To match those IP packets that have the given destination IP.
    error = rtnl_u32_add_key(
        cls.get(),
        in->s_addr,
        htonl(0xffffffff),
        16, // Offset from which to start matching.
        0);

    if (error != 0) {
      return Error(
          "Failed to add selector for destination IP address: " +
           string(nl_geterror(error)));
    }
  }

  return Nothing();
}


// Decodes the ICMP classifier from the libnl filter 'cls'. Each type
// of classifier needs to implement this function. Returns None if the
// libnl filter is not an ICMP packet filter.
template <>
Result<icmp::Classifier> decode<icmp::Classifier>(
    const Netlink<struct rtnl_cls>& cls)
{
  if (rtnl_cls_get_protocol(cls.get()) != ETH_P_IP ||
      rtnl_tc_get_kind(TC_CAST(cls.get())) != string("u32")) {
    return None();
  }

  // Raw values.
  Option<uint32_t> protocol;
  Option<net::IP> destinationIP;

  // There are at most 0xff keys.
  for (uint8_t i = 0; i <= 0xff; i++) {
    uint32_t value;
    uint32_t mask;
    int offset;
    int offsetmask;

    // Decode a selector from the libnl filter 'cls'.
    int error = rtnl_u32_get_key(
        cls.get(),
        i,
        &value,
        &mask,
        &offset,
        &offsetmask);

    if (error != 0) {
      if (error == -NLE_INVAL) {
        // This is the case where cls does not have a u32 selector. In
        // that case, we just return None.
        return None();
      } else if (error == -NLE_RANGE) {
        break;
      } else {
        return Error(
            "Failed to decode a u32 selector: " +
            string(nl_geterror(error)));
      }
    }

    // The function "rtnl_u32_get_key" sets value and mask in network
    // order. Convert them back to host order.
    value = ntohl(value);
    mask = ntohl(mask);

    // IP protocol field.
    if (offset == 8 && value == 0x00010000 && mask == 0x00ff0000) {
      protocol = value;
    }

    // Destination IP address.
    if (offset == 16 && mask == 0xffffffff) {
      destinationIP = net::IP(value);
    }
  }

  if (protocol.isSome()) {
    return icmp::Classifier(destinationIP);
  }

  return None();
}

} // namespace internal {

/////////////////////////////////////////////////
// Public interfaces.
/////////////////////////////////////////////////

namespace icmp {

Try<bool> exists(
    const string& link,
    const Handle& parent,
    const Classifier& classifier)
{
  return internal::exists(link, parent, classifier);
}


Try<bool> create(
    const string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Redirect& redirect)
{
  return internal::create(
      link,
      Filter<Classifier>(
          parent,
          classifier,
          priority,
          None(),
          None(),
          redirect));
}


Try<bool> create(
    const string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Mirror& mirror)
{
  return internal::create(
      link,
      Filter<Classifier>(
          parent,
          classifier,
          priority,
          None(),
          None(),
          mirror));
}


Try<bool> create(
    const string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const Option<Handle>& classid)
{
  return internal::create(
      link,
      Filter<Classifier>(
          parent,
          classifier,
          priority,
          None(),
          classid));
}


Try<bool> remove(
    const string& link,
    const Handle& parent,
    const Classifier& classifier)
{
  return internal::remove(link, parent, classifier);
}


Try<bool> update(
    const string& link,
    const Handle& parent,
    const Classifier& classifier,
    const action::Mirror& mirror)
{
  return internal::update(
      link,
      Filter<Classifier>(
          parent,
          classifier,
          None(),
          None(),
          None(),
          mirror));
}


Result<vector<Classifier>> classifiers(
    const string& link,
    const Handle& parent)
{
  return internal::classifiers<Classifier>(link, parent);
}

} // namespace icmp {
} // namespace filter {
} // namespace routing {
