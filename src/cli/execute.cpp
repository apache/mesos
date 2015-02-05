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

#include <iostream>
#include <vector>

#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <process/pid.hpp>

#include <stout/check.hpp>
#include <stout/flags.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>

#include "common/type_utils.hpp"
#include "common/protobuf_utils.hpp"

#include "hdfs/hdfs.hpp"

using namespace mesos;

using process::UPID;

using std::cerr;
using std::cout;
using std::endl;
using std::string;
using std::vector;


void usage(const char* argv0, const flags::FlagsBase& flags)
{
  cerr << "Usage: " << os::basename(argv0).get() << " [...]" << endl
       << endl
       << "Supported options:" << endl
       << flags.usage();
}


class Flags : public flags::FlagsBase
{
public:
  Flags()
  {
    add(&master,
        "master",
        "Mesos master (e.g., IP1:PORT1)");

    add(&name,
        "name",
        "Name for the command");

    add(&command,
        "command",
        "Shell command to launch");

    add(&resources,
        "resources",
        "Resources for the command",
        "cpus:1;mem:128");

    add(&hadoop,
        "hadoop",
        "Path to `hadoop' script (used for copying packages)",
        "hadoop");

    add(&hdfs,
        "hdfs",
        "The ip:port of the NameNode service",
        "localhost:9000");

    add(&package,
        "package",
        "Package to upload into HDFS and copy into command's\n"
        "working directory (requires `hadoop', see --hadoop)");

    add(&overwrite,
        "overwrite",
        "Overwrite the package in HDFS if it already exists",
        false);

    add(&checkpoint,
        "checkpoint",
        "Enable checkpointing for the framework (requires slave checkpointing)",
        false);

    add(&docker_image,
        "docker_image",
        "Docker image that follows the Docker CLI naming <image>:<tag>."
        "(ie: ubuntu, busybox:latest).");
  }

  Option<string> master;
  Option<string> name;
  Option<string> command;
  string resources;
  string hadoop;
  string hdfs;
  Option<string> package;
  bool overwrite;
  bool checkpoint;
  Option<string> docker_image;
};


class CommandScheduler : public Scheduler
{
public:
  CommandScheduler(
      const string& _name,
      const string& _command,
      const string& _resources,
      const Option<string>& _uri,
      const Option<string>& _dockerImage)
    : name(_name),
      command(_command),
      resources(_resources),
      uri(_uri),
      dockerImage(_dockerImage),
      launched(false) {}

  virtual ~CommandScheduler() {}

  virtual void registered(
      SchedulerDriver* _driver,
      const FrameworkID& _frameworkId,
      const MasterInfo& _masterInfo) {
    cout << "Framework registered with " << _frameworkId << endl;
  }

  virtual void reregistered(
      SchedulerDriver* _driver,
      const MasterInfo& _masterInfo) {
    cout << "Framework re-registered" << endl;
  }

  virtual void disconnected(
      SchedulerDriver* driver) {}

  virtual void resourceOffers(
      SchedulerDriver* driver,
      const vector<Offer>& offers)
  {
    static const Try<Resources> TASK_RESOURCES = Resources::parse(resources);

    if (TASK_RESOURCES.isError()) {
      cerr << "Failed to parse resources '" << resources
           << "': " << TASK_RESOURCES.error() << endl;
      driver->abort();
      return;
    }

    foreach (const Offer& offer, offers) {
      if (!launched &&
          Resources(offer.resources()).contains(TASK_RESOURCES.get())) {
        TaskInfo task;
        task.set_name(name);
        task.mutable_task_id()->set_value(name);
        task.mutable_slave_id()->MergeFrom(offer.slave_id());
        task.mutable_resources()->CopyFrom(TASK_RESOURCES.get());
        task.mutable_command()->set_value(command);
        if (uri.isSome()) {
          task.mutable_command()->add_uris()->set_value(uri.get());
        }

        if (dockerImage.isSome()) {
          ContainerInfo containerInfo;
          containerInfo.set_type(ContainerInfo::DOCKER);

          ContainerInfo::DockerInfo dockerInfo;
          dockerInfo.set_image(dockerImage.get());

          containerInfo.mutable_docker()->CopyFrom(dockerInfo);
          task.mutable_container()->CopyFrom(containerInfo);
        }

        vector<TaskInfo> tasks;
        tasks.push_back(task);

        driver->launchTasks(offer.id(), tasks);
        cout << "task " << name << " submitted to slave "
             << offer.slave_id() << endl;

        launched = true;
      } else {
        driver->declineOffer(offer.id());
      }
    }
  }

