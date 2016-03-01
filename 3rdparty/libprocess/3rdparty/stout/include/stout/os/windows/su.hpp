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

#ifndef __STOUT_OS_WINDOWS_SU_HPP__
#define __STOUT_OS_WINDOWS_SU_HPP__

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include <stout/windows.hpp>

namespace os {

// NOTE: We delete these functions because they are not meaningful on Windows.
// `su` and `user` are the most important of these functions. The POSIX code
// uses them prodigiously, but in Windows we have been able to divest ourselves
// of all uses.
//
// `su` is important to the launcher API; if the `user` flag (not to be
// confused with the `user` function, which we delete below) is present, we
// will `su` to that user before launching the command. On Windows we avoid
// this problem by simply conditionally compiling out the `user` flag
// altogether, which means that we never have to call `su`.
//
// The `user` function itself is already mostly conditionally compiled out of
// every platform except linux. So in this case it is simply safe to return an
// error on Windows.


inline Result<uid_t> getuid(const Option<std::string>& user = None()) = delete;


inline Result<gid_t> getgid(const Option<std::string>& user = None()) = delete;


inline Result<std::string> user(Option<uid_t> uid = None())
{
  SetLastError(ERROR_NOT_SUPPORTED);
  return WindowsError();
}


inline Try<Nothing> su(const std::string& user) = delete;

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SU_HPP__
