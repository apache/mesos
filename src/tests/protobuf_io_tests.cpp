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

#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>

#include "common/type_utils.hpp"

#include "messages/messages.hpp"

#include "tests/utils.hpp"

using namespace mesos;
using namespace mesos::internal;


// TODO(bmahler): Move this file into stout.
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

  const size_t writes = 10;

  for (size_t i = 0; i < writes; i++) {
    FrameworkID frameworkId;
    frameworkId.set_value(stringify(i));
    Try<Nothing> result = ::protobuf::write(fdw, frameworkId);
    ASSERT_SOME(result);
  }

  Result<FrameworkID> read = None();
  size_t reads = 0;
  while (true) {
    read = ::protobuf::read<FrameworkID>(fdr);
    if (!read.isSome()) {
      break;
    }

    EXPECT_EQ(read.get().value(), stringify(reads++));
  }

  // Ensure we've hit the end of the file without reading a partial protobuf.
  ASSERT_TRUE(read.isNone());
  ASSERT_EQ(writes, reads);

  os::close(fdw);
  os::close(fdr);

  os::rm(file);
}
