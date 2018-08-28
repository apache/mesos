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
#include <stout/json.hpp>

#include <mesos/docker/spec.hpp>

namespace spec = docker::spec;

using std::string;

namespace mesos {
namespace internal {
namespace tests {

class DockerSpecTest : public ::testing::Test {};


TEST_F(DockerSpecTest, ParseImageReference)
{
  Try<spec::ImageReference> reference =
    spec::parseImageReference("library/busybox");

  ASSERT_SOME(reference);
  EXPECT_FALSE(reference->has_registry());
  EXPECT_EQ("library/busybox", reference->repository());
  EXPECT_FALSE(reference->has_tag());
  EXPECT_FALSE(reference->has_digest());

  reference = spec::parseImageReference("busybox");

  ASSERT_SOME(reference);
  EXPECT_FALSE(reference->has_registry());
  EXPECT_EQ("busybox", reference->repository());
  EXPECT_FALSE(reference->has_tag());
  EXPECT_FALSE(reference->has_digest());

  reference = spec::parseImageReference("library/busybox:tag");

  ASSERT_SOME(reference);
  EXPECT_FALSE(reference->has_registry());
  EXPECT_EQ("library/busybox", reference->repository());
  EXPECT_EQ("tag", reference->tag());
  EXPECT_FALSE(reference->has_digest());

  reference = spec::parseImageReference("library/busybox@sha256:bc8813ea7b3603864987522f02a76101c17ad122e1c46d790efc0fca78ca7bfb"); // NOLINT(whitespace/line_length)

  ASSERT_SOME(reference);
  EXPECT_FALSE(reference->has_registry());
  EXPECT_EQ("library/busybox", reference->repository());
  EXPECT_FALSE(reference->has_tag());
  EXPECT_EQ("sha256:bc8813ea7b3603864987522f02a76101c17ad122e1c46d790efc0fca78ca7bfb", reference->digest()); // NOLINT(whitespace/line_length)

  reference = spec::parseImageReference("registry.io/library/busybox");

  ASSERT_SOME(reference);
  EXPECT_EQ("registry.io", reference->registry());
  EXPECT_EQ("library/busybox", reference->repository());
  EXPECT_FALSE(reference->has_tag());
  EXPECT_FALSE(reference->has_digest());

  reference = spec::parseImageReference("registry.io/library/busybox:tag");

  ASSERT_SOME(reference);
  EXPECT_EQ("registry.io", reference->registry());
  EXPECT_EQ("library/busybox", reference->repository());
  EXPECT_EQ("tag", reference->tag());
  EXPECT_FALSE(reference->has_digest());

  reference = spec::parseImageReference("registry.io:80/library/busybox:tag");

  ASSERT_SOME(reference);
  EXPECT_EQ("registry.io:80", reference->registry());
  EXPECT_EQ("library/busybox", reference->repository());
  EXPECT_EQ("tag", reference->tag());
  EXPECT_FALSE(reference->has_digest());

  reference = spec::parseImageReference("registry.io:80/library/busybox@sha256:bc8813ea7b3603864987522f02a76101c17ad122e1c46d790efc0fca78ca7bfb"); // NOLINT(whitespace/line_length)

  ASSERT_SOME(reference);
  EXPECT_EQ("registry.io:80", reference->registry());
  EXPECT_EQ("library/busybox", reference->repository());
  EXPECT_FALSE(reference->has_tag());
  EXPECT_EQ("sha256:bc8813ea7b3603864987522f02a76101c17ad122e1c46d790efc0fca78ca7bfb", reference->digest()); // NOLINT(whitespace/line_length)
}


TEST_F(DockerSpecTest, GetRegistrySpec)
{
  string registry = "";
  Result<int> port = spec::getRegistryPort(registry);
  Try<string> scheme = spec::getRegistryScheme(registry);
  string host = spec::getRegistryHost(registry);

  ASSERT_NONE(port);
  EXPECT_SOME_EQ("https", scheme);
  EXPECT_TRUE(host.empty());

  registry = ":";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  ASSERT_ERROR(port);
  ASSERT_ERROR(scheme);
  EXPECT_TRUE(host.empty());

  registry = "invalid_host:x80";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  ASSERT_ERROR(port);
  ASSERT_ERROR(scheme);
  EXPECT_EQ("invalid_host", host);

  registry = "invalid_port:80:80";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  ASSERT_ERROR(port);
  ASSERT_ERROR(scheme);
  EXPECT_EQ("invalid_port", host);

  registry = "index.docker.io";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  ASSERT_NONE(port);
  EXPECT_SOME_EQ("https", scheme);
  EXPECT_EQ("index.docker.io", host);

  registry = "localhost:80";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  EXPECT_SOME_EQ(80, port);
  EXPECT_SOME_EQ("http", scheme);
  EXPECT_EQ("localhost", host);

  registry = "registry-1.docker.io:443";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  EXPECT_SOME_EQ(443, port);
  EXPECT_SOME_EQ("https", scheme);
  EXPECT_EQ("registry-1.docker.io", host);

  registry = "127.0.0.1:443";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  EXPECT_SOME_EQ(443, port);
  EXPECT_SOME_EQ("https", scheme);
  EXPECT_EQ("127.0.0.1", host);

  registry = "127.0.0.1:1234";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  EXPECT_SOME_EQ(1234, port);
  EXPECT_SOME_EQ("http", scheme);
  EXPECT_EQ("127.0.0.1", host);

  registry = "localhost:1234";
  port = spec::getRegistryPort(registry);
  scheme = spec::getRegistryScheme(registry);
  host = spec::getRegistryHost(registry);

  EXPECT_SOME_EQ(1234, port);
  EXPECT_SOME_EQ("http", scheme);
  EXPECT_EQ("localhost", host);
}


// This test verifies docker::spec::parseAuthConfig works as expected
// for new docker config file format (e.g., ~/.docker/config.json).
TEST_F(DockerSpecTest, ParseDockerConfig)
{
  string config =
    R"~(
    {
      "auths": {
        "https://index.docker.io/v1/": {
          "auth": "bWVzb3M6dGVzdA==",
          "email": "user@example.com"
        },
        "localhost:5000": {
          "auth": "dW5pZmllZDpjb250YWluZXJpemVy",
          "email": "user@example.com"
        }
      },
      "HttpHeaders": {
        "User-Agent": "Docker-Client/1.10.2 (linux)"
      }
    })~";

