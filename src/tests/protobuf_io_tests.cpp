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

#include <string>

#include <gmock/gmock.h>

#include "common/utils.hpp"
#include "common/type_utils.hpp"

#include "messages/messages.hpp"

using namespace mesos;
using namespace mesos::internal;


TEST(ProtobufIOTest, Basic)
{
  const std::string file = ".protobuf_io_test_basic";

  Try<int> result = utils::os::open(file, O_CREAT | O_WRONLY | O_SYNC,
                                    S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  ASSERT_TRUE(result.isSome());
  int fdw = result.get();

  result = utils::os::open(file, O_CREAT | O_RDONLY,
                           S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  ASSERT_TRUE(result.isSome());
  int fdr = result.get();

  const int writes = 10;

  for (int i = 0; i < writes; i++) {
    FrameworkID frameworkId;
    frameworkId.set_value(utils::stringify(i));
    Try<bool> result = utils::protobuf::write(fdw, frameworkId);
    ASSERT_TRUE(result.isSome());
    EXPECT_TRUE(result.get());
  }

  for (int i = 0; i < writes; i++) {
    FrameworkID frameworkId;
    Result<bool> result = utils::protobuf::read(fdr, &frameworkId);
    ASSERT_TRUE(result.isSome());
    EXPECT_TRUE(result.get());
    EXPECT_EQ(frameworkId.value(), utils::stringify(i));
  }

  utils::os::close(fdw);
  utils::os::close(fdr);

  utils::os::rm(file);
}
