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

#include <string>

#include <gmock/gmock.h>

#include <mesos/type_utils.hpp>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/result.hpp>

#include <stout/tests/utils.hpp>

#include "messages/messages.hpp"

using std::string;

using google::protobuf::RepeatedPtrField;

namespace mesos {
namespace internal {
namespace tests {


class ProtobufIOTest : public TemporaryDirectoryTest {};


// TODO(bmahler): Move this file into stout.
TEST_F(ProtobufIOTest, Basic)
{
  const string file = ".protobuf_io_test_basic";

  Try<int_fd> result = os::open(
      file,
      O_CREAT | O_WRONLY | O_SYNC | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(result);

  int_fd fdw = result.get();

  result = os::open(
      file,
      O_CREAT | O_RDONLY | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(result);

  int_fd fdr = result.get();

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

    EXPECT_EQ(read->value(), stringify(reads++));
  }

  // Ensure we've hit the end of the file without reading a partial
  // protobuf.
  ASSERT_NONE(read);
  ASSERT_EQ(writes, reads);

  os::close(fdw);
  os::close(fdr);
}


TEST_F(ProtobufIOTest, Append)
{
  const string file = ".protobuf_io_test_append";

  const size_t writes = 10;
  for (size_t i = 0; i < writes; i++) {
    FrameworkID frameworkId;
    frameworkId.set_value(stringify(i));

    Try<Nothing> result = ::protobuf::append(file, frameworkId);
    ASSERT_SOME(result);
  }

  Try<int_fd> fd = os::open(
      file,
      O_CREAT | O_RDONLY | O_CLOEXEC,
      S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);

  ASSERT_SOME(fd);

  Result<FrameworkID> read = None();
  size_t reads = 0;
  while (true) {
    read = ::protobuf::read<FrameworkID>(fd.get());
    if (!read.isSome()) {
      break;
    }

    EXPECT_EQ(read->value(), stringify(reads++));
  }

  // Ensure we've hit the end of the file without reading a partial
  // protobuf.
  ASSERT_NONE(read);
  ASSERT_EQ(writes, reads);

  os::close(fd.get());
}


TEST_F(ProtobufIOTest, RepeatedPtrField)
{
  const string file = ".protobuf_io_test_repeated_ptr_field";

  RepeatedPtrField<FrameworkID> expected;

  // NOTE: This uses `int` instead of `size_t` because eventually the iterator
  // is used in `Get(int)`, and converting from `size_t` generates a warning.
  const int size = 10;
  for (int i = 0; i < size; i++) {
    FrameworkID frameworkId;
    frameworkId.set_value(stringify(i));
    expected.Add()->CopyFrom(frameworkId);
  }

  Try<Nothing> write = ::protobuf::write(file, expected);
  ASSERT_SOME(write);

  Result<RepeatedPtrField<FrameworkID>> actual =
    ::protobuf::read<RepeatedPtrField<FrameworkID>>(file);

  ASSERT_SOME(actual);

  ASSERT_EQ(expected.size(), actual->size());
  for (int i = 0; i < size; i++) {
    EXPECT_EQ(expected.Get(i), actual->Get(i));
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