  Try<hashmap<string, spec::Config::Auth>> map =
    spec::parseAuthConfig(config);

  EXPECT_EQ("bWVzb3M6dGVzdA==",
            map.get()["https://index.docker.io/v1/"].auth());

  EXPECT_EQ("user@example.com",
            map.get()["https://index.docker.io/v1/"].email());

  EXPECT_EQ("dW5pZmllZDpjb250YWluZXJpemVy", map.get()["localhost:5000"].auth());
  EXPECT_EQ("user@example.com", map.get()["localhost:5000"].email());
}


// This test verifies docker::spec::parseAuthConfig works as expected
// for old docker config file format (e.g., ~/.dockercfg).
TEST_F(DockerSpecTest, ParseDockercfg)
{
  string dockercfg =
    R"~(
    {
      "quay.io": {
        "auth": "cXVheTp0ZXN0",
        "email": "user@example.com"
      },
      "https://index.docker.io/v1/": {
        "auth": "cHJpdmF0ZTpyZWdpc3RyeQ==",
        "email": "user@example.com"
      },
      "https://192.168.0.1:5050": {
        "auth": "aXA6YWRkcmVzcw==",
        "email": "user@example.com"
      }
    })~";

  Try<hashmap<string, spec::Config::Auth>> map =
    spec::parseAuthConfig(dockercfg);

  EXPECT_EQ("cXVheTp0ZXN0", map.get()["quay.io"].auth());
  EXPECT_EQ("user@example.com", map.get()["quay.io"].email());

  EXPECT_EQ("cHJpdmF0ZTpyZWdpc3RyeQ==",
            map.get()["https://index.docker.io/v1/"].auth());

  EXPECT_EQ("user@example.com",
            map.get()["https://index.docker.io/v1/"].email());

