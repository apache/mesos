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

#include <mesos/agent/agent.hpp>

#include <mesos/master/master.hpp>

#include <mesos/v1/agent/agent.hpp>

#include <mesos/v1/master/master.hpp>

#include <process/pid.hpp>

#include <stout/check.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>

#include "internal/evolve.hpp"

#include "master/constants.hpp"

using std::string;

using google::protobuf::RepeatedPtrField;

using process::UPID;

namespace mesos {
namespace internal {

// Helper for evolving a type by serializing/parsing when the types
// have not changed across versions.
template <typename T>
static T evolve(const google::protobuf::Message& message)
{
  T t;

  string data;

  // NOTE: We need to use 'SerializePartialToString' instead of
  // 'SerializeToString' because some required fields might not be set
  // and we don't want an exception to get thrown.
  CHECK(message.SerializePartialToString(&data))
    << "Failed to serialize " << message.GetTypeName()
    << " while evolving to " << t.GetTypeName();

  // NOTE: We need to use 'ParsePartialFromString' instead of
  // 'ParseFromString' because some required fields might not
  // be set and we don't want an exception to get thrown.
  CHECK(t.ParsePartialFromString(data))
    << "Failed to parse " << t.GetTypeName()
    << " while evolving from " << message.GetTypeName();

  return t;
}


v1::AgentID evolve(const SlaveID& slaveId)
{
  // NOTE: Not using 'evolve<SlaveID, v1::AgentID>(slaveId)' since
  // this will be a common 'evolve' call and we wanted to speed up
  // performance.

  v1::AgentID id;
  id.set_value(slaveId.value());
  return id;
}


v1::AgentInfo evolve(const SlaveInfo& slaveInfo)
{
  return evolve<v1::AgentInfo>(slaveInfo);
}


v1::ExecutorID evolve(const ExecutorID& executorId)
{
  return evolve<v1::ExecutorID>(executorId);
}


v1::ExecutorInfo evolve(const ExecutorInfo& executorInfo)
{
  return evolve<v1::ExecutorInfo>(executorInfo);
}


v1::FileInfo evolve(const FileInfo& fileInfo)
{
  return evolve<v1::FileInfo>(fileInfo);
}


v1::FrameworkID evolve(const FrameworkID& frameworkId)
{
  return evolve<v1::FrameworkID>(frameworkId);
}


v1::FrameworkInfo evolve(const FrameworkInfo& frameworkInfo)
{
  return evolve<v1::FrameworkInfo>(frameworkInfo);
}


v1::InverseOffer evolve(const InverseOffer& inverseOffer)
{
  return evolve<v1::InverseOffer>(inverseOffer);
}


v1::KillPolicy evolve(const KillPolicy& killPolicy)
{
  return evolve<v1::KillPolicy>(killPolicy);
}


v1::MachineID evolve(const MachineID& machineId)
{
  return evolve<v1::MachineID>(machineId);
}


v1::MasterInfo evolve(const MasterInfo& masterInfo)
{
  return evolve<v1::MasterInfo>(masterInfo);
}


v1::Offer evolve(const Offer& offer)
{
  return evolve<v1::Offer>(offer);
}


v1::OfferID evolve(const OfferID& offerId)
{
  return evolve<v1::OfferID>(offerId);
}


v1::Resource evolve(const Resource& resource)
{
  return evolve<v1::Resource>(resource);
}


v1::Resources evolve(const Resources& resources)
{
  return evolve<v1::Resource>(
      static_cast<const RepeatedPtrField<Resource>&>(resources));
}


v1::Task evolve(const Task& task)
{
  return evolve<v1::Task>(task);
}


v1::TaskID evolve(const TaskID& taskId)
{
  return evolve<v1::TaskID>(taskId);
}


v1::TaskInfo evolve(const TaskInfo& taskInfo)
{
  return evolve<v1::TaskInfo>(taskInfo);
}


v1::TaskStatus evolve(const TaskStatus& status)
{
  return evolve<v1::TaskStatus>(status);
}


v1::agent::Call evolve(const mesos::agent::Call& call)
{
  return evolve<v1::agent::Call>(call);
}


v1::agent::ProcessIO evolve(const mesos::agent::ProcessIO& processIO)
{
  return evolve<v1::agent::ProcessIO>(processIO);
}


v1::agent::Response evolve(const mesos::agent::Response& response)
{
  return evolve<v1::agent::Response>(response);
}


v1::maintenance::ClusterStatus evolve(const maintenance::ClusterStatus& status)
{
  return evolve<v1::maintenance::ClusterStatus>(status);
}


v1::maintenance::Schedule evolve(const maintenance::Schedule& schedule)
{
  return evolve<v1::maintenance::Schedule>(schedule);
}


v1::master::Response evolve(const mesos::master::Response& response)
{
  return evolve<v1::master::Response>(response);
}


v1::scheduler::Call evolve(const scheduler::Call& call)
{
  return evolve<v1::scheduler::Call>(call);
}


v1::scheduler::Event evolve(const scheduler::Event& event)
{
  return evolve<v1::scheduler::Event>(event);
}


v1::scheduler::Event evolve(const ExitedExecutorMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::FAILURE);

