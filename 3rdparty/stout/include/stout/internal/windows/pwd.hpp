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

#ifndef __STOUT_INTERNAL_WINDOWS_PWD_HPP__
#define __STOUT_INTERNAL_WINDOWS_PWD_HPP__

#include <sys/types.h>

#include <stout/windows.hpp>


// Dummy struct for POSIX compliance.
struct passwd
{
  char* pw_name;  // User's login name.
  uid_t pw_uid;   // Numerical user ID.
  gid_t pw_gid;   // Numerical group ID.
  char* pw_dir;   // Initial working directory.
  char* pw_shell; // Program to use as shell.
};


// Dummy implementation of `getpwuid` for POSIX compliance. Per the POSIX
// specification[1], we are to return `nullptr` if an entry matching the UID is
// not found. On Windows, we will never find such an entry, so we always return
// `nullptr`. Just to be safe, we also set `errno` to `ENOSYS` which indicates
// the function is not implemented.
//
// [1] http://pubs.opengroup.org/onlinepubs/009695399/functions/getgrgid.html
inline struct passwd* getpwuid(uid_t)
{
  errno = ENOSYS;
  return nullptr;
}

#endif // __STOUT_INTERNAL_WINDOWS_PWD_HPP__
