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

#include <iostream>
#include <queue>
#include <vector>

#include <mesos/type_utils.hpp>

#include <mesos/v1/mesos.hpp>
#include <mesos/v1/resources.hpp>
#include <mesos/v1/scheduler.hpp>

#include <process/delay.hpp>
#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/protobuf.hpp>

#include <stout/check.hpp>
#include <stout/duration.hpp>
#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/hashmap.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>

#include "common/parse.hpp"
#include "common/protobuf_utils.hpp"

#include "hdfs/hdfs.hpp"

#include "internal/devolve.hpp"

#include "logging/logging.hpp"

#include "v1/parse.hpp"

using std::cerr;
using std::cout;
using std::endl;
using std::queue;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using mesos::internal::devolve;

using mesos::v1::AgentID;
using mesos::v1::CapabilityInfo;
using mesos::v1::CheckInfo;
using mesos::v1::CheckStatusInfo;
using mesos::v1::CommandInfo;
using mesos::v1::ContainerInfo;
using mesos::v1::Credential;
using mesos::v1::Environment;
using mesos::v1::ExecutorInfo;
using mesos::v1::FrameworkID;
using mesos::v1::FrameworkInfo;
using mesos::v1::Image;
using mesos::v1::Label;
using mesos::v1::Labels;
using mesos::v1::Offer;
using mesos::v1::Resource;
using mesos::v1::Resources;
using mesos::v1::RLimitInfo;
using mesos::v1::TaskGroupInfo;
using mesos::v1::TaskID;
using mesos::v1::TaskInfo;
using mesos::v1::TaskState;
using mesos::v1::TaskStatus;
using mesos::v1::Volume;

using mesos::v1::scheduler::Call;
using mesos::v1::scheduler::Event;
using mesos::v1::scheduler::Mesos;

using process::Future;
using process::Owned;


