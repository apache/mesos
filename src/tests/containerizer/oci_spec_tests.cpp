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

#include <mesos/oci/spec.hpp>

#include "tests/mesos.hpp"

namespace image = ::oci::spec::image;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class OCISpecTest : public ::testing::Test {};


TEST_F(OCISpecTest, ParseDescriptor)
{
  const string json =
      R"~(
      {
        "mediaType": "application/vnd.oci.image.manifest.v1+json",
        "digest": "sha256:5b0bcabd1ed22e9fb1310cf6c2dec7cdef19f0ad69efa1f392e94a4333501270",
        "size": 7682,
        "urls": [
          "https://example.com/example-manifest"
        ]
      })~";

  Try<image::v1::Descriptor> descriptor =
      image::v1::parse<image::v1::Descriptor>(json);

  ASSERT_SOME(descriptor);

  EXPECT_EQ(
      "application/vnd.oci.image.manifest.v1+json",
      descriptor->mediatype());

  EXPECT_EQ(
      "sha256:5b0bcabd1ed22e9fb1310cf6c2dec7cdef19f0ad69efa1f392e94a4333501270",
      descriptor->digest());

  EXPECT_EQ(7682u, descriptor->size());

  EXPECT_EQ(
      "https://example.com/example-manifest",
      descriptor->urls(0));
}


TEST_F(OCISpecTest, ParseIndex)
{
  const string json =
      R"~(
      {
        "schemaVersion": 2,
        "manifests": [
          {
            "mediaType": "application/vnd.oci.image.manifest.v1+json",
            "size": 7143,
            "digest": "sha256:e692418e4cbaf90ca69d05a66403747baa33ee08806650b51fab815ad7fc331f",
            "platform": {
              "architecture": "ppc64le",
              "os": "linux",
              "os.version": "16.04"
            }
          },
          {
            "mediaType": "application/vnd.oci.image.manifest.v1+json",
            "size": 7682,
            "digest": "sha256:5b0bcabd1ed22e9fb1310cf6c2dec7cdef19f0ad69efa1f392e94a4333501270",
            "platform": {
              "architecture": "amd64",
              "os": "linux",
              "os.features": [
                "sse4"
              ]
            }
          }
        ],
        "annotations": {
          "com.example.key1": "value1",
          "com.example.key2": "value2"
        }
      })~";

  Try<image::v1::Index> index =
      image::v1::parse<image::v1::Index>(json);

  ASSERT_SOME(index);

  EXPECT_EQ(2u, index->schemaversion());

  EXPECT_EQ(
      "application/vnd.oci.image.manifest.v1+json",
      index->manifests(0).mediatype());

  EXPECT_EQ(7143u, index->manifests(0).size());

  EXPECT_EQ(
      "sha256:e692418e4cbaf90ca69d05a66403747baa33ee08806650b51fab815ad7fc331f",
      index->manifests(0).digest());

  EXPECT_EQ(
      "ppc64le",
      index->manifests(0).platform().architecture());

  EXPECT_EQ(
      "linux",
      index->manifests(0).platform().os());

  EXPECT_EQ(
      "16.04",
      index->manifests(0).platform().os_version());

  EXPECT_EQ(
      "sse4",
      index->manifests(1).platform().os_features(0));

  EXPECT_EQ(
      "value1",
      index->annotations().at("com.example.key1"));

  EXPECT_EQ(
      "value2",
      index->annotations().at("com.example.key2"));
}


TEST_F(OCISpecTest, ParseManifest)
{
  const string json =
      R"~(
      {
        "schemaVersion": 2,
        "config": {
          "mediaType": "application/vnd.oci.image.config.v1+json",
          "size": 7023,
          "digest": "sha256:b5b2b2c507a0944348e0303114d8d93aaaa081732b86451d9bce1f432a537bc7"
        },
        "layers": [
          {
            "mediaType": "application/vnd.oci.image.layer.v1.tar+gzip",
            "size": 32654,
            "digest": "sha256:e692418e4cbaf90ca69d05a66403747baa33ee08806650b51fab815ad7fc331f"
          },
          {
            "mediaType": "application/vnd.oci.image.layer.v1.tar+gzip",
            "size": 16724,
            "digest": "sha256:3c3a4604a545cdc127456d94e421cd355bca5b528f4a9c1905b15da2eb4a4c6b"
          }
        ],
        "annotations": {
          "com.example.key1": "value1",
          "com.example.key2": "value2"
        }
      })~";

  Try<image::v1::Manifest> manifest =
    image::v1::parse<image::v1::Manifest>(json);

  ASSERT_SOME(manifest);

  EXPECT_EQ(2u, manifest->schemaversion());

  EXPECT_EQ(
      "application/vnd.oci.image.config.v1+json",
      manifest->config().mediatype());

  EXPECT_EQ(7023u, manifest->config().size());

  EXPECT_EQ(
      "sha256:b5b2b2c507a0944348e0303114d8d93aaaa081732b86451d9bce1f432a537bc7",
      manifest->config().digest());

  EXPECT_EQ(
      "application/vnd.oci.image.layer.v1.tar+gzip",
      manifest->layers(0).mediatype());

  EXPECT_EQ(32654u, manifest->layers(0).size());

  EXPECT_EQ(
      "sha256:e692418e4cbaf90ca69d05a66403747baa33ee08806650b51fab815ad7fc331f",
      manifest->layers(0).digest());

  EXPECT_EQ(
        "application/vnd.oci.image.layer.v1.tar+gzip",
        manifest->layers(1).mediatype());

  EXPECT_EQ(16724u, manifest->layers(1).size());

  EXPECT_EQ(
      "sha256:3c3a4604a545cdc127456d94e421cd355bca5b528f4a9c1905b15da2eb4a4c6b",
      manifest->layers(1).digest());

  EXPECT_EQ(
      "value1",
      manifest->annotations().at("com.example.key1"));

  EXPECT_EQ(
      "value2",
      manifest->annotations().at("com.example.key2"));
}


