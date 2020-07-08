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

#include <mesos/csi/v0.hpp>

#include <mesos/module/disk_profile_adaptor.hpp>

#include <mesos/resource_provider/storage/disk_profile_adaptor.hpp>

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

#include "module/manager.hpp"

#include "resource_provider/storage/disk_profile_utils.hpp"
#include "resource_provider/storage/uri_disk_profile_adaptor.hpp"

#include "tests/flags.hpp"
#include "tests/mesos.hpp"
#include "tests/disk_profile_server.hpp"
#include "tests/utils.hpp"

using namespace process;

using std::map;
using std::shared_ptr;
using std::string;
using std::tuple;
using std::vector;

using google::protobuf::Map;

using mesos::resource_provider::DiskProfileMapping;

using testing::_;
using testing::DoAll;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {

using VolumeCapability = mesos::Volume::Source::CSIVolume::VolumeCapability;


constexpr char URI_DISK_PROFILE_ADAPTOR_NAME[] =
  "org_apache_mesos_UriDiskProfileAdaptor";


class UriDiskProfileAdaptorTest : public MesosTest
{
public:
  void SetUp() override
  {
    MesosTest::SetUp();

    string libraryPath = getModulePath("uri_disk_profile_adaptor");

    Modules::Library* library = modules.add_libraries();
    library->set_name("uri_disk_profile_adaptor");
    library->set_file(libraryPath);

    Modules::Library::Module* module = library->add_modules();
    module->set_name(URI_DISK_PROFILE_ADAPTOR_NAME);

    ASSERT_SOME(modules::ModuleManager::load(modules));
  }

  void TearDown() override
  {
    foreach (const Modules::Library& library, modules.libraries()) {
      foreach (const Modules::Library::Module& module, library.modules()) {
        if (module.has_name()) {
          ASSERT_SOME(modules::ModuleManager::unload(module.name()));
        }
      }
    }

    MesosTest::TearDown();
  }

protected:
  Modules modules;
};


// Exercises the disk profile map parsing method with the example found
// in the UriDiskProfileAdaptor module's help string.
TEST_F(UriDiskProfileAdaptorTest, ParseExample)
{
  const string example = R"~(
    {
      "profile_matrix" : {
        "my-profile" : {
          "csi_plugin_type_selector" : {
            "plugin_type" : "org.apache.mesos.csi.test"
          },
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

  Try<DiskProfileMapping> parsed =
    mesos::internal::storage::parseDiskProfileMapping(example);
  ASSERT_SOME(parsed);

  const string key = "my-profile";
  ASSERT_EQ(1u, parsed->profile_matrix().count(key));

  csi::v0::VolumeCapability capability =
    parsed->profile_matrix().at(key).volume_capabilities();

  ASSERT_TRUE(capability.has_block());
  ASSERT_TRUE(capability.has_access_mode());
  ASSERT_EQ(
      csi::v0::VolumeCapability::AccessMode::SINGLE_NODE_WRITER,
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


// Exercises the disk profile map parsing method with some slightly incorrect
// inputs. Each item in the array of examples should error at a different area
// of the code (and are ordered corresponding to the code as well).
TEST_F(UriDiskProfileAdaptorTest, ParseInvalids)
{
  const vector<string> examples = {
    "Not an object, but still JSON",

    R"~({
        "profile_matrix" : {
          "profile" : "This is not an object"
        }
      })~",

    // Missing one of 'resource_provider_selector' or
    // 'csi_plugin_type_selector'.
    R"~({
        "profile_matrix" : {
          "profile" : {}
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "resource_provider_selector" : "Wrong JSON type"
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "resource_provider_selector" : {
              "resource_providers" : "Wrong JSON type"
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "resource_provider_selector" : {
              "resource_providers" : [
                {
                  "not-type" : "Missing required key"
                }
              ]
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "resource_provider_selector" : {
              "resource_providers" : [
                {
                  "type" : "org.apache.mesos.rp.local.storage",
                  "not-name" : "Missing required key"
                }
              ]
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "csi_plugin_type_selector" : "Wrong JSON type"
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "csi_plugin_type_selector" : {
              "not-plugin_type" : "Missing required key",
            }
          }
        }
      })~",

    // More than one selector.
    R"~({
        "profile_matrix" : {
          "profile" : {
            "resource_provider_selector" : {
              "resource_providers" : [
                {
                  "type" : "org.apache.mesos.rp.local.storage",
                  "name" : "test"
                }
              ]
            },
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            }
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
            "not-volume_capabilities" : "Missing required key"
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
            "volume_capabilities" : "Wrong JSON type"
          }
        }
      })~",

    // Missing one of 'block' or 'mount'.
    R"~({
        "profile_matrix" : {
          "profile" : {
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
            "volume_capabilities" : {}
          }
        }
      })~",

    R"~({
        "profile_matrix" : {
          "profile" : {
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
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
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
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
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
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
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
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
            "csi_plugin_type_selector" : {
              "plugin_type" : "org.apache.mesos.csi.test",
            },
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
    Try<DiskProfileMapping> parsed =
      mesos::internal::storage::parseDiskProfileMapping(examples[i]);

    ASSERT_ERROR(parsed) << examples[i];
    ASSERT_EQ(0u, errors.count(parsed.error())) << parsed.error();

    errors.insert(parsed.error());
  }
}


// This creates a UriDiskProfileAdaptor module configured to read from a
// file and tests the basic `watch` -> `translate` workflow which
// callers of the module are expected to follow.
TEST_F(UriDiskProfileAdaptorTest, FetchFromFile)
{
  Clock::pause();

  const string contents =R"~(
    {
      "profile_matrix" : {
        "profile" : {
          "resource_provider_selector" : {
            "resource_providers" : [
              {
                "type" : "resource_provider_type",
                "name" : "resource_provider_name"
              }
            ]
          },
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

  ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("resource_provider_type");
  resourceProviderInfo.set_name("resource_provider_name");

  Parameters params;

  Parameter* pollIntervalFlag = params.add_parameter();
  pollIntervalFlag->set_key("poll_interval");
  pollIntervalFlag->set_value(stringify(pollInterval));

  // NOTE: We cannot use the `file://` URI to specify the file location,
  // otherwise the file contents will be prematurely read. Therefore, we
  // specify the absolute path of the file in the `uri` flag.
  Parameter* uriFlag = params.add_parameter();
  uriFlag->set_key("uri");
  uriFlag->set_value(profileFile);

  // Create the module before we've written anything to the file.
  // This means the first poll will fail, so the module believes there
  // are no profiles at the moment.
  Try<DiskProfileAdaptor*> _module =
    modules::ModuleManager::create<DiskProfileAdaptor>(
        URI_DISK_PROFILE_ADAPTOR_NAME,
        params);

  ASSERT_SOME(_module);

  Owned<DiskProfileAdaptor> module(_module.get());

  // Start watching for updates.
  // By the time this returns, we'll know that the first poll has finished
  // because when the module reads from file, it does so immediately upon
  // being initialized.
  Future<hashset<string>> future =
    module->watch(hashset<string>::EMPTY, resourceProviderInfo);

  // Write the single profile to the file.
  ASSERT_SOME(os::write(profileFile, contents));

  // Trigger the next poll.
  Clock::advance(pollInterval);

  AWAIT_ASSERT_READY(future);
  ASSERT_EQ(1u, future->size());
  EXPECT_EQ(profileName, *(future->begin()));

  // Translate the profile name into the profile mapping.
  Future<DiskProfileAdaptor::ProfileInfo> mapping =
    module->translate(profileName, resourceProviderInfo);

  AWAIT_ASSERT_READY(mapping);
  ASSERT_TRUE(mapping->capability.has_block());
  ASSERT_EQ(
      VolumeCapability::AccessMode::MULTI_NODE_SINGLE_WRITER,
      mapping->capability.access_mode().mode());

  Clock::resume();
}


// This creates a UriDiskProfileAdaptor module configured to read from
// an HTTP URI. The HTTP server will return a different profile mapping
// between each of the calls. We expect the module to ignore the third
// call because the module does not allow profiles to be mutated. This
// is not a fatal error however, as the HTTP server can be "fixed"
// without restarting the agent.
TEST_F(UriDiskProfileAdaptorTest, FetchFromHTTP)
{
  Clock::pause();

  const string contents1 =
    R"~(
    {
      "profile_matrix" : {
        "profile" : {
          "resource_provider_selector" : {
            "resource_providers" : [
              {
                "type" : "resource_provider_type",
                "name" : "resource_provider_name"
              }
            ]
          },
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "SINGLE_NODE_WRITER" }
          }
        }
      }
    }
    )~";

  const string contents2 =
    R"~(
    {
      "profile_matrix" : {
        "another-profile" : {
          "resource_provider_selector" : {
            "resource_providers" : [
              {
                "type" : "resource_provider_type",
                "name" : "resource_provider_name"
              }
            ]
          },
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "MULTI_NODE_MULTI_WRITER" }
          }
        }
      }
    }
    )~";

  const string contents3 =
    R"~(
    {
      "profile_matrix" : {
        "profile" : {
          "resource_provider_selector" : {
            "resource_providers" : [
              {
                "type" : "resource_provider_type",
                "name" : "resource_provider_name"
              }
            ]
          },
          "volume_capabilities" : {
            "block" : {},
            "access_mode" : { "mode": "MULTI_NODE_MULTI_WRITER" }
          }
        }
      }
    }
    )~";

  const Duration pollInterval = Seconds(10);

  ResourceProviderInfo resourceProviderInfo;
  resourceProviderInfo.set_type("resource_provider_type");
  resourceProviderInfo.set_name("resource_provider_name");

  Future<shared_ptr<TestDiskProfileServer>> server =
    TestDiskProfileServer::create();
  AWAIT_READY(server);

  // We need to intercept this call since the module is expected to
  // ignore the result of the third call.
  Future<Nothing> thirdCall;

  EXPECT_CALL(*server.get()->process, profiles(_))
    .WillOnce(Return(http::OK(contents1)))
    .WillOnce(Return(http::OK(contents2)))
    .WillOnce(DoAll(FutureSatisfy(&thirdCall), Return(http::OK(contents3))))
    .WillOnce(Return(http::OK(contents1)));

  Parameters params;

  Parameter* pollIntervalFlag = params.add_parameter();
  pollIntervalFlag->set_key("poll_interval");
  pollIntervalFlag->set_value(stringify(pollInterval));

  Parameter* uriFlag = params.add_parameter();
  uriFlag->set_key("uri");
  uriFlag->set_value(stringify(server.get()->process->url()));

  Try<DiskProfileAdaptor*> _module =
    modules::ModuleManager::create<DiskProfileAdaptor>(
        URI_DISK_PROFILE_ADAPTOR_NAME,
        params);

  ASSERT_SOME(_module);

  Owned<DiskProfileAdaptor> module(_module.get());

  // Wait for the first HTTP poll to complete.
  Future<hashset<string>> future =
    module->watch(hashset<string>::EMPTY, resourceProviderInfo);

  AWAIT_READY(future);
  ASSERT_EQ(1u, future->size());
  EXPECT_EQ("profile", *future->begin());

  // Start watching for an update to the list of profiles.
  future = module->watch({"profile"}, resourceProviderInfo);

  // Trigger the second HTTP poll.
  Clock::advance(pollInterval);

  AWAIT_READY(future);
  ASSERT_EQ(1u, future->size());
  EXPECT_EQ("another-profile", *future->begin());

  // Watching for another update to the list of profiles.
  future = module->watch({"another-profile"}, resourceProviderInfo);

  Future<Nothing> contentsPolled =
    FUTURE_DISPATCH(_, &storage::UriDiskProfileAdaptorProcess::_poll);

  // Trigger the third HTTP poll.
  Clock::advance(pollInterval);
  AWAIT_READY(thirdCall);

  // Ensure that the polling has actually completed.
  AWAIT_READY(contentsPolled);

  // We don't expect the module to notify watcher(s) because the server's
  // response is considered invalid (the module does not allow profiles
  // to be renamed).
  Clock::settle();
  ASSERT_TRUE(future.isPending());

  // Trigger the fourth HTTP poll.
  Clock::advance(pollInterval);

  // This time, the server's response includes the correct first profile.
  AWAIT_READY(future);
  ASSERT_EQ(1u, future->size());
  EXPECT_EQ("profile", *future->begin());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