class Flags : public virtual flags::FlagsBase
{
public:
  Flags()
  {
    add(&Flags::master,
        "master",
        "Mesos master (e.g., IP:PORT).");

    add(&Flags::task,
        "task",
        "The value could be a JSON-formatted string of `TaskInfo` or a\n"
        "file path containing the JSON-formatted `TaskInfo`. Path must\n"
        "be of the form `file:///path/to/file` or `/path/to/file`.\n"
        "\n"
        "See the `TaskInfo` message in `mesos.proto` for the expected\n"
        "format. NOTE: `agent_id` need not to be set.\n"
        "\n"
        "Example:\n"
        "{\n"
        "  \"name\": \"Name of the task\",\n"
        "  \"task_id\": {\"value\" : \"Id of the task\"},\n"
        "  \"agent_id\": {\"value\" : \"\"},\n"
        "  \"resources\": [\n"
        "    {\n"
        "      \"name\": \"cpus\",\n"
        "      \"type\": \"SCALAR\",\n"
        "      \"scalar\": {\n"
        "        \"value\": 0.1\n"
        "      }\n"
        "    },\n"
        "    {\n"
        "      \"name\": \"mem\",\n"
        "      \"type\": \"SCALAR\",\n"
        "      \"scalar\": {\n"
        "        \"value\": 32\n"
        "      }\n"
        "    }\n"
        "  ],\n"
        "  \"command\": {\n"
        "    \"value\": \"sleep 1000\"\n"
        "  }\n"
        "}");

    add(&Flags::task_group,
        "task_group",
        "The value could be a JSON-formatted string of `TaskGroupInfo` or a\n"
        "file path containing the JSON-formatted `TaskGroupInfo`. Path must\n"
        "be of the form `file:///path/to/file` or `/path/to/file`.\n"
        "\n"
        "See the `TaskGroupInfo` message in `mesos.proto` for the expected\n"
        "format. NOTE: `agent_id` need not to be set.\n"
        "\n"
        "Example:\n"
        "{\n"
        "  \"tasks\":\n"
        "     [\n"
        "        {\n"
        "         \"name\": \"Name of the task\",\n"
        "         \"task_id\": {\"value\" : \"Id of the task\"},\n"
        "         \"agent_id\": {\"value\" : \"\"},\n"
        "         \"resources\": [{\n"
        "            \"name\": \"cpus\",\n"
        "            \"type\": \"SCALAR\",\n"
        "            \"scalar\": {\n"
        "                \"value\": 0.1\n"
        "             }\n"
        "           },\n"
        "           {\n"
        "            \"name\": \"mem\",\n"
        "            \"type\": \"SCALAR\",\n"
        "            \"scalar\": {\n"
        "                \"value\": 32\n"
        "             }\n"
        "          }],\n"
        "         \"command\": {\n"
        "            \"value\": \"sleep 1000\"\n"
        "           }\n"
        "       }\n"
        "     ]\n"
        "}");

    add(&Flags::name,
        "name",
        "Name for the command.");

    add(&Flags::shell,
        "shell",
        "Determine the command is a shell or not. If not, 'command' will be\n"
        "treated as executable value and arguments (TODO).",
        true);

    // TODO(alexr): Once MESOS-4882 lands, elaborate on what `command` can
    // mean: an executable, a shell command, an entrypoint for a container.
    add(&Flags::command,
        "command",
        "Command to launch.");

    add(&Flags::environment,
        "env",
        "Shell command environment variables.\n"
        "The value could be a JSON formatted string of environment variables\n"
        "(i.e., {\"name1\": \"value1\"}) or a file path containing the JSON\n"
        "formatted environment variables. Path should be of the form\n"
        "'file:///path/to/file'.");

    add(&Flags::resources,
        "resources",
        "Resources for the command.",
        "cpus:1;mem:128");

    add(&Flags::hadoop,
        "hadoop",
        "Path to 'hadoop' script (used for copying packages).",
        "hadoop");

    add(&Flags::hdfs,
        "hdfs",
        "The ip:port of the NameNode service.",
        "localhost:9000");

    add(&Flags::package,
        "package",
        "Package to upload into HDFS and copy into command's\n"
        "working directory (requires 'hadoop', see --hadoop).");

    add(&Flags::overwrite,
        "overwrite",
        "Overwrite the package in HDFS if it already exists.",
        false);

    add(&Flags::checkpoint,
        "checkpoint",
        "Enable checkpointing for the framework.",
        false);

    add(&Flags::appc_image,
        "appc_image",
        "Appc image name that follows the Appc spec\n"
        "(e.g., ubuntu, example.com/reduce-worker).");

    add(&Flags::docker_image,
        "docker_image",
        "Docker image that follows the Docker CLI naming <image>:<tag>\n"
        "(i.e., ubuntu, busybox:latest).");

    add(&Flags::framework_capabilities,
        "framework_capabilities",
        "Comma-separated list of optional framework capabilities to enable.\n"
        "RESERVATION_REFINEMENT and TASK_KILLING_STATE are always enabled.\n"
        "PARTITION_AWARE is enabled unless --no-partition-aware is specified.");

    add(&Flags::containerizer,
        "containerizer",
        "Containerizer to be used (i.e., docker, mesos).",
        "mesos");

    add(&Flags::effective_capabilities,
        "effective_capabilities",
        "JSON representation of effective system capabilities that should be\n"
        "granted to the command.\n"
        "\n"
        "Example:\n"
        "{\n"
        "   \"capabilities\": [\n"
        "       \"NET_RAW\",\n"
        "       \"SYS_ADMIN\"\n"
        "     ]\n"
        "}");

    add(&Flags::bounding_capabilities,
        "bounding_capabilities",
        "JSON representation of system capabilities bounding set that should\n"
        "be applied to the command.\n"
        "\n"
        "Example:\n"
        "{\n"
        "   \"capabilities\": [\n"
        "       \"NET_RAW\",\n"
        "       \"SYS_ADMIN\"\n"
        "     ]\n"
        "}");

    add(&Flags::rlimits,
        "rlimits",
        "JSON representation of resource limits for the command. For\n"
        "example, the following sets the limit for CPU time to be one\n"
        "second, and the size of created files to be unlimited:\n"
        "{\n"
        "  \"rlimits\": [\n"
        "    {\n"
        "      \"type\":\"RLMT_CPU\",\n"
        "      \"soft\":\"1\",\n"
        "      \"hard\":\"1\"\n"
        "    },\n"
        "    {\n"
        "      \"type\":\"RLMT_FSIZE\"\n"
        "    }\n"
        "  ]\n"
        "}");

    add(&Flags::role,
        "role",
        "Role to use when registering.",
        "*");

    add(&Flags::kill_after,
        "kill_after",
        "Specifies a delay after which the task is killed\n"
        "(e.g., 10secs, 2mins, etc).");

    add(&Flags::networks,
        "networks",
        "Comma-separated list of networks that the container will join,\n"
        "e.g., `net1,net2`.");

    add(&Flags::principal,
        "principal",
        "The principal to use for framework authentication.");

    add(&Flags::secret,
        "secret",
        "The secret to use for framework authentication.");

    add(&Flags::volumes,
        "volumes",
        "The value could be a JSON-formatted string of volumes or a\n"
        "file path containing the JSON-formatted volumes. Path must\n"
        "be of the form `file:///path/to/file` or `/path/to/file`.\n"
        "\n"
        "See the `Volume` message in `mesos.proto` for the expected format.\n"
        "\n"
        "Example:\n"
        "[\n"
        "  {\n"
        "    \"container_path\":\"/path/to/container\",\n"
        "    \"mode\":\"RW\",\n"
        "    \"source\":\n"
        "    {\n"
        "      \"docker_volume\":\n"
        "        {\n"
        "          \"driver\": \"volume_driver\",\n"
        "          \"driver_options\":\n"
        "            {\"parameter\":[\n"
        "              {\n"
        "                \"key\": \"key\",\n"
        "                \"value\": \"value\"\n"
        "              }\n"
        "            ]},\n"
        "            \"name\": \"volume_name\"\n"
        "        },\n"
        "      \"type\": \"DOCKER_VOLUME\"\n"
        "    }\n"
        "  }\n"
        "]");

    add(&Flags::content_type,
        "content_type",
        "The content type to use for scheduler protocol messages. 'json'\n"
        "and 'protobuf' are valid choices.",
        "protobuf");

    add(&Flags::tty,
        "tty",
        "Attach a TTY to the task being launched",
        false);

    add(&Flags::partition_aware,
        "partition_aware",
        "Enable partition-awareness for the framework.",
        true);
  }

