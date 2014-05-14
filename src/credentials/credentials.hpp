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

#ifndef __CREDENTIALS_HPP__
#define __CREDENTIALS_HPP__

#include <string>
#include <vector>

#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace credentials {

inline Result<std::vector<Credential> > read(const std::string& path)
{
  LOG(INFO) << "Loading credentials for authentication from '" << path << "'";

  Try<std::string> read = os::read(path);
  if (read.isError()) {
    return Error("Failed to read credentials file '" + path +
                 "': " + read.error());
  } else if (read.get().empty()) {
    return None();
  }

  Try<os::Permissions> permissions = os::permissions(path);
  if (permissions.isError()) {
    LOG(WARNING) << "Failed to stat credentials file '" << path
                 << "': " << permissions.error();
  } else if (permissions.get().others.rwx) {
    LOG(WARNING) << "Permissions on credentials file '" << path
                 << "' are too open. It is recommended that your "
                 << "credentials file is NOT accessible by others.";
  }

  std::vector<Credential> credentials;
  foreach (const std::string& line, strings::tokenize(read.get(), "\n")) {
    const std::vector<std::string>& pairs = strings::tokenize(line, " ");
    if (pairs.size() != 2) {
      return Error("Invalid credential format at line: " +
                   stringify(credentials.size() + 1));
    }

    // Add the credential.
    Credential credential;
    credential.set_principal(pairs[0]);
    credential.set_secret(pairs[1]);
    credentials.push_back(credential);
  }

  return credentials;
}

} // namespace credentials {
} // namespace internal {
} // namespace mesos {

#endif // __CREDENTIALS_HPP__
