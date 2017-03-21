// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_OS_POSIX_XATTR_HPP__
#define __STOUT_OS_POSIX_XATTR_HPP__

#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/extattr.h>
#else
#include <sys/xattr.h>
#endif

#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>

namespace os {

inline Try<Nothing> setxattr(
    const std::string& path,
    const std::string& name,
    const std::string& value,
    int flags)
{
#ifdef __APPLE__
  if (::setxattr(
      path.c_str(),
      name.c_str(),
      value.c_str(),
      value.length(),
      0,
      flags) < 0) {
#elif __FreeBSD__
  if (::extattr_set_file(
        path.c_str(),
        EXTATTR_NAMESPACE_USER,
        name.c_str(),
        value.c_str(),
        value.length()) < 0) {
#else
  if (::setxattr(
        path.c_str(),
        name.c_str(),
        value.c_str(),
        value.length(),
        flags) < 0) {
#endif
    return ErrnoError();
  }

  return Nothing();
}


inline Try<std::string> getxattr(
    const std::string& path,
    const std::string& name)
{
  // Get the current size of the attribute.
#ifdef __APPLE__
  ssize_t size = ::getxattr(path.c_str(), name.c_str(), nullptr, 0, 0, 0);
#elif __FreeBSD__
  ssize_t size = ::extattr_get_file(path.c_str(),
                                    EXTATTR_NAMESPACE_USER,
                                    name.c_str(),
                                    nullptr,
                                    0);
#else
  ssize_t size = ::getxattr(path.c_str(), name.c_str(), nullptr, 0);
#endif
  if (size < 0) {
    return ErrnoError();
  }

  char* temp = new char[size + 1];
  ::memset(temp, 0, (size_t)size + 1);

#ifdef __APPLE__
  if (::getxattr(path.c_str(), name.c_str(), temp, (size_t)size, 0, 0) < 0) {
#elif __FreeBSD__
  if (::extattr_get_file(
              path.c_str(),
              EXTATTR_NAMESPACE_USER,
              name.c_str(),
              temp,
              (size_t)size) < 0) {
#else
  if (::getxattr(path.c_str(), name.c_str(), temp, (size_t)size) < 0) {
#endif
    delete[] temp;
    return ErrnoError();
  }

  std::string result(temp);
  delete[] temp;

  return result;
}


inline Try<Nothing> removexattr(
    const std::string& path,
    const std::string& name)
{
#ifdef __APPLE__
  if (::removexattr(path.c_str(), name.c_str(), 0) < 0) {
#elif __FreeBSD__
  if (::extattr_delete_file(path.c_str(),
                            EXTATTR_NAMESPACE_USER,
                            name.c_str())) {
#else
  if (::removexattr(path.c_str(), name.c_str()) < 0) {
#endif
    return ErrnoError();
  }

  return Nothing();
}

} // namespace os {

#endif /* __STOUT_OS_POSIX_XATTR_HPP__  */
