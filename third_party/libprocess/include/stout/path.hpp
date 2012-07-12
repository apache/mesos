#ifndef __STOUT_PATH_HPP__
#define __STOUT_PATH_HPP__

#include <string>

#include "strings.hpp"

namespace path {

inline std::string join(const std::string& path1, const std::string& path2)
{
  return
    strings::remove(path1, "/", strings::SUFFIX) + "/" +
    strings::remove(path2, "/", strings::PREFIX);
}

// TODO(benh): Provide implementations for more than two paths.

} // namespace path {

#endif // __STOUT_PATH_HPP__
