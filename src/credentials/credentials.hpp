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
#include <stout/path.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace credentials {

inline Result<Credentials> read(const Path& path)
{
  LOG(INFO) << "Loading credentials for authentication from '" << path << "'";

  Try<std::string> read = os::read(path.value);
  if (read.isError()) {
    return Error("Failed to read credentials file '" + path.value +
                 "': " + read.error());
  } else if (read.get().empty()) {
    return None();
  }

  Try<os::Permissions> permissions = os::permissions(path.value);
  if (permissions.isError()) {
    LOG(WARNING) << "Failed to stat credentials file '" << path
                 << "': " << permissions.error();
  } else if (permissions.get().others.rwx) {
    LOG(WARNING) << "Permissions on credentials file '" << path
                 << "' are too open. It is recommended that your "
                 << "credentials file is NOT accessible by others.";
  }

  // TODO(ijimenez): Deprecate text and support only JSON like ACLs.
  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  if (!json.isError()) {
    Try<Credentials> credentials = ::protobuf::parse<Credentials>(json.get());
    if (!credentials.isError()) {
      return credentials.get();
    }
  }

  Credentials credentials;
  foreach (const std::string& line, strings::tokenize(read.get(), "\n")) {
    const std::vector<std::string>& pairs = strings::tokenize(line, " ");
    if (pairs.size() != 2) {
        return Error("Invalid credential format at line " +
                     stringify(credentials.credentials().size() + 1));
    }

    // Add the credential.
    Credential* credential = credentials.add_credentials();
    credential->set_principal(pairs[0]);
    credential->set_secret(pairs[1]);
  }
  return credentials;
}


inline Result<Credential> readCredential(const Path& path)
{
  LOG(INFO) << "Loading credential for authentication from '" << path << "'";

  Try<std::string> read = os::read(path.value);
  if (read.isError()) {
    return Error("Failed to read credential file '" + path.value +
                 "': " + read.error());
  } else if (read.get().empty()) {
    return None();
  }

  Try<os::Permissions> permissions = os::permissions(path.value);
  if (permissions.isError()) {
    LOG(WARNING) << "Failed to stat credential file '" << path
                 << "': " << permissions.error();
  } else if (permissions.get().others.rwx) {
    LOG(WARNING) << "Permissions on credential file '" << path
                 << "' are too open. It is recommended that your "
                 << "credential file is NOT accessible by others.";
  }

  // TODO(ijimenez): Deprecate text support for only JSON ACLs.
  Try<JSON::Object> json = JSON::parse<JSON::Object>(read.get());
  if (!json.isError()) {
    Try<Credential> credential = ::protobuf::parse<Credential>(json.get());
    if (!credential.isError()) {
      return credential.get();
    }
  }

  Credential credential;
  const std::vector<std::string>& line = strings::tokenize(read.get(), "\n");
  if (line.size() != 1) {
    return Error("Expecting only one credential");
  }
  const std::vector<std::string>& pairs = strings::tokenize(line[0], " ");
  if (pairs.size() != 2) {
    return Error("Invalid credential format");
  }
  // Add the credential.
  credential.set_principal(pairs[0]);
  credential.set_secret(pairs[1]);
  return credential;
}

} // namespace credentials {
} // namespace mesos {

#endif // __CREDENTIALS_HPP__