TEST_F(OCISpecTest, ParseConfiguration)
{
  const string json =
      R"~(
      {
        "created": "2015-10-31T22:22:56.015925234Z",
        "author": "Alyssa P. Hacker <alyspdev@example.com>",
        "architecture": "amd64",
        "os": "linux",
        "config": {
            "User": "alice",
            "ExposedPorts": {
                "8080/tcp": {}
            },
            "Env": [
                "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
                "FOO=oci_is_a"
            ],
            "Entrypoint": [
                "/bin/my-app-binary"
            ],
            "Cmd": [
                "--config",
                "/etc/my-app.d/default.cfg"
            ],
            "Volumes": {
                "/var/job-result-data": {},
                "/var/log/my-app-logs": {}
            },
            "WorkingDir": "/home/alice",
            "Labels": {
                "com.example.project.git.commit": "45a939b2999782a3f005621a8d0f29aa387e1d6b",
                "com.example.project.git.url": "https://example.com/project.git"
            }
        },
        "rootfs": {
          "diff_ids": [
            "sha256:c6f988f4874bb0add23a778f753c65efe992244e148a1d2ec2a8b664fb66bbd1",
            "sha256:5f70bf18a086007016e948b04aed3b82103a36bea41755b6cddfaf10ace3c6ef"
          ],
          "type": "layers"
        },
        "history": [
          {
            "created": "2015-10-31T22:22:54.690851953Z",
            "created_by": "/bin/sh -c #(nop) ADD file:a3bc1e842b69636f9df5256c49c5 in /"
          },
          {
            "created": "2015-10-31T22:22:55.613815829Z",
            "created_by": "/bin/sh -c #(nop) CMD [\"sh\"]",
            "empty_layer": true
          }
        ]
      })~";

  Try<image::v1::Configuration> configuration =
      image::v1::parse<image::v1::Configuration>(json);

  ASSERT_SOME(configuration);

  EXPECT_EQ(
      "2015-10-31T22:22:56.015925234Z",
      configuration->created());

  EXPECT_EQ(
      "Alyssa P. Hacker <alyspdev@example.com>",
      configuration->author());

  EXPECT_EQ(
      "amd64",
      configuration->architecture());

  EXPECT_EQ(
      "linux",
      configuration->os());

  EXPECT_EQ(
      "alice",
      configuration->config().user());

  EXPECT_EQ(
      1u,
      configuration->config().exposedports().count("8080/tcp"));

  EXPECT_EQ(
      "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
      configuration->config().env(0));

  EXPECT_EQ(
      "FOO=oci_is_a",
      configuration->config().env(1));

  EXPECT_EQ(
      "/bin/my-app-binary",
      configuration->config().entrypoint(0));

  EXPECT_EQ(
      "--config",
      configuration->config().cmd(0));

  EXPECT_EQ(
      "/etc/my-app.d/default.cfg",
      configuration->config().cmd(1));

  EXPECT_EQ(
      1u,
      configuration->config().volumes().count("/var/job-result-data"));

  EXPECT_EQ(
      1u,
      configuration->config().volumes().count("/var/log/my-app-logs"));

  EXPECT_EQ(
      "/home/alice",
      configuration->config().workingdir());

  EXPECT_EQ(
      "45a939b2999782a3f005621a8d0f29aa387e1d6b",
      configuration->config().labels().at("com.example.project.git.commit"));

  EXPECT_EQ(
      "https://example.com/project.git",
      configuration->config().labels().at("com.example.project.git.url"));

  EXPECT_EQ(
      "sha256:c6f988f4874bb0add23a778f753c65efe992244e148a1d2ec2a8b664fb66bbd1",
      configuration->rootfs().diff_ids(0));

  EXPECT_EQ(
      "sha256:5f70bf18a086007016e948b04aed3b82103a36bea41755b6cddfaf10ace3c6ef",
      configuration->rootfs().diff_ids(1));

  EXPECT_EQ(
      "layers",
      configuration->rootfs().type());

  EXPECT_EQ(
      "2015-10-31T22:22:54.690851953Z",
      configuration->history(0).created());

  EXPECT_EQ(
      "/bin/sh -c #(nop) ADD file:a3bc1e842b69636f9df5256c49c5 in /",
      configuration->history(0).created_by());

  EXPECT_EQ(
      true,
      configuration->history(1).empty_layer());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
