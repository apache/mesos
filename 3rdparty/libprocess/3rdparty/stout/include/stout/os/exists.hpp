#ifndef __STOUT_OS_EXISTS_HPP__	
#define __STOUT_OS_EXISTS_HPP__	

#include <sys/stat.h>

#include <string>

namespace os {

inline bool exists(const std::string& path)
{
  struct stat s;
  if (::lstat(path.c_str(), &s) < 0) {
    return false;
  }
  return true;
}

} // namespace os {

#endif // __STOUT_OS_EXISTS_HPP__
