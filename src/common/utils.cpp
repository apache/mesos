/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

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
