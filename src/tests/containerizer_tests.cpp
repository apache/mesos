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

#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include "slave/flags.hpp"

#include "slave/containerizer/mesos_containerizer.hpp"

using namespace mesos;
using namespace mesos::internal;
using namespace mesos::internal::slave;

using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

// Forward declaration.
map<string, string> fetcherEnvironment(
    const CommandInfo& commandInfo,
    const string& directory,
    const Option<string>& user,
    const Flags& flags);

}  // namespace slave {
}  // namespace internal {
}  // namespace mesos {

class MesosContainerizerProcessTest : public ::testing::Test {};


TEST_F(MesosContainerizerProcessTest, Simple)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri");
  uri.set_executable(false);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, user, flags);
  EXPECT_EQ(5u, environment.size());
  EXPECT_EQ("hdfs:///uri+0X", environment["MESOS_EXECUTOR_URIS"]);
  EXPECT_EQ(directory, environment["MESOS_WORK_DIRECTORY"]);
  EXPECT_EQ(user.get(), environment["MESOS_USER"]);
  EXPECT_EQ(flags.frameworks_home, environment["MESOS_FRAMEWORKS_HOME"]);
  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);
}


TEST_F(MesosContainerizerProcessTest, MultipleURIs)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri1");
  uri.set_executable(false);
  commandInfo.add_uris()->MergeFrom(uri);
  uri.set_value("hdfs:///uri2");
  uri.set_executable(true);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";
  Option<string> user("user");

  Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, user, flags);

  EXPECT_EQ(5u, environment.size());
  EXPECT_EQ(
      "hdfs:///uri1+0X hdfs:///uri2+1X", environment["MESOS_EXECUTOR_URIS"]);
  EXPECT_EQ(directory, environment["MESOS_WORK_DIRECTORY"]);
  EXPECT_EQ(user.get(), environment["MESOS_USER"]);
  EXPECT_EQ(flags.frameworks_home, environment["MESOS_FRAMEWORKS_HOME"]);
  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);
}


TEST_F(MesosContainerizerProcessTest, NoUser)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri");
  uri.set_executable(false);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";

  Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, None(), flags);

  EXPECT_EQ(4u, environment.size());
  EXPECT_EQ("hdfs:///uri+0X", environment["MESOS_EXECUTOR_URIS"]);
  EXPECT_EQ(directory, environment["MESOS_WORK_DIRECTORY"]);
  EXPECT_EQ(flags.frameworks_home, environment["MESOS_FRAMEWORKS_HOME"]);
  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);
}


TEST_F(MesosContainerizerProcessTest, EmptyHadoop)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri");
  uri.set_executable(false);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "";

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, user, flags);

  EXPECT_EQ(4u, environment.size());
  EXPECT_EQ("hdfs:///uri+0X", environment["MESOS_EXECUTOR_URIS"]);
  EXPECT_EQ(directory, environment["MESOS_WORK_DIRECTORY"]);
  EXPECT_EQ(user.get(), environment["MESOS_USER"]);
  EXPECT_EQ(flags.frameworks_home, environment["MESOS_FRAMEWORKS_HOME"]);
}


TEST_F(MesosContainerizerProcessTest, NoHadoop)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri");
  uri.set_executable(false);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  Flags flags;
  flags.frameworks_home = "/tmp/frameworks";

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, user, flags);

  EXPECT_EQ(4u, environment.size());
  EXPECT_EQ("hdfs:///uri+0X", environment["MESOS_EXECUTOR_URIS"]);
  EXPECT_EQ(directory, environment["MESOS_WORK_DIRECTORY"]);
  EXPECT_EQ(user.get(), environment["MESOS_USER"]);
  EXPECT_EQ(flags.frameworks_home, environment["MESOS_FRAMEWORKS_HOME"]);
}


TEST_F(MesosContainerizerProcessTest, NoExtract)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri");
  uri.set_executable(false);
  uri.set_extract(false);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, user, flags);
  EXPECT_EQ(5u, environment.size());
  EXPECT_EQ("hdfs:///uri+0N", environment["MESOS_EXECUTOR_URIS"]);
  EXPECT_EQ(directory, environment["MESOS_WORK_DIRECTORY"]);
  EXPECT_EQ(user.get(), environment["MESOS_USER"]);
  EXPECT_EQ(flags.frameworks_home, environment["MESOS_FRAMEWORKS_HOME"]);
  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);
}


TEST_F(MesosContainerizerProcessTest, NoExtractExecutable)
{
  CommandInfo commandInfo;
  CommandInfo::URI uri;
  uri.set_value("hdfs:///uri");
  uri.set_executable(true);
  uri.set_extract(false);
  commandInfo.add_uris()->MergeFrom(uri);

  string directory = "/tmp/directory";
  Option<string> user = "user";

  Flags flags;
  flags.frameworks_home = "/tmp/frameworks";
  flags.hadoop_home = "/tmp/hadoop";

  map<string, string> environment =
    fetcherEnvironment(commandInfo, directory, user, flags);
  EXPECT_EQ(5u, environment.size());
  EXPECT_EQ("hdfs:///uri+1N", environment["MESOS_EXECUTOR_URIS"]);
  EXPECT_EQ(directory, environment["MESOS_WORK_DIRECTORY"]);
  EXPECT_EQ(user.get(), environment["MESOS_USER"]);
  EXPECT_EQ(flags.frameworks_home, environment["MESOS_FRAMEWORKS_HOME"]);
  EXPECT_EQ(flags.hadoop_home, environment["HADOOP_HOME"]);
}
