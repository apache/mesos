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

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/try.hpp>

#include "linux/capabilities.hpp"
#include "linux/seccomp/seccomp_parser.hpp"

using mesos::seccomp::ContainerSeccompProfile;

using std::multiset;
using std::string;

namespace mesos {
namespace internal {
namespace tests {

class SeccompParserTest : public ::testing::Test {};


// This test verifies that the Seccomp parser can parse a valid Seccomp profile.
TEST_F(SeccompParserTest, SECCOMP_ParseSeccompProfile)
{
  const string config =
    R"~(
    {
      "defaultAction": "SCMP_ACT_ERRNO",
      "archMap": [
        {
          "architecture": "SCMP_ARCH_X86_64",
          "subArchitectures": [
            "SCMP_ARCH_X86",
            "SCMP_ARCH_X32"
          ]
        },
        {
          "architecture": "SCMP_ARCH_AARCH64",
          "subArchitectures": [
            "SCMP_ARCH_ARM"
          ]
        }
      ],
      "syscalls": [
        {
          "names": [
            "accept",
            "writev"
          ],
          "action": "SCMP_ACT_ALLOW",
          "args": [],
          "comment": "",
          "includes": {},
          "excludes": {}
        },
        {
          "names": [
            "clone"
          ],
          "action": "SCMP_ACT_ALLOW",
          "args": [
            {
              "index": 0,
              "value": 2080505856,
              "valueTwo": 0,
              "op": "SCMP_CMP_MASKED_EQ"
            }
          ],
          "comment": "",
          "includes": {},
          "excludes": {
            "caps": [
              "CAP_SYS_ADMIN"
            ],
            "arches": [
              "s390",
              "s390x"
            ]
          }
        }
      ]
    })~";

  Try<ContainerSeccompProfile> profile =
    mesos::internal::seccomp::parseProfileData(config);

  ASSERT_SOME(profile);

  EXPECT_EQ(profile->default_action(),
            ContainerSeccompProfile::Syscall::ACT_ERRNO);

  // Note that the Seccomp parser doesn't add an architecture with all its
  // subarchitectures when the architecture is not native.
  const auto& architectures = profile->architectures();
  EXPECT_EQ(multiset<int>(architectures.begin(), architectures.end()),
            multiset<int>({ContainerSeccompProfile::ARCH_X86_64,
                           ContainerSeccompProfile::ARCH_X86,
                           ContainerSeccompProfile::ARCH_X32}));

  EXPECT_EQ(profile->syscalls_size(), 2);

  {
    const auto& rule = profile->syscalls().Get(0);

    const auto& names = rule.names();
    EXPECT_EQ(multiset<string>(names.begin(), names.end()),
              multiset<string>({"accept", "writev"}));

    EXPECT_EQ(rule.action(), ContainerSeccompProfile::Syscall::ACT_ALLOW);
    EXPECT_EQ(rule.args_size(), 0);
    EXPECT_FALSE(rule.has_includes());
    EXPECT_FALSE(rule.has_excludes());
  }

