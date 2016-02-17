// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __PROVISIONER_APPC_FETCHER_HPP__
#define __PROVISIONER_APPC_FETCHER_HPP__

#include <string>

#include <process/process.hpp>
#include <process/shared.hpp>

#include <stout/path.hpp>

#include "slave/flags.hpp"

#include "uri/fetcher.hpp"

namespace mesos {
namespace internal {
namespace slave {
namespace appc {

class Fetcher
{
public:
  /**
   * Factory method for creating the fetcher component.
   *
   * @param flags Slave flags.
   * @param fetcher Shared pointer to the uri fetcher.
   */
  static Try<process::Owned<Fetcher>> create(
      const Flags& flags,
      const process::Shared<uri::Fetcher>& fetcher);

  /*
   * Fetches Appc image to the given directory.
   *
   * Reference: https://github.com/appc/spec/blob/master/spec/discovery.md
   *
   * @param image Encapsulated information about the appc image.
   * @param directory Path of directory where the image has to be saved.
   * @returns Nothing on success.
   *          Failure in case of any error.
   */
  process::Future<Nothing> fetch(
      const Image::Appc& appc,
      const Path& directory);

private:
  Fetcher(
      const std::string& uriPrefix,
      const process::Shared<uri::Fetcher>& fetcher);

  Fetcher(const Fetcher&) = delete;
  Fetcher& operator=(const Fetcher&) = delete;

  const std::string uriPrefix;
  process::Shared<uri::Fetcher> fetcher;
};

} // namespace appc {
} // namespace slave {
} // namespace internal {
} // namespace mesos {

#endif // __PROVISIONER_APPC_FETCHER_HPP__