  string master;
  Option<string> name;
  Option<TaskInfo> task;
  Option<TaskGroupInfo> task_group;
  bool shell;
  Option<string> command;
  Option<hashmap<string, string>> environment;
  string resources;
  string hadoop;
  string hdfs;
  Option<string> package;
  bool overwrite;
  bool checkpoint;
  Option<string> appc_image;
  Option<string> docker_image;
  Option<std::set<string>> framework_capabilities;
  Option<JSON::Array> volumes;
  string containerizer;
  Option<CapabilityInfo> effective_capabilities;
  Option<CapabilityInfo> bounding_capabilities;
  Option<RLimitInfo> rlimits;
  string role;
  Option<Duration> kill_after;
  Option<string> networks;
  Option<string> principal;
  Option<string> secret;
  string content_type;
  bool partition_aware;
  bool tty;
};


class CommandScheduler : public process::Process<CommandScheduler>
{
public:
  CommandScheduler(
      const FrameworkInfo& _frameworkInfo,
      const string& _master,
      mesos::ContentType _contentType,
      const Option<Duration>& _killAfter,
      const Option<Credential>& _credential,
      const Option<TaskInfo>& _task,
      const Option<TaskGroupInfo>& _taskGroup,
      const Option<string>& _networks)
    : state(DISCONNECTED),
      frameworkInfo(_frameworkInfo),
      master(_master),
      contentType(_contentType),
      killAfter(_killAfter),
      credential(_credential),
      task(_task),
      taskGroup(_taskGroup),
      networks(_networks),
      launched(false),
      terminatedTaskCount(0) {}

  ~CommandScheduler() override {}

protected:
  void initialize() override
  {
    // We initialize the library here to ensure that callbacks are only invoked
    // after the process has spawned.
    mesos.reset(new Mesos(
      master,
      contentType,
      process::defer(self(), &Self::connected),
      process::defer(self(), &Self::disconnected),
      process::defer(self(), &Self::received, lambda::_1),
      credential));
  }

  void connected()
  {
    state = CONNECTED;

    doReliableRegistration();
  }

  void disconnected()
  {
    state = DISCONNECTED;
  }

  void doReliableRegistration()
  {
    if (state == SUBSCRIBED || state == DISCONNECTED) {
      return;
    }

    Call call;
    call.set_type(Call::SUBSCRIBE);

    if (frameworkInfo.has_id()) {
      call.mutable_framework_id()->CopyFrom(frameworkInfo.id());
    }

    Call::Subscribe* subscribe = call.mutable_subscribe();
    subscribe->mutable_framework_info()->CopyFrom(frameworkInfo);

    mesos->send(call);

    process::delay(Seconds(1), self(), &Self::doReliableRegistration);
  }

  void killTask(const TaskID& taskId, const AgentID& agentId)
  {
    cout << "Asked to kill task '" << taskId
         << "' on agent '" << agentId << "'" << endl;

    Call call;
    call.set_type(Call::KILL);

    CHECK(frameworkInfo.has_id());
    call.mutable_framework_id()->CopyFrom(frameworkInfo.id());

    Call::Kill* kill = call.mutable_kill();
    kill->mutable_task_id()->CopyFrom(taskId);
    kill->mutable_agent_id()->CopyFrom(agentId);

    mesos->send(call);
  }

