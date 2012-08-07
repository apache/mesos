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

#ifndef __MASTER_HTTP_HPP__
#define __MASTER_HTTP_HPP__

#include <process/future.hpp>
#include <process/http.hpp>

namespace mesos {
namespace internal {
namespace master {

// Forward declaration (necessary to break circular dependency).
class Master;

namespace http {

// Returns current vars in "key value\n" format (keys do not contain
// spaces, values may contain spaces but are ended by a newline).
process::Future<process::http::Response> vars(
    const Master& master,
    const process::http::Request& request);

// Redirects immediately to the current leader. If there's no leader, this
// redirects to the master.
process::Future<process::http::Response> redirect(
    const Master& master,
    const process::http::Request& request);

namespace json {

// Returns current statistics of the master.
process::Future<process::http::Response> stats(
    const Master& master,
    const process::http::Request& request);


// Returns current state of the cluster that the master knows about.
process::Future<process::http::Response> state(
    const Master& master,
    const process::http::Request& request);

// Returns data from the log.
process::Future<process::http::Response> log(
    const Master& master,
    const process::http::Request& request);

} // namespace json {
} // namespace http {
} // namespace master {
} // namespace internal {
} // namespace mesos {

#endif // __MASTER_HTTP_HPP__
