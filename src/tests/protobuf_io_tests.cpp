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

#include <stout/os.hpp>
#include <stout/protobuf.hpp>

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;


TEST(ProtobufIOTest, Basic)
{
  const std::string file = ".protobuf_io_test_basic";

  Try<int> result = os::open(file, O_CREAT | O_WRONLY | O_SYNC,
                             S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  ASSERT_SOME(result);
  int fdw = result.get();

  result = os::open(file, O_CREAT | O_RDONLY,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IRWXO);

  ASSERT_SOME(result);
  int fdr = result.get();

  const int writes = 10;

  for (int i = 0; i < writes; i++) {
    FrameworkID frameworkId;
    frameworkId.set_value(stringify(i));
    Try<bool> result = protobuf::write(fdw, frameworkId);
    ASSERT_SOME(result);
    EXPECT_TRUE(result.get());
  }

  for (int i = 0; i < writes; i++) {
    FrameworkID frameworkId;
    Result<bool> result = protobuf::read(fdr, &frameworkId);
    ASSERT_SOME(result);
    EXPECT_TRUE(result.get());
    EXPECT_EQ(frameworkId.value(), stringify(i));
  }

  os::close(fdw);
  os::close(fdr);

  os::rm(file);
}
