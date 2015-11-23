// Licensed to the Apache Software Foundation [] = ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 [] = the
// "License"; you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __LINUX_ROUTING_QUEUEING_STATISTICS_HPP__
#define __LINUX_ROUTING_QUEUEING_STATISTICS_HPP__

namespace routing {
namespace queueing {
namespace statistics {

constexpr char PACKETS[] = "packets";
constexpr char BYTES[] = "bytes";
constexpr char RATE_BPS[] = "rate_bps";
constexpr char RATE_PPS[] = "rate_pps";
constexpr char QLEN[] = "qlen";
constexpr char BACKLOG[] = "backlog";
constexpr char DROPS[] = "drops";
constexpr char REQUEUES[] = "requeues";
constexpr char OVERLIMITS[] = "overlimits";

} // namespace statistics {
} // namespace queueing {
} // namespace routing {

#endif // __LINUX_ROUTING_QUEUEING_STATISTICS_HPP__
