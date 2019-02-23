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

#ifndef __LINUX_MEMFD_HPP__
#define __LINUX_MEMFD_HPP__

#include <string>

#include <stout/try.hpp>

#include <stout/os/int_fd.hpp>

#if !defined(MFD_CLOEXEC)
#define MFD_CLOEXEC 0x0001U
#endif

#if !defined(MFD_ALLOW_SEALING)
#define MFD_ALLOW_SEALING 0x0002U
#endif

namespace mesos {
namespace internal {
namespace memfd {

// Creates an anonymous in-memory file via `memfd_create`.
Try<int_fd> create(const std::string& name, unsigned int flags);


// Clone a file into a sealed private copy such that any attempt to
// modify it will not modify the original binary. Returns the memfd of
// the sealed copy.
Try<int_fd> cloneSealedFile(const std::string& filePath);

} // namespace memfd {
} // namespace internal {
} // namespace mesos {

#endif // __LINUX_MEMFD_HPP__
