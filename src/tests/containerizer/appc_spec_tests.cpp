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

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/os.hpp>

#include <mesos/appc/spec.hpp>

#include "tests/mesos.hpp"

using std::string;

namespace spec = ::appc::spec;

namespace mesos {
namespace internal {
namespace tests {

class AppcSpecTest : public TemporaryDirectoryTest {};


TEST_F(AppcSpecTest, ValidateImageManifest)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
        {
          "acKind": "ImageManifest",
          "acVersion": "0.6.1",
          "name": "foo.com/bar",
          "labels": [
            {
              "name": "version",
              "value": "1.0.0"
            },
            {
              "name": "arch",
              "value": "amd64"
            },
            {
              "name": "os",
              "value": "linux"
            }
          ],
          "app" : {
            "workingDirectory": "/opt/work",
            "environment": [
               {
                 "name": "REDUCE_WORKER_DEBUG",
                 "value": "true"
               }
            ],
            "exec": [
              "/usr/bin/sh",
              "--quiet"
            ]
          },
          "annotations": [
            {
              "name": "created",
              "value": "1438983392"
            }
          ]
        })~");

  ASSERT_SOME(json);

  Try<spec::ImageManifest> imageManifest = spec::parse(stringify(json.get()));
  ASSERT_SOME(imageManifest);

  // Check app object.
  ASSERT_TRUE(imageManifest->has_app());
  ASSERT_TRUE(imageManifest->app().has_workingdirectory());
  EXPECT_EQ(imageManifest->app().workingdirectory(), "/opt/work");
  ASSERT_EQ(1, imageManifest->app().environment_size());
  EXPECT_EQ(imageManifest->app().environment(0).name(), "REDUCE_WORKER_DEBUG");
  EXPECT_EQ(imageManifest->app().environment(0).value(), "true");
  ASSERT_EQ(2, imageManifest->app().exec_size());
  EXPECT_EQ(imageManifest->app().exec(0), "/usr/bin/sh");
  EXPECT_EQ(imageManifest->app().exec(1), "--quiet");

  // Check annotations.
  ASSERT_EQ(1, imageManifest->annotations_size());
  EXPECT_EQ(imageManifest->annotations(0).name(), "created");
  EXPECT_EQ(imageManifest->annotations(0).value(), "1438983392");

  // Incorrect acKind for image manifest.
  Try<JSON::Object> jsonError = JSON::parse<JSON::Object>(
      R"~(
        {
          "acKind": "PodManifest",
          "acVersion": "0.6.1",
          "name": "foo.com/bar"
        })~");

  ASSERT_SOME(jsonError);

  Try<spec::ImageManifest> manifest = spec::parse(stringify(jsonError.get()));
  EXPECT_ERROR(manifest);
}


TEST_F(AppcSpecTest, ValidateLayout)
{
  string image = os::getcwd();

  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
        {
          "acKind": "ImageManifest",
          "acVersion": "0.6.1",
          "name": "foo.com/bar"
        })~");

  ASSERT_SOME(json);
  Try<spec::ImageManifest> manifest = spec::parse(stringify(json.get()));

  ASSERT_SOME(manifest);
  ASSERT_SOME(os::write(path::join(image, "manifest"), stringify(json.get())));

  // Missing rootfs.
  EXPECT_SOME(spec::validateLayout(image));

  ASSERT_SOME(os::mkdir(path::join(image, "rootfs", "tmp")));
  ASSERT_SOME(os::write(path::join(image, "rootfs", "tmp", "test"), "test"));

  EXPECT_NONE(spec::validateLayout(image));
}


TEST_F(AppcSpecTest, ValidateImageManifestWithNullExec)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
        {
          "acKind":"ImageManifest",
          "acVersion":"0.7.4",
          "name":"example.com/hello",
          "labels":[
             {
               "name":"version",
               "value":"0.0.1"
             },
             {
               "name":"arch",
               "value":"amd64"
             },
             {
               "name":"os",
               "value":"linux"
             }
          ],
          "app":{
            "exec":null,
            "user":"0",
            "group":"0",
            "ports":[
               {
                  "name":"www",
                  "protocol":"tcp",
                  "port":5000,
                  "count":1,
                  "socketActivated":false
               }
             ]
           },
           "annotations":[
              {
                 "name":"appc.io/acbuild/command-1",
                 "value":"acbuild set-name \"example.com/hello\""
              },
              {
                 "name":"appc.io/acbuild/command-2",
                 "value":"acbuild copy \"hello\" \"/rootfs/hello\""
              },
              {
                 "name":"appc.io/acbuild/command-3",
                 "value":"acbuild port add \"www\" \"tcp\" \"5000\""
              },
              {
                 "name":"appc.io/acbuild/command-4",
                 "value":"acbuild label add \"version\" \"0.0.1\""
              },
              {
                 "name":"appc.io/acbuild/command-5",
                 "value":"acbuild label add \"arch\" \"amd64\""
              },
              {
                 "name":"appc.io/acbuild/command-6",
                 "value":"acbuild label add \"os\" \"linux\""
              },
              {
                 "name":"authors",
                 "value":"Carly Container \u003ccarly@example.com\u003e"
              },
              {
                 "name":"appc.io/acbuild/command-7",
                 "value":"acbuild annotation add \"authors\" \"Carly Container \u003ccarly@example.com\u003e\""
              }
           ]
         })~");

  ASSERT_SOME(json);

  Try<spec::ImageManifest> imageManifest = spec::parse(stringify(json.get()));
  ASSERT_SOME(imageManifest);

  // Check app object.
  ASSERT_TRUE(imageManifest->has_app());
  EXPECT_FALSE(imageManifest->app().has_workingdirectory());
  EXPECT_TRUE(imageManifest->app().environment().empty());
  EXPECT_TRUE(imageManifest->app().exec().empty());

  // Check annotations.
  ASSERT_EQ(imageManifest->annotations_size(), 8);

  EXPECT_EQ(
      imageManifest->annotations(0).name(),
      "appc.io/acbuild/command-1");

  EXPECT_EQ(
      imageManifest->annotations(0).value(),
      "acbuild set-name \"example.com/hello\"");
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
