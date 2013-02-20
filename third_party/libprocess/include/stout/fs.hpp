#ifndef __STOUT_FS_HPP__
#define __STOUT_FS_HPP__

#include <unistd.h> // For symlink.

#include <sys/statvfs.h>

#include <string>

#include "error.hpp"
#include "nothing.hpp"
#include "try.hpp"

// TODO(bmahler): Migrate the appropriate 'os' namespace funtions here.
namespace fs {

inline Try<uint64_t> available(const std::string& path = "/")
{
  struct statvfs buf;
  if (::statvfs(path.c_str(), &buf) < 0) {
    return ErrnoError();
  }
  return buf.f_bavail * buf.f_frsize;
}


inline Try<Nothing> symlink(
    const std::string& original,
    const std::string& link)
{
  if (::symlink(original.c_str(), link.c_str()) < 0) {
    return ErrnoError();
  }
  return Nothing();
}

} // namespace fs {

#endif // __STOUT_FS_HPP__