  virtual void offerRescinded(
      SchedulerDriver* driver,
      const OfferID& offerId) {}

  virtual void statusUpdate(
      SchedulerDriver* driver,
      const TaskStatus& status)
  {
    CHECK_EQ(name, status.task_id().value());
    cout << "Received status update " << status.state()
         << " for task " << status.task_id() << endl;
    if (protobuf::isTerminalState(status.state())) {
      driver->stop();
    }
  }

  virtual void frameworkMessage(
      SchedulerDriver* driver,
      const ExecutorID& executorId,
      const SlaveID& slaveId,
      const string& data) {}

  virtual void slaveLost(
      SchedulerDriver* driver,
      const SlaveID& sid) {}

  virtual void executorLost(
      SchedulerDriver* driver,
      const ExecutorID& executorID,
      const SlaveID& slaveID,
      int status) {}

  virtual void error(
      SchedulerDriver* driver,
      const string& message) {}

private:
  const string name;
  const string command;
  const string resources;
  const Option<string> uri;
  const Option<string> dockerImage;
  bool launched;
};


int main(int argc, char** argv)
{
  Flags flags;

  bool help;
  flags.add(&help,
            "help",
            "Prints this help message",
            false);

  // Load flags from environment and command line.
  Try<Nothing> load = flags.load(None(), argc, argv);

  if (load.isError()) {
    cerr << load.error() << endl;
    usage(argv[0], flags);
    return -1;
  }

  if (help) {
    usage(argv[0], flags);
    return -1;
  }

  if (flags.master.isNone()) {
    cerr << "Missing --master=IP:PORT" << endl;
    usage(argv[0], flags);
    return -1;
  }

  UPID master("master@" + flags.master.get());

  if (!master) {
    cerr << "Could not parse --master=" << flags.master.get() << endl;
    usage(argv[0], flags);
    return -1;
  }

  if (flags.name.isNone()) {
    cerr << "Missing --name=NAME" << endl;
    usage(argv[0], flags);
    return -1;
  }

  if (flags.command.isNone()) {
    cerr << "Missing --command=COMMAND" << endl;
    usage(argv[0], flags);
    return -1;
  }

  Result<string> user = os::user();
  if (!user.isSome()) {
    if (user.isError()) {
      cerr << "Failed to get username: " << user.error() << endl;
    } else {
      cerr << "No username for uid " << ::getuid() << endl;
    }
    return -1;
  }

  // Copy the package to HDFS if requested save it's location as a URI
  // for passing to the command (in CommandInfo).
  Option<string> uri = None();

  if (flags.package.isSome()) {
    HDFS hdfs(flags.hadoop);

    // TODO(benh): If HDFS is not properly configured with
    // 'fs.default.name' then we'll copy to the local
    // filesystem. Currently this will silently fail on our end (i.e.,
    // the 'copyFromLocal' will be sucessful) but we'll fail to
    // download the URI when we launch the executor (unless it's
    // already been uploaded before ...).

    // Store the file at '/user/package'.
    string path = path::join("/", user.get(), flags.package.get());

    // Check if the file exists and remove it if we're overwriting.
    Try<bool> exists = hdfs.exists(path);
    if (exists.isError()) {
      cerr << "Failed to check if file exists: " << exists.error() << endl;
      return -1;
    } else if (exists.get() && flags.overwrite) {
      Try<Nothing> rm = hdfs.rm(path);
      if (rm.isError()) {
        cerr << "Failed to remove existing file: " << rm.error() << endl;
        return -1;
      }
    } else if (exists.get()) {
      cerr << "File already exists (see --overwrite)" << endl;
      return -1;
    }

    Try<Nothing> copy = hdfs.copyFromLocal(flags.package.get(), path);
    if (copy.isError()) {
      cerr << "Failed to copy package: " << copy.error() << endl;
      return -1;
    }

    // Now save the URI.
    uri = "hdfs://" + flags.hdfs + path;
  }

  Option<string> dockerImage;

  if (flags.docker_image.isSome()) {
    dockerImage = flags.docker_image.get();
  }

  CommandScheduler scheduler(
      flags.name.get(),
      flags.command.get(),
      flags.resources,
      uri,
      dockerImage);

  FrameworkInfo framework;
  framework.set_user(user.get());
  framework.set_name("");
  framework.set_checkpoint(flags.checkpoint);

  MesosSchedulerDriver driver(&scheduler, framework, flags.master.get());

  return driver.run() == DRIVER_STOPPED ? 0 : 1;
}