  EXPECT_EQ("aXA6YWRkcmVzcw==", map.get()["https://192.168.0.1:5050"].auth());
  EXPECT_EQ("user@example.com", map.get()["https://192.168.0.1:5050"].email());
}


TEST_F(DockerSpecTest, ParseAuthUrl)
{
  EXPECT_EQ("registry.example.com",
            spec::parseAuthUrl("https://registry.example.com/v1/"));
  EXPECT_EQ("registry.example.com",
            spec::parseAuthUrl("http://registry.example.com/v1/"));
  EXPECT_EQ("registry.example.com",
            spec::parseAuthUrl("registry.example.com"));
  EXPECT_EQ("registry.example.com",
            spec::parseAuthUrl("registry.example.com/v1/"));

  EXPECT_EQ("localhost:8000", spec::parseAuthUrl("https://localhost:8000/v1/"));
  EXPECT_EQ("localhost:8000", spec::parseAuthUrl("http://localhost:8000/v1/"));
  EXPECT_EQ("localhost:8000", spec::parseAuthUrl("localhost:8000"));
  EXPECT_EQ("localhost:8000", spec::parseAuthUrl("localhost:8000/v1/"));

  EXPECT_EQ("registry.com", spec::parseAuthUrl("https://registry.com/v1/"));
  EXPECT_EQ("registry.com", spec::parseAuthUrl("http://registry.com/v1/"));
  EXPECT_EQ("registry.com", spec::parseAuthUrl("registry.com"));
  EXPECT_EQ("registry.com", spec::parseAuthUrl("registry.com/v1/"));
}


TEST_F(DockerSpecTest, ParseV1ImageManifest)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
      {
        "container": "7f652467f9e6d1b3bf51172868b9b0c2fa1c711b112f4e987029b1624dd6295f",
        "parent": "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
        "created": "2015-09-21T20:15:47.866196515Z",
        "config": {
          "Hostname": "5f8e0e129ff1",
          "Entrypoint": null,
          "Env": null,
          "OnBuild": null,
          "OpenStdin": false,
          "MacAddress": "",
          "User": "",
          "VolumeDriver": "",
          "AttachStderr": false,
          "AttachStdout": false,
          "PublishService": "",
          "NetworkDisabled": false,
          "StdinOnce": false,
          "Cmd": [ "sh" ],
          "WorkingDir": "",
          "AttachStdin": false,
          "Volumes": null,
          "Tty": false,
          "Domainname": "",
          "Image": "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
          "Labels": {
            "com.nvidia.cuda.version": "7.5",
            "com.nvidia.volumes.needed": "nvidia_driver"
          },
          "ExposedPorts": null
        },
        "container_config": {
          "Hostname": "5f8e0e129ff1",
          "Entrypoint": [ "./bin/start" ],
          "Env": [
            "LANG=C.UTF-8",
            "JAVA_VERSION=8u66",
            "JAVA_DEBIAN_VERSION=8u66-b01-1~bpo8+1",
            "CA_CERTIFICATES_JAVA_VERSION=20140324"
          ],
          "OnBuild": null,
          "OpenStdin": false,
          "MacAddress": "",
          "User": "",
          "VolumeDriver": "",
          "AttachStderr": false,
          "AttachStdout": false,
          "PublishService": "",
          "NetworkDisabled": false,
          "StdinOnce": false,
          "Cmd": [
            "/bin/sh",
            "-c",
            "#(nop) CMD [\"sh\"]"
          ],
          "WorkingDir": "/marathon",
          "AttachStdin": false,
          "Volumes": null,
          "Tty": false,
          "Domainname": "",
          "Image": "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
          "Labels": {
            "com.nvidia.caffe.version": "0.14",
            "com.nvidia.digits.version": "3.0"
          },
          "ExposedPorts": null
        },
        "architecture": "amd64",
        "docker_version": "1.8.2",
        "os": "linux",
        "id": "d7057cb020844f245031d27b76cb18af05db1cc3a96a29fa7777af75f5ac91a3",
        "Size": 0
      })~");

  ASSERT_SOME(json);

  Try<spec::v1::ImageManifest> manifest = spec::v1::parse(json.get());
  ASSERT_SOME(manifest);

  EXPECT_EQ(
      "7f652467f9e6d1b3bf51172868b9b0c2fa1c711b112f4e987029b1624dd6295f",
      manifest->container());

  EXPECT_EQ(
      "cfa753dfea5e68a24366dfba16e6edf573daa447abf65bc11619c1a98a3aff54",
      manifest->parent());

  EXPECT_EQ(
      "./bin/start",
      manifest->container_config().entrypoint(0));

  EXPECT_EQ(
      "LANG=C.UTF-8",
      manifest->container_config().env(0));

  EXPECT_EQ(
      "JAVA_VERSION=8u66",
      manifest->container_config().env(1));

  EXPECT_EQ(
      "JAVA_DEBIAN_VERSION=8u66-b01-1~bpo8+1",
      manifest->container_config().env(2));

  EXPECT_EQ(
      "CA_CERTIFICATES_JAVA_VERSION=20140324",
      manifest->container_config().env(3));

  EXPECT_EQ("/bin/sh", manifest->container_config().cmd(0));
  EXPECT_EQ("-c", manifest->container_config().cmd(1));

  EXPECT_EQ(
      "#(nop) CMD [\"sh\"]",
      manifest->container_config().cmd(2));

  EXPECT_EQ(
      "0.14",
      manifest->container_config().labels().at("com.nvidia.caffe.version"));

  EXPECT_EQ(
      "3.0",
      manifest->container_config().labels().at("com.nvidia.digits.version"));

  EXPECT_EQ("sh", manifest->config().cmd(0));

  EXPECT_EQ(
      "7.5",
      manifest->config().labels().at("com.nvidia.cuda.version"));

  EXPECT_EQ(
      "nvidia_driver",
      manifest->config().labels().at("com.nvidia.volumes.needed"));

  EXPECT_EQ("1.8.2", manifest->docker_version());
  EXPECT_EQ("amd64", manifest->architecture());
  EXPECT_EQ("linux", manifest->os());
  EXPECT_EQ(0u, manifest->size());
}


