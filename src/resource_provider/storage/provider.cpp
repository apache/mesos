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

#include "resource_provider/storage/provider.hpp"

#include <cctype>

#include <glog/logging.h>

#include <process/defer.hpp>
#include <process/delay.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/v1/resource_provider.hpp>

#include <stout/path.hpp>

#include <stout/os/realpath.hpp>

#include "common/http.hpp"

#include "internal/devolve.hpp"
#include "internal/evolve.hpp"

#include "resource_provider/detector.hpp"

#include "slave/paths.hpp"

namespace http = process::http;

using std::queue;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;

using process::defer;
using process::spawn;

using process::http::authentication::Principal;

using mesos::ResourceProviderInfo;

using mesos::resource_provider::Call;
using mesos::resource_provider::Event;

using mesos::v1::resource_provider::Driver;

namespace mesos {
namespace internal {

// Returns true if the string is a valid Java identifier.
static bool isValidName(const string& s)
{
  if (s.empty()) {
    return false;
  }

  foreach (const char c, s) {
    if (!isalnum(c) && c != '_') {
      return false;
    }
  }

  return true;
}


// Returns true if the string is a valid Java package name.
static bool isValidType(const string& s)
{
  if (s.empty()) {
    return false;
  }

  foreach (const string& token, strings::split(s, ".")) {
    if (!isValidName(token)) {
      return false;
    }
  }

  return true;
}


// Returns a prefix for naming standalone containers to run CSI plugins
// for the resource provider. The prefix is of the following format:
//     <rp_type>-<rp_name>--
// where <rp_type> and <rp_name> are the type and name of the resource
// provider, with dots replaced by dashes. We use a double-dash at the
// end to explicitly mark the end of the prefix.
static inline string getContainerIdPrefix(const ResourceProviderInfo& info)
{
  return strings::join(
      "-",
      strings::replace(info.type(), ".", "-"),
      info.name(),
      "-");
}


class StorageLocalResourceProviderProcess
  : public Process<StorageLocalResourceProviderProcess>
{
public:
  explicit StorageLocalResourceProviderProcess(
      const http::URL& _url,
      const string& _workDir,
      const ResourceProviderInfo& _info,
      const SlaveID& _slaveId,
      const Option<string>& _authToken)
    : ProcessBase(process::ID::generate("storage-local-resource-provider")),
      state(RECOVERING),
      url(_url),
      workDir(_workDir),
      metaDir(slave::paths::getMetaRootDir(_workDir)),
      contentType(ContentType::PROTOBUF),
      info(_info),
      slaveId(_slaveId),
      authToken(_authToken) {}

  StorageLocalResourceProviderProcess(
      const StorageLocalResourceProviderProcess& other) = delete;

  StorageLocalResourceProviderProcess& operator=(
      const StorageLocalResourceProviderProcess& other) = delete;

  void connected();
  void disconnected();
  void received(const Event& event);

private:
  void initialize() override;
  void fatal(const string& messsage, const string& failure);

  Future<Nothing> recover();
  void doReliableRegistration();

  // Functions for received events.
  void subscribed(const Event::Subscribed& subscribed);
  void operation(const Event::Operation& operation);
  void publish(const Event::Publish& publish);

  enum State
  {
    RECOVERING,
    DISCONNECTED,
    CONNECTED,
    SUBSCRIBED,
    READY
  } state;

  const http::URL url;
  const string workDir;
  const string metaDir;
  const ContentType contentType;
  ResourceProviderInfo info;
  const SlaveID slaveId;
  const Option<string> authToken;

  Owned<v1::resource_provider::Driver> driver;
};


void StorageLocalResourceProviderProcess::connected()
{
  CHECK_EQ(DISCONNECTED, state);

  state = CONNECTED;

  doReliableRegistration();
}


void StorageLocalResourceProviderProcess::disconnected()
{
  CHECK(state == CONNECTED || state == SUBSCRIBED || state == READY);

  LOG(INFO) << "Disconnected from resource provider manager";

  state = DISCONNECTED;
}


void StorageLocalResourceProviderProcess::received(const Event& event)
{
  LOG(INFO) << "Received " << event.type() << " event";

  switch (event.type()) {
    case Event::SUBSCRIBED: {
      CHECK(event.has_subscribed());
      subscribed(event.subscribed());
      break;
    }
    case Event::OPERATION: {
      CHECK(event.has_operation());
      operation(event.operation());
      break;
    }
    case Event::PUBLISH: {
      CHECK(event.has_publish());
      publish(event.publish());
      break;
    }
    case Event::UNKNOWN: {
      LOG(WARNING) << "Received an UNKNOWN event and ignored";
      break;
    }
  }
}


void StorageLocalResourceProviderProcess::initialize()
{
  const string message =
    "Failed to recover resource provider with type '" + info.type() +
    "' and name '" + info.name() + "'";

  recover()
    .onFailed(defer(self(), &Self::fatal, message, lambda::_1))
    .onDiscarded(defer(self(), &Self::fatal, message, "future discarded"));
}


void StorageLocalResourceProviderProcess::fatal(
    const string& message,
    const string& failure)
{
  LOG(ERROR) << message << ": " << failure;

  // Force the disconnection early.
  driver.reset();

  process::terminate(self());
}


Future<Nothing> StorageLocalResourceProviderProcess::recover()
{
  CHECK_EQ(RECOVERING, state);

  // Recover the resource provider ID from the latest symlink. If the
  // symlink does not exist or it points to a non-exist directory,
  // treat this as a new resource provider.
  // TODO(chhsiao): State recovery.
  Result<string> realpath = os::realpath(
      slave::paths::getLatestResourceProviderPath(
          metaDir, slaveId, info.type(), info.name()));

  if (realpath.isError()) {
    return Failure(
        "Failed to read the latest symlink for resource provider with type '" +
        info.type() + "' and name '" + info.name() + "': " + realpath.error());
  }

  if (realpath.isSome()) {
    info.mutable_id()->set_value(Path(realpath.get()).basename());
  }

  state = DISCONNECTED;

  driver.reset(new Driver(
      Owned<EndpointDetector>(new ConstantEndpointDetector(url)),
      contentType,
      defer(self(), &Self::connected),
      defer(self(), &Self::disconnected),
      defer(self(), [this](queue<v1::resource_provider::Event> events) {
        while(!events.empty()) {
          const v1::resource_provider::Event& event = events.front();
          received(devolve(event));
          events.pop();
        }
      }),
      None())); // TODO(nfnt): Add authentication as part of MESOS-7854.

  return Nothing();
}


void StorageLocalResourceProviderProcess::doReliableRegistration()
{
  if (state == DISCONNECTED || state == SUBSCRIBED || state == READY) {
    return;
  }

  CHECK_EQ(CONNECTED, state);

  const string message =
    "Failed to subscribe resource provider with type '" + info.type() +
    "' and name '" + info.name() + "'";

  Call call;
  call.set_type(Call::SUBSCRIBE);

  Call::Subscribe* subscribe = call.mutable_subscribe();
  subscribe->mutable_resource_provider_info()->CopyFrom(info);

  driver->send(evolve(call));

  // TODO(chhsiao): Consider doing an exponential backoff.
  delay(Seconds(1), self(), &Self::doReliableRegistration);
}


void StorageLocalResourceProviderProcess::subscribed(
    const Event::Subscribed& subscribed)
{
  CHECK_EQ(CONNECTED, state);

  LOG(INFO) << "Subscribed with ID " << subscribed.provider_id().value();

  state = SUBSCRIBED;

  if (!info.has_id()) {
    // New subscription.
    info.mutable_id()->CopyFrom(subscribed.provider_id());
    slave::paths::createResourceProviderDirectory(
        metaDir,
        slaveId,
        info.type(),
        info.name(),
        info.id());
  }
}


void StorageLocalResourceProviderProcess::operation(
    const Event::Operation& operation)
{
  if (state == SUBSCRIBED) {
    // TODO(chhsiao): Reject this operation.
    return;
  }

  CHECK_EQ(READY, state);
}


void StorageLocalResourceProviderProcess::publish(const Event::Publish& publish)
{
  if (state == SUBSCRIBED) {
    // TODO(chhsiao): Reject this publish request.
    return;
  }

  CHECK_EQ(READY, state);
}


Try<Owned<LocalResourceProvider>> StorageLocalResourceProvider::create(
    const http::URL& url,
    const string& workDir,
    const ResourceProviderInfo& info,
    const SlaveID& slaveId,
    const Option<string>& authToken)
{
  // Verify that the name follows Java package naming convention.
  // TODO(chhsiao): We should move this check to a validation function
  // for `ResourceProviderInfo`.
  if (!isValidName(info.name())) {
    return Error(
        "Resource provider name '" + info.name() +
        "' does not follow Java package naming convention");
  }

  if (!info.has_storage()) {
    return Error("'ResourceProviderInfo.storage' must be set");
  }

  // Verify that the type and name of the CSI plugin follow Java package
  // naming convention.
  // TODO(chhsiao): We should move this check to a validation function
  // for `CSIPluginInfo`.
  if (!isValidType(info.storage().type()) ||
      !isValidName(info.storage().name())) {
    return Error(
        "CSI plugin type '" + info.storage().type() +
        "' and name '" + info.storage().name() +
        "' does not follow Java package naming convention");
  }

  bool hasControllerService = false;
  bool hasNodeService = false;

  foreach (const CSIPluginContainerInfo& container,
           info.storage().containers()) {
    for (int i = 0; i < container.services_size(); i++) {
      const CSIPluginContainerInfo::Service service = container.services(i);
      if (service == CSIPluginContainerInfo::CONTROLLER_SERVICE) {
        hasControllerService = true;
      } else if (service == CSIPluginContainerInfo::NODE_SERVICE) {
        hasNodeService = true;
      }
    }
  }

  if (!hasControllerService) {
    return Error(
        stringify(CSIPluginContainerInfo::CONTROLLER_SERVICE) + " not found");
  }

  if (!hasNodeService) {
    return Error(
        stringify(CSIPluginContainerInfo::NODE_SERVICE) + " not found");
  }

  return Owned<LocalResourceProvider>(
      new StorageLocalResourceProvider(url, workDir, info, slaveId, authToken));
}


Try<Principal> StorageLocalResourceProvider::principal(
    const ResourceProviderInfo& info)
{
  return Principal(
      Option<string>::none(),
      {{"cid_prefix", getContainerIdPrefix(info)}});
}


StorageLocalResourceProvider::StorageLocalResourceProvider(
    const http::URL& url,
    const string& workDir,
    const ResourceProviderInfo& info,
    const SlaveID& slaveId,
    const Option<string>& authToken)
  : process(new StorageLocalResourceProviderProcess(
        url, workDir, info, slaveId, authToken))
{
  spawn(CHECK_NOTNULL(process.get()));
}


StorageLocalResourceProvider::~StorageLocalResourceProvider()
{
  process::terminate(process.get());
  process::wait(process.get());
}

} // namespace internal {
} // namespace mesos {
