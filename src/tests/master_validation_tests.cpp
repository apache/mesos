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

#include <google/protobuf/repeated_field.h>

#include <gtest/gtest.h>

#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>

#include <stout/gtest.hpp>

#include "master/validation.hpp"

#include "tests/mesos.hpp"

using namespace mesos;
using namespace mesos::internal::tests;
using namespace mesos::internal::master::validation;

using google::protobuf::RepeatedPtrField;


class ResourceValidationTest : public ::testing::Test
{
protected:
  RepeatedPtrField<Resource> CreateResources(const Resource& resource)
  {
    RepeatedPtrField<Resource> resources;
    resources.Add()->CopyFrom(resource);
    return resources;
  }
};


TEST_F(ResourceValidationTest, PersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  EXPECT_NONE(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, UnreservedDiskInfo)
{
  Resource volume = Resources::parse("disk", "128", "*").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, InvalidPersistenceID)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1/", "path1"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, PersistentVolumeWithoutVolumeInfo)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", None()));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, ReadOnlyPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1", Volume::RO));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, PersistentVolumeWithHostPath)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(
      createDiskInfo("id1", "path1", Volume::RW, "foo"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


TEST_F(ResourceValidationTest, NonPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo(None(), "path1"));

  EXPECT_SOME(resource::validate(CreateResources(volume)));
}


class CreateOperationValidationTest : public ::testing::Test {};


// This test verifies that all resources specified in the CREATE
// operation are persistent volumes.
TEST_F(CreateOperationValidationTest, PersistentVolumes)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume);

  EXPECT_NONE(operation::validate(create, Resources()));

  Resource cpus = Resources::parse("cpus", "2", "*").get();

  create.add_volumes()->CopyFrom(cpus);

  EXPECT_SOME(operation::validate(create, Resources()));
}


TEST_F(CreateOperationValidationTest, DuplicatedPersistenceID)
{
  Resource volume1 = Resources::parse("disk", "128", "role1").get();
  volume1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume1);

  EXPECT_NONE(operation::validate(create, Resources()));

  Resource volume2 = Resources::parse("disk", "64", "role1").get();
  volume2.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  EXPECT_SOME(operation::validate(create, volume1));

  create.add_volumes()->CopyFrom(volume2);

  EXPECT_SOME(operation::validate(create, Resources()));
}


class DestroyOperationValidationTest : public ::testing::Test {};


// This test verifies that all resources specified in the DESTROY
// operation are persistent volumes.
TEST_F(DestroyOperationValidationTest, PersistentVolumes)
{
  Resource volume1 = Resources::parse("disk", "128", "role1").get();
  volume1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Resource volume2 = Resources::parse("disk", "64", "role1").get();
  volume2.mutable_disk()->CopyFrom(createDiskInfo("id2", "path2"));

  Resources volumes;
  volumes += volume1;
  volumes += volume2;

  Offer::Operation::Destroy destroy;
  destroy.add_volumes()->CopyFrom(volume1);

  EXPECT_NONE(operation::validate(destroy, volumes));

  Resource cpus = Resources::parse("cpus", "2", "*").get();

  destroy.add_volumes()->CopyFrom(cpus);

  EXPECT_SOME(operation::validate(destroy, volumes));
}


TEST_F(DestroyOperationValidationTest, UnknownPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Destroy destroy;
  destroy.add_volumes()->CopyFrom(volume);

  EXPECT_NONE(operation::validate(destroy, volume));
  EXPECT_SOME(operation::validate(destroy, Resources()));
}
