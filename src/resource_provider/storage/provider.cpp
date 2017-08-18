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

#include <glog/logging.h>

#include <process/defer.hpp>
#include <process/id.hpp>
#include <process/process.hpp>

#include <mesos/resource_provider/resource_provider.hpp>

#include <mesos/v1/resource_provider.hpp>

#include "internal/devolve.hpp"

#include "resource_provider/detector.hpp"

using std::queue;

using process::Owned;
using process::Process;

using process::defer;
using process::spawn;
using process::terminate;
using process::wait;

using mesos::ResourceProviderInfo;

using mesos::resource_provider::Event;

using mesos::v1::resource_provider::Driver;

namespace mesos {
namespace internal {

class StorageLocalResourceProviderProcess
  : public Process<StorageLocalResourceProviderProcess>
{
public:
  explicit StorageLocalResourceProviderProcess(
      const process::http::URL& _url,
      const ResourceProviderInfo& _info)
    : ProcessBase(process::ID::generate("storage-local-resource-provider")),
      url(_url),
      contentType(ContentType::PROTOBUF),
      info(_info) {}

  StorageLocalResourceProviderProcess(
      const StorageLocalResourceProviderProcess& other) = delete;

  StorageLocalResourceProviderProcess& operator=(
      const StorageLocalResourceProviderProcess& other) = delete;

  void connected();
  void disconnected();
  void received(const Event& event);

private:
  void initialize() override;

  const process::http::URL url;
  const ContentType contentType;
  ResourceProviderInfo info;
  Owned<v1::resource_provider::Driver> driver;
};


void StorageLocalResourceProviderProcess::connected()
{
}


void StorageLocalResourceProviderProcess::disconnected()
{
}


void StorageLocalResourceProviderProcess::received(const Event& event)
{
  // TODO(jieyu): Print resource provider ID.
  LOG(INFO) << "Received " << event.type() << " event";

  switch (event.type()) {
    case Event::SUBSCRIBED: {
      break;
    }
    case Event::OPERATION: {
      CHECK(event.has_operation());
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
}


Try<Owned<LocalResourceProvider>> StorageLocalResourceProvider::create(
    const process::http::URL& url,
    const ResourceProviderInfo& info)
{
  return Owned<LocalResourceProvider>(
      new StorageLocalResourceProvider(url, info));
}


StorageLocalResourceProvider::StorageLocalResourceProvider(
    const process::http::URL& url,
    const ResourceProviderInfo& info)
  : process(new StorageLocalResourceProviderProcess(url, info))
{
  spawn(CHECK_NOTNULL(process.get()));
}


StorageLocalResourceProvider::~StorageLocalResourceProvider()
{
  terminate(process.get());
  wait(process.get());
}

} // namespace internal {
} // namespace mesos {
