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

#ifndef __SYSTEMD_HPP__
#define __SYSTEMD_HPP__

namespace systemd {

/**
 * Check if we are on a systemd environment by:
 * (1) Testing whether `/sbin/init` links to systemd.
 * (2) Testing whether we have a systemd version.
 * TODO(jmlvanre): This logic can be made more robust, but there does not seem
 * to be a standardized way to test the executing init system in a
 * cross-platform way. The task is made slightly easier because we are only
 * interested in identifying if we are running on systemd, not which specific
 * init system is running.
 *
 * @return Whether running on a systemd environment.
 */
bool exists();

} // namespace systemd {

#endif // __SYSTEMD_HPP__
