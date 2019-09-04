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

#ifndef __RESOURCE_PROVIDER_STORAGE_PROVIDER_HPP__
#define __RESOURCE_PROVIDER_STORAGE_PROVIDER_HPP__

#include <string>

#include <mesos/mesos.hpp>

#include <process/http.hpp>
#include <process/owned.hpp>

#include <stout/error.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include "resource_provider/local.hpp"

namespace mesos {
namespace internal {

// Forward declarations.
class StorageLocalResourceProviderProcess;


class StorageLocalResourceProvider : public LocalResourceProvider
{
public:
  static Try<process::Owned<LocalResourceProvider>> create(
      const process::http::URL& url,
      const std::string& workDir,
      const ResourceProviderInfo& info,
      const SlaveID& slaveId,
      const Option<std::string>& authToken,
      bool strict);

  static Option<Error> validate(const ResourceProviderInfo& info);

  ~StorageLocalResourceProvider() override;

  StorageLocalResourceProvider(
      const StorageLocalResourceProvider& other) = delete;

  StorageLocalResourceProvider& operator=(
      const StorageLocalResourceProvider& other) = delete;

private:
  explicit StorageLocalResourceProvider(
      const process::http::URL& url,
      const std::string& workDir,
      const ResourceProviderInfo& info,
      const SlaveID& slaveId,
      const Option<std::string>& authToken,
      bool strict);

  process::Owned<StorageLocalResourceProviderProcess> process;
};

} // namespace internal {
} // namespace mesos {

#endif // __RESOURCE_PROVIDER_STORAGE_PROVIDER_HPP__
