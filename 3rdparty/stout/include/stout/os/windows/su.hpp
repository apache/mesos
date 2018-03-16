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
#include <vector>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/result.hpp>
#include <stout/try.hpp>

#include <stout/windows.hpp>

// Include for `GetUserNameEx`. `SECURITY_WIN32` or `SECURITY_KERNEL` must be
// defined to include `SecExt.h`, which defines `GetUserNameEx` (the choice
// depends on whether you want the functions defined to be usermode or kernel
// operations). We include `security.h` instead of `SecExt.h` because comments
// in this header indicate that it should only be included from `security.h`.
// Finally, we `#undef` to avoid accidentally interfering with Windows headers
// that might be sensitive to `SECURITY_WIN32`.
#if defined(SECURITY_WIN32) || defined(SECURITY_KERNEL)
#include <security.h>
#else
#define SECURITY_WIN32
#include <security.h>
#undef SECURITY_WIN32
#endif // SECURITY_WIN32 || SECURITY_KERNEL


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


// Returns the SAM account name for the current user. This username is
// unprocessed, meaning it contains punctuation, possibly including '\'.
// NOTE: The `uid` parameter is unsupported on Windows, and will result in an
// error.
inline Result<std::string> user(Option<uid_t> uid = None())
{
  if (uid.isSome()) {
    return Error(
        "os::user: Retrieving user information via uid "
        "is not supported on Windows");
  }

  EXTENDED_NAME_FORMAT username_format = NameSamCompatible;
  ULONG buffer_size = 0;
  if (::GetUserNameExW(username_format, nullptr, &buffer_size) == FALSE) {
    if (::GetLastError() != ERROR_MORE_DATA) {
      return WindowsError("os::user: Failed to get buffer size for username");
    }
  }

  std::vector<wchar_t> user_name;
  user_name.reserve(buffer_size);
  if (::GetUserNameExW(username_format, user_name.data(), &buffer_size)
      == FALSE) {
    return WindowsError("os::user: Failed to get username from OS");
  }

  return stringify(std::wstring(user_name.data()));
}


inline Try<Nothing> su(const std::string& user) = delete;

} // namespace os {

#endif // __STOUT_OS_WINDOWS_SU_HPP__
