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

#ifndef __STOUT_OS_POSIX_RMDIR_HPP__
#define __STOUT_OS_POSIX_RMDIR_HPP__

#include <fts.h>
#include <unistd.h>
#include <string>

#include <stout/error.hpp>
#include <stout/nothing.hpp>
#include <stout/try.hpp>


namespace os {

// By default, recursively deletes a directory akin to: 'rm -r'. If the
// programmer sets recursive to false, it deletes a directory akin to: 'rmdir'.
// Note that this function expects an absolute path.
#ifndef __sun // FTS is not available on Solaris.
inline Try<Nothing> rmdir(const std::string& directory, bool recursive = true)
{
  if (!recursive) {
    if (::rmdir(directory.c_str()) < 0) {
      return ErrnoError();
    }
  } else {
    char* paths[] = {const_cast<char*>(directory.c_str()), NULL};

    FTS* tree = fts_open(paths, FTS_NOCHDIR, NULL);
    if (tree == NULL) {
      return ErrnoError();
    }

    FTSENT* node;
    while ((node = fts_read(tree)) != NULL) {
      switch (node->fts_info) {
        case FTS_DP:
          if (::rmdir(node->fts_path) < 0 && errno != ENOENT) {
            Error error = ErrnoError();
            fts_close(tree);
            return error;
          }
          break;
        case FTS_F:
        case FTS_SL:
          if (::unlink(node->fts_path) < 0 && errno != ENOENT) {
            Error error = ErrnoError();
            fts_close(tree);
            return error;
          }
          break;
        default:
          break;
      }
    }

    if (errno != 0) {
      Error error = ErrnoError();
      fts_close(tree);
      return error;
    }

    if (fts_close(tree) < 0) {
      return ErrnoError();
    }
  }

  return Nothing();
}
#endif // __sun

} // namespace os {


#endif // __STOUT_OS_POSIX_RMDIR_HPP__
