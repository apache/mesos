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

  Result<int> result = Result<int>::none();

  result = utils::os::open(file, O_CREAT | O_WRONLY | O_SYNC,
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
    Result<bool> result = utils::protobuf::write(fdw, frameworkId);
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