  v1::scheduler::Event::Failure* failure = event.mutable_failure();
  failure->mutable_agent_id()->CopyFrom(evolve(message.slave_id()));
  failure->mutable_executor_id()->CopyFrom(evolve(message.executor_id()));
  failure->set_status(message.status());

  return event;
}


v1::scheduler::Event evolve(const ExecutorToFrameworkMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::MESSAGE);

  v1::scheduler::Event::Message* message_ = event.mutable_message();
  message_->mutable_agent_id()->CopyFrom(evolve(message.slave_id()));
  message_->mutable_executor_id()->CopyFrom(evolve(message.executor_id()));
  message_->set_data(message.data());

  return event;
}


v1::scheduler::Event evolve(const FrameworkErrorMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::ERROR);

  v1::scheduler::Event::Error* error = event.mutable_error();
  error->set_message(message.message());

  return event;
}


v1::scheduler::Event evolve(const FrameworkRegisteredMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::SUBSCRIBED);

  v1::scheduler::Event::Subscribed* subscribed = event.mutable_subscribed();
  subscribed->mutable_framework_id()->CopyFrom(evolve(message.framework_id()));

  // TODO(anand): The master should pass the heartbeat interval as an argument
  // to `evolve()`.
  subscribed->set_heartbeat_interval_seconds(
      master::DEFAULT_HEARTBEAT_INTERVAL.secs());

  subscribed->mutable_master_info()->CopyFrom(evolve(message.master_info()));

  return event;
}


v1::scheduler::Event evolve(const FrameworkReregisteredMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::SUBSCRIBED);

  v1::scheduler::Event::Subscribed* subscribed = event.mutable_subscribed();
  subscribed->mutable_framework_id()->CopyFrom(evolve(message.framework_id()));

  // TODO(anand): The master should pass the heartbeat interval as an argument
  // to `evolve()`.
  subscribed->set_heartbeat_interval_seconds(
      master::DEFAULT_HEARTBEAT_INTERVAL.secs());

  subscribed->mutable_master_info()->CopyFrom(evolve(message.master_info()));

  return event;
}


v1::scheduler::Event evolve(const InverseOffersMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::INVERSE_OFFERS);

  v1::scheduler::Event::InverseOffers* inverse_offers =
    event.mutable_inverse_offers();

  inverse_offers->mutable_inverse_offers()->CopyFrom(evolve<v1::InverseOffer>(
      message.inverse_offers()));

  return event;
}


v1::scheduler::Event evolve(const LostSlaveMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::FAILURE);

  v1::scheduler::Event::Failure* failure = event.mutable_failure();
  failure->mutable_agent_id()->CopyFrom(evolve(message.slave_id()));

  return event;
}


v1::scheduler::Event evolve(const ResourceOffersMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::OFFERS);

  v1::scheduler::Event::Offers* offers = event.mutable_offers();
  offers->mutable_offers()->CopyFrom(evolve<v1::Offer>(message.offers()));

  return event;
}


v1::scheduler::Event evolve(const RescindInverseOfferMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::RESCIND_INVERSE_OFFER);

  v1::scheduler::Event::RescindInverseOffer* rescindInverseOffer =
    event.mutable_rescind_inverse_offer();

  rescindInverseOffer->mutable_inverse_offer_id()->CopyFrom(evolve(
      message.inverse_offer_id()));

  return event;
}


v1::scheduler::Event evolve(const RescindResourceOfferMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::RESCIND);

  v1::scheduler::Event::Rescind* rescind = event.mutable_rescind();

  rescind->mutable_offer_id()->CopyFrom(evolve(message.offer_id()));

  return event;
}


