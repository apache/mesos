#ifndef __STOUT_FS_HPP__
#define __STOUT_FS_HPP__

#include <errno.h>

#include <sys/statvfs.h>

#include <cstring>
#include <string>

#include "try.hpp"

// TODO(bmahler): Migrate the appropriate 'os' namespace funtions here.
namespace fs {


inline Try<uint64_t> available(const std::string& path = "/")
{
  struct statvfs buf;

  if (statvfs(path.c_str(), &buf) < 0) {
    return Try<uint64_t>::error(strerror(errno));
  }
  return buf.f_bavail * buf.f_frsize;
}


} // namespace fs {

#endif // __STOUT_FS_HPP__
