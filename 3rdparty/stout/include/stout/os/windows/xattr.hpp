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

#ifndef __STOUT_OS_WINDOWS_XATTR_HPP__
#define __STOUT_OS_WINDOWS_XATTR_HPP__

namespace os {

// NOTE: These functions are deleted because Windows does
// not support POSIX extended attribute semantics.
inline Try<Nothing> setxattr(
    const std::string& path,
    const std::string& name,
    const std::string& value,
    int flags) = delete;


inline Try<std::string> getxattr(
    const std::string& path,
    const std::string& name) = delete;


inline Try<std::string> removexattr(
    const std::string& path,
    const std::string& name) = delete;

} // namespace os {

#endif /* __STOUT_OS_WINDOWS_XATTR_HPP__  */