  void offers(const vector<Offer>& offers)
  {
    CHECK_EQ(SUBSCRIBED, state);

    foreach (const Offer& offer, offers) {
      // Strip the allocation from the offer since we use a single role.
      Resources offered = offer.resources();
      offered.unallocate();

      Resources requiredResources;

      CHECK_NE(task.isSome(), taskGroup.isSome())
        << "Either task or task group should be set but not both";

      if (task.isSome()) {
        requiredResources = Resources(task->resources());
      } else {
        foreach (const TaskInfo& _task, taskGroup->tasks()) {
          requiredResources += Resources(_task.resources());
        }
      }

      if (!launched && offered.toUnreserved().contains(requiredResources)) {
        TaskInfo _task;
        TaskGroupInfo _taskGroup;

        if (task.isSome()) {
          _task = task.get();
          _task.mutable_agent_id()->MergeFrom(offer.agent_id());

          // Takes resources first from the specified role, then from '*'.
          Option<Resources> resources = [&]() {
            if (frameworkInfo.role() == "*") {
              return offered.find(Resources(_task.resources()));
            } else {
              Resource::ReservationInfo reservation;
              reservation.set_type(Resource::ReservationInfo::STATIC);
              reservation.set_role(frameworkInfo.role());

              return offered.find(
                  Resources(_task.resources()).pushReservation(reservation));
            }
          }();

          CHECK_SOME(resources);

          _task.mutable_resources()->CopyFrom(resources.get());
        } else {
          foreach (TaskInfo _task, taskGroup->tasks()) {
            _task.mutable_agent_id()->MergeFrom(offer.agent_id());

            // Takes resources first from the specified role, then from '*'.
            Option<Resources> resources = [&]() {
              if (frameworkInfo.role() == "*") {
                return offered.find(Resources(_task.resources()));
              } else {
                Resource::ReservationInfo reservation;
                reservation.set_type(Resource::ReservationInfo::STATIC);
                reservation.set_role(frameworkInfo.role());

                return offered.find(
                    Resources(_task.resources()).pushReservation(reservation));
              }
            }();

            CHECK_SOME(resources);

            _task.mutable_resources()->CopyFrom(resources.get());
            _taskGroup.add_tasks()->CopyFrom(_task);
          }
       }
       Call call;
       call.set_type(Call::ACCEPT);

       CHECK(frameworkInfo.has_id());
       call.mutable_framework_id()->CopyFrom(frameworkInfo.id());

       Call::Accept* accept = call.mutable_accept();
       accept->add_offer_ids()->CopyFrom(offer.id());

       Offer::Operation* operation = accept->add_operations();

       if (task.isSome()) {
         operation->set_type(Offer::Operation::LAUNCH);
         operation->mutable_launch()->add_task_infos()->CopyFrom(_task);
       } else {
         operation->set_type(Offer::Operation::LAUNCH_GROUP);

         ExecutorInfo* executorInfo =
           operation->mutable_launch_group()->mutable_executor();

         executorInfo->set_type(ExecutorInfo::DEFAULT);
         executorInfo->mutable_executor_id()->set_value(
             "default-executor");

         executorInfo->mutable_framework_id()->CopyFrom(frameworkInfo.id());
         executorInfo->mutable_resources()->CopyFrom(
             Resources::parse("cpus:0.1;mem:32;disk:32").get());

         // Setup any CNI networks that the `task_group` needs to
         // join, in case the `--networks` flag was specified.
         if (networks.isSome() && !networks->empty()) {
           ContainerInfo* containerInfo = executorInfo->mutable_container();
           containerInfo->set_type(ContainerInfo::MESOS);

           foreach (const string& network,
                    strings::tokenize(networks.get(), ",")) {
             containerInfo->add_network_infos()->set_name(network);
           }
         }

         operation->mutable_launch_group()->mutable_task_group()->CopyFrom(
             _taskGroup);
       }

       mesos->send(call);

       if (task.isSome()) {
         cout << "Submitted task '" << task->name() << "' to agent '"
              << offer.agent_id() << "'" << endl;
       } else {
         vector<TaskID> taskIds;

         foreach (const TaskInfo& _task, taskGroup->tasks()) {
           taskIds.push_back(_task.task_id());
         }

         cout << "Submitted task group with tasks "<< taskIds
              << " to agent '" << offer.agent_id() << "'" << endl;
       }

       launched = true;
      } else {
        Call call;
        call.set_type(Call::DECLINE);

        CHECK(frameworkInfo.has_id());
        call.mutable_framework_id()->CopyFrom(frameworkInfo.id());

        Call::Decline* decline = call.mutable_decline();
        decline->add_offer_ids()->CopyFrom(offer.id());

        mesos->send(call);

        call.Clear();
        call.set_type(Call::SUPPRESS);
        call.mutable_framework_id()->CopyFrom(frameworkInfo.id());

        mesos->send(call);
      }
    }
  }