v1::scheduler::Event evolve(const StatusUpdateMessage& message)
{
  v1::scheduler::Event event;
  event.set_type(v1::scheduler::Event::UPDATE);

  v1::scheduler::Event::Update* update = event.mutable_update();

  update->mutable_status()->CopyFrom(evolve(message.update().status()));

  if (message.update().has_slave_id()) {
    update->mutable_status()->mutable_agent_id()->CopyFrom(
        evolve(message.update().slave_id()));
  }

  if (message.update().has_executor_id()) {
    update->mutable_status()->mutable_executor_id()->CopyFrom(
        evolve(message.update().executor_id()));
  }

  update->mutable_status()->set_timestamp(message.update().timestamp());

  // If the update does not have a 'uuid', it does not need
  // acknowledging. However, prior to 0.23.0, the update uuid
  // was required and always set. In 0.24.0, we can rely on the
  // update uuid check here, until then we must still check for
  // this being sent from the driver (from == UPID()) or from
  // the master (pid == UPID()).
  // TODO(vinod): Get rid of this logic in 0.25.0 because master
  // and slave correctly set task status in 0.24.0.
  if (!message.update().has_uuid() || message.update().uuid() == "") {
    update->mutable_status()->clear_uuid();
  } else if (UPID(message.pid()) == UPID()) {
    update->mutable_status()->clear_uuid();
  } else {
    update->mutable_status()->set_uuid(message.update().uuid());
  }

  return event;
}


v1::executor::Call evolve(const executor::Call& call)
{
  return evolve<v1::executor::Call>(call);
}


v1::executor::Event evolve(const executor::Event& event)
{
  return evolve<v1::executor::Event>(event);
}


v1::executor::Event evolve(const ExecutorRegisteredMessage& message)
{
  v1::executor::Event event;
  event.set_type(v1::executor::Event::SUBSCRIBED);

  v1::executor::Event::Subscribed* subscribed = event.mutable_subscribed();

  subscribed->mutable_executor_info()->
    CopyFrom(evolve(message.executor_info()));

  subscribed->mutable_framework_info()->
    CopyFrom(evolve(message.framework_info()));

  subscribed->mutable_agent_info()->
    CopyFrom(evolve(message.slave_info()));

  return event;
}


v1::executor::Event evolve(const FrameworkToExecutorMessage& message)
{
  v1::executor::Event event;
  event.set_type(v1::executor::Event::MESSAGE);

  v1::executor::Event::Message* message_ = event.mutable_message();

  message_->set_data(message.data());

  return event;
}


v1::executor::Event evolve(const KillTaskMessage& message)
{
  v1::executor::Event event;
  event.set_type(v1::executor::Event::KILL);

  v1::executor::Event::Kill* kill = event.mutable_kill();

  kill->mutable_task_id()->CopyFrom(evolve(message.task_id()));

  if (message.has_kill_policy()) {
    kill->mutable_kill_policy()->CopyFrom(evolve(message.kill_policy()));
  }

  return event;
}


v1::executor::Event evolve(const RunTaskMessage& message)
{
  v1::executor::Event event;
  event.set_type(v1::executor::Event::LAUNCH);

  v1::executor::Event::Launch* launch = event.mutable_launch();

  launch->mutable_task()->CopyFrom(evolve(message.task()));

  return event;
}


v1::executor::Event evolve(const ShutdownExecutorMessage&)
{
  v1::executor::Event event;
  event.set_type(v1::executor::Event::SHUTDOWN);

  return event;
}


v1::executor::Event evolve(
    const StatusUpdateAcknowledgementMessage& message)
{
  v1::executor::Event event;
  event.set_type(v1::executor::Event::ACKNOWLEDGED);

  v1::executor::Event::Acknowledged* acknowledged =
    event.mutable_acknowledged();

  acknowledged->mutable_task_id()->CopyFrom(evolve(message.task_id()));
  acknowledged->set_uuid(message.uuid());

  return event;
}


v1::master::Event evolve(const mesos::master::Event& event)
{
  return evolve<v1::master::Event>(event);
}


