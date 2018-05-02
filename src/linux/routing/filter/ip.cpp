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

#include <netlink/route/tc.h>

#include <netlink/route/cls/u32.h>

#include <ostream>

#include <stout/error.hpp>
#include <stout/none.hpp>

#include "linux/routing/handle.hpp"
#include "linux/routing/internal.hpp"

#include "linux/routing/filter/action.hpp"
#include "linux/routing/filter/filter.hpp"
#include "linux/routing/filter/internal.hpp"
#include "linux/routing/filter/ip.hpp"
#include "linux/routing/filter/priority.hpp"

using std::ostream;
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


// Encodes the IP classifier into the libnl filter 'cls'. Each type of
// classifier needs to implement this function.
template <>
Try<Nothing> encode<ip::Classifier>(
    const Netlink<struct rtnl_cls>& cls,
    const ip::Classifier& classifier)
{
  rtnl_cls_set_protocol(cls.get(), ETH_P_IP);

  int error = rtnl_tc_set_kind(TC_CAST(cls.get()), "u32");
  if (error != 0) {
    return Error(
        "Failed to set the kind of the classifier: " +
        string(nl_geterror(error)));
  }

  // TODO(jieyu): Do we need to check the protocol (e.g., TCP/UDP)?

  // This filter ignores IP packets that contain IP options. In other
  // words, we only match those IP packets with header length equal to
  // 5 bytes.
  //
  // Format of an IP packet at offset 0 (IHL is an abbrevation for
  // Internet Header Length).
  //        +--------+--------+--------+--------+
  //        |    IHL |   X    |   X    |   X    |
  //        +--------+--------+--------+--------+
  // Offset:     0        1        2        3
  error = rtnl_u32_add_key(
      cls.get(),
      htonl(0x05000000),
      htonl(0x0f000000),
      0, // Offset from which to start matching.
      0);

  if (error != 0) {
    return Error(
        "Failed to add selector for IP header length: " +
        string(nl_geterror(error)));
  }

  if (classifier.destinationMAC.isSome()) {
    // Since we set the protocol of this classifier to be ETH_P_IP
    // above, all IP packets that contain 802.1Q tag (i.e., VLAN tag)
    // will not match this classifier (those packets have protocol
    // ETH_P_8021Q). Therefore, the offset of the start of the MAC
    // destination address is at -14 (0xfffffff2).
    net::MAC mac = classifier.destinationMAC.get();

    // To avoid confusion, we only use u32 selectors which are used to
    // match arbitrary 32-bit content in a packet.

    // We need two u32 selectors as MAC address contains 6 bytes.
    uint32_t value[2];

    // Format of an IP packet at offset -16.
    //        +--------+--------+--------+--------+
    //        |   X    |   X    | mac[0] | mac[1] |
    //        +--------+--------+--------+--------+
    // Offset:   -16      -15      -14      -13
    value[0] = (((uint32_t) mac[0]) << 8) + ((uint32_t) mac[1]);

    // Format of an IP packet at offset -12:
    //        +--------+--------+--------+--------+
    //        | mac[2] | mac[3] | mac[4] | mac[5] |
    //        +--------+--------+--------+--------+
    // Offset:   -12      -11      -10      -09
    value[1] = (((uint32_t) mac[2]) << 24) +
               (((uint32_t) mac[3]) << 16) +
               (((uint32_t) mac[4]) << 8) +
               ((uint32_t) mac[5]);

    // To match the first two bytes of the MAC address.
    error = rtnl_u32_add_key(
        cls.get(),
        htonl(value[0]),
        htonl(0x0000ffff), // Ignore offset -16 and -15.
        -16, // Offset from which to start matching.
        0);

    if (error != 0) {
      return Error(
          "Failed to add selector for destination MAC address: " +
          string(nl_geterror(error)));
    }

    // To match the last four bytes of the MAC address.
    error = rtnl_u32_add_key(
        cls.get(),
        htonl(value[1]),
        htonl(0xffffffff),
        -12, // Offset from which to start matching.
        0);

    if (error != 0) {
      return Error(
          "Failed to add selector for destination MAC address: " +
          string(nl_geterror(error)));
    }
  }

  if (classifier.destinationIP.isSome()) {
    Try<struct in_addr> in = classifier.destinationIP->in();
    if (in.isError()) {
      return Error(in.error());
    }

    // To match those IP packets that have the given destination IP.
    error = rtnl_u32_add_key(
        cls.get(),
        in->s_addr,
        htonl(0xffffffff),
        16,
        0);

      if (error != 0) {
        return Error(
            "Failed to add selector for destination IP address: " +
            string(nl_geterror(error)));
      }
  }

  // TODO(jieyu): Here, we assume that the IP packet does not contain
  // IP options. As a result, we can hard code the offsets of the
  // source port and the destination ports to be 20 and 22
  // respectively. Users can choose to add a high priority filter to
  // filter all the IP packets that have IP options.
  if (classifier.sourcePorts.isSome()) {
    // Format of an IP packet at offset 20:
    //        +--------+--------+--------+--------+
    //        |   Source Port   |   X    |   X    |
    //        +--------+--------+--------+--------+
    // Offset:    20       21       22       23
    uint32_t value = ((uint32_t) classifier.sourcePorts->begin()) << 16;
    uint32_t mask = ((uint32_t) classifier.sourcePorts->mask()) << 16;

    // To match IP packets that have the given source ports.
    error = rtnl_u32_add_key(
        cls.get(),
        htonl(value),
        htonl(mask),
        20, // Offset to which to start matching.
        0);

    if (error != 0) {
      return Error(
          "Failed to add selector for source ports: " +
          string(nl_geterror(error)));
    }
  }

  if (classifier.destinationPorts.isSome()) {
    // Format of an IP packet at offset 20:
    //        +--------+--------+--------+--------+
    //        |   X    |   X    |    Dest. Port   |
    //        +--------+--------+--------+--------+
    // Offset:    20       21       22       23
    uint32_t value = (uint32_t) classifier.destinationPorts->begin();
    uint32_t mask = (uint32_t) classifier.destinationPorts->mask();

    // To match IP packets that have the given destination ports.
    error = rtnl_u32_add_key(
        cls.get(),
        htonl(value),
        htonl(mask),
        20,
        0);

    if (error != 0) {
      return Error(
          "Failed to add selector for destination ports: " +
          string(nl_geterror(error)));
    }
  }

  return Nothing();
}


