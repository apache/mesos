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

#ifndef __LINUX_ROUTING_DIAGNOSIS_DIAGNOSIS_HPP__
#define __LINUX_ROUTING_DIAGNOSIS_DIAGNOSIS_HPP__

#include <sys/socket.h> // For protocol families, e.g., AF_INET6 is IPv6.

#include <netinet/tcp.h> // For tcp_info.

#include <vector>

#include <stout/ip.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace routing {
namespace diagnosis {
namespace socket {
namespace state {

// The different connection states of a socket.
// TODO(chzhcn): libnl3-idiag still uses the old idiag kernel API,
// which only supports TCP sockets. When it moves to the newer API,
// consider changing this to a per-family state structure.
const int UNKNOWN = 0;
const int ESTABLISHED = 1 << 1;
const int SYN_SENT = 1 << 2;
const int SYN_RECV = 1 << 3;
const int FIN_WAIT1 = 1 << 4;
const int FIN_WAIT2 = 1 << 5;
const int TIME_WAIT = 1 << 6;
const int CLOSE = 1 << 7;
const int CLOSE_WAIT = 1 << 8;
const int LAST_ACK = 1 << 9;
const int LISTEN = 1 << 10;
const int CLOSING = 1 << 11;
const int MAX = 1 << 12;
const int ALL = MAX - 1;

} // namespace state {

// The diagnosis information of a socket. We only included a few
// members of 'struct idiagnl_msg' from libnl3-idiag, but more could
// be added later on.
struct Info
{
  Info(int _family,
       int _state,
       uint32_t _inode,
       const Option<uint16_t>& _sourcePort,
       const Option<uint16_t>& _destinationPort,
       const Option<net::IP>& _sourceIP,
       const Option<net::IP>& _destinationIP,
       const Option<struct tcp_info>& _tcpInfo)
    : family(_family),
      state(_state),
      inode(_inode),
      sourcePort(_sourcePort),
      destinationPort(_destinationPort),
      sourceIP(_sourceIP),
      destinationIP(_destinationIP),
      tcpInfo(_tcpInfo) {}

  int family;
  int state;
  uint32_t inode;

  // sourcePort, destinationPort, sourceIP and destinationIP should
  // all be present because this version of kernel API that libnl3
  // uses can only return TCP sockets. We leave them as optional here
  // because future support of other families could leave them as
  // empty values.
  Option<uint16_t> sourcePort;
  Option<uint16_t> destinationPort;
  Option<net::IP> sourceIP;
  Option<net::IP> destinationIP;

  // tcp_info is included in the kernel header files so we expose it
  // directly.
  Option<struct tcp_info> tcpInfo;
};


// Return a list of socket information that matches the given protocol
// family and socket states. 'states' can accept multiple states using
// bitwise OR.
// NOTE: 'family' is actually ignored here because the older kernel
// idiag API libnl3 uses only supports TCP and ignores this value. We
// keep it here to follow libnl3-idiag's suit.
Try<std::vector<Info>> infos(int familiy, int states);

} // namespace socket {
} // namespace diagnosis {
} // namespace routing {

#endif // __LINUX_ROUTING_DIAGNOSIS_DIAGNOSIS_HPP__
