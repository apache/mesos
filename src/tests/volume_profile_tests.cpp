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

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include <mesos/resource_provider/volume_profile.hpp>

#include <process/clock.hpp>
#include <process/future.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>

#include <stout/duration.hpp>
#include <stout/gtest.hpp>
#include <stout/hashset.hpp>
#include <stout/path.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/os/write.hpp>

#include "resource_provider/uri_volume_profile.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/utils.hpp"

using namespace process;

using mesos::VolumeProfileAdaptor;

using std::map;
using std::string;
using std::tuple;
using std::vector;

using google::protobuf::Map;

using mesos::csi::UriVolumeProfileMapping;

using testing::_;
using testing::DoAll;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


class UriVolumeProfileTest : public MesosTest {};


// Exercises the volume profile map parsing method with the example found
// in the UriVolumeProfile module's help string.
TEST_F(UriVolumeProfileTest, ParseExample)
{
  const string example = R"~(
    {
      "profile_matrix" : {
        "my-profile" : {
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode" : "SINGLE_NODE_WRITER" }
          },
          "create_parameters" : {
            "mesos-does-not" : "interpret-these",
            "type" : "raid5",
            "stripes" : "3",
            "stripesize" : "64"
          }
        }
      }
    })~";

  Try<UriVolumeProfileMapping> parsed =
    mesos::internal::profile::UriVolumeProfileAdaptorProcess::parse(example);
  ASSERT_SOME(parsed);

  const string key = "my-profile";
  ASSERT_EQ(1u, parsed->profile_matrix().count(key));

  csi::VolumeCapability capability =
    parsed->profile_matrix().at(key).volume_capabilities();

  ASSERT_TRUE(capability.has_block());
  ASSERT_TRUE(capability.has_access_mode());
  ASSERT_EQ(
      csi::VolumeCapability::AccessMode::SINGLE_NODE_WRITER,
      capability.access_mode().mode());

  Map<string, string> parameters =
    parsed->profile_matrix().at(key).create_parameters();

  ASSERT_EQ(4u, parameters.size());
  ASSERT_EQ(1u, parameters.count("mesos-does-not"));
  ASSERT_EQ(1u, parameters.count("type"));
  ASSERT_EQ(1u, parameters.count("stripes"));
  ASSERT_EQ(1u, parameters.count("stripesize"));

  ASSERT_EQ("interpret-these", parameters.at("mesos-does-not"));
  ASSERT_EQ("raid5", parameters.at("type"));
  ASSERT_EQ("3", parameters.at("stripes"));
  ASSERT_EQ("64", parameters.at("stripesize"));
}


// Exercises the volume profile map parsing method with some slightly incorrect
// inputs. Each item in the array of examples should error at a different area
// of the code (and are ordered corresponding to the code as well).
TEST_F(UriVolumeProfileTest, ParseInvalids)
{
  const vector<string> examples = {
    "Not an object, but still JSON",

    R"~({
        "profile_matrix" : {
          "profile" : "This is not an object"
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "volume_capabilities" : "Wrong JSON type"
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "not-volume_capabilities" : "Missing required key"
          }
        }
      })~",

    // Missing one of 'block' or 'mount'.
    R"~({
        "profile_matrix" : {
          "profile" : {
            "volume_capabilities" : {}
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "volume_capabilities" : {
              "mount" : {
                "fs_type" : [ "This should not be an array" ]
              }
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "volume_capabilities" : {
              "block" : {},
              "access_mode" : { "mode": "No-enum-of-this-name" }
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "volume_capabilities" : {
              "mount" : {
                "mount_flags" : [ "a", "b", "c" ]
              },
              "access_mode" : { "mode": "SINGLE_NODE_WRITER" }
            },
            "create_parameters" : "Wrong JSON type"
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "volume_capabilities" : {
              "mount" : { "fs_type" : "abc" },
              "access_mode" : { "mode": "SINGLE_NODE_READER_ONLY" }
            },
            "create_parameters" : {
              "incorrect" : [ "JSON type of parameter" ]
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "volume_capabilities" : {
              "block" : {},
              "access_mode" : { "mode": "MULTI_NODE_READER_ONLY" }
            }
          },
          "first profile is fine, second profile is broken" : {}
        }
      })~",
    };

  hashset<string> errors;
  for (size_t i = 0; i < examples.size(); i++) {
    Try<UriVolumeProfileMapping> parsed =
      mesos::internal::profile::UriVolumeProfileAdaptorProcess::parse(
          examples[i]);

    ASSERT_ERROR(parsed) << examples[i];
    ASSERT_EQ(0u, errors.count(parsed.error())) << parsed.error();

    errors.insert(parsed.error());
  }
}