// Decodes the IP classifier from the libnl filter 'cls'. Each type of
// classifier needs to implement this function. Returns None if the
// libnl filter is not an IP packet filter.
template <>
Result<ip::Classifier> decode<ip::Classifier>(
    const Netlink<struct rtnl_cls>& cls)
{
  if (rtnl_cls_get_protocol(cls.get()) != ETH_P_IP ||
      rtnl_tc_get_kind(TC_CAST(cls.get())) != string("u32")) {
    return None();
  }

  // Raw values.
  Option<uint32_t> protocol;
  Option<uint32_t> headerLength;
  Option<uint32_t> valueDestinationMAC1;
  Option<uint32_t> valueDestinationMAC2;
  Option<uint32_t> valueDestinationIP;
  Option<uint32_t> valueSourcePorts;
  Option<uint32_t> valueSourcePortsMask;
  Option<uint32_t> valueDestinationPorts;
  Option<uint32_t> valueDestinationPortsMask;

  // There are at most 0xff keys.
  for (uint8_t i = 0; i <= 0xff; i++) {
    uint32_t value;
    uint32_t mask;
    int offset;
    int offsetmask;

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
        // that case, we just return none.
        return None();
      } else if (error == -NLE_RANGE) {
        break;
      } else {
        return Error(
            "Failed to decode a u32 classifier: " +
            string(nl_geterror(error)));
      }
    }

    // The function "rtnl_u32_get_key" sets value and mask in network
    // order. Convert them back to host order.
    value = ntohl(value);
    mask = ntohl(mask);

    // IP protocol field.
    if (offset == 8 && mask == 0x00ff0000) {
      protocol = value;
    }

    // IP header length.
    if (offset == 0 && mask == 0x0f000000) {
      headerLength = value;
    }

    // First two bytes of the destination MAC address.
    if (offset == -16 && mask == 0x0000ffff) {
      valueDestinationMAC1 = value;
    }

    // Last four bytes of the MAC address.
    if (offset == -12 && mask == 0xffffffff) {
      valueDestinationMAC2 = value;
    }

    // Destination IP address.
    if (offset == 16 && mask == 0xffffffff) {
      valueDestinationIP = value;
    }

    // Source or destination ports, depending on the mask.
    if (offset == 20) {
      if ((mask | 0xffff0000) == 0xffff0000) {
        valueSourcePorts = value;
        valueSourcePortsMask = mask;
      } else if ((mask | 0x0000ffff) == 0x0000ffff) {
        valueDestinationPorts = value;
        valueDestinationPortsMask = mask;
      }
    }
  }

  // IP packet filters do not check IP protocol field.
  if (protocol.isSome()) {
    return None();
  }

  // IP packet filters only handle IP packets without options.
  // NOTE: We also allow an IP packet filter which does not check for
  // IP header length (for backwards compatibility).
  if (headerLength.isSome() && headerLength.get() != 0x05000000) {
    return None();
  }

  // Sanity checks.
  if (valueDestinationMAC1.isSome() && valueDestinationMAC2.isNone()) {
    return Error("Missing the last 4 bytes of the destination MAC address");
  }

  if (valueDestinationMAC1.isNone() && valueDestinationMAC2.isSome()) {
    return Error("Missing the first 2 bytes of the destination MAC address");
  }

  if (valueSourcePorts.isSome() && valueSourcePortsMask.isNone()) {
    return Error("Missing source ports mask");
  }

  if (valueSourcePorts.isNone() && valueSourcePortsMask.isSome()) {
    return Error("Missing source ports value");
  }

  if (valueDestinationPorts.isSome() && valueDestinationPortsMask.isNone()) {
    return Error("Missing destination ports mask");
  }

  if (valueDestinationPorts.isNone() && valueDestinationPortsMask.isSome()) {
    return Error("Missing destination ports value");
  }

  // Pack the values into the classifier.
  Option<net::MAC> destinationMAC;
  Option<net::IP> destinationIP;
  Option<ip::PortRange> sourcePorts;
  Option<ip::PortRange> destinationPorts;

  if (valueDestinationMAC1.isSome() && valueDestinationMAC2.isSome()) {
    uint8_t bytes[6];

    bytes[0] = (uint8_t) (valueDestinationMAC1.get() >> 8);
    bytes[1] = (uint8_t) valueDestinationMAC1.get();
    bytes[2] = (uint8_t) (valueDestinationMAC2.get() >> 24);
    bytes[3] = (uint8_t) (valueDestinationMAC2.get() >> 16);
    bytes[4] = (uint8_t) (valueDestinationMAC2.get() >> 8);
    bytes[5] = (uint8_t) valueDestinationMAC2.get();

    destinationMAC = net::MAC(bytes);
  }

  if (valueDestinationIP.isSome()) {
    destinationIP = net::IP(valueDestinationIP.get());
  }

  if (valueSourcePorts.isSome() && valueSourcePortsMask.isSome()) {
    uint16_t begin = (uint16_t) (valueSourcePorts.get() >> 16);
    uint16_t mask = (uint16_t) (valueSourcePortsMask.get() >> 16);

    Try<ip::PortRange> ports = ip::PortRange::fromBeginMask(begin, mask);
    if (ports.isError()) {
      return Error("Invalid source ports: " + ports.error());
    }

    sourcePorts = ports.get();
  }

  if (valueDestinationPorts.isSome() && valueDestinationPortsMask.isSome()) {
    uint16_t begin = (uint16_t) valueDestinationPorts.get();
    uint16_t mask = (uint16_t) valueDestinationPortsMask.get();

    Try<ip::PortRange> ports = ip::PortRange::fromBeginMask(begin, mask);
    if (ports.isError()) {
      return Error("Invalid destination ports: " + ports.error());
    }

    destinationPorts = ports.get();
  }

  return ip::Classifier(
      destinationMAC,
      destinationIP,
      sourcePorts,
      destinationPorts);
}

} // namespace internal {