  {
    const auto& rule = profile->syscalls().Get(1);

    const auto& names = rule.names();
    EXPECT_EQ(multiset<string>(names.begin(), names.end()),
              multiset<string>({"clone"}));

    EXPECT_EQ(rule.action(), ContainerSeccompProfile::Syscall::ACT_ALLOW);

    EXPECT_EQ(rule.args_size(), 1);

    const auto& arg = rule.args().Get(0);
    EXPECT_EQ(arg.index(), 0u);
    EXPECT_EQ(arg.value(), 2080505856u);
    EXPECT_EQ(arg.value_two(), 0u);
    EXPECT_EQ(arg.op(), ContainerSeccompProfile::Syscall::Arg::CMP_MASKED_EQ);

    EXPECT_FALSE(rule.has_includes());

    EXPECT_TRUE(rule.has_excludes());
    EXPECT_EQ(rule.excludes().capabilities_size(), 1);
    EXPECT_EQ(rule.excludes().capabilities().Get(0), CapabilityInfo::SYS_ADMIN);
  }
}


// This test verifies handling of errors caused by an invalid Seccomp profile.
TEST_F(SeccompParserTest, SECCOMP_ParseInvalidSeccompProfile)
{
  auto check = [](const string& config, const string& failureMessage) {
    CHECK(!failureMessage.empty());

    Try<ContainerSeccompProfile> profile =
      mesos::internal::seccomp::parseProfileData(config);

    ASSERT_ERROR(profile);
    EXPECT_TRUE(strings::contains(profile.error(), failureMessage));
  };

  // Check for the existence of the top-most objects.
  check(
      R"~(
      {
        "defaultAction!!BUG": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": []
      })~",
      "Cannot determine 'defaultAction'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap!!BUG": [],
        "syscalls": []
      })~",
      "Cannot determine 'archMap'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls!!BUG": []
      })~",
      "Cannot determine 'syscalls'");

  // Check `defaultAction` object.
  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_!!BUG",
        "archMap": [],
        "syscalls": []
      })~",
      "Unknown syscall action: 'SCMP_ACT_!!BUG'");

  // Check `archMap` object.
  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [
          {
            "architecture": "SCMP_ARCH_!!BUG",
            "subArchitectures": []
          }
        ],
        "syscalls": []
      })~",
      "Unknown architecture: 'SCMP_ARCH_!!BUG'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [
          {
            "architecture": "SCMP_ARCH_X86_64",
            "subArchitectures": [
              "SCMP_ARCH_X86",
              "SCMP_ARCH_!!BUG"
            ]
          }
        ],
        "syscalls": []
      })~",
      "Unknown architecture: 'SCMP_ARCH_!!BUG'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [
          {
            "architecture": "SCMP_ARCH_X86_64",
            "subArchitectures!!BUG": []
          }
        ],
        "syscalls": []
      })~",
      "Cannot determine 'subArchitectures'");

  // Check `syscalls` section.
  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names!!BUG": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'names'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action!!BUG": "SCMP_ACT_ALLOW",
            "args": [],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'action'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args!!BUG": [],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'args'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [],
            "includes!!BUG": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'includes'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [],
            "includes": {},
            "excludes!!BUG": {}
          }
        ]
      })~",
      "Cannot determine 'excludes'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": [
              "accept",
              "writev!!BUG"
            ],
            "action": "SCMP_ACT_ALLOW",
            "args": [],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Unrecognized syscall 'writev!!BUG'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_!!BUG",
            "args": [],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Unknown syscall action: 'SCMP_ACT_!!BUG'");

  // Check `args` object which belongs to the `syscalls` object.
  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [
              {
                "index!!BUG": 0,
                "value": 2080505856,
                "valueTwo": 0,
                "op": "SCMP_CMP_MASKED_EQ"
              }
            ],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'index'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [
              {
                "index": 0,
                "value!!BUG": 2080505856,
                "valueTwo": 0,
                "op": "SCMP_CMP_MASKED_EQ"
              }
            ],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'value'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [
              {
                "index": 0,
                "value": 2080505856,
                "valueTwo!!BUG": 0,
                "op": "SCMP_CMP_MASKED_EQ"
              }
            ],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'valueTwo'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [
              {
                "index": 0,
                "value": 2080505856,
                "valueTwo": 0,
                "op!!BUG": "SCMP_CMP_MASKED_EQ"
              }
            ],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Cannot determine 'op'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [
              {
                "index": 0,
                "value": 2080505856,
                "valueTwo": 0,
                "op": "SCMP_CMP_MASKED_EQ!!BUG"
              }
            ],
            "includes": {},
            "excludes": {}
          }
        ]
      })~",
      "Unknown operator: 'SCMP_CMP_MASKED_EQ!!BUG'");

  // Check `excludes` object which belongs to the `syscalls` object.
  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [],
            "includes": {},
            "excludes": {
              "arches": [
                "amd64!!SPECTRE"
              ]
            }
          }
        ]
      })~",
      "Unknown architecture: 'amd64!!SPECTRE'");

  check(
      R"~(
      {
        "defaultAction": "SCMP_ACT_ERRNO",
        "archMap": [],
        "syscalls": [
          {
            "names": ["accept"],
            "action": "SCMP_ACT_ALLOW",
            "args": [],
            "includes": {},
            "excludes": {
              "caps": [
                "CAP_SYS_ADMIN!!BUG"
              ]
            }
          }
        ]
      })~",
      "Unknown capability: 'CAP_SYS_ADMIN!!BUG'");
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
