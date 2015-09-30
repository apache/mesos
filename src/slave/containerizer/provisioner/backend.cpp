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

#include <glog/logging.h>

#include <stout/os.hpp>

#include "slave/containerizer/provisioner/backend.hpp"

#include "slave/containerizer/provisioner/backends/bind.hpp"
#include "slave/containerizer/provisioner/backends/copy.hpp"

using namespace process;

using std::string;

namespace mesos {
namespace internal {
namespace slave {

hashmap<string, Owned<Backend>> Backend::create(const Flags& flags)
{
  hashmap<string, Try<Owned<Backend>>(*)(const Flags&)> creators;

#ifdef __linux__
  creators.put("bind", &BindBackend::create);
#endif // __linux__
  creators.put("copy", &CopyBackend::create);

  hashmap<string, Owned<Backend>> backends;

  foreachkey (const string& name, creators) {
    Try<Owned<Backend>> backend = creators[name](flags);
    if (backend.isError()) {
      LOG(WARNING) << "Failed to create '" << name << "' backend: "
                   << backend.error();
      continue;
    }
    backends.put(name, backend.get());
  }

  return backends;
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
