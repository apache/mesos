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
#include <vector>

#include <google/protobuf/util/json_util.h>

#include <gtest/gtest.h>

#include <mesos/csi/types.hpp>
#include <mesos/csi/v0.hpp>

#include "csi/v0_utils.hpp"

namespace util = google::protobuf::util;

using std::string;
using std::vector;

namespace mesos {
namespace internal {
namespace tests {

// This test verifies that a versioned CSI `VolumeCapability` protobuf can be
// devolved to an unversioned protobuf then be evolved back to the same one.
//
// TODO(chhsiao): Parameterize this test with CSI versions.
TEST(CsiUtilsTest, VolumeCapabilityEvolve)
{
  // The following JSON examples contains both valid and invalid CSI volume
  // capabilities. However, no matter if the capability is valid, they should be
  // able to be devolved and evolved back.
  const vector<string> examples = {
    // Missing `access_mode`; missing `block` or `mount`.
    R"~(
    {}
    )~",

    // Missing `access_mode.mode`; missing `block` or `mount`.
    R"~(
    {
      "access_mode": {}
    }
    )~",

    // Missing `access_mode`.
    R"~(
    {
      "block": {},
    }
    )~",

    // Missing `access_mode`.
    R"~(
    {
      "mount": {}
    }
    )~",

    // `access_mode.mode` is `UNKNOWN`; missing `block or `mount`.
    R"~(
    {
      "access_mode": {
        "mode": "UNKNOWN"
      }
    }
    )~",

    // Missing `block` or `mount`.
    R"~(
    {
      "access_mode": {
        "mode": "SINGLE_NODE_WRITER"
      }
    }
    )~",

    R"~(
    {
      "block": {},
      "access_mode": {
        "mode": "SINGLE_NODE_WRITER"
      }
    }
    )~",

    R"~(
    {
      "mount": {},
      "access_mode": {
        "mode": "SINGLE_NODE_WRITER"
      }
    }
    )~",

    R"~(
    {
      "mount": {
        "fs_type": ""
      },
      "access_mode": {
        "mode": "SINGLE_NODE_READER_ONLY"
      }
    }
    )~",

    R"~(
    {
      "mount": {
        "fs_type": "xfs"
      },
      "access_mode": {
        "mode": "MULTI_NODE_READER_ONLY"
      }
    }
    )~",

    R"~(
    {
      "mount": {
        "mount_flags": ["-o", "noatime,nodev,nosuid"]
      },
      "access_mode": {
        "mode": "MULTI_NODE_SINGLE_WRITER"
      }
    }
    )~",

    R"~(
    {
      "mount": {
        "fs_type": "xfs",
        "mount_flags": ["-o", "noatime,nodev,nosuid"]
      },
      "access_mode": {
        "mode": "MULTI_NODE_MULTI_WRITER"
      }
    }
    )~"
  };

  foreach (const string& example, examples) {
    // NOTE: We use Google's JSON utility functions for proto3.
    csi::v0::VolumeCapability versioned;
    ASSERT_EQ(util::Status::OK, util::JsonStringToMessage(example, &versioned));
    EXPECT_EQ(versioned, csi::v0::evolve(csi::v0::devolve(versioned)))
      << example;
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