  void received(queue<Event> events)
  {
    while (!events.empty()) {
      Event event = events.front();
      events.pop();

      switch (event.type()) {
        case Event::SUBSCRIBED: {
          frameworkInfo.mutable_id()->
            CopyFrom(event.subscribed().framework_id());

          state = SUBSCRIBED;

          cout << "Subscribed with ID " << frameworkInfo.id() << endl;
          break;
        }

        case Event::OFFERS: {
          offers(google::protobuf::convert(event.offers().offers()));
          break;
        }

        case Event::UPDATE: {
          update(event.update().status());
          break;
        }

        case Event::ERROR: {
          EXIT(EXIT_FAILURE)
            << "Received an ERROR event: " << event.error().message();

          break;
        }

        case Event::HEARTBEAT:
        case Event::INVERSE_OFFERS:
        case Event::FAILURE:
        case Event::RESCIND:
        case Event::RESCIND_INVERSE_OFFER:
        case Event::UPDATE_OPERATION_STATUS:
        case Event::MESSAGE: {
          break;
        }

        case Event::UNKNOWN: {
          LOG(WARNING) << "Received an UNKNOWN event and ignored";
          break;
        }
      }
    }
  }

  void update(const TaskStatus& status)
  {
    CHECK_EQ(SUBSCRIBED, state);

    cout << "Received status update " << status.state()
         << " for task '" << status.task_id() << "'" << endl;

    if (status.has_message()) {
      cout << "  message: '" << status.message() << "'" << endl;
    }
    if (status.has_source()) {
      cout << "  source: " << TaskStatus::Source_Name(status.source()) << endl;
    }
    if (status.has_reason()) {
      cout << "  reason: " << TaskStatus::Reason_Name(status.reason()) << endl;
    }
    if (status.has_healthy()) {
      cout << "  healthy?: " << status.healthy() << endl;
    }
    if (status.has_check_status()) {
      cout << "  check status: " << status.check_status() << endl;
    }
    if (status.has_limitation() && !status.limitation().resources().empty()) {
      cout << "  resource limit violation: "
           << status.limitation().resources() << endl;
    }

    if (status.has_uuid()) {
      Call call;
      call.set_type(Call::ACKNOWLEDGE);

      CHECK(frameworkInfo.has_id());
      call.mutable_framework_id()->CopyFrom(frameworkInfo.id());

      Call::Acknowledge* acknowledge = call.mutable_acknowledge();
      acknowledge->mutable_agent_id()->CopyFrom(status.agent_id());
      acknowledge->mutable_task_id()->CopyFrom(status.task_id());
      acknowledge->set_uuid(status.uuid());

      mesos->send(call);
    }

    // If a task kill delay has been specified, schedule task kill.
    if (killAfter.isSome() && TaskState::TASK_RUNNING == status.state()) {
      delay(killAfter.get(),
            self(),
            &Self::killTask,
            status.task_id(),
            status.agent_id());
    }

    if (mesos::internal::protobuf::isTerminalState(devolve(status).state())) {
      CHECK_NE(task.isSome(), taskGroup.isSome())
        << "Either task or task group should be set but not both";

      if (task.isSome()) {
        terminate(self());
      } else {
        terminatedTaskCount++;

        if (terminatedTaskCount == taskGroup->tasks().size()) {
          terminate(self());
        }
      }
    }
  }

private:
  enum State
  {
    DISCONNECTED,
    CONNECTED,
    SUBSCRIBED
  } state;

  FrameworkInfo frameworkInfo;
  const string master;
  mesos::ContentType contentType;
  const Option<Duration> killAfter;
  const Option<Credential> credential;
  const Option<TaskInfo> task;
  const Option<TaskGroupInfo> taskGroup;
  const Option<string> networks;
  bool launched;
  int terminatedTaskCount;
  Owned<Mesos> mesos;
};


