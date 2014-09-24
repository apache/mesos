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

#ifndef __LINUX_ROUTING_DIAGNOSIS_DIAGNOSIS_HPP__
#define __LINUX_ROUTING_DIAGNOSIS_DIAGNOSIS_HPP__

#include <sys/socket.h> // For protocol families, e.g., AF_INET6 is IPv6.

#include <netinet/tcp.h> // For tcp_info.

#include <vector>

#include <stout/net.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace routing {
namespace diagnosis {

namespace socket {

// The connection state of a socket.
// TODO(chzhcn): libnl3-idiag still uses the old idiag kernel API,
// which only supports TCP sockets. When it moves to the newer API,
// consider changing this to a per-family state structure.
enum State
{
  UNKNOWN,
  ESTABLISHED,
  SYN_SENT,
  SYN_RECV,
  FIN_WAIT1,
  FIN_WAIT2,
  TIME_WAIT,
  CLOSE,
  CLOSE_WAIT,
  LAST_ACK,
  LISTEN,
  CLOSING,
  MAX,
  ALL = (1 << MAX) - 1
};


// The diagnosis information of a socket. We only included a few
// members of 'struct idiagnl_msg' from libnl3-idiag, but more could
// be added later on.
class Info
{
public:
  Info(int _family,
       State _state,
       const Option<uint16_t>& _sourcePort,
       const Option<uint16_t>& _destinationPort,
       const Option<net::IP>& _sourceIP,
       const Option<net::IP>& _destinationIP,
       const Option<struct tcp_info>& _tcpInfo)
    : family_(_family),
      state_(_state),
      sourcePort_(_sourcePort),
      destinationPort_(_destinationPort),
      sourceIP_(_sourceIP),
      destinationIP_(_destinationIP),
      tcpInfo_(_tcpInfo) {}

  int family() const { return family_; }
  State state() const { return state_; }
  const Option<uint16_t>& sourcePort() const { return sourcePort_; }
  const Option<uint16_t>& destinationPort() const { return destinationPort_; }
  const Option<net::IP>& sourceIP() const { return sourceIP_; }
  const Option<net::IP>& destinationIP() const { return destinationIP_; }
  const Option<struct tcp_info>& tcpInfo() const { return tcpInfo_; }

private:
  int family_;
  State state_;

  // sourcePort, destinationPort, sourceIP and destinationIP should
  // all be present because this version of kernel API that libnl3
  // uses can only return TCP sockets. We leave them as optional here
  // because future support of other families could leave them as
  // empty values.
  Option<uint16_t> sourcePort_;
  Option<uint16_t> destinationPort_;
  Option<net::IP> sourceIP_;
  Option<net::IP> destinationIP_;

  // tcp_info is included in the kernel header files so we expose it
  // directly.
  Option<struct tcp_info> tcpInfo_;
};


// Return a list of socket information that matches the given protocol
// family and socket state.
// NOTE: 'family' is actually igored here because the older kernel
// idiag API libnl3 uses only supports TCP and ingores this value. We
// keep it here to follow libnl3-idiag's suit.
Try<std::vector<Info> > infos(int familiy, State states);

} // namespace socket {

} // namespace diagnosis {
} // namespace routing {

#endif // __LINUX_ROUTING_DIAGNOSIS_DIAGNOSIS_HPP__