template<>
v1::master::Response evolve<v1::master::Response::GET_FLAGS>(
    const JSON::Object& object)
{
  v1::master::Response response;
  response.set_type(v1::master::Response::GET_FLAGS);

  v1::master::Response::GetFlags* getFlags = response.mutable_get_flags();

  Result<JSON::Object> flags = object.at<JSON::Object>("flags");
  CHECK_SOME(flags) << "Failed to find 'flags' key in the JSON object";

  foreachpair (const string& key,
               const JSON::Value& value,
               flags.get().values) {
    v1::Flag* flag = getFlags->add_flags();
    flag->set_name(key);

    CHECK(value.is<JSON::String>())
      << "Flag '" + key + "' value is not a string";

    flag->set_value(value.as<JSON::String>().value);
  }

  return response;
}


// TODO(vinod): Consolidate master and agent flags evolution.
template<>
v1::agent::Response evolve<v1::agent::Response::GET_FLAGS>(
    const JSON::Object& object)
{
  v1::agent::Response response;
  response.set_type(v1::agent::Response::GET_FLAGS);

  v1::agent::Response::GetFlags* getFlags = response.mutable_get_flags();

  Result<JSON::Object> flags = object.at<JSON::Object>("flags");
  CHECK_SOME(flags) << "Failed to find 'flags' key in the JSON object";

  foreachpair (const string& key,
               const JSON::Value& value,
               flags.get().values) {
    v1::Flag* flag = getFlags->add_flags();
    flag->set_name(key);

    CHECK(value.is<JSON::String>())
      << "Flag '" + key + "' value is not a string";

    flag->set_value(value.as<JSON::String>().value);
  }

  return response;
}


template<>
v1::master::Response evolve<v1::master::Response::GET_VERSION>(
    const JSON::Object& object)
{
  v1::master::Response response;
  response.set_type(v1::master::Response::GET_VERSION);

  Try<v1::VersionInfo> version = ::protobuf::parse<v1::VersionInfo>(object);
  CHECK_SOME(version);

  response.mutable_get_version()->mutable_version_info()->CopyFrom(
      version.get());

  return response;
}


// TODO(vinod): Consolidate master and agent version evolution.
template<>
v1::agent::Response evolve<v1::agent::Response::GET_VERSION>(
    const JSON::Object& object)
{
  v1::agent::Response response;
  response.set_type(v1::agent::Response::GET_VERSION);

  Try<v1::VersionInfo> version = ::protobuf::parse<v1::VersionInfo>(object);
  CHECK_SOME(version);

  response.mutable_get_version()->mutable_version_info()->CopyFrom(
      version.get());

  return response;
}


template<>
v1::agent::Response evolve<v1::agent::Response::GET_CONTAINERS>(
    const JSON::Array& array)
{
  v1::agent::Response response;
  response.set_type(v1::agent::Response::GET_CONTAINERS);

  foreach (const JSON::Value& value, array.values) {
    v1::agent::Response::GetContainers::Container* container =
      response.mutable_get_containers()->add_containers();

    JSON::Object object = value.as<JSON::Object>();

    Result<JSON::String> container_id =
      object.find<JSON::String>("container_id");

    CHECK_SOME(container_id);
    container->mutable_container_id()->set_value(container_id.get().value);

    Result<JSON::String> framework_id =
      object.find<JSON::String>("framework_id");

    CHECK_SOME(framework_id);
    container->mutable_framework_id()->set_value(framework_id.get().value);

    Result<JSON::String> executor_id = object.find<JSON::String>("executor_id");

    CHECK_SOME(executor_id);
    container->mutable_executor_id()->set_value(executor_id.get().value);

    Result<JSON::String> executor_name =
      object.find<JSON::String>("executor_name");

    CHECK_SOME(executor_name);
    container->set_executor_name(executor_name.get().value);

    Result<JSON::Object> container_status = object.find<JSON::Object>("status");
    if (container_status.isSome()) {
      Try<v1::ContainerStatus> status =
        ::protobuf::parse<v1::ContainerStatus>(container_status.get());

      CHECK_SOME(status);
      container->mutable_container_status()->CopyFrom(status.get());
    }

    Result<JSON::Object> resource_statistics =
      object.find<JSON::Object>("statistics");

    if (resource_statistics.isSome()) {
      Try<v1::ResourceStatistics> statistics =
        ::protobuf::parse<v1::ResourceStatistics>(resource_statistics.get());

      CHECK_SOME(statistics);
      container->mutable_resource_statistics()->CopyFrom(statistics.get());
    }
  }

  return response;
}

} // namespace internal {
} // namespace mesos {