// TODO(jojy): Consider breaking down the method for each 'containerizer'.
static Result<ContainerInfo> getContainerInfo(
    const string& containerizer,
    const Option<vector<Volume>>& volumes,
    const Option<string>& networks,
    const Option<string>& appcImage,
    const Option<string>& dockerImage,
    const Option<CapabilityInfo>& effective_capabilities,
    const Option<CapabilityInfo>& bounding_capabilities,
    const Option<RLimitInfo>& rlimits,
    const bool tty)
{
  if (containerizer.empty()) {
    return None();
  }

  ContainerInfo containerInfo;

  if (volumes.isSome()) {
    foreach (const Volume& volume, volumes.get()) {
      containerInfo.add_volumes()->CopyFrom(volume);
    }
  }

  // Mesos containerizer supports 'appc' and 'docker' images.
  if (containerizer == "mesos") {
    if (!tty &&
        appcImage.isNone() &&
        dockerImage.isNone() &&
        effective_capabilities.isNone() &&
        bounding_capabilities.isNone() &&
        rlimits.isNone() &&
        (networks.isNone() || networks->empty()) &&
        (volumes.isNone() || volumes->empty())) {
      return None();
    }

    containerInfo.set_type(ContainerInfo::MESOS);

    if (dockerImage.isSome()) {
      Image* image = containerInfo.mutable_mesos()->mutable_image();
      image->set_type(Image::DOCKER);
      image->mutable_docker()->set_name(dockerImage.get());
    } else if (appcImage.isSome()) {
      Image::Appc appc;

      appc.set_name(appcImage.get());

      // TODO(jojy): Labels are hard coded right now. Consider
      // adding label flags for customization.
      Label arch;
      arch.set_key("arch");
      arch.set_value("amd64");

      Label os;
      os.set_key("os");
      os.set_value("linux");

      Labels labels;
      labels.add_labels()->CopyFrom(os);
      labels.add_labels()->CopyFrom(arch);

      appc.mutable_labels()->CopyFrom(labels);

      Image* image = containerInfo.mutable_mesos()->mutable_image();
      image->set_type(Image::APPC);
      image->mutable_appc()->CopyFrom(appc);
    }

    if (networks.isSome() && !networks->empty()) {
      foreach (const string& network,
               strings::tokenize(networks.get(), ",")) {
        containerInfo.add_network_infos()->set_name(network);
      }
    }

    if (effective_capabilities.isSome()) {
      containerInfo
        .mutable_linux_info()
        ->mutable_effective_capabilities()
        ->CopyFrom(effective_capabilities.get());
    }

    if (bounding_capabilities.isSome()) {
      containerInfo
        .mutable_linux_info()
        ->mutable_bounding_capabilities()
        ->CopyFrom(bounding_capabilities.get());
    }

    if (rlimits.isSome()) {
      containerInfo.mutable_rlimit_info()->CopyFrom(rlimits.get());
    }

    if (tty) {
      containerInfo.mutable_tty_info();
    }

    return containerInfo;
  } else if (containerizer == "docker") {
    // 'docker' containerizer only supports 'docker' images.
    if (dockerImage.isNone()) {
      return Error("'Docker' containerizer requires docker image name");
    }

    containerInfo.set_type(ContainerInfo::DOCKER);
    containerInfo.mutable_docker()->set_image(dockerImage.get());

    if (networks.isSome() && !networks->empty()) {
      vector<string> tokens = strings::tokenize(networks.get(), ",");
      if (tokens.size() > 1) {
        EXIT(EXIT_FAILURE)
          << "'Docker' containerizer can only support a single network";
      } else {
        containerInfo.mutable_docker()->set_network(
            ContainerInfo::DockerInfo::USER);
        containerInfo.add_network_infos()->set_name(tokens.front());
      }
    }

    if (tty) {
      return Error("'Docker' containerizer does not allow attaching a TTY");
    }

    return containerInfo;
  }

  return Error("Unsupported containerizer: " + containerizer);
}


