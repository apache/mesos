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

#ifndef __STOUT_OS_POSIX_MKDTEMP_HPP__
#define __STOUT_OS_POSIX_MKDTEMP_HPP__

#include <stdlib.h>
#include <string.h>

#include <string>

#include <stout/error.hpp>
#include <stout/try.hpp>


namespace os {

// Creates a temporary directory using the specified path
// template. The template may be any path with _6_ `Xs' appended to
// it, for example /tmp/temp.XXXXXX. The trailing `Xs' are replaced
// with a unique alphanumeric combination.
inline Try<std::string> mkdtemp(const std::string& path = "/tmp/XXXXXX")
{
  char* temp = new char[path.size() + 1];
  ::memcpy(temp, path.c_str(), path.size() + 1);

  if (::mkdtemp(temp) != nullptr) {
    std::string result(temp);
    delete[] temp;
    return result;
  } else {
    delete[] temp;
    return ErrnoError();
  }
}

} // namespace os {


#endif // __STOUT_OS_POSIX_MKDTEMP_HPP__
