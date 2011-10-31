#include <ftw.h>

#include "common/utils.hpp"

namespace mesos {
namespace internal {
namespace utils {
namespace os {

static int remove(const char* path,
                  const struct stat* s,
                  int type,
                  FTW* ftw)
{
  if (type == FTW_F || type == FTW_SL) {
    return ::unlink(path);
  } else {
    return ::rmdir(path);
  }
}


bool rmdir(const std::string& directory)
{
  int result = nftw(directory.c_str(), remove, 1,
      FTW_DEPTH | FTW_PHYS | FTW_CHDIR);
  return result == 0;
}

} // namespace os
} // namespace utils
} // namespace internal
} // namespace mesos