int main(int argc, char** argv)
{
  Flags flags;
  mesos::ContentType contentType = mesos::ContentType::PROTOBUF;

  // Load flags from command line only.
  Try<flags::Warnings> load = flags.load(None(), argc, argv);

  // TODO(marco): this should be encapsulated entirely into the
  // FlagsBase API - possibly with a 'guard' that prevents FlagsBase
  // from calling ::exit(EXIT_FAILURE) after calling usage() (which
  // would be the default behavior); see MESOS-2766.
  if (flags.help) {
    cout << flags.usage() << endl;
    return EXIT_SUCCESS;
  }

  if (load.isError()) {
    cerr << flags.usage(load.error()) << endl;
    return EXIT_FAILURE;
  }

  mesos::internal::logging::initialize(argv[0], false);

  // Log any flag warnings.
  foreach (const flags::Warning& warning, load->warnings) {
    LOG(WARNING) << warning.message;
  }

  if (flags.task.isSome() && flags.task_group.isSome()) {
    EXIT(EXIT_FAILURE) << flags.usage(
        "Either task or task group should be set but not both."
        " Provide either '--task' OR '--task_group'");
  } else if (flags.task.isNone() && flags.task_group.isNone()) {
    if (flags.name.isNone()) {
      EXIT(EXIT_FAILURE) << flags.usage("Missing required option --name");
    }

    if (flags.shell && flags.command.isNone()) {
      EXIT(EXIT_FAILURE) << flags.usage("Missing required option --command");
    }
  } else {
    // Either --task or --task_group is set.
    if (flags.name.isSome() ||
        flags.command.isSome() ||
        flags.environment.isSome() ||
        flags.appc_image.isSome()  ||
        flags.docker_image.isSome() ||
        flags.volumes.isSome()) {
      EXIT(EXIT_FAILURE) << flags.usage(
          "'--name, --command, --env, --appc_image, --docker_image,"
          " --volumes' can only be set when both '--task'"
          " and '--task_group' are not set");
    }

    if (flags.task.isSome() && flags.networks.isSome()) {
      EXIT(EXIT_FAILURE) << flags.usage(
          "'--networks' can only be set when"
          " '--task' is not set");
    }
  }

  if (flags.content_type == "json" ||
      flags.content_type == mesos::APPLICATION_JSON) {
    contentType = mesos::ContentType::JSON;
  } else if (flags.content_type == "protobuf" ||
             flags.content_type == mesos::APPLICATION_PROTOBUF) {
    contentType = mesos::ContentType::PROTOBUF;
  } else {
    EXIT(EXIT_FAILURE) << "Invalid content type '" << flags.content_type << "'";
  }

  Result<string> user = os::user();
  if (!user.isSome()) {
    if (user.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to get username: " << user.error();
    } else {
#ifndef __WINDOWS__
      EXIT(EXIT_FAILURE) << "No username for uid " << ::getuid();
#else
      // NOTE: The `::getuid()` function does not exist on Windows.
      EXIT(EXIT_FAILURE) << "No username for current user";
#endif // __WINDOWS__
    }
  }

  Option<hashmap<string, string>> environment = None();

  if (flags.environment.isSome()) {
    environment = flags.environment.get();
  }

  // Copy the package to HDFS, if requested. Save its location
  // as a URI for passing to the command (in CommandInfo).
  Option<string> uri = None();

  if (flags.package.isSome()) {
    Try<Owned<HDFS>> hdfs = HDFS::create(flags.hadoop);
    if (hdfs.isError()) {
      EXIT(EXIT_FAILURE) << "Failed to create HDFS client: " << hdfs.error();
    }

    // TODO(benh): If HDFS is not properly configured with
    // 'fs.default.name' then we'll copy to the local
    // filesystem. Currently this will silently fail on our end (i.e.,
    // the 'copyFromLocal' will be successful) but we'll fail to
    // download the URI when we launch the executor (unless it's
    // already been uploaded before ...).

    // Store the file at '/user/package'.
    string path = path::join("/", user.get(), flags.package.get());

    // Check if the file exists and remove it if we're overwriting.
    Future<bool> exists = hdfs.get()->exists(path);
    exists.await();

    if (!exists.isReady()) {
      EXIT(EXIT_FAILURE)
        << "Failed to check if file exists: "
        << (exists.isFailed() ? exists.failure() : "discarded");
    } else if (exists.get() && flags.overwrite) {
      Future<Nothing> rm = hdfs.get()->rm(path);
      rm.await();

      if (!rm.isReady()) {
        EXIT(EXIT_FAILURE)
          << "Failed to remove existing file: "
          << (rm.isFailed() ? rm.failure() : "discarded");
      }
    } else if (exists.get()) {
      EXIT(EXIT_FAILURE) << "File already exists (see --overwrite)";
    }

    Future<Nothing> copy = hdfs.get()->copyFromLocal(flags.package.get(), path);
    copy.await();

    if (!copy.isReady()) {
      EXIT(EXIT_FAILURE)
        << "Failed to copy package: "
        << (copy.isFailed() ? copy.failure() : "discarded");
    }

    // Now save the URI.
    uri = "hdfs://" + flags.hdfs + path;
  }

  Option<string> appcImage;
  if (flags.appc_image.isSome()) {
    appcImage = flags.appc_image.get();
  }

  Option<string> dockerImage;
  if (flags.docker_image.isSome()) {
    dockerImage = flags.docker_image.get();
  }

  if (appcImage.isSome() && dockerImage.isSome()) {
    EXIT(EXIT_FAILURE)
      << "Flags '--docker-image' and '--appc-image' are both set";
  }

  // Always enable the following capabilities.
  vector<FrameworkInfo::Capability::Type> frameworkCapabilities = {
    FrameworkInfo::Capability::RESERVATION_REFINEMENT,
    FrameworkInfo::Capability::TASK_KILLING_STATE,
    FrameworkInfo::Capability::REVOCABLE_RESOURCES,
  };

  // Enable PARTITION_AWARE unless disabled by the user.
  if (flags.partition_aware) {
    frameworkCapabilities.push_back(
        FrameworkInfo::Capability::PARTITION_AWARE);
  }

  if (flags.framework_capabilities.isSome()) {
    foreach (const string& capability, flags.framework_capabilities.get()) {
      FrameworkInfo::Capability::Type type;

      if (!FrameworkInfo::Capability::Type_Parse(capability, &type)) {
        EXIT(EXIT_FAILURE)
          << "Flags '--framework_capabilities' specifies an unknown"
          << " capability '" << capability << "'";
      }

      if (type != FrameworkInfo::Capability::GPU_RESOURCES) {
        EXIT(EXIT_FAILURE)
          << "Flags '--framework_capabilities' specifies an unsupported"
          << " capability '" << capability << "'";
      }

      frameworkCapabilities.push_back(type);
    }
  }

  Option<vector<Volume>> volumes = None();

  if (flags.volumes.isSome()) {
    Try<RepeatedPtrField<Volume>> parse =
      ::protobuf::parse<RepeatedPtrField<Volume>>(flags.volumes.get());

    if (parse.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to convert '--volumes' to protobuf: " << parse.error();
    }

    vector<Volume> _volumes;

    foreach (const Volume& volume, parse.get()) {
      _volumes.push_back(volume);
    }

    volumes = _volumes;
  }

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_user(user.get());
  frameworkInfo.set_name("mesos-execute instance");
  frameworkInfo.set_role(flags.role);
  frameworkInfo.set_checkpoint(flags.checkpoint);
  foreach (const FrameworkInfo::Capability::Type& capability,
           frameworkCapabilities) {
    frameworkInfo.add_capabilities()->set_type(capability);
  }

  Option<Credential> credential = None();

  if (flags.principal.isSome()) {
    frameworkInfo.set_principal(flags.principal.get());

    if (flags.secret.isSome()) {
      Credential credential_;
      credential_.set_principal(flags.principal.get());
      credential_.set_secret(flags.secret.get());
      credential = credential_;
    }
  }

  Option<TaskInfo> taskInfo = flags.task;

  if (flags.task.isNone() && flags.task_group.isNone()) {
    TaskInfo task;
    task.set_name(flags.name.get());
    task.mutable_task_id()->set_value(flags.name.get());

    static const Try<Resources> resources = Resources::parse(flags.resources);

    if (resources.isError()) {
      EXIT(EXIT_FAILURE)
        << "Failed to parse resources '" << flags.resources << "': "
        << resources.error();
    }

    task.mutable_resources()->CopyFrom(resources.get());

    CommandInfo* commandInfo = task.mutable_command();

    if (flags.shell) {
      CHECK_SOME(flags.command);

      commandInfo->set_shell(true);
      commandInfo->set_value(flags.command.get());
    } else {
      // TODO(gilbert): Treat 'command' as executable value and arguments.
      commandInfo->set_shell(false);
    }

    if (flags.environment.isSome()) {
      Environment* environment_ = commandInfo->mutable_environment();
      foreachpair (
          const string& name, const string& value, environment.get()) {
        Environment::Variable* environmentVariable =
          environment_->add_variables();

        environmentVariable->set_name(name);
        environmentVariable->set_value(value);
      }
    }

    if (uri.isSome()) {
      task.mutable_command()->add_uris()->set_value(uri.get());
    }

    Result<ContainerInfo> containerInfo =
      getContainerInfo(
        flags.containerizer,
        volumes,
        flags.networks,
        appcImage,
        dockerImage,
        flags.effective_capabilities,
        flags.bounding_capabilities,
        flags.rlimits,
        flags.tty);

    if (containerInfo.isError()){
      EXIT(EXIT_FAILURE) << containerInfo.error();
    }

    if (containerInfo.isSome()) {
      task.mutable_container()->CopyFrom(containerInfo.get());
    }

    taskInfo = task;
  }

  Owned<CommandScheduler> scheduler(
      new CommandScheduler(
        frameworkInfo,
        flags.master,
        contentType,
        flags.kill_after,
        credential,
        taskInfo,
        flags.task_group,
        flags.networks));

  process::spawn(scheduler.get());
  process::wait(scheduler.get());

  return EXIT_SUCCESS;
}