// This creates a UriVolumeProfile module configured to read from a file
// and tests the basic `watch` -> `translate` workflow which callers of
// the module are expected to follow.
TEST_F(UriVolumeProfileTest, FetchFromFile)
{
  Clock::pause();

  const string contents =R"~(
    {
      "profile_matrix" : {
        "profile" : {
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "MULTI_NODE_SINGLE_WRITER" }
          }
        }
      }
    })~";

  const string profileName = "profile";
  const string profileFile = path::join(sandbox.get(), "profiles.json");
  const Duration pollInterval = Seconds(10);
  const string csiPluginType = "ignored";

  mesos::internal::profile::Flags flags;
  flags.uri = Path(profileFile);
  flags.poll_interval = pollInterval;

  // Create the module before we've written anything to the file.
  // This means the first poll will fail, so the module believes there
  // are no profiles at the moment.
  mesos::internal::profile::UriVolumeProfileAdaptor module(flags);

  // Start watching for updates.
  // By the time this returns, we'll know that the first poll has finished
  // because when the module reads from file, it does so immediately upon
  // being initialized.
  Future<hashset<string>> future =
    module.watch(hashset<string>::EMPTY, csiPluginType);

  // Write the single profile to the file.
  ASSERT_SOME(os::write(profileFile, contents));

  // Trigger the next poll.
  Clock::advance(pollInterval);

  AWAIT_ASSERT_READY(future);
  ASSERT_EQ(1u, future->size());
  EXPECT_EQ(profileName, *(future->begin()));

  // Translate the profile name into the profile mapping.
  Future<VolumeProfileAdaptor::ProfileInfo> mapping =
    module.translate(profileName, csiPluginType);

  AWAIT_ASSERT_READY(mapping);
  ASSERT_TRUE(mapping.get().capability.has_block());
  ASSERT_EQ(
      csi::VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER,
      mapping.get().capability.access_mode().mode());

  Clock::resume();
}


// Basic helper for UriVolumeProfile modules configured to fetch from HTTP URIs.
class MockProfileServer : public Process<MockProfileServer>
{
public:
  MOCK_METHOD1(profiles, Future<http::Response>(const http::Request&));

protected:
  virtual void initialize()
  {
    route("/profiles", None(), &MockProfileServer::profiles);
  }
};


class ServerWrapper
{
public:
  ServerWrapper() : process(new MockProfileServer())
  {
    spawn(process.get());
  }

  ~ServerWrapper()
  {
    terminate(process.get());
    wait(process.get());
  }

  Owned<MockProfileServer> process;
};


// This creates a UriVolumeProfile module configured to read from an HTTP URI.
// The HTTP server will return a different profile mapping between each of the
// calls. We expect the module to ignore the second call because the module
// does not allow profiles to be renamed. This is not a fatal error however,
// as the HTTP server can be "fixed" without restarting the agent.
TEST_F(UriVolumeProfileTest, FetchFromHTTP)
{
  Clock::pause();

  const string contents1 =R"~(
    {
      "profile_matrix" : {
        "profile" : {
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "MULTI_NODE_MULTI_WRITER" }
          }
        }
      }
    })~";

  const string contents2 =R"~(
    {
      "profile_matrix" : {
        "renamed-profile" : {
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "SINGLE_NODE_WRITER" }
          }
        }
      }
    })~";

  const string contents3 =R"~(
    {
      "profile_matrix" : {
        "profile" : {
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "MULTI_NODE_MULTI_WRITER" }
          }
        },
        "another-profile" : {
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "SINGLE_NODE_WRITER" }
          }
        }
      }
    })~";

  const Duration pollInterval = Seconds(10);
  const string csiPluginType = "ignored";

  ServerWrapper server;

  // Wait for the server to finish initializing so that the routes are ready.
  AWAIT_READY(dispatch(server.process->self(), []() { return Nothing(); }));

  // We need to intercept this call since the module is expected to
  // ignore the result of the second call.
  Future<Nothing> secondCall;

  EXPECT_CALL(*server.process, profiles(_))
    .WillOnce(Return(http::OK(contents1)))
    .WillOnce(DoAll(FutureSatisfy(&secondCall), Return(http::OK(contents2))))
    .WillOnce(Return(http::OK(contents3)));

  mesos::internal::profile::Flags flags;
  flags.poll_interval = pollInterval;

  // NOTE: Although we use the `Path` class here, this URI is not actually
  // a path. The `Path` class is purely used so that `file://` type URIs are
  // do not result in prematurely reading the file contents.
  flags.uri = Path(stringify(process::http::URL(
      "http",
      process::address().ip,
      process::address().port,
      server.process->self().id + "/profiles")));

  mesos::internal::profile::UriVolumeProfileAdaptor module(flags);

  // Wait for the first HTTP poll to complete.
  Future<hashset<string>> future =
    module.watch(hashset<string>::EMPTY, csiPluginType);

  AWAIT_ASSERT_READY(future);
  ASSERT_EQ(1u, future->size());
  EXPECT_EQ("profile", *(future->begin()));

  // Start watching for an update to the list of profiles.
  future = module.watch({"profile"}, csiPluginType);

  // Trigger the second HTTP poll.
  Clock::advance(pollInterval);
  AWAIT_ASSERT_READY(secondCall);

  // Dispatch a call to the module, which ensures that the polling has actually
  // completed (not just the HTTP call).
  AWAIT_ASSERT_READY(module.translate("profile", csiPluginType));

  // We don't expect the module to notify watcher(s) because the server's
  // response is considered invalid (the module does not allow profiles
  // to be renamed).
  ASSERT_TRUE(future.isPending());

  // Trigger the third HTTP poll.
  Clock::advance(pollInterval);

  // This time, the server's response is correct and also includes a second
  // profile, which means that the watcher(s) should be notified.
  AWAIT_ASSERT_READY(future);
  ASSERT_EQ(2u, future->size());
  EXPECT_EQ((hashset<string>{"profile", "another-profile"}), future.get());

  Clock::resume();
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