TEST_F(DockerSpecTest, ParseV2ImageManifest)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
      {
        "name": "dmcgowan/test-image",
        "tag": "latest",
        "architecture": "amd64",
        "fsLayers": [
          { "blobSum": "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
          { "blobSum": "sha256:cea0d2071b01b0a79aa4a05ea56ab6fdf3fafa03369d9f4eea8d46ea33c43e5f" },
          { "blobSum": "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" },
          { "blobSum": "sha256:2a7812e636235448785062100bb9103096aa6655a8f6bb9ac9b13fe8290f66df" }
        ],
        "history": [
          {
            "v1Compatibility": "{\"id\": \"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\",\"parent\": \"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\"}"
          },
          {
            "v1Compatibility": "{\"id\": \"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\",\"parent\": \"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\"}"
          },
          {
            "v1Compatibility": "{\"id\": \"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\",\"parent\": \"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\"}"
          },
          {
            "v1Compatibility": "{\"id\": \"2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea\",\"parent\": \"cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff\"}"
          }
        ],
        "schemaVersion": 1,
        "signatures": [
          {
            "header": {
              "jwk": {
                "crv": "P-256",
                "kid": "LYRA:YAG2:QQKS:376F:QQXY:3UNK:SXH7:K6ES:Y5AU:XUN5:ZLVY:KBYL",
                "kty": "EC",
                "x": "Cu_UyxwLgHzE9rvlYSmvVdqYCXY42E9eNhBb0xNv0SQ",
                "y": "zUsjWJkeKQ5tv7S-hl1Tg71cd-CqnrtiiLxSi6N_yc8"
              },
              "alg": "ES256"
            },
            "signature": "m3bgdBXZYRQ4ssAbrgj8Kjl7GNgrKQvmCSY-00yzQosKi-8UBrIRrn3Iu5alj82B6u_jNrkGCjEx3TxrfT1rig",
            "protected": "eyJmb3JtYXRMZW5ndGgiOjYwNjMsImZvcm1hdFRhaWwiOiJDbjAiLCJ0aW1lIjoiMjAxNC0wOS0xMVQxNzoxNDozMFoifQ"
          }
        ]
      })~");

  ASSERT_SOME(json);

  Try<spec::v2::ImageManifest> manifest = spec::v2::parse(json.get());
  ASSERT_SOME(manifest);

  EXPECT_EQ("dmcgowan/test-image", manifest->name());
  EXPECT_EQ("latest", manifest->tag());
  EXPECT_EQ("amd64", manifest->architecture());

  EXPECT_EQ(
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", // NOLINT(whitespace/line_length)
      manifest->fslayers(0).blobsum());

  EXPECT_EQ(
      "sha256:cea0d2071b01b0a79aa4a05ea56ab6fdf3fafa03369d9f4eea8d46ea33c43e5f", // NOLINT(whitespace/line_length)
      manifest->fslayers(1).blobsum());

  EXPECT_EQ(
      "sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", // NOLINT(whitespace/line_length)
      manifest->fslayers(2).blobsum());

  EXPECT_EQ(
      "sha256:2a7812e636235448785062100bb9103096aa6655a8f6bb9ac9b13fe8290f66df", // NOLINT(whitespace/line_length)
      manifest->fslayers(3).blobsum());

  EXPECT_EQ(
      "2ce2e90b0bc7224de3db1f0d646fe8e2c4dd37f1793928287f6074bc451a57ea",
      manifest->history(1).v1().id());

  EXPECT_EQ(
      "cf2616975b4a3cba083ca99bc3f0bf25f5f528c3c52be1596b30f60b0b1c37ff",
      manifest->history(2).v1().parent());

  EXPECT_EQ(1u, manifest->schemaversion());

  EXPECT_EQ(
      "LYRA:YAG2:QQKS:376F:QQXY:3UNK:SXH7:K6ES:Y5AU:XUN5:ZLVY:KBYL",
      manifest->signatures(0).header().jwk().kid());

  EXPECT_EQ(
      "m3bgdBXZYRQ4ssAbrgj8Kjl7GNgrKQvmCSY-00yzQosKi-8UBrIRrn3Iu5alj82B6u_jNrkGCjEx3TxrfT1rig", // NOLINT(whitespace/line_length)
      manifest->signatures(0).signature());
}


