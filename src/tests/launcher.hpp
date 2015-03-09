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

#include <list>
#include <map>
#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/mesos.hpp>

#include <mesos/slave/isolator.hpp>

#include <process/future.hpp>
#include <process/owned.hpp>
#include <process/subprocess.hpp>

#include <stout/flags.hpp>
#include <stout/lambda.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>

#include "slave/containerizer/launcher.hpp"

namespace mesos {
namespace internal {
namespace tests {


ACTION_P(InvokeRecover, launcher)
{
  return launcher->real->recover(arg0);
}


ACTION_P(InvokeFork, launcher)
{
  return launcher->real->fork(
      arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8);
}


ACTION_P(InvokeDestroy, launcher)
{
  return launcher->real->destroy(arg0);
}


class TestLauncher : public slave::Launcher
{
public:
  TestLauncher(const process::Owned<slave::Launcher>& _real)
    : real(_real)
  {
    using testing::_;
    using testing::DoDefault;

    ON_CALL(*this, recover(_))
      .WillByDefault(InvokeRecover(this));
    EXPECT_CALL(*this, recover(_))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, fork(_, _, _, _, _, _, _, _, _))
      .WillByDefault(InvokeFork(this));
    EXPECT_CALL(*this, fork(_, _, _, _, _, _, _, _, _))
      .WillRepeatedly(DoDefault());

    ON_CALL(*this, destroy(_))
      .WillByDefault(InvokeDestroy(this));
    EXPECT_CALL(*this, destroy(_))
      .WillRepeatedly(DoDefault());
  }

  ~TestLauncher() {}

  MOCK_METHOD1(
      recover,
      process::Future<Nothing>(
          const std::list<mesos::slave::ExecutorRunState>& states));

  MOCK_METHOD9(
      fork,
      Try<pid_t>(
          const ContainerID& containerId,
          const std::string& path,
          const std::vector<std::string>& argv,
          const process::Subprocess::IO& in,
          const process::Subprocess::IO& out,
          const process::Subprocess::IO& err,
          const Option<flags::FlagsBase>& flags,
          const Option<std::map<std::string, std::string> >& env,
          const Option<lambda::function<int()> >& setup));

  MOCK_METHOD1(
      destroy,
      process::Future<Nothing>(const ContainerID& containerId));

  process::Owned<slave::Launcher> real;
};

} // namespace tests {
} // namespace internal {
} // namespace mesos {