/////////////////////////////////////////////////
// Public interfaces.
/////////////////////////////////////////////////

namespace ip {

Try<PortRange> PortRange::fromBeginEnd(uint16_t begin, uint16_t end)
{
  if (begin > end) {
    return Error("'begin' is larger than 'end'");
  }

  uint16_t size = end - begin + 1;

  // Test if the size is a power of 2.
  if ((size & (size - 1)) != 0) {
    return Error("The size " + stringify(size) + " is not a power of 2");
  }

  // Test if begin is aligned.
  if (begin % size != 0) {
    return Error("'begin' is not size aligned");
  }

  return PortRange(begin, end);
}


Try<PortRange> PortRange::fromBeginMask(uint16_t begin, uint16_t mask)
{
  uint16_t size = ~mask + 1;
  return fromBeginEnd(begin, begin + size - 1);
}


ostream& operator<<(ostream& stream, const PortRange& range)
{
  return stream << "[" << range.begin() << "," << range.end() << "]";
}


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
    const Option<Handle>& handle,
    const action::Redirect& redirect)
{
  return internal::create(
      link,
      Filter<Classifier>(
          parent,
          classifier,
          priority,
          handle,
          None(),
          redirect));
}


Try<bool> create(
    const string& link,
    const Handle& parent,
    const Classifier& classifier,
    const Option<Priority>& priority,
    const action::Terminal& terminal)
{
  return internal::create(
      link,
      Filter<Classifier>(
          parent,
          classifier,
          priority,
          None(),
          None(),
          terminal));
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
          classid,
          action::Terminal()));
}


Try<bool> remove(
    const string& link,
    const Handle& parent,
    const Classifier& classifier)
{
  return internal::remove(link, parent, classifier);
}


Result<vector<Filter<Classifier>>> filters(
    const string& link,
    const Handle& parent)
{
  return internal::filters<Classifier>(link, parent);
}


Result<vector<Classifier>> classifiers(
    const string& link,
    const Handle& parent)
{
  return internal::classifiers<Classifier>(link, parent);
}

} // namespace ip {
} // namespace filter {
} // namespace routing {