TEST_F(DockerSpecTest, ParseInvalidV2ImageManifest)
{
  // This is an invalid manifest. The size of the repeated fields
  // 'history' and 'fsLayers' must be >= 1. The 'signatures' and
  // 'schemaVersion' fields are not set.
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
      {
        "name": "dmcgowan/test-image",
        "tag": "latest",
        "architecture": "amd64"
      })~");

  ASSERT_SOME(json);

  Try<spec::v2::ImageManifest> manifest = spec::v2::parse(json.get());
  EXPECT_ERROR(manifest);
}


TEST_F(DockerSpecTest, ValidateV2ImageManifestFsLayersNonEmpty)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
    R"~(
    {
      "name": "dmcgowan/test-image",
      "tag": "latest",
      "architecture": "amd64",
      "schemaVersion": 1,
      "signatures": [
        {
          "header": {
            "jwk": {
              "crv": "P-256",
              "kid": "LYRA:YAG2:QQKS:376F:QQXY:3UNK:SXH7:K6ES:Y5AU:XUN5:ZLVY:KBYL",
              "kty": "EC",
              "x": "Cu_UyxwLgHzE9rvlYSmvVdqYCXY42E9eNhBb0xNv0SQ",
              "y": "zUsjWJkeKQ5tv7S-hl1Tg71cd-CqnrtiiLxSi6N_yc8"
            },
            "alg": "ES256"
          },
          "signature": "m3bgdBXZYRQ4ssAbrgj8Kjl7GNgrKQvmCSY-00yzQosKi-8UBrIRrn3Iu5alj82B6u_jNrkGCjEx3TxrfT1rig",
          "protected": "eyJmb3JtYXRMZW5ndGgiOjYwNjMsImZvcm1hdFRhaWwiOiJDbjAiLCJ0aW1lIjoiMjAxNC0wOS0xMVQxNzoxNDozMFoifQ"
        }
      ]
    })~");

  ASSERT_SOME(json);

  Try<spec::v2::ImageManifest> manifest = spec::v2::parse(json.get());
  EXPECT_ERROR(manifest);
}


TEST_F(DockerSpecTest, ValidateV2ImageManifestSignaturesNonEmpty)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
    R"~(
    {
      "name": "dmcgowan/test-image",
      "tag": "latest",
      "architecture": "amd64",
      "fsLayers": [
        {
          "blobSum": "sha256:2a7812e636235448785062100bb9103096aa6655a8f6bb9ac9b13fe8290f66df"
        }
      ],
      "schemaVersion": 1
    })~");

  ASSERT_SOME(json);

  Try<spec::v2::ImageManifest> manifest = spec::v2::parse(json.get());
  EXPECT_ERROR(manifest);
}


