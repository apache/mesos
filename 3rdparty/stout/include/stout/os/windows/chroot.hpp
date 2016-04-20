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

#ifndef __STOUT_OS_WINDOWS_CHROOT_HPP__
#define __STOUT_OS_WINDOWS_CHROOT_HPP__

#include <string>

#include <stout/nothing.hpp>
#include <stout/try.hpp>


namespace os {

// NOTE: `chroot` is deleted because Windows does not support POSIX `chroot`
// semantics. On POSIX platforms it remains important to (e.g.) the launcher
// API; passing in the `rootfs` flag, for example, will tell the launcher to
// `chroot` to that directory, before launching the process. On Windows, we
// simply conditionally compile out the `rootfs` flag so we can be guaranteed
// to never have to invoke `chroot`.

inline Try<Nothing> chroot(const std::string& directory) = delete;

} // namespace os {


#endif // __STOUT_OS_WINDOWS_CHROOT_HPP__
