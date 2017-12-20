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

#include <string>

#include <mesos/mesos.hpp>

#include <mesos/module/volume_profile.hpp>

#include <mesos/resource_provider/storage/volume_profile.hpp>

#include <process/future.hpp>

#include <stout/hashset.hpp>

#include <csi/spec.hpp>

#include "module/manager.hpp"

using std::string;
using std::tuple;

using process::Failure;
using process::Future;

using google::protobuf::Map;

namespace mesos {
namespace internal {

// The default implementation does nothing and always returns a Failure
// whenever called.
class DefaultVolumeProfileAdaptor : public VolumeProfileAdaptor
{
public:
  DefaultVolumeProfileAdaptor() {}

  ~DefaultVolumeProfileAdaptor() {}

  virtual Future<VolumeProfileAdaptor::ProfileInfo> translate(
      const string& profile,
      const string& csiPluginInfoType) override
  {
    return Failure("By default, volume profiles are not supported");
  }

  virtual Future<hashset<string>> watch(
      const hashset<string>& knownProfiles,
      const string& csiPluginInfoType) override
  {
    // If the input set of profiles is empty, that means the caller is in sync
    // with this module. Hence, we return a future that will never be satisified
    // because this module will never return a non-empty set of profiles.
    if (knownProfiles.empty()) {
      return Future<hashset<string>>();
    }

    return hashset<string>::EMPTY;
  }
};

} // namespace internal {


Try<VolumeProfileAdaptor*> VolumeProfileAdaptor::create(
    const Option<string>& moduleName)
{
  if (moduleName.isNone()) {
    LOG(INFO) << "Creating default volume profile adaptor module";
    return new internal::DefaultVolumeProfileAdaptor();
  }

  LOG(INFO)
    << "Creating volume profile adaptor module '" << moduleName.get() << "'";

  Try<VolumeProfileAdaptor*> result =
    modules::ModuleManager::create<VolumeProfileAdaptor>(moduleName.get());

  if (result.isError()) {
    return Error(
        "Failed to initialize volume profile adaptor module: "
        + result.error());
  }

  return result;
}


// NOTE: This is a pointer because we avoid using non-POD types
// as global variables.
//
// NOTE: This is a `weak_ptr` because the ownership of the module should
// belong to the caller of the `create` method above. This will, for example,
// allow tests to instantiate an Agent and subsequently destruct the Agent
// without leaving a module behind in a global variable.
static std::weak_ptr<VolumeProfileAdaptor>* currentAdaptor = nullptr;


void VolumeProfileAdaptor::setAdaptor(
    const std::shared_ptr<VolumeProfileAdaptor>& adaptor)
{
  if (currentAdaptor != nullptr) {
    delete currentAdaptor;
  }

  currentAdaptor = new std::weak_ptr<VolumeProfileAdaptor>(adaptor);
}


std::shared_ptr<VolumeProfileAdaptor> VolumeProfileAdaptor::getAdaptor()
{
  // This method should never be called before `setAdaptor` has been called.
  CHECK_NOTNULL(currentAdaptor);

  return currentAdaptor->lock();
}

} // namespace mesos {