TEST_F(DockerSpecTest, ParseV2_2ImageManifest)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
      {
        "schemaVersion": 2,
        "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
        "config": {
            "mediaType": "application/vnd.docker.container.image.v1+json",
            "size": 7023,
            "digest": "sha256:b5b2b2c507a0944348e0303114d8d93aaaa081732b86451d9bce1f432a537bc7"
        },
        "layers": [
            {
                "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
                "size": 32654,
                "digest": "sha256:e692418e4cbaf90ca69d05a66403747baa33ee08806650b51fab815ad7fc331f"
            },
            {
                "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
                "size": 16724,
                "digest": "sha256:3c3a4604a545cdc127456d94e421cd355bca5b528f4a9c1905b15da2eb4a4c6b"
            },
            {
                "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
                "size": 73109,
                "digest": "sha256:ec4b8955958665577945c89419d1af06b5f7636b4ac3da7f12184802ad867736"
            }
        ]
      })~");

  ASSERT_SOME(json);

  Try<spec::v2_2::ImageManifest> manifest = spec::v2_2::parse(json.get());
  ASSERT_SOME(manifest);

  EXPECT_EQ(2u, manifest->schemaversion());
  EXPECT_EQ(
      "application/vnd.docker.distribution.manifest.v2+json",
      manifest->mediatype());

  EXPECT_EQ(
      "application/vnd.docker.container.image.v1+json",
      manifest->config().mediatype());
  EXPECT_EQ(7023u, manifest->config().size());
  EXPECT_EQ(
      "sha256:b5b2b2c507a0944348e0303114d8d93aaaa081732b86451d9bce1f432a537bc7",
      manifest->config().digest());

  EXPECT_EQ(3, manifest->layers_size());

  EXPECT_EQ(
      "application/vnd.docker.image.rootfs.diff.tar.gzip",
      manifest->layers(0).mediatype());
  EXPECT_EQ(32654u, manifest->layers(0).size());
  EXPECT_EQ(
      "sha256:e692418e4cbaf90ca69d05a66403747baa33ee08806650b51fab815ad7fc331f",
      manifest->layers(0).digest());

  EXPECT_EQ(
      "application/vnd.docker.image.rootfs.diff.tar.gzip",
      manifest->layers(1).mediatype());
  EXPECT_EQ(16724u, manifest->layers(1).size());
  EXPECT_EQ(
      "sha256:3c3a4604a545cdc127456d94e421cd355bca5b528f4a9c1905b15da2eb4a4c6b",
      manifest->layers(1).digest());

  EXPECT_EQ(
      "application/vnd.docker.image.rootfs.diff.tar.gzip",
      manifest->layers(2).mediatype());
  EXPECT_EQ(73109u, manifest->layers(2).size());
  EXPECT_EQ(
      "sha256:ec4b8955958665577945c89419d1af06b5f7636b4ac3da7f12184802ad867736",
      manifest->layers(2).digest());
}


TEST_F(DockerSpecTest, ParseInvalidV2_2ImageManifest)
{
  // This is an invalid manifest. The size of the repeated fields
  // 'layers' must be >= 1. The 'signatures' and
  // 'schemaVersion' fields are not set.
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
      {
        "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
        "config": {
            "mediaType": "application/vnd.docker.container.image.v1+json",
            "size": 7023,
            "digest": "sha256:b5b2b2c507a0944348e0303114d8d93aaaa081732b86451d9bce1f432a537bc7"
        }
      })~");

  ASSERT_SOME(json);

  Try<spec::v2_2::ImageManifest> manifest = spec::v2_2::parse(json.get());
  EXPECT_ERROR(manifest);
}


TEST_F(DockerSpecTest, ValidateV2_2ImageManifestLayersNonEmpty)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(
      R"~(
      {
        "schemaVersion": 2,
        "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
        "config": {
            "mediaType": "application/vnd.docker.container.image.v1+json",
            "size": 7023,
            "digest": "sha256:b5b2b2c507a0944348e0303114d8d93aaaa081732b86451d9bce1f432a537bc7"
        }
      })~");

  ASSERT_SOME(json);

  Try<spec::v2_2::ImageManifest> manifest = spec::v2_2::parse(json.get());
  EXPECT_ERROR(manifest);
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
