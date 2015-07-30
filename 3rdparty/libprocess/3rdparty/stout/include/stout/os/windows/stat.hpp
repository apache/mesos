/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_OS_WINDOWS_STAT_HPP__
#define __STOUT_OS_WINDOWS_STAT_HPP__

#include <string>

#include <stout/try.hpp>


namespace os {

namespace stat {

inline bool isdir(const std::string& path)
{
  UNIMPLEMENTED;
}


inline bool isfile(const std::string& path)
{
  UNIMPLEMENTED;
}



inline bool islink(const std::string& path)
{
  UNIMPLEMENTED;
}


// Describes the different semantics supported for the implementation
// of `size` defined below.
enum FollowSymlink
{
  DO_NOT_FOLLOW_SYMLINK,
  FOLLOW_SYMLINK
};


// Returns the size in Bytes of a given file system entry. When
// applied to a symbolic link with `follow` set to
// `DO_NOT_FOLLOW_SYMLINK`, this will return the length of the entry
// name (strlen).
inline Try<Bytes> size(
    const std::string& path,
    const FollowSymlink follow = FOLLOW_SYMLINK)
{
  UNIMPLEMENTED;
}


inline Try<long> mtime(const std::string& path)
{
  UNIMPLEMENTED;
}


inline Try<mode_t> mode(const std::string& path)
{
  UNIMPLEMENTED;
}


inline Try<dev_t> rdev(const std::string& path)
{
  UNIMPLEMENTED;
}


inline Try<ino_t> inode(const std::string& path)
{
  UNIMPLEMENTED;
}

} // namespace stat {

} // namespace os {

#endif // __STOUT_OS_WINDOWS_STAT_HPP__
