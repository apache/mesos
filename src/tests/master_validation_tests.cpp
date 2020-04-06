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

#include <google/protobuf/repeated_field.h>

#include <string>
#include <vector>

#include <gmock/gmock.h>

#include <mesos/executor.hpp>
#include <mesos/mesos.hpp>
#include <mesos/resources.hpp>
#include <mesos/scheduler.hpp>

#include <process/clock.hpp>
#include <process/gmock.hpp>
#include <process/gtest.hpp>
#include <process/owned.hpp>
#include <process/pid.hpp>

#include <stout/gtest.hpp>
#include <stout/none.hpp>
#include <stout/strings.hpp>
#include <stout/uuid.hpp>

#include "master/master.hpp"
#include "master/quota.hpp"
#include "master/validation.hpp"

#include "master/detector/standalone.hpp"

#include "slave/slave.hpp"

#include "tests/containerizer.hpp"
#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

#include "master/validation.hpp"

#include "master/detector/standalone.hpp"

using namespace mesos::internal::master::validation;

using google::protobuf::Map;
using google::protobuf::RepeatedPtrField;

using mesos::internal::master::Master;

using mesos::internal::slave::Slave;

using mesos::master::detector::MasterDetector;
using mesos::master::detector::StandaloneMasterDetector;

using process::Clock;
using process::Future;
using process::Message;
using process::Owned;
using process::PID;

using std::pair;
using std::string;
using std::vector;

using testing::_;
using testing::AtMost;
using testing::Eq;
using testing::Return;

namespace mesos {
namespace internal {
namespace tests {


TEST(MasterCallValidationTest, UpdateQuota)
{
  Option<Error> error;

  // Test validation at the call level.
  mesos::master::Call updateQuota;
  updateQuota.set_type(mesos::master::Call::UPDATE_QUOTA);

  // Missing `update_quota` field.
  error = master::validation::master::call::validate(updateQuota);
  EXPECT_SOME(error);

  updateQuota.mutable_update_quota();

  error = master::validation::master::call::validate(updateQuota);
  EXPECT_NONE(error);

  // Test validation at the config level.
  mesos::quota::QuotaConfig config;

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(error->message, "'QuotaConfig.role' must be set"))
    << error->message;

  config.set_role("");

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(error->message, "Invalid 'QuotaConfig.role'"))
    << error->message;

  // Once a role is set, it is valid to have no guarantee and no limit.
  config.set_role("role");
  error = mesos::internal::master::quota::validate(config);
  ASSERT_NONE(error);

  // Setting quota on "*" role is allowed.
  config.set_role("*");
  error = mesos::internal::master::quota::validate(config);
  ASSERT_NONE(error);

  auto resourceMap = [](const vector<pair<string, double>>& vector)
    -> Map<string, Value::Scalar> {
    Map<string, Value::Scalar> result;

    foreachpair (const string& name, double value, vector) {
      Value::Scalar scalar;
      scalar.set_value(value);
      result[name] = scalar;
    }

    return result;
  };

  // The quota endpoint only allows memory / disk up to
  // 1 exabyte (in megabytes) or 1 trillion cores/ports/other.
  double largestMegabytes = 1024.0 * 1024.0 * 1024.0 * 1024.0;
  double largestCpuPortsOrOther = 1000.0 * 1000.0 * 1000.0 * 1000.0;

  *config.mutable_guarantees() = resourceMap({
    {"disk", largestMegabytes},
    {"mem", largestMegabytes},
    {"cpus", largestCpuPortsOrOther},
    {"ports", largestCpuPortsOrOther},
    {"foobars", largestCpuPortsOrOther}
  });
  *config.mutable_limits() = resourceMap({
    {"disk", largestMegabytes},
    {"mem", largestMegabytes},
    {"cpus", largestCpuPortsOrOther},
    {"ports", largestCpuPortsOrOther},
    {"foobars", largestCpuPortsOrOther},
  });

  error = mesos::internal::master::quota::validate(config);
  EXPECT_NONE(error)
    << error->message;

  config.clear_guarantees();
  *config.mutable_limits() = resourceMap({{"disk", largestMegabytes + 1.0}});

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "values greater than 1 exabyte (1099511627776) are not supported"))
    << error->message;

  config.clear_guarantees();
  *config.mutable_limits() = resourceMap({{"mem", largestMegabytes + 1.0}});

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "values greater than 1 exabyte (1099511627776) are not supported"))
    << error->message;

  config.clear_limits();
  *config.mutable_guarantees() = resourceMap({
    {"cpus", largestCpuPortsOrOther + 1.0}});

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "values greater than 1 trillion (1000000000000) are not supported"))
    << error->message;

  config.clear_limits();
  *config.mutable_guarantees() = resourceMap({
    {"ports", largestCpuPortsOrOther + 1.0}});

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "values greater than 1 trillion (1000000000000) are not supported"))
    << error->message;

  config.clear_limits();
  *config.mutable_guarantees() = resourceMap({
    {"foobars", largestCpuPortsOrOther + 1.0}});

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "values greater than 1 trillion (1000000000000) are not supported"))
    << error->message;

  // Now test the guarantees <= limits validation.

  // Guarantees > limits.
  Map<string, Value::Scalar> superset =
    resourceMap({{"cpus", 20}, {"mem", 40}});
  Map<string, Value::Scalar> subset = resourceMap({{"cpus", 10}, {"mem", 20}});

  *config.mutable_guarantees() = superset;
  *config.mutable_limits() = subset;

  error = mesos::internal::master::quota::validate(config);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'QuotaConfig.guarantees' { cpus: 20, mem: 40 } is not"
      " contained within the 'QuotaConfig.limits' { cpus: 10, mem: 20 }"))
    << error->message;

  // Guarantees = limits.
  *config.mutable_guarantees() = subset;
  *config.mutable_limits() = subset;

  error = mesos::internal::master::quota::validate(config);
  EXPECT_NONE(error);

  // Guarantees < limits.
  *config.mutable_guarantees() = subset;
  *config.mutable_limits() = superset;

  error = mesos::internal::master::quota::validate(config);
  EXPECT_NONE(error);

  // Now we ensure that the guarantees <= limits check is a
  // per-resource check. This is important because it's ok to:
  //
  //   (1) Set a limit for a resource when there is no guarantee
  //       for the resource.
  //
  //   (2) Set a guarantee for a resource when there is no limit
  //       for the resource.
  //
  // We test both cases at once by having both guarantee and
  // limit contain a resource not specified in the other.
  Map<string, Value::Scalar> cpuMemWithDisk =
    resourceMap({{"cpus", 10}, {"mem", 20}, {"disk", 10}});
  Map<string, Value::Scalar> cpuMemWithGpu =
    resourceMap({{"cpus", 10}, {"mem", 20}, {"gpu", 1}});

  *config.mutable_guarantees() = cpuMemWithDisk;
  *config.mutable_limits() = cpuMemWithGpu;

  error = mesos::internal::master::quota::validate(config);
  EXPECT_NONE(error);
}


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


TEST_F(ResourceValidationTest, StaticReservation)
{
  Resource resource = Resources::parse("cpus", "8", "role").get();

  Option<Error> error = resource::validate(CreateResources(resource));

  EXPECT_NONE(error);
}


TEST_F(ResourceValidationTest, DynamicReservation)
{
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Option<Error> error = resource::validate(CreateResources(resource));

  EXPECT_NONE(error);
}


TEST_F(ResourceValidationTest, RevocableDynamicReservation)
{
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));
  resource.mutable_revocable();

  Option<Error> error = resource::validate(CreateResources(resource));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message, "cannot be created from revocable resources"));
}


TEST_F(ResourceValidationTest, PersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Option<Error> error = resource::validate(CreateResources(volume));

  EXPECT_NONE(error);
}


TEST_F(ResourceValidationTest, UnreservedDiskInfo)
{
  Resource volume = Resources::parse("disk", "128", "*").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Option<Error> error = resource::validate(CreateResources(volume));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "Persistent volumes cannot be created from unreserved resources"));
}


TEST_F(ResourceValidationTest, InvalidPersistenceID)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1/", "path1"));

  Option<Error> error = resource::validate(CreateResources(volume));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "Invalid persistence ID for persistent volume: 'id1/' contains "
          "invalid characters"));
}


TEST_F(ResourceValidationTest, PersistentVolumeWithoutVolumeInfo)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", None()));

  Option<Error> error = resource::validate(CreateResources(volume));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "Expecting 'volume' to be set for persistent volume"));
}


TEST_F(ResourceValidationTest, PersistentVolumeWithHostPath)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(
      createDiskInfo("id1", "path1", Volume::RW, "foo"));

  Option<Error> error = resource::validate(CreateResources(volume));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "Expecting 'host_path' to be unset for persistent volume"));
}


TEST_F(ResourceValidationTest, NonPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo(None(), "path1"));

  Option<Error> error = resource::validate(CreateResources(volume));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(error->message, "Non-persistent volume not supported"));
}


TEST_F(ResourceValidationTest, RevocablePersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));
  volume.mutable_revocable();

  Option<Error> error = resource::validate(CreateResources(volume));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "Persistent volumes cannot be created from revocable resources"));
}


TEST_F(ResourceValidationTest, UnshareableResource)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_shared();

  Option<Error> error = resource::validate(CreateResources(volume));

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message, "Only persistent volumes can be shared"));
}


TEST_F(ResourceValidationTest, SharedPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));
  volume.mutable_shared();

  Option<Error> error = resource::validate(CreateResources(volume));

  EXPECT_NONE(error);
}


TEST_F(ResourceValidationTest, SingleResourceProvider)
{
  Resource r1 = Resources::parse("disk", "128", "*").get();
  r1.mutable_provider_id()->set_value("provider1");

  Resource r2 = Resources::parse("cpu", "4", "*").get();
  r2.mutable_provider_id()->set_value("provider2");

  Resource r3 = Resources::parse("mem", "256", "*").get();
  r3.mutable_provider_id()->set_value("provider2");

  Resource r4 = Resources::parse("disk", "64", "*").get();

  {
    Resources resources = r1;

    EXPECT_NONE(resource::internal::validateSingleResourceProvider(resources));
  }

  {
    Resources resources = r4;

    EXPECT_NONE(resource::internal::validateSingleResourceProvider(resources));
  }

  {
    Resources resources;
    resources += r2;
    resources += r3;

    EXPECT_NONE(resource::internal::validateSingleResourceProvider(resources));
  }

  {
    Resources resources;
    resources += r1;
    resources += r2;

    Option<Error> error =
      resource::internal::validateSingleResourceProvider(resources);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "The resources have multiple resource providers"));
  }

  {
    Resources resources;
    resources += r1;
    resources += r4;

    Option<Error> error =
      resource::internal::validateSingleResourceProvider(resources);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Some resources have a resource provider and some do not"));
  }
}


// Unit test for the `resource::detectOverlappingSetAndRangeResources()` helper
// function. This function should return `true` when it receives two or more
// `Resources` objects which contain overlapping sets or ranges.
TEST_F(ResourceValidationTest, OverlappingSetsAndRanges)
{
  // An empty vector, which cannot overlap.
  EXPECT_FALSE(resource::detectOverlappingSetAndRangeResources({}));

  // One group of resources containing a set, which cannot overlap.
  EXPECT_FALSE(
      resource::detectOverlappingSetAndRangeResources(
          {CHECK_NOTERROR(Resources::parse("zones:{a,b,c}"))}));

  // Two groups of resources with non-overlapping ranges.
  EXPECT_FALSE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("ports:[5555-5556]")),
          CHECK_NOTERROR(Resources::parse("ports:[5557-5558]"))
      }));

  // Two groups of resources with identical ranges.
  EXPECT_TRUE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("ports:[5555-5556]")),
          CHECK_NOTERROR(Resources::parse("ports:[5555-5556]"))
      }));

  // Two groups of resources with overlapping ranges.
  EXPECT_TRUE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("ports:[5555-5557]")),
          CHECK_NOTERROR(Resources::parse("ports:[5556-5558]"))
      }));

  // Three groups of resources, two of which have overlapping ranges.
  EXPECT_TRUE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("ports:[5555-5557]")),
          CHECK_NOTERROR(Resources::parse("ports:[5558-5559]")),
          CHECK_NOTERROR(Resources::parse("ports:[5559-5560]"))
      }));

  // Two groups of resources with overlapping ranges
  // and other scalar resources.
  EXPECT_TRUE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("cpus:0.1;mem:32;ports:[5555-5557]")),
          CHECK_NOTERROR(
              Resources::parse("cpus:1.0;mem:2048;disk:4096;ports:[5556-5558]"))
      }));

  // Two groups of resources with non-overlapping sets.
  EXPECT_FALSE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("zones:{a,b}")),
          CHECK_NOTERROR(Resources::parse("zones:{c,d}"))
      }));

  // Two groups of resources with identical sets.
  EXPECT_TRUE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("zones:{a,b}")),
          CHECK_NOTERROR(Resources::parse("zones:{a,b}"))
      }));

  // Two groups of resources with overlapping sets.
  EXPECT_TRUE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("zones:{a,b,c}")),
          CHECK_NOTERROR(Resources::parse("zones:{c,d,e}"))
      }));

  // Three groups of resources, two of which have overlapping sets.
  EXPECT_TRUE(
      resource::detectOverlappingSetAndRangeResources({
          CHECK_NOTERROR(Resources::parse("zones:{a,b,c}")),
          CHECK_NOTERROR(Resources::parse("zones:{d,e,f}")),
          CHECK_NOTERROR(Resources::parse("zones:{g,h,a}"))
      }));
}


class ReserveOperationValidationTest : public MesosTest {};


// This test verifies that validation fails if the reservation's role
// doesn't match the framework's role.
TEST_F(ReserveOperationValidationTest, MatchingRole)
{
  protobuf::slave::Capabilities capabilities;

  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("resourceRole", "principal"));

  Offer::Operation::Reserve reserve;
  reserve.mutable_resources()->CopyFrom(
      allocatedResources(resource, Resources::reservationRole(resource)));

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("frameworkRole");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "A reserve operation was attempted for a resource allocated "
          "to role 'resourceRole', but the framework only has roles "
          "'{ frameworkRole }'"));

  // Now verify with a MULTI_ROLE framework.
  frameworkInfo.clear_role();
  frameworkInfo.add_roles("role1");
  frameworkInfo.add_roles("role2");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  // We expect an error due to the framework not having the role of the reserved
  // resource. We only check part of the error message here as internally the
  // list of the framework's roles does not have a particular order.
  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "A reserve operation was attempted for a resource allocated to "
          "role 'resourceRole', but the framework only has roles "));
}


// This test verifies that validation fails if reserving to the "*" role.
TEST_F(ReserveOperationValidationTest, DisallowReservingToStar)
{
  protobuf::slave::Capabilities capabilities;

  // The role "*" matches, but is invalid since frameworks with
  // "*" role cannot reserve resources.
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("*", "principal"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("*");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message, "Invalid reservation: role \"*\" cannot be reserved"));

  // Now verify with a MULTI_ROLE framework.
  frameworkInfo.clear_role();
  frameworkInfo.add_roles("role");
  frameworkInfo.add_roles("*");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message, "Invalid reservation: role \"*\" cannot be reserved"));
}


// This test verifies that the 'principal' specified in the resources
// of the RESERVE operation needs to match the framework's 'principal'.
TEST_F(ReserveOperationValidationTest, MatchingPrincipal)
{
  protobuf::slave::Capabilities capabilities;

  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Offer::Operation::Reserve reserve;
  reserve.mutable_resources()->CopyFrom(
      allocatedResources(resource, Resources::reservationRole(resource)));

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("role");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  EXPECT_NONE(error) << error->message;
}


// This test verifies that validation fails if the 'principal'
// specified in the resources of the RESERVE operation do not match
// the framework's 'principal'.
TEST_F(ReserveOperationValidationTest, NonMatchingPrincipal)
{
  protobuf::slave::Capabilities capabilities;

  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal2"));

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("role");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal1", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "A reserve operation was attempted by authenticated principal "
      "'principal1', which does not match a reserved resource in the "
      "request with principal 'principal2'"));
}


// This test verifies that validation fails if the `principal`
// in `ReservationInfo` is not set.
TEST_F(ReserveOperationValidationTest, ReservationInfoMissingPrincipal)
{
  protobuf::slave::Capabilities capabilities;

  Resource::ReservationInfo reservationInfo;
  reservationInfo.set_type(Resource::ReservationInfo::DYNAMIC);
  reservationInfo.set_role("role");

  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(reservationInfo);

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(resource);

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("role");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "A reserve operation was attempted by principal 'principal', but "
      "there is a reserved resource in the request with no principal set"));
}


// This test verifies that validation fails if there are statically
// reserved resources specified in the RESERVE operation.
TEST_F(ReserveOperationValidationTest, StaticReservation)
{
  protobuf::slave::Capabilities capabilities;

  Resource staticallyReserved = Resources::parse("cpus", "8", "role").get();

  Offer::Operation::Reserve reserve;
  reserve.add_resources()->CopyFrom(staticallyReserved);

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("role");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(error->message, "is not dynamically reserved"));
}


// This test verifies that validation fails if there are persistent
// volumes specified in the resources of the RESERVE operation.
TEST_F(ReserveOperationValidationTest, NoPersistentVolumes)
{
  protobuf::slave::Capabilities capabilities;

  Resource reserved = Resources::parse("cpus", "8", "*").get();
  reserved.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Resource volume = Resources::parse("disk", "128", "role").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Reserve reserve;

  reserve.mutable_resources()->CopyFrom(
      allocatedResources(reserved, Resources::reservationRole(reserved)));

  reserve.mutable_resources()->MergeFrom(
      allocatedResources(volume, Resources::reservationRole(volume)));

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("role");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(error->message, "is not dynamically reserved"));
}


// This test verifies that validation fails if a resource is reserved
// for a role different from the one it was allocated to.
TEST_F(ReserveOperationValidationTest, MismatchedAllocation)
{
  protobuf::slave::Capabilities capabilities;

  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role1", "principal"));

  Offer::Operation::Reserve reserve;
  reserve.mutable_resources()->CopyFrom(
      allocatedResources(resource, "role2"));

  FrameworkInfo frameworkInfo;
  frameworkInfo.add_roles("role1");
  frameworkInfo.add_roles("role2");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "A reserve operation was attempted for a resource with role "
          "'role1', but the resource was allocated to role 'role2'"));
}


// This test verifies that validation fails if an allocated resource
// is used in the operator HTTP API.
TEST_F(ReserveOperationValidationTest, UnexpectedAllocatedResource)
{
  protobuf::slave::Capabilities capabilities;

  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Offer::Operation::Reserve reserve;
  reserve.mutable_resources()->CopyFrom(allocatedResources(resource, "role"));

  // HTTP-API style invocations do not pass a `FrameworkInfo`.
  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, None());

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "A reserve operation was attempted with an allocated resource,"
          " but the operator API only allows reservations to be made"
          " to unallocated resources"));
}


TEST_F(ReserveOperationValidationTest, MixedAllocationRoles)
{
  protobuf::slave::Capabilities capabilities;

  Resource resource1 = Resources::parse("cpus", "8", "*").get();
  resource1.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role1", "principal"));
  Resource resource2 = Resources::parse("mem", "8", "*").get();
  resource2.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role2", "principal"));

  Offer::Operation::Reserve reserve;
  reserve.mutable_resources()->CopyFrom(
      allocatedResources(resource1, "role1") +
      allocatedResources(resource2, "role2"));

  FrameworkInfo frameworkInfo;
  frameworkInfo.add_roles("role1");
  frameworkInfo.add_roles("role2");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error =
    operation::validate(reserve, "principal", capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "Invalid reservation resources: The resources have multiple"
          " allocation roles ('role2' and 'role1') but only one allocation"
          " role is allowed"));
}


TEST_F(ReserveOperationValidationTest, AgentHierarchicalRoleCapability)
{
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("foo/bar", "principal"));

  Offer::Operation::Reserve reserve;
  reserve.mutable_resources()->CopyFrom(
      allocatedResources(resource, Resources::reservationRole(resource)));

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("foo/bar");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  {
    protobuf::slave::Capabilities capabilities;
    capabilities.hierarchicalRole = true;

    Option<Error> error =
      operation::validate(reserve, "principal", capabilities, frameworkInfo);

    EXPECT_NONE(error);
  }

  {
    protobuf::slave::Capabilities capabilities;
    capabilities.hierarchicalRole = false;

    Option<Error> error =
      operation::validate(reserve, "principal", capabilities, frameworkInfo);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "with reservation for hierarchical role 'foo/bar' cannot be reserved "
        "on an agent without HIERARCHICAL_ROLE capability"));
  }
}


// This test verifies that validation fails if resources from multiple
// resource providers are used.
TEST_F(ReserveOperationValidationTest, MultipleResourceProviders)
{
  protobuf::slave::Capabilities capabilities;

  FrameworkInfo frameworkInfo;
  frameworkInfo.set_role("role");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Resource resource1 = Resources::parse("cpus", "8", "*").get();
  resource1.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Resource resource2 = Resources::parse("disk", "10", "*").get();
  resource2.mutable_provider_id()->set_value("provider1");
  resource2.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Resource resource3 = Resources::parse("disk", "10", "*").get();
  resource3.mutable_provider_id()->set_value("provider2");
  resource3.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  {
    Offer::Operation::Reserve reserve;
    reserve.mutable_resources()->CopyFrom(
        allocatedResources(Resources(resource1) + resource2, "role"));

    Option<Error> error =
      operation::validate(reserve, "principal", capabilities, frameworkInfo);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: Some resources have a resource provider "
        "and some do not"));
  }

  {
    Offer::Operation::Reserve reserve;
    reserve.mutable_resources()->CopyFrom(
        allocatedResources(Resources(resource2) + resource3, "role"));

    Option<Error> error =
      operation::validate(reserve, "principal", capabilities, frameworkInfo);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: The resources have multiple resource providers: "
        "provider2, provider1"));
  }

  {
    Offer::Operation::Reserve reserve;
    reserve.mutable_resources()->CopyFrom(
        allocatedResources(resource2, "role"));

    Option<Error> error =
      operation::validate(reserve, "principal", capabilities, frameworkInfo);

    EXPECT_NONE(error) << error->message;
  }
}


class UnreserveOperationValidationTest : public MesosTest {};


// This test verifies that validation succeeds if the reservation includes a
// `principal`.
TEST_F(UnreserveOperationValidationTest, WithoutACL)
{
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(resource);

  Option<Error> error = operation::validate(unreserve);

  EXPECT_NONE(error);
}


// This test verifies that validation succeeds if the reservation's
// `principal` is not set.
TEST_F(UnreserveOperationValidationTest, FrameworkMissingPrincipal)
{
  Resource resource = Resources::parse("cpus", "8", "*").get();
  resource.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role"));

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(resource);

  Option<Error> error = operation::validate(unreserve);

  EXPECT_NONE(error);
}


// This test verifies that validation fails if there are statically
// reserved resources specified in the UNRESERVE operation.
TEST_F(UnreserveOperationValidationTest, StaticReservation)
{
  Resource staticallyReserved = Resources::parse("cpus", "8", "role").get();

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(staticallyReserved);

  Option<Error> error = operation::validate(unreserve);

  EXPECT_SOME(error);
}


// This test verifies that validation fails if there are persistent
// volumes specified in the resources of the UNRESERVE operation.
TEST_F(UnreserveOperationValidationTest, NoPersistentVolumes)
{
  Resource reserved = Resources::parse("cpus", "8", "*").get();
  reserved.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Resource volume = Resources::parse("disk", "128", "role").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Unreserve unreserve;
  unreserve.add_resources()->CopyFrom(reserved);
  unreserve.add_resources()->CopyFrom(volume);

  Option<Error> error = operation::validate(unreserve);

  EXPECT_SOME(error);
}


// This test verifies that validation fails if resources from multiple
// resource providers are used.
TEST_F(UnreserveOperationValidationTest, MultipleResourceProviders)
{
  Resource resource1 = Resources::parse("cpus", "8", "*").get();
  resource1.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Resource resource2 = Resources::parse("disk", "10", "*").get();
  resource2.mutable_provider_id()->set_value("provider1");
  resource2.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  Resource resource3 = Resources::parse("disk", "10", "*").get();
  resource3.mutable_provider_id()->set_value("provider2");
  resource3.add_reservations()->CopyFrom(
      createDynamicReservationInfo("role", "principal"));

  {
    Offer::Operation::Unreserve unreserve;
    unreserve.add_resources()->CopyFrom(resource1);
    unreserve.add_resources()->CopyFrom(resource2);

    Option<Error> error = operation::validate(unreserve);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: Some resources have a resource provider "
        "and some do not"));
  }

  {
    Offer::Operation::Unreserve unreserve;
    unreserve.add_resources()->CopyFrom(resource2);
    unreserve.add_resources()->CopyFrom(resource3);

    Option<Error> error = operation::validate(unreserve);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: The resources have multiple resource providers: "
        "provider2, provider1"));
  }

  {
    Offer::Operation::Unreserve unreserve;
    unreserve.add_resources()->CopyFrom(resource2);

    Option<Error> error = operation::validate(unreserve);

    EXPECT_NONE(error) << error->message;
  }
}


class CreateOperationValidationTest : public MesosTest {};


// This test verifies that validation fails if some resources specified in
// the CREATE operation are not persistent volumes.
TEST_F(CreateOperationValidationTest, PersistentVolumes)
{
  protobuf::slave::Capabilities capabilities;

  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume);

  Option<Error> error =
    operation::validate(create, Resources(), None(), capabilities);

  EXPECT_NONE(error);

  Resource cpus = Resources::parse("cpus", "2", "*").get();

  create.add_volumes()->CopyFrom(cpus);

  error = operation::validate(create, Resources(), None(), capabilities);

  EXPECT_SOME(error);
}


TEST_F(CreateOperationValidationTest, DuplicatedPersistenceID)
{
  protobuf::slave::Capabilities capabilities;

  Resource volume1 = Resources::parse("disk", "128", "role1").get();
  volume1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume1);

  Option<Error> error =
    operation::validate(create, Resources(), None(), capabilities);

  EXPECT_NONE(error);

  Resource volume2 = Resources::parse("disk", "64", "role1").get();
  volume2.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  error = operation::validate(create, volume1, None(), capabilities);

  EXPECT_SOME(error);

  create.add_volumes()->CopyFrom(volume2);

  error = operation::validate(create, Resources(), None(), capabilities);

  EXPECT_SOME(error);
}


// This test confirms that Create operations will be invalidated if they contain
// a principal in `DiskInfo` that does not match the principal of the framework
// or operator performing the operation.
TEST_F(CreateOperationValidationTest, NonMatchingPrincipal)
{
  protobuf::slave::Capabilities capabilities;

  // An operation with an incorrect principal in `DiskInfo.Persistence`.
  {
    Resource volume = Resources::parse("disk", "128", "role1").get();
    volume.mutable_disk()->CopyFrom(
        createDiskInfo("id1", "path1", None(), None(), None(), "principal"));

    Offer::Operation::Create create;
    create.add_volumes()->CopyFrom(volume);

    Option<Error> error =
      operation::validate(create, Resources(), "other-principal", capabilities);

    EXPECT_SOME(error);
  }

  // An operation without a principal in `DiskInfo.Persistence`.
  {
    Resource volume = Resources::parse("disk", "128", "role1").get();
    volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

    Offer::Operation::Create create;
    create.add_volumes()->CopyFrom(volume);

    Option<Error> error =
      operation::validate(create, Resources(), "principal", capabilities);

    EXPECT_SOME(error);
  }
}


TEST_F(CreateOperationValidationTest, ReadOnlyPersistentVolume)
{
  protobuf::slave::Capabilities capabilities;

  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1", Volume::RO));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume);

  Option<Error> error =
    operation::validate(create, Resources(), None(), capabilities);

  EXPECT_SOME(error);
}


TEST_F(CreateOperationValidationTest, SharedVolumeBasedOnCapability)
{
  protobuf::slave::Capabilities capabilities;

  Resource volume = createDiskResource(
      "128", "role1", "1", "path1", None(), true); // Shared.
  volume.mutable_allocation_info()->set_role("role1");

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume);

  // When no FrameworkInfo is specified, validation is not dependent
  // on any framework.
  Option<Error> error =
    operation::validate(create, Resources(), None(), capabilities);

  EXPECT_NONE(error);

  // When a FrameworkInfo with no SHARED_RESOURCES capability is
  // specified, the validation should fail.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");

  error = operation::validate(
      create, Resources(), None(), capabilities, frameworkInfo);

  EXPECT_SOME(error);

  // When a FrameworkInfo with SHARED_RESOURCES capability is specified,
  // the validation should succeed.
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::SHARED_RESOURCES);

  error = operation::validate(
      create, Resources(), None(), capabilities, frameworkInfo);

  EXPECT_NONE(error);
}


// This test verifies that creating a persistent volume that is larger
// than the offered disk resource results won't succeed.
TEST_F(CreateOperationValidationTest, InsufficientDiskResource)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_roles(0, "role1");

  master::Flags masterFlags = CreateMasterFlags();

  ACLs acls;
  mesos::ACL::RegisterFramework* acl = acls.add_register_frameworks();
  acl->mutable_principals()->add_values(frameworkInfo.principal());
  acl->mutable_roles()->add_values(frameworkInfo.roles(0));

  masterFlags.acls = acls;
  masterFlags.roles = "role1";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  slave::Flags slaveFlags = CreateSlaveFlags();
  slaveFlags.resources = "disk(role1):1024";

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return());        // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];

  // Since the CREATE operation will fail, we don't expect any
  // CheckpointResourcesMessage to be sent.
  EXPECT_NO_FUTURE_PROTOBUFS(CheckpointResourcesMessage(), _, _);

  Resources volume = createPersistentVolume(
      Megabytes(2048),
      "role1",
      "id1",
      "path1");

  // We want to be notified immediately with new offer.
  Filters filters;
  filters.set_refuse_seconds(0);

  driver.acceptOffers(
      {offer1.id()},
      {CREATE(volume)},
      filters);

  // Advance the clock to trigger another allocation.
  Clock::pause();

  Clock::settle();
  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  Offer offer2 = offers2.get()[0];

  EXPECT_EQ(Resources(offer1.resources()), Resources(offer2.resources()));

  Clock::resume();

  driver.stop();
  driver.join();
}


TEST_F(CreateOperationValidationTest, MixedAllocationRole)
{
  protobuf::slave::Capabilities capabilities;

  Resource volume1 = Resources::parse("disk", "128", "role1").get();
  volume1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));
  Resource volume2 = Resources::parse("disk", "256", "role2").get();
  volume2.mutable_disk()->CopyFrom(createDiskInfo("id2", "path2"));

  Offer::Operation::Create create;
  create.mutable_volumes()->CopyFrom(
      allocatedResources(volume1, "role1") +
      allocatedResources(volume2, "role2"));

  FrameworkInfo frameworkInfo;
  frameworkInfo.add_roles("role1");
  frameworkInfo.add_roles("role2");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::RESERVATION_REFINEMENT);

  Option<Error> error = operation::validate(
      create, Resources(), None(), capabilities, frameworkInfo);

  ASSERT_SOME(error);
  EXPECT_TRUE(
      strings::contains(
          error->message,
          "Invalid volume resources: The resources have multiple allocation"
          " roles ('role2' and 'role1') but only one allocation role is"
          " allowed"));
}


TEST_F(CreateOperationValidationTest, AgentHierarchicalRoleCapability)
{
  Resource volume = Resources::parse("disk", "128", "foo/bar").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Create create;
  create.add_volumes()->CopyFrom(volume);

  {
    protobuf::slave::Capabilities capabilities;
    capabilities.hierarchicalRole = true;

    Option<Error> error =
      operation::validate(create, Resources(), None(), capabilities);

    EXPECT_NONE(error);
  }

  {
    protobuf::slave::Capabilities capabilities;
    capabilities.hierarchicalRole = false;

    Option<Error> error =
      operation::validate(create, Resources(), None(), capabilities);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "with reservation for hierarchical role 'foo/bar' cannot be created "
        "on an agent without HIERARCHICAL_ROLE capability"));
  }
}


// This test verifies that validation fails if resources from multiple
// resource providers are used.
TEST_F(CreateOperationValidationTest, MultipleResourceProviders)
{
  protobuf::slave::Capabilities capabilities;

  Resource resource1 = Resources::parse("disk", "10", "role").get();
  resource1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));


  Resource resource2 = Resources::parse("disk", "10", "role").get();
  resource2.mutable_provider_id()->set_value("provider1");
  resource2.mutable_disk()->CopyFrom(createDiskInfo("id2", "path2"));

  Resource resource3 = Resources::parse("disk", "10", "role").get();
  resource3.mutable_provider_id()->set_value("provider2");
  resource3.mutable_disk()->CopyFrom(createDiskInfo("id3", "path3"));

  {
    Offer::Operation::Create create;
    create.add_volumes()->CopyFrom(resource1);
    create.add_volumes()->CopyFrom(resource2);

    Option<Error> error =
      operation::validate(create, Resources(), None(), capabilities);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: Some resources have a resource provider "
        "and some do not"));
  }

  {
    Offer::Operation::Create create;
    create.add_volumes()->CopyFrom(resource2);
    create.add_volumes()->CopyFrom(resource3);

    Option<Error> error =
      operation::validate(create, Resources(), None(), capabilities);

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: The resources have multiple resource providers: "
        "provider2, provider1"));
  }

  {
    Offer::Operation::Create create;
    create.add_volumes()->CopyFrom(resource2);

    Option<Error> error =
      operation::validate(create, Resources(), None(), capabilities);

    EXPECT_NONE(error) << error->message;
  }
}


class DestroyOperationValidationTest : public ::testing::Test {};


// This test verifies that validation fails if some resources specified in
// the DESTROY operation are not persistent volumes.
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

  Option<Error> error = operation::validate(destroy, volumes, {}, {});

  EXPECT_NONE(error);

  Resource cpus = Resources::parse("cpus", "2", "*").get();

  destroy.add_volumes()->CopyFrom(cpus);

  error = operation::validate(destroy, volumes, {}, {});

  EXPECT_SOME(error);
}


// This test verifies that DESTROY for shared persistent volumes
// is only valid when the volumes are no longer in use.
TEST_F(DestroyOperationValidationTest, SharedPersistentVolumeInUse)
{
  Resource cpus = Resources::parse("cpus", "1", "*").get();
  Resource mem = Resources::parse("mem", "5", "*").get();
  Resource disk1 = createDiskResource(
      "50", "role1", "1", "path1", None(), true); // Shared.
  Resource disk2 = createDiskResource("100", "role1", "2", "path2");

  Resources volumes;
  volumes += disk1;
  volumes += disk2;

  hashmap<FrameworkID, Resources> usedResources;
  FrameworkID frameworkId1;
  FrameworkID frameworkId2;
  frameworkId1.set_value("id1");
  frameworkId2.set_value("id2");

  // Add used resources for 1st framework.
  usedResources[frameworkId1] = Resources(cpus) + mem + disk1 + disk2;

  // Add used resources for 2nd framework.
  usedResources[frameworkId2] = Resources(cpus) + mem + disk1;

  Offer::Operation::Destroy destroy;
  destroy.add_volumes()->CopyFrom(disk1);

  Option<Error> error =
    operation::validate(destroy, volumes, usedResources, {});

  EXPECT_SOME(error);

  usedResources[frameworkId1] -= disk1;
  usedResources[frameworkId2] -= disk1;

  error = operation::validate(destroy, volumes, usedResources, {});

  EXPECT_NONE(error);
}


TEST_F(DestroyOperationValidationTest, UnknownPersistentVolume)
{
  Resource volume = Resources::parse("disk", "128", "role1").get();
  volume.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Offer::Operation::Destroy destroy;
  destroy.add_volumes()->CopyFrom(volume);

  Option<Error> error = operation::validate(destroy, volume, {}, {});

  EXPECT_NONE(error);

  error = operation::validate(destroy, Resources(), {}, {});

  EXPECT_SOME(error);
}


// This test verifies that validation fails if resources from multiple
// resource providers are used.
TEST_F(DestroyOperationValidationTest, MultipleResourceProviders)
{
  Resource resource1 = Resources::parse("disk", "10", "role").get();
  resource1.mutable_disk()->CopyFrom(createDiskInfo("id1", "path1"));

  Resource resource2 = Resources::parse("disk", "10", "role").get();
  resource2.mutable_provider_id()->set_value("provider1");
  resource2.mutable_disk()->CopyFrom(createDiskInfo("id2", "path2"));

  Resource resource3 = Resources::parse("disk", "10", "role").get();
  resource3.mutable_provider_id()->set_value("provider2");
  resource3.mutable_disk()->CopyFrom(createDiskInfo("id3", "path3"));

  Resources volumes = Resources(resource1) + resource2 + resource3;

  {
    Offer::Operation::Destroy destroy;
    destroy.add_volumes()->CopyFrom(resource1);
    destroy.add_volumes()->CopyFrom(resource2);

    Option<Error> error = operation::validate(destroy, volumes, {}, {});

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: Some resources have a resource provider "
        "and some do not"));
  }

  {
    Offer::Operation::Destroy destroy;
    destroy.add_volumes()->CopyFrom(resource2);
    destroy.add_volumes()->CopyFrom(resource3);

    Option<Error> error = operation::validate(destroy, volumes, {}, {});

    ASSERT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "Invalid resources: The resources have multiple resource providers: "
        "provider2, provider1"));
  }

  {
    Offer::Operation::Destroy destroy;
    destroy.add_volumes()->CopyFrom(resource2);

    Option<Error> error = operation::validate(destroy, volumes, {}, {});

    EXPECT_NONE(error) << error->message;
  }
}


class GrowVolumeOperationValidationTest : public MesosTest {
protected:
  Offer::Operation::GrowVolume createGrowVolume()
  {
    Resource volume = createPersistentVolume(
        Megabytes(128),
        "role1",
        "id1",
        "path1");

    Resource addition = Resources::parse("disk", "128", "role1").get();

    Offer::Operation::GrowVolume growVolume;
    growVolume.mutable_volume()->CopyFrom(volume);
    growVolume.mutable_addition()->CopyFrom(addition);

    return growVolume;
  }
};


// This test verifies that validation succeeds on a valid operation.
TEST_F(GrowVolumeOperationValidationTest, Valid)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::GrowVolume growVolume = createGrowVolume();

  Option<Error> error = operation::validate(growVolume, capabilities);
  EXPECT_NONE(error);
}


// This test verifies that validation fails if `GrowVolume.volume` is not a
// persistent volume.
TEST_F(GrowVolumeOperationValidationTest, NonPersistentVolume)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::GrowVolume growVolume = createGrowVolume();
  growVolume.mutable_volume()->mutable_disk()->clear_persistence();

  Option<Error> error = operation::validate(growVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `GrowVolume.addition` has a zero
// value.
TEST_F(GrowVolumeOperationValidationTest, ZeroAddition)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::GrowVolume growVolume = createGrowVolume();
  growVolume.mutable_addition()->mutable_scalar()->set_value(0);

  Option<Error> error = operation::validate(growVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `GrowVolume.volume` and
// `GrowVolume.addition' are incompatible.
TEST_F(GrowVolumeOperationValidationTest, IncompatibleDisk)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  // Make the volume on a PATH disk so it cannot be grown with a ROOT disk.
  Resource pathVolume = createPersistentVolume(
       Megabytes(128),
        "role1",
        "id1",
        "path1",
        None(),
        createDiskSourcePath("root"));

  Offer::Operation::GrowVolume growVolume = createGrowVolume();
  growVolume.mutable_volume()->CopyFrom(pathVolume);

  Option<Error> error = operation::validate(growVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `GrowVolume.volume` is a shared
// persistent volume.
TEST_F(GrowVolumeOperationValidationTest, Shared)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::GrowVolume growVolume = createGrowVolume();
  growVolume.mutable_volume()->mutable_shared();

  Option<Error> error = operation::validate(growVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `GrowVolume.volume` has resource
// provider id.
TEST_F(GrowVolumeOperationValidationTest, ResourceProvider)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::GrowVolume growVolume = createGrowVolume();
  growVolume.mutable_volume()->mutable_provider_id()->set_value("provider");

  Option<Error> error = operation::validate(growVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `GrowVolume.volume` and
// `GrowVolume.addition` are on MOUNT disks, which are not addable.
TEST_F(GrowVolumeOperationValidationTest, Mount)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Resource mountVolume = createPersistentVolume(
       Megabytes(128),
        "role1",
        "id1",
        "path1",
        None(),
        createDiskSourceMount());

  Resource mountDisk = createDiskResource(
      "128", "role1", None(), None(), createDiskSourceMount());

  Offer::Operation::GrowVolume growVolume = createGrowVolume();
  growVolume.mutable_volume()->CopyFrom(mountVolume);
  growVolume.mutable_addition()->CopyFrom(mountDisk);

  Option<Error> error = operation::validate(growVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation  fails if agent has no RESIZE_VOLUME
// capability.
TEST_F(GrowVolumeOperationValidationTest, MissingCapability)
{
  protobuf::slave::Capabilities capabilities;

  Option<Error> error = operation::validate(createGrowVolume(), capabilities);
  EXPECT_SOME(error);
}


class ShrinkVolumeOperationValidationTest : public MesosTest {
protected:
  Offer::Operation::ShrinkVolume createShrinkVolume()
  {
    Resource volume = createPersistentVolume(
        Megabytes(128),
        "role1",
        "id1",
        "path1");

    Offer::Operation::ShrinkVolume shrinkVolume;
    shrinkVolume.mutable_volume()->CopyFrom(volume);
    shrinkVolume.mutable_subtract()->set_value(64);

    return shrinkVolume;
  }
};


// This test verifies that validation succeeds on a valid `ShrinkVolume`
// operation.
TEST_F(ShrinkVolumeOperationValidationTest, Valid)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::ShrinkVolume shrinkVolume = createShrinkVolume();

  Option<Error> error = operation::validate(shrinkVolume, capabilities);
  EXPECT_NONE(error);
}


// This test verifies that validation fails if `ShrinkVolume.volume` is not a
// persistent volume.
TEST_F(ShrinkVolumeOperationValidationTest, NonPersistentVolume)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::ShrinkVolume shrinkVolume = createShrinkVolume();
  shrinkVolume.mutable_volume()->mutable_disk()->clear_persistence();

  Option<Error> error = operation::validate(shrinkVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `ShrinkVolume.subtract` has a
// zero value.
TEST_F(ShrinkVolumeOperationValidationTest, ZeroSubtract)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::ShrinkVolume shrinkVolume = createShrinkVolume();
  shrinkVolume.mutable_subtract()->set_value(0);

  Option<Error> error = operation::validate(shrinkVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `ShrinkVolume.subtract` has a
// value equal to the size of `ShrinkVolume.volume`
TEST_F(ShrinkVolumeOperationValidationTest, EmptyAfterShrink)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::ShrinkVolume shrinkVolume = createShrinkVolume();
  shrinkVolume.mutable_subtract()->CopyFrom(shrinkVolume.volume().scalar());

  Option<Error> error = operation::validate(shrinkVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `ShrinkVolume.volume` is a
// MOUNT disk.
TEST_F(ShrinkVolumeOperationValidationTest, Mount)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Resource mountVolume = createPersistentVolume(
       Megabytes(128),
        "role1",
        "id1",
        "path1",
        None(),
        createDiskSourceMount());

  Offer::Operation::ShrinkVolume shrinkVolume = createShrinkVolume();
  shrinkVolume.mutable_volume()->CopyFrom(mountVolume);

  Option<Error> error = operation::validate(shrinkVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `ShrinkVolume.volume` is a
// shared volume.
TEST_F(ShrinkVolumeOperationValidationTest, Shared)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::ShrinkVolume shrinkVolume = createShrinkVolume();
  shrinkVolume.mutable_volume()->mutable_shared();

  Option<Error> error = operation::validate(shrinkVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if `ShrinkVolume.volume` has
// resource provider id.
TEST_F(ShrinkVolumeOperationValidationTest, ResourceProvider)
{
  protobuf::slave::Capabilities capabilities;
  capabilities.resizeVolume = true;

  Offer::Operation::ShrinkVolume shrinkVolume = createShrinkVolume();
  shrinkVolume.mutable_volume()->mutable_provider_id()->set_value("provider");

  Option<Error> error = operation::validate(shrinkVolume, capabilities);
  EXPECT_SOME(error);
}


// This test verifies that validation fails if agent has no RESIZE_VOLUME
// capability.
TEST_F(ShrinkVolumeOperationValidationTest, MissingCapability)
{
  protobuf::slave::Capabilities capabilities;

  Option<Error> error = operation::validate(createShrinkVolume(), capabilities);
  EXPECT_SOME(error);
}


TEST(OperationValidationTest, CreateDisk)
{
  Resource disk1 = createDiskResource(
      "10", "*", None(), None(), createDiskSourceRaw(None(), "profile"));

  Resource disk2 = createDiskResource(
      "20", "*", None(), None(), createDiskSourceRaw());

  Resource disk3 = createDiskResource(
      "30", "*", None(), None(), createDiskSourceMount());

  Resource disk4 = createDiskResource(
      "40", "*", None(), None(), createDiskSourceRaw(None(), "profile"));

  disk1.mutable_provider_id()->set_value("provider1");
  disk2.mutable_provider_id()->set_value("provider2");
  disk3.mutable_provider_id()->set_value("provider3");

  Offer::Operation::CreateDisk createDisk;
  createDisk.mutable_source()->CopyFrom(disk1);
  createDisk.set_target_type(Resource::DiskInfo::Source::MOUNT);
  createDisk.clear_target_profile();

  Option<Error> error = operation::validate(createDisk);
  EXPECT_NONE(error);

  createDisk.mutable_source()->CopyFrom(disk1);
  createDisk.set_target_type(Resource::DiskInfo::Source::BLOCK);
  createDisk.clear_target_profile();

  error = operation::validate(createDisk);
  EXPECT_NONE(error);

  createDisk.mutable_source()->CopyFrom(disk2);
  createDisk.set_target_type(Resource::DiskInfo::Source::MOUNT);
  createDisk.set_target_profile("profile");

  error = operation::validate(createDisk);
  EXPECT_NONE(error);

  createDisk.mutable_source()->CopyFrom(disk1);
  createDisk.set_target_type(Resource::DiskInfo::Source::PATH);
  createDisk.clear_target_profile();

  error = operation::validate(createDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'target_type' is neither MOUNT or BLOCK"));

  createDisk.mutable_source()->CopyFrom(disk1);
  createDisk.set_target_type(Resource::DiskInfo::Source::MOUNT);
  createDisk.set_target_profile("profile");

  error = operation::validate(createDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'target_profile' must not be set when 'source' has a profile"));

  createDisk.mutable_source()->CopyFrom(disk2);
  createDisk.set_target_type(Resource::DiskInfo::Source::MOUNT);
  createDisk.clear_target_profile();

  error = operation::validate(createDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'target_profile' must be set when 'source' has no profile"));

  createDisk.mutable_source()->CopyFrom(disk3);
  createDisk.set_target_type(Resource::DiskInfo::Source::MOUNT);
  createDisk.clear_target_profile();

  error = operation::validate(createDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'source' is not a RAW disk resource"));

  createDisk.mutable_source()->CopyFrom(disk4);
  createDisk.set_target_type(Resource::DiskInfo::Source::MOUNT);
  createDisk.clear_target_profile();

  error = operation::validate(createDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'source' is not managed by a resource provider"));
}


TEST(OperationValidationTest, DestroyDisk)
{
  Resource disk1 = createDiskResource(
      "10", "*", None(), None(), createDiskSourceMount(None(), "volume1"));

  Resource disk2 = createDiskResource(
      "20", "*", None(), None(), createDiskSourceBlock("volume2"));

  Resource disk3 = createDiskResource(
      "40", "*", None(), None(), createDiskSourceRaw("volume3"));

  Resource disk4 = createDiskResource(
      "40", "*", None(), None(), createDiskSourcePath("volume4"));

  Resource disk5 = createDiskResource(
      "50", "*", None(), None(), createDiskSourceMount(None(), "volume5"));

  Resource disk6 = createDiskResource(
      "60", "*", None(), None(), createDiskSourceRaw(None(), "profile"));

  Resource disk7 = createPersistentVolume(
      Megabytes(70),
      "role",
      "id",
      "path",
      None(),
      createDiskSourceMount(None(), "volume7"));

  disk1.mutable_provider_id()->set_value("provider1");
  disk2.mutable_provider_id()->set_value("provider2");
  disk3.mutable_provider_id()->set_value("provider3");
  disk4.mutable_provider_id()->set_value("provider4");
  disk6.mutable_provider_id()->set_value("provider6");
  disk7.mutable_provider_id()->set_value("provider7");

  Offer::Operation::DestroyDisk destroyDisk;
  destroyDisk.mutable_source()->CopyFrom(disk1);

  Option<Error> error = operation::validate(destroyDisk);
  EXPECT_NONE(error);

  destroyDisk.mutable_source()->CopyFrom(disk2);

  error = operation::validate(destroyDisk);
  EXPECT_NONE(error);

  destroyDisk.mutable_source()->CopyFrom(disk3);

  error = operation::validate(destroyDisk);
  EXPECT_NONE(error);

  destroyDisk.mutable_source()->CopyFrom(disk4);

  error = operation::validate(destroyDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'source' is neither a MOUNT, BLOCK or RAW disk resource"));

  destroyDisk.mutable_source()->CopyFrom(disk5);

  error = operation::validate(destroyDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'source' is not managed by a resource provider"));

  destroyDisk.mutable_source()->CopyFrom(disk6);

  error = operation::validate(destroyDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "'source' is not backed by a CSI volume"));

  destroyDisk.mutable_source()->CopyFrom(disk7);

  error = operation::validate(destroyDisk);
  ASSERT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "Please destroy the persistent volume first"));
}


// TODO(jieyu): All of the task validation tests have the same flow:
// launch a task, expect an update of a particular format (invalid w/
// message). Consider providing common functionalities in the test
// fixture to avoid code bloat. Ultimately, we should make task or
// offer validation unit testable.
class TaskValidationTest : public MesosTest {};


TEST_F(TaskValidationTest, ExecutorUsesInvalidFrameworkID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create an executor with a random framework id.
  ExecutorInfo executor;
  executor = DEFAULT_EXECUTOR_INFO;
  executor.mutable_framework_id()->set_value(id::UUID::random().toString());

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::startsWith(
      status->message(), "ExecutorInfo has an invalid FrameworkID"));

  // Make sure the task is not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// Verifies that an invalid `ExecutorInfo.command.environment` will be rejected.
// This test ensures that the common validation code is being executed;
// comprehensive tests for the `Environment` message can be found in the agent
// validation tests.
TEST_F(TaskValidationTest, ExecutorEnvironmentInvalid)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  ExecutorInfo executor;
  executor = DEFAULT_EXECUTOR_INFO;
  Environment::Variable* variable =
    executor.mutable_command()->mutable_environment()
        ->mutable_variables()->Add();
  variable->set_name("ENV_VAR_KEY");

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(
      "Executor's `CommandInfo` is invalid: Environment variable 'ENV_VAR_KEY' "
      "of type 'VALUE' must have a value set",
      status->message());

  // Make sure the task is not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// The master should fill in the `ExecutorInfo.framework_id`
// if it is not set by the framework.
TEST_F(TaskValidationTest, ExecutorMissingFrameworkID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Start the first slave.
  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create an executor with a missing framework id.
  ExecutorInfo executor;
  executor = DEFAULT_EXECUTOR_INFO;
  executor.clear_framework_id();

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  EXPECT_CALL(exec, registered(_, _, _, _));

  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  // The task should pass validation since the framework id
  // is filled in, and when it reaches the dummy executor
  // it will fail because the executor just exits.
  AWAIT_READY(status);
  EXPECT_EQ(TASK_RUNNING, status->state());

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesCommandInfoAndExecutorInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Create a task that uses both command info and task info.
  TaskInfo task = createTask(offers.get()[0], ""); // Command task.
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO); // Executor task.

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::contains(
      status->message(), "CommandInfo or ExecutorInfo present"));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesExecutorInfoWithoutCommandInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create an executor without command info.
  // Note that we don't set type as 'CUSTOM' because it is not
  // required for `LAUNCH` operation.
  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::startsWith(
      status->message(), "'ExecutorInfo.command' must be set"));

  driver.stop();
  driver.join();
}


// This test verifies that a scheduler cannot explicitly specify
// a 'DEFAULT' executor when using `LAUNCH` operation.
// TODO(vinod): Revisit this when the above is allowed.
TEST_F(TaskValidationTest, TaskUsesDefaultExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  // Create a 'DEFAULT' executor.
  ExecutorInfo executor;
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_executor_id()->set_value("default");

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(LaunchTasks(executor, 1, 1, 16, "*"))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.start();

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::startsWith(
      status->message(), "'ExecutorInfo.type' must be 'CUSTOM'"));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesNoResources)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(status->has_message());
  EXPECT_EQ("Task uses no resources", status->message());

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, TaskUsesMoreResourcesThanOffered)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

  Resource* cpus = task.add_resources();
  cpus->set_name("cpus");
  cpus->set_type(Value::SCALAR);
  cpus->mutable_scalar()->set_value(2.01);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);

  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(status->has_message());
  EXPECT_TRUE(strings::contains(
      status->message(), "more than available"));

  driver.stop();
  driver.join();
}


// This test verifies that if two tasks are launched with the same
// task ID, the second task will get rejected.
TEST_F(TaskValidationTest, DuplicatedTaskID)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value("exit 1");

  // Create two tasks with the same id.
  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:32").get());
  task1.mutable_executor()->MergeFrom(executor);

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("1");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(Resources::parse("cpus:1;mem:32").get());
  task2.mutable_executor()->MergeFrom(executor);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Grab the first task but don't send a status update.
  Future<TaskInfo> task;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&task));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(task);
  EXPECT_EQ(task1.task_id(), task->task_id());

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());

  EXPECT_TRUE(strings::startsWith(
      status->message(), "Task has duplicate ID"));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that two tasks launched on the same slave with
// the same executor id but different executor info are rejected.
TEST_F(TaskValidationTest, ExecutorInfoDiffersOnSameSlave)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), &containerizer);
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  ExecutorInfo executor;
  executor.mutable_executor_id()->set_value("default");
  executor.mutable_command()->set_value("exit 1");

  TaskInfo task1;
  task1.set_name("");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task1.mutable_executor()->MergeFrom(executor);

  executor.mutable_command()->set_value("exit 2");

  TaskInfo task2;
  task2.set_name("");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(
      Resources::parse("cpus:1;mem:512").get());
  task2.mutable_executor()->MergeFrom(executor);

  vector<TaskInfo> tasks;
  tasks.push_back(task1);
  tasks.push_back(task2);

  EXPECT_CALL(exec, registered(_, _, _, _));

  // Grab the "good" task but don't send a status update.
  Future<TaskInfo> task;
  EXPECT_CALL(exec, launchTask(_, _))
    .WillOnce(FutureArg<1>(&task));

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offers.get()[0].id(), tasks);

  AWAIT_READY(task);
  EXPECT_EQ(task1.task_id(), task->task_id());

  AWAIT_READY(status);
  EXPECT_EQ(task2.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(status->has_message());
  EXPECT_TRUE(strings::contains(
      status->message(), "ExecutorInfo is not compatible"));

  EXPECT_CALL(exec, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test verifies that two tasks each launched on a different
// slave with same executor id but different executor info are
// allowed.
TEST_F(TaskValidationTest, ExecutorInfoDiffersOnDifferentSlaves)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  // Start the first slave.
  MockExecutor exec1(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer1(&exec1);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave1 =
    StartSlave(detector.get(), &containerizer1);
  ASSERT_SOME(slave1);

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  // Launch the first task with the default executor id.
  ExecutorInfo executor1;
  executor1 = DEFAULT_EXECUTOR_INFO;
  executor1.mutable_command()->set_value("exit 1");

  TaskInfo task1 = createTask(
      offers1.get()[0], executor1.command().value(), executor1.executor_id());

  EXPECT_CALL(exec1, registered(_, _, _, _));

  EXPECT_CALL(exec1, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status1;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status1));

  driver.launchTasks(offers1.get()[0].id(), {task1});

  AWAIT_READY(status1);
  ASSERT_EQ(TASK_RUNNING, status1->state());

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  // Now start the second slave.
  MockExecutor exec2(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer2(&exec2);

  Try<Owned<cluster::Slave>> slave2 =
    StartSlave(detector.get(), &containerizer2);
  ASSERT_SOME(slave2);

  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // Now launch the second task with the same executor id but
  // a different executor command.
  ExecutorInfo executor2;
  executor2 = executor1;
  executor2.mutable_command()->set_value("exit 2");

  TaskInfo task2 = createTask(
      offers2.get()[0], executor2.command().value(), executor2.executor_id());

  EXPECT_CALL(exec2, registered(_, _, _, _));

  EXPECT_CALL(exec2, launchTask(_, _))
    .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

  Future<TaskStatus> status2;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status2));

  driver.launchTasks(offers2.get()[0].id(), {task2});

  AWAIT_READY(status2);
  ASSERT_EQ(TASK_RUNNING, status2->state());

  EXPECT_CALL(exec1, shutdown(_))
    .Times(AtMost(1));

  EXPECT_CALL(exec2, shutdown(_))
    .Times(AtMost(1));

  driver.stop();
  driver.join();
}


// This test checks that if a task is launched with the same task ID
// as an unreachable task, the second task will be rejected. The
// master does not store all unreachable task IDs so we cannot prevent
// all task ID collisions, but we try to prevent the common case.
TEST_F(TaskValidationTest, TaskReusesUnreachableTaskID)
{
  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  // Allow the master to PING the slave, but drop all PONG messages
  // from the slave. Note that we don't match on the master / slave
  // PIDs because it's actually the `SlaveObserver` process that sends
  // the pings.
  Future<Message> ping = FUTURE_MESSAGE(
      Eq(PingSlaveMessage().GetTypeName()), _, _);

  DROP_PROTOBUFS(PongSlaveMessage(), _, _);

  StandaloneMasterDetector detector1(master.get()->pid);
  Try<Owned<cluster::Slave>> slave1 = StartSlave(&detector1);
  ASSERT_SOME(slave1);

  // Start a partition-aware scheduler.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::PARTITION_AWARE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers1;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers1));

  driver.start();

  AWAIT_READY(offers1);
  ASSERT_FALSE(offers1->empty());

  Offer offer1 = offers1.get()[0];
  TaskInfo task1 = createTask(offer1, "sleep 60");

  Future<TaskStatus> startingStatus;
  Future<TaskStatus> runningStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&startingStatus))
    .WillOnce(FutureArg<1>(&runningStatus));

  Future<Nothing> statusUpdateAck = FUTURE_DISPATCH(
      slave1.get()->pid, &Slave::_statusUpdateAcknowledgement);

  driver.launchTasks(offer1.id(), {task1});

  AWAIT_READY(startingStatus);
  EXPECT_EQ(TASK_STARTING, startingStatus->state());
  EXPECT_EQ(task1.task_id(), startingStatus->task_id());

  AWAIT_READY(runningStatus);
  EXPECT_EQ(TASK_RUNNING, runningStatus->state());
  EXPECT_EQ(task1.task_id(), runningStatus->task_id());

  const SlaveID slaveId1 = runningStatus->slave_id();

  AWAIT_READY(statusUpdateAck);

  // Now, induce a partition of the slave by having the master
  // timeout the slave.
  Clock::pause();

  Future<TaskStatus> unreachableStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&unreachableStatus));

  Future<Nothing> slaveLost;
  EXPECT_CALL(sched, slaveLost(&driver, _))
    .WillOnce(FutureSatisfy(&slaveLost));

  size_t pings = 0;
  while (true) {
    AWAIT_READY(ping);
    pings++;
    if (pings == masterFlags.max_agent_ping_timeouts) {
      break;
    }
    ping = FUTURE_MESSAGE(Eq(PingSlaveMessage().GetTypeName()), _, _);
    Clock::advance(masterFlags.agent_ping_timeout);
  }

  Clock::advance(masterFlags.agent_ping_timeout);

  AWAIT_READY(unreachableStatus);
  EXPECT_EQ(TASK_UNREACHABLE, unreachableStatus->state());
  EXPECT_EQ(TaskStatus::REASON_SLAVE_REMOVED, unreachableStatus->reason());
  EXPECT_EQ(task1.task_id(), unreachableStatus->task_id());
  EXPECT_EQ(slaveId1, unreachableStatus->slave_id());

  AWAIT_READY(slaveLost);

  Clock::resume();

  // Shutdown the first agent.
  slave1->reset();

  // Start a second agent.
  StandaloneMasterDetector detector2(master.get()->pid);
  slave::Flags agentFlags2 = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave2 = StartSlave(&detector2, agentFlags2);
  ASSERT_SOME(slave2);

  Future<vector<Offer>> offers2;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers2));

  Clock::advance(agentFlags2.registration_backoff_factor);
  AWAIT_READY(offers2);
  ASSERT_FALSE(offers2->empty());

  // Attempt to launch a new task that reuses the ID of the first
  // (unreachable) task. This should result in TASK_ERROR.

  Offer offer2 = offers2.get()[0];
  TaskInfo task2 = createTask(
      offer2,
      "sleep 60",
      None(),
      "test-task-2",
      task1.task_id().value());

  Future<TaskStatus> errorStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&errorStatus));

  driver.launchTasks(offer2.id(), {task2});

  AWAIT_READY(errorStatus);
  EXPECT_EQ(TASK_ERROR, errorStatus->state());
  EXPECT_EQ(task2.task_id(), errorStatus->task_id());

  driver.stop();
  driver.join();
}


// This test verifies that a task is not allowed to mix revocable and
// non-revocable resources.
TEST_F(TaskValidationTest, TaskUsesRevocableResources)
{
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("task");
  task.mutable_slave_id()->set_value("slave");

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);
  cpus.mutable_allocation_info()->set_role("role");

  // A task with only non-revocable cpus is valid.
  task.add_resources()->CopyFrom(cpus);

  Option<Error> error = task::internal::validateResources(task);

  EXPECT_NONE(error);

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task with only revocable cpus is valid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);

  error = task::internal::validateResources(task);

  EXPECT_NONE(error);

  // A task with both revocable and non-revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(cpus);
  task.add_resources()->CopyFrom(revocableCpus);

  error = task::internal::validateResources(task);

  EXPECT_SOME(error);
}


TEST_F(TaskValidationTest, TaskInfoAllocatedResources)
{
  // Validation should pass if the task has resources
  // allocated to a single role.
  {
    TaskInfo task;
    Resources resources = Resources::parse("cpus:1;mem:1").get();
    task.mutable_resources()->CopyFrom(allocatedResources(resources, "role"));

    EXPECT_NONE(::task::internal::validateResources(task));
  }

  // Validation should fail if the task has unallocated resources.
  {
    TaskInfo task;
    Resources resources = Resources::parse("cpus:1;mem:1").get();
    task.mutable_resources()->CopyFrom(resources);

    EXPECT_SOME(::task::internal::validateResources(task));
  }

  // Validation should fail if the task has resources
  // allocated to multiple roles.
  {
    TaskInfo task;
    Resources resources1 = Resources::parse("cpus:1").get();
    Resources resources2 = Resources::parse("mem:1").get();

    task.mutable_resources()->CopyFrom(
        allocatedResources(resources1, "role1") +
        allocatedResources(resources2, "role2"));

    EXPECT_SOME(::task::internal::validateResources(task));
  }
}


// This test verifies that a task and its executor are not allowed to
// mix revocable and non-revocable resources.
TEST_F(TaskValidationTest, TaskAndExecutorUseRevocableResources)
{
  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("task");
  task.mutable_slave_id()->set_value("slave");

  ExecutorInfo executor = DEFAULT_EXECUTOR_INFO;

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  // A task and executor with only non-revocable cpus is valid.
  task.add_resources()->CopyFrom(cpus);
  executor.add_resources()->CopyFrom(cpus);
  task.mutable_executor()->CopyFrom(executor);

  Option<Error> error = task::internal::validateTaskAndExecutorResources(task);

  EXPECT_NONE(error);

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task and executor with only revocable cpus is valid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);
  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);
  task.mutable_executor()->CopyFrom(executor);

  error = task::internal::validateTaskAndExecutorResources(task);

  EXPECT_NONE(error);

  // A task with revocable cpus and its executor with non-revocable
  // cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);
  executor.clear_resources();
  executor.add_resources()->CopyFrom(cpus);
  task.mutable_executor()->CopyFrom(executor);

  error = task::internal::validateTaskAndExecutorResources(task);

  EXPECT_SOME(error);

  // A task with non-revocable cpus and its executor with
  // non-revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(cpus);
  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);
  task.mutable_executor()->CopyFrom(executor);

  error = task::internal::validateTaskAndExecutorResources(task);

  EXPECT_SOME(error);
}


// Ensures that negative executor shutdown grace period in `ExecutorInfo`
// is rejected during `TaskInfo` validation.
TEST_F(TaskValidationTest, ExecutorShutdownGracePeriodIsNonNegative)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  ExecutorInfo executorInfo(DEFAULT_EXECUTOR_INFO);
  executorInfo.mutable_shutdown_grace_period()->set_nanoseconds(
      Seconds(-1).ns());

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(executorInfo);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(status->has_message());
  EXPECT_EQ("ExecutorInfo's 'shutdown_grace_period' must be non-negative",
            status->message());

  driver.stop();
  driver.join();
}


// Ensures that negative grace period in `KillPolicy`
// is rejected during `TaskInfo` validation.
TEST_F(TaskValidationTest, KillPolicyGracePeriodIsNonNegative)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  TaskInfo task;
  task.set_name("");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(offer.resources());
  task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);
  task.mutable_kill_policy()->mutable_grace_period()->set_nanoseconds(
      Seconds(-1).ns());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(task.task_id(), status->task_id());
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_INVALID, status->reason());
  EXPECT_TRUE(status->has_message());
  EXPECT_EQ("Task's 'kill_policy.grace_period' must be non-negative",
            status->message());

  driver.stop();
  driver.join();
}


// Verifies that an invalid `TaskInfo.command.environment` will be rejected.
// This test ensures that the common validation code is being executed;
// comprehensive tests for the `Environment` message can be found in the agent
// validation tests.
TEST_F(TaskValidationTest, TaskEnvironmentInvalid)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Create a task that contains a `CommandInfo.Environment` with an
  // unset environment variable value.
  TaskInfo task = createTask(offers.get()[0], "exit 0"); // Command task.
  Environment::Variable* variable =
    task.mutable_command()->mutable_environment()->mutable_variables()->Add();
  variable->set_name("ENV_VAR_KEY");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(
      "Task's `CommandInfo` is invalid: Environment variable 'ENV_VAR_KEY' "
      "of type 'VALUE' must have a value set",
      status->message());

  driver.stop();
  driver.join();
}


// This test verifies that a task that has `ContainerInfo` set as DOCKER
// but has no `DockerInfo` is rejected during `TaskInfo` validation.
TEST_F(TaskValidationTest, TaskMissingDockerInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Create an invalid task that has `ContainerInfo` set
  // as DOCKER but has no `DockerInfo`.
  TaskInfo task = createTask(offers.get()[0], "exit 0");
  task.mutable_container()->set_type(ContainerInfo::DOCKER);

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(
      "Task's `ContainerInfo` is invalid: "
      "DockerInfo 'docker' is not set for DOCKER typed ContainerInfo",
      status->message());

  driver.stop();
  driver.join();
}

// This test verifies that a task that has `ContainerInfo` set as MESOS
// but has a `DockerInfo` is rejected during `TaskInfo` validation.
// TODO(josephw): Enable this regression test when we officially deprecate
// this invalid protobuf. See MESOS-6874 and MESOS-9740.
TEST_F(TaskValidationTest, DISABLED_TaskMesosTypeWithDockerInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Create an invalid task that has `ContainerInfo` set
  // as MESOS and has a `DockerInfo` set.
  TaskInfo task = createTask(offers.get()[0], "exit 0");
  task.mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_container()->mutable_docker()->set_image("alpine");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(
    "Task's `ContainerInfo` is invalid: "
    "Protobuf union `mesos.ContainerInfo` with `Type == MESOS` "
    "should not have the field `docker` set.",
    status->message());

  driver.stop();
  driver.join();
}


// This test verifies that a task that has `name` parameter set
// in `DockerInfo` is rejected during `TaskInfo` validation.
TEST_F(TaskValidationTest, TaskSettingDockerParameterName)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  // Create an invalid task that has `name` parameter set in `DockerInfo`.
  TaskInfo task = createTask(offers.get()[0], "exit 0");
  task.mutable_container()->set_type(ContainerInfo::DOCKER);
  task.mutable_container()->mutable_docker()->set_image("alpine");

  Parameter* parameter =
      task.mutable_container()->mutable_docker()->add_parameters();

  parameter->set_key("name");
  parameter->set_value("test");

  driver.launchTasks(offers.get()[0].id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_EQ(
      "Task's `ContainerInfo` is invalid: "
      "Parameter in DockerInfo must not be 'name'",
      status->message());

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, ResourceLimitLessThanRequest)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Map<string, Value::Scalar> limits;
  limits["cpus"].set_value(0.01);

  TaskInfo task = createTask(
      offers->at(0),
      "exit 0",
      None(),
      "test-task",
      id::UUID::random().toString(),
      limits);

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::contains(
      status->message(),
      "The cpu limit must be greater than or equal to the cpu request"));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, LimitOtherThanCpuOrMem)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  Map<string, Value::Scalar> limits;
  limits["disk"].set_value(128);

  TaskInfo task = createTask(
      offers->at(0),
      "exit 0",
      None(),
      "test-task",
      id::UUID::random().toString(),
      limits);

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::contains(
      status->message(),
      "Only cpus and mem may be included in a task's resource limits"));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, NestedCgroupInLaunchOperation)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  TaskInfo task = createTask(offers->at(0), "exit 0");

  task.mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_container()->mutable_linux_info()->set_share_cgroups(false);

  driver.launchTasks(offers->at(0).id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::contains(
      status->message(),
      "Only tasks in a task group may have 'share_cgroups' set to 'false'"));

  driver.stop();
  driver.join();
}


TEST_F(TaskValidationTest, SharedCgroupOnExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);

  Future<TaskStatus> status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&status));

  ExecutorID executorId;
  executorId.set_value("test-executor");

  TaskInfo task = createTask(offer, "exit 0", executorId);
  task.mutable_executor()->mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_executor()->mutable_container()
    ->mutable_linux_info()->set_share_cgroups(true);

  driver.launchTasks(offer.id(), {task});

  AWAIT_READY(status);
  EXPECT_EQ(TASK_ERROR, status->state());
  EXPECT_TRUE(strings::contains(
      status->message(),
      "The 'share_cgroups' field cannot be set to 'true' on "
      "executor containers"));

  driver.stop();
  driver.join();
}

// TODO(jieyu): Add tests for checking duplicated persistence ID
// against offered resources.

// TODO(jieyu): Add tests for checking duplicated persistence ID
// across task and executors.

// TODO(jieyu): Add tests for checking duplicated persistence ID
// within an executor.

// TODO(benh): Add tests for checking correct slave IDs.

// TODO(benh): Add tests for checking executor resource usage.

// TODO(benh): Add tests which launch multiple tasks and check for
// aggregate resource usage.


class ExecutorValidationTest : public MesosTest {};


TEST_F(ExecutorValidationTest, ExecutorType)
{
  ExecutorInfo executorInfo;
  executorInfo = DEFAULT_EXECUTOR_INFO;
  executorInfo.mutable_framework_id()->set_value(id::UUID::random().toString());

  {
    // 'CUSTOM' executor with `CommandInfo` set is valid.
    executorInfo.set_type(ExecutorInfo::CUSTOM);
    executorInfo.mutable_command();

    Option<Error> error = ::executor::internal::validateType(executorInfo);

    EXPECT_NONE(error);
  }

  {
    // 'CUSTOM' executor without `CommandInfo` set is invalid.
    executorInfo.set_type(ExecutorInfo::CUSTOM);
    executorInfo.clear_command();

    Option<Error> error = ::executor::internal::validateType(executorInfo);

    EXPECT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "'ExecutorInfo.command' must be set for 'CUSTOM' executor"));
  }

  {
    // 'DEFAULT' executor without `CommandInfo` set is valid.
    executorInfo.set_type(ExecutorInfo::DEFAULT);
    executorInfo.clear_command();

    Option<Error> error = ::executor::internal::validateType(executorInfo);

    EXPECT_NONE(error);
  }

  {
    // 'DEFAULT' executor with `CommandInfo` set is invalid.
    executorInfo.set_type(ExecutorInfo::DEFAULT);
    executorInfo.mutable_command();

    Option<Error> error = ::executor::internal::validateType(executorInfo);

    EXPECT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "'ExecutorInfo.command' must not be set for 'DEFAULT' executor"));
  }

  {
    // 'DEFAULT' executor with `ContainerInfo` must be a Mesos container.
    executorInfo.set_type(ExecutorInfo::DEFAULT);
    executorInfo.clear_command();
    executorInfo.mutable_container()->set_type(ContainerInfo::DOCKER);

    Option<Error> error = ::executor::internal::validateType(executorInfo);

    EXPECT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "'ExecutorInfo.container.type' must be 'MESOS' for "
        "'DEFAULT' executor"));
  }

  {
    // 'DEFAULT' executor with `ContainerInfo` may not have a container image.
    executorInfo.set_type(ExecutorInfo::DEFAULT);
    executorInfo.clear_command();
    executorInfo.mutable_container()->set_type(ContainerInfo::MESOS);
    executorInfo.mutable_container()->mutable_mesos()->mutable_image();

    Option<Error> error = ::executor::internal::validateType(executorInfo);

    EXPECT_SOME(error);
    EXPECT_TRUE(strings::contains(
        error->message,
        "'ExecutorInfo.container.mesos.image' must not be set for "
        "'DEFAULT' executor"));
  }
}


TEST_F(ExecutorValidationTest, ExecutorID)
{
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
    executorInfo.mutable_executor_id()->set_value("abc");

    EXPECT_NONE(::executor::internal::validateExecutorID(executorInfo));
  }

  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
    executorInfo.mutable_executor_id()->set_value("");

    EXPECT_SOME(::executor::internal::validateExecutorID(executorInfo));
  }

  // This is currently allowed.
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
    executorInfo.mutable_executor_id()->set_value("ab c");

    EXPECT_NONE(::executor::internal::validateExecutorID(executorInfo));
  }

  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
    executorInfo.mutable_executor_id()->set_value("ab/c");

    EXPECT_SOME(::executor::internal::validateExecutorID(executorInfo));
  }

  // Containing a dot is allowed.
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
    executorInfo.mutable_executor_id()->set_value("a.b");

    EXPECT_NONE(::executor::internal::validateExecutorID(executorInfo));
  }

  // Being only a dot is not allowed.
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
    executorInfo.mutable_executor_id()->set_value(".");

    EXPECT_SOME(::executor::internal::validateExecutorID(executorInfo));
  }

  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;
    executorInfo.mutable_executor_id()->set_value("..");

    EXPECT_SOME(::executor::internal::validateExecutorID(executorInfo));
  }
}


TEST_F(ExecutorValidationTest, ExecutorInfoAllocatedResources)
{
  // Validation should pass if the executor has no resources.
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;

    EXPECT_NONE(::executor::internal::validateResources(executorInfo));
  }

  // Validation should pass if the executor has resources
  // allocated to a single role.
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;

    EXPECT_NONE(::executor::internal::validateResources(executorInfo));

    Resources resources = Resources::parse("cpus:1;mem:128").get();
    executorInfo.mutable_resources()->CopyFrom(
        allocatedResources(resources, "role"));

    EXPECT_NONE(::executor::internal::validateResources(executorInfo));
  }

  // Validation should fail if the executor has unallocated resources.
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;

    Resources resources = Resources::parse("cpus:1;mem:128").get();
    executorInfo.mutable_resources()->CopyFrom(resources);

    EXPECT_SOME(::executor::internal::validateResources(executorInfo));
  }

  // Validation should fail if the executor has resources
  // allocated to multiple role.
  {
    ExecutorInfo executorInfo = DEFAULT_EXECUTOR_INFO;

    Resources resources1 = Resources::parse("cpus:1").get();
    Resources resources2 = Resources::parse("mem:1").get();

    executorInfo.mutable_resources()->CopyFrom(
        allocatedResources(resources1, "role1") +
        allocatedResources(resources2, "role2"));

    EXPECT_SOME(::executor::internal::validateResources(executorInfo));
  }
}


class TaskGroupValidationTest : public MesosTest {};


// This test verifies that tasks in a task group cannot mix
// revocable and non-revocable resources.
TEST_F(TaskGroupValidationTest, TaskGroupUsesRevocableResources)
{
  TaskInfo task1;
  task1.set_name("test1");
  task1.mutable_task_id()->set_value("task1");
  task1.mutable_slave_id()->set_value("slave");

  TaskInfo task2;
  task2.set_name("test2");
  task2.mutable_task_id()->set_value("task2");
  task2.mutable_slave_id()->set_value("slave");

  ExecutorInfo executor = DEFAULT_EXECUTOR_INFO;

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  // A task group with only non-revocable cpus is valid.
  task1.add_resources()->CopyFrom(cpus);
  task2.add_resources()->CopyFrom(cpus);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Option<Error> error =
    task::group::internal::validateTaskGroupAndExecutorResources(
        taskGroup, executor);

  EXPECT_NONE(error);

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task group with only revocable cpus is valid.
  task1.clear_resources();
  task2.clear_resources();
  task1.add_resources()->CopyFrom(revocableCpus);
  task2.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  error = task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor);

  EXPECT_NONE(error);

  // A task group with one task using revocable resources and another task
  // using non-revocable cpus is invalid.
  task1.clear_resources();
  task2.clear_resources();
  task1.add_resources()->CopyFrom(cpus);
  task2.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  error = task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor);

  EXPECT_SOME(error);
}


// This test verifies that tasks in a task group and executor
// cannot mix revocable and non-revocable resources.
TEST_F(TaskGroupValidationTest, TaskGroupAndExecutorUsesRevocableResources)
{
  TaskInfo task;
  task.set_name("test1");
  task.mutable_task_id()->set_value("task1");
  task.mutable_slave_id()->set_value("slave");

  ExecutorInfo executor = DEFAULT_EXECUTOR_INFO;

  // Non-revocable cpus.
  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);

  // A task group and executor with only non-revocable cpus is valid.
  task.add_resources()->CopyFrom(cpus);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  executor.add_resources()->CopyFrom(cpus);

  Option<Error> error =
    task::group::internal::validateTaskGroupAndExecutorResources(
        taskGroup, executor);

  EXPECT_NONE(error);

  // Revocable cpus.
  Resource revocableCpus = cpus;
  revocableCpus.mutable_revocable();

  // A task group and executor with only revocable cpus is valid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task);

  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);

  error = task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor);

  EXPECT_NONE(error);

  // A task group with the task using revocable resources and executor
  // using non-revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(revocableCpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task);

  executor.clear_resources();
  executor.add_resources()->CopyFrom(cpus);

  error = task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor);

  EXPECT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "Task group and executor mix revocable and non-revocable resources"));

  // A task group with the task using non-revocable resources and executor
  // using revocable cpus is invalid.
  task.clear_resources();
  task.add_resources()->CopyFrom(cpus);

  taskGroup.clear_tasks();
  taskGroup.add_tasks()->CopyFrom(task);

  executor.clear_resources();
  executor.add_resources()->CopyFrom(revocableCpus);

  error = task::group::internal::validateTaskGroupAndExecutorResources(
      taskGroup, executor);

  EXPECT_SOME(error);
  EXPECT_TRUE(strings::contains(
      error->message,
      "Task group and executor mix revocable and non-revocable resources"));
}


// Verifies that an executor with `ContainerInfo` set as DOCKER
// is rejected during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, ExecutorUsesDockerContainerInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.mutable_id()->set_value("Test_Framework");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  // Create an invalid executor with `ContainerInfo` set as DOCKER.
  ExecutorInfo executor;
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_executor_id()->set_value("E");
  executor.mutable_framework_id()->CopyFrom(frameworkInfo.id());
  executor.mutable_container()->set_type(ContainerInfo::DOCKER);

  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offer.slave_id());
  task1.mutable_resources()->MergeFrom(offer.resources());

  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offer.slave_id());
  task2.mutable_resources()->MergeFrom(offer.resources());

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offer.id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
  EXPECT_EQ(
      "'ExecutorInfo.container.type' must be 'MESOS' for 'DEFAULT' executor",
      task1Status->message());

  AWAIT_READY(task2Status);
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
  EXPECT_EQ(
      "'ExecutorInfo.container.type' must be 'MESOS' for 'DEFAULT' executor",
      task2Status->message());

  // Make sure the task is not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// Ensures that an executor without a framework id is
// rejected during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, ExecutorWithoutFrameworkId)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Create an invalid executor without framework id.
  ExecutorInfo executor;
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_executor_id()->set_value("E");

  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(offers.get()[0].resources());

  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(offers.get()[0].resources());

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offers.get()[0].id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());

  AWAIT_READY(task2Status);
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());

  // Make sure the task is not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// Verifies that a task in a task group that has `ContainerInfo`
// set as DOCKER is rejected during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, TaskUsesDockerContainerInfo)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, DEFAULT_FRAMEWORK_INFO, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  Offer offer = offers.get()[0];

  Resources resources = Resources::parse("cpus:1;mem:512;disk:32").get();

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.set_type(ExecutorInfo::CUSTOM);
  executor.mutable_resources()->CopyFrom(resources);

  // Create an invalid task that has `ContainerInfo` set as DOCKER.
  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offer.slave_id());
  task1.mutable_resources()->MergeFrom(resources);
  task1.mutable_container()->set_type(ContainerInfo::DOCKER);
  task1.mutable_container()->mutable_docker()->set_image("alpine");

  // Create a valid task.
  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offer.slave_id());
  task2.mutable_resources()->MergeFrom(resources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offer.id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(task1.task_id(), task1Status->task_id());
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
  EXPECT_EQ(
      "Task '1' is invalid: Docker ContainerInfo is not supported on the task",
      task1Status->message());

  AWAIT_READY(task2Status);
  EXPECT_EQ(task2.task_id(), task2Status->task_id());
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());

  driver.stop();
  driver.join();
}


// Ensures that a task in a task group that has `NetworkInfo` set does
// not have HTTP health checks during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, TaskWithNetworkInfosDoesNotHaveHTTPHealthChecks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;

  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  Resources resources = Resources::parse("cpus:0.5;mem:300;disk:100").get();

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.set_type(ExecutorInfo::CUSTOM);
  executor.mutable_resources()->CopyFrom(resources);
  executor.mutable_framework_id()->CopyFrom(frameworkId.get());

  TaskInfo task;
  task.set_name("1");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(resources);
  task.mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_container()->add_network_infos();

  // Add a HTTP health check to this task.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::HTTP);
  healthCheck.mutable_http()->set_port(80);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  Future<TaskStatus> taskStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&taskStatus));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offer.id()}, {operation});

  const string expected =
    "Task '1' is invalid: HTTP and TCP health checks are not supported "
    "for nested containers not joining parent's network";

  AWAIT_READY(taskStatus);
  EXPECT_EQ(task.task_id(), taskStatus->task_id());
  EXPECT_EQ(TASK_ERROR, taskStatus->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, taskStatus->reason());
  EXPECT_EQ(expected, taskStatus->message());

  driver.stop();
  driver.join();
}


// Ensures that a task in a task group that has `NetworkInfo` set does
// not have TCP health checks during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, TaskWithNetworkInfosDoesNotHaveTCPHealthChecks)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();

  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched,
      DEFAULT_FRAMEWORK_INFO,
      master.get()->pid,
      DEFAULT_CREDENTIAL);

  Future<FrameworkID> frameworkId;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureArg<1>(&frameworkId));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(frameworkId);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  Resources resources = Resources::parse("cpus:0.5;mem:300;disk:100").get();

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.mutable_resources()->CopyFrom(resources);
  executor.mutable_framework_id()->CopyFrom(frameworkId.get());

  TaskInfo task;
  task.set_name("1");
  task.mutable_task_id()->set_value("1");
  task.mutable_slave_id()->MergeFrom(offer.slave_id());
  task.mutable_resources()->MergeFrom(resources);
  task.mutable_container()->set_type(ContainerInfo::MESOS);
  task.mutable_container()->add_network_infos();

  // Add a TCP health check to this task.
  HealthCheck healthCheck;
  healthCheck.set_type(HealthCheck::TCP);
  healthCheck.mutable_tcp()->set_port(30000);
  healthCheck.set_delay_seconds(0);
  healthCheck.set_interval_seconds(0);
  healthCheck.set_grace_period_seconds(15);

  task.mutable_health_check()->CopyFrom(healthCheck);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task);

  Future<TaskStatus> taskStatus;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&taskStatus));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offer.id()}, {operation});

  const string expected =
    "Task '1' is invalid: HTTP and TCP health checks are not supported "
    "for nested containers not joining parent's network";

  AWAIT_READY(taskStatus);
  EXPECT_EQ(task.task_id(), taskStatus->task_id());
  EXPECT_EQ(TASK_ERROR, taskStatus->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, taskStatus->reason());
  EXPECT_EQ(expected, taskStatus->message());

  driver.stop();
  driver.join();
}


// Ensures that a task in a task group with a different executor
// is rejected during `TaskGroupInfo` validation.
TEST_F(TaskGroupValidationTest, TaskUsesDifferentExecutor)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.mutable_id()->set_value("Test_Framework");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());
  Offer offer = offers.get()[0];

  Resources resources = Resources::parse("cpus:1;mem:512;disk:32").get();

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.mutable_framework_id()->CopyFrom(frameworkInfo.id());
  executor.set_type(ExecutorInfo::CUSTOM);
  executor.mutable_resources()->CopyFrom(resources);

  // Create an invalid task that has a different executor.
  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offer.slave_id());
  task1.mutable_resources()->MergeFrom(resources);
  task1.mutable_executor()->MergeFrom(executor);
  task1.mutable_executor()->set_type(ExecutorInfo::DEFAULT);

  // Create a valid task.
  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offer.slave_id());
  task2.mutable_resources()->MergeFrom(resources);

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offer.id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(task1.task_id(), task1Status->task_id());
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
  EXPECT_EQ(
      "The `ExecutorInfo` of task '1' is different from executor 'default'",
      task1Status->message());

  AWAIT_READY(task2Status);
  EXPECT_EQ(task2.task_id(), task2Status->task_id());
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());

  driver.stop();
  driver.join();
}


// Verifies that a task group which specifies an invalid environment in
// `ExecutorInfo` will be rejected. This test ensures that the common validation
// code is being executed; comprehensive tests for the `Environment` message can
// be found in the agent validation tests.
TEST_F(TaskGroupValidationTest, ExecutorEnvironmentInvalid)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.mutable_id()->set_value("Test_Framework");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(offers.get()[0].resources());

  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(offers.get()[0].resources());

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.mutable_framework_id()->CopyFrom(frameworkInfo.id());
  Environment::Variable* variable =
    executor.mutable_command()->mutable_environment()
        ->mutable_variables()->Add();
  variable->set_name("ENV_VAR_KEY");

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offers.get()[0].id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
  EXPECT_EQ(
      "Executor's `CommandInfo` is invalid: Environment variable 'ENV_VAR_KEY' "
      "of type 'VALUE' must have a value set",
      task1Status->message());

  AWAIT_READY(task2Status);
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
  EXPECT_EQ(
      "Executor's `CommandInfo` is invalid: Environment variable 'ENV_VAR_KEY' "
      "of type 'VALUE' must have a value set",
      task2Status->message());

  // Make sure the tasks are not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


// Verifies that a task group which specifies an invalid environment in
// `TaskGroupInfo` will be rejected. This test ensures that the common
// validation code is being executed; comprehensive tests for the `Environment`
// message can be found in the agent validation tests.
TEST_F(TaskGroupValidationTest, TaskEnvironmentInvalid)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.mutable_id()->set_value("Test_Framework");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers))
    .WillRepeatedly(Return()); // Ignore subsequent offers.

  driver.start();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  TaskInfo task1;
  task1.set_name("1");
  task1.mutable_task_id()->set_value("1");
  task1.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task1.mutable_resources()->MergeFrom(offers.get()[0].resources());

  Environment::Variable* variable =
    task1.mutable_command()->mutable_environment()
        ->mutable_variables()->Add();
  variable->set_name("ENV_VAR_KEY");

  TaskInfo task2;
  task2.set_name("2");
  task2.mutable_task_id()->set_value("2");
  task2.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
  task2.mutable_resources()->MergeFrom(offers.get()[0].resources());

  Future<TaskStatus> task1Status;
  Future<TaskStatus> task2Status;
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .WillOnce(FutureArg<1>(&task1Status))
    .WillOnce(FutureArg<1>(&task2Status));

  Offer::Operation operation;
  operation.set_type(Offer::Operation::LAUNCH_GROUP);

  ExecutorInfo executor(DEFAULT_EXECUTOR_INFO);
  executor.mutable_framework_id()->CopyFrom(frameworkInfo.id());

  TaskGroupInfo taskGroup;
  taskGroup.add_tasks()->CopyFrom(task1);
  taskGroup.add_tasks()->CopyFrom(task2);

  Offer::Operation::LaunchGroup* launchGroup =
    operation.mutable_launch_group();

  launchGroup->mutable_executor()->CopyFrom(executor);
  launchGroup->mutable_task_group()->CopyFrom(taskGroup);

  driver.acceptOffers({offers.get()[0].id()}, {operation});

  AWAIT_READY(task1Status);
  EXPECT_EQ(TASK_ERROR, task1Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
  EXPECT_EQ(
      "Task '1' is invalid: Task's `CommandInfo` is invalid: Environment "
      "variable 'ENV_VAR_KEY' of type 'VALUE' must have a value set",
      task1Status->message());

  AWAIT_READY(task2Status);
  EXPECT_EQ(TASK_ERROR, task2Status->state());
  EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
  EXPECT_EQ(
      "Task '1' is invalid: Task's `CommandInfo` is invalid: Environment "
      "variable 'ENV_VAR_KEY' of type 'VALUE' must have a value set",
      task2Status->message());

  // Allow status updates to arrive.
  Clock::pause();
  Clock::settle();

  // Make sure the tasks are not known to master anymore.
  EXPECT_CALL(sched, statusUpdate(&driver, _))
    .Times(0);

  driver.reconcileTasks({});

  // We settle the clock here to ensure any updates sent by the master
  // are received. There shouldn't be any updates in this case.
  Clock::pause();
  Clock::settle();

  driver.stop();
  driver.join();
}


TEST_F(TaskGroupValidationTest, ResourceLimits)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const Offer& offer = offers->at(0);

  const SlaveID slaveId = offer.slave_id();
  frameworkInfo.mutable_id()->CopyFrom(offer.framework_id());

  TaskInfo task1 = createTask(
      slaveId,
      Resources::parse("cpus:0.5;mem:32").get(),
      "exit 0",
      None(),
      "test-task-1",
      id::UUID::random().toString());

  TaskInfo task2 = createTask(
      slaveId,
      Resources::parse("cpus:0.5;mem:32").get(),
      "exit 0",
      None(),
      "test-task-2",
      id::UUID::random().toString());

  Value::Scalar cpuLimit;
  cpuLimit.set_value(1);
  (*task1.mutable_limits())["cpus"] = cpuLimit;
  (*task2.mutable_limits())["cpus"] = cpuLimit;

  Value::Scalar validMemLimit;
  validMemLimit.set_value(64);
  (*task1.mutable_limits())["mem"] = validMemLimit;

  Value::Scalar invalidMemLimit;
  invalidMemLimit.set_value(16);
  (*task2.mutable_limits())["mem"] = invalidMemLimit;

  ContainerInfo containerInfo;
  containerInfo.set_type(ContainerInfo::MESOS);
  containerInfo.mutable_linux_info()->set_share_cgroups(false);

  task1.mutable_container()->CopyFrom(containerInfo);
  task2.mutable_container()->CopyFrom(containerInfo);

  ExecutorInfo executor = createExecutorInfo(
      "test-executor",
      None(),
      Resources::parse("cpus:0.5;mem:32;disk:128").get(),
      ExecutorInfo::DEFAULT,
      frameworkInfo.id());

  // Use short filters so that resources are re-offered.
  Filters filters;
  filters.set_refuse_seconds(0);

  // Error: A memory limit less than the memory request.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Status;
    Future<TaskStatus> task2Status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Status))
      .WillOnce(FutureArg<1>(&task2Status));

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Status);
    EXPECT_EQ(TASK_ERROR, task1Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
    EXPECT_TRUE(strings::contains(
          task1Status->message(),
          "memory limit must be greater"));

    AWAIT_READY(task2Status);
    EXPECT_EQ(TASK_ERROR, task2Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
    EXPECT_TRUE(strings::contains(
          task2Status->message(),
          "memory limit must be greater"));
  }

  (*task2.mutable_limits())["mem"] = validMemLimit;

  containerInfo.mutable_linux_info()->set_share_cgroups(true);

  task2.mutable_container()->CopyFrom(containerInfo);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Error: limits set when 'share_cgroups' is 'true'.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Status;
    Future<TaskStatus> task2Status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Status))
      .WillOnce(FutureArg<1>(&task2Status));

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Status);
    EXPECT_EQ(TASK_ERROR, task1Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
    EXPECT_TRUE(strings::contains(
          task1Status->message(),
          "Resource limits may only be set for tasks within a task group when "
          "the 'share_cgroups' field is set to 'false'."));

    AWAIT_READY(task2Status);
    EXPECT_EQ(TASK_ERROR, task2Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
    EXPECT_TRUE(strings::contains(
          task2Status->message(),
          "Resource limits may only be set for tasks within a task group when "
          "the 'share_cgroups' field is set to 'false'."));
  }

  containerInfo.mutable_linux_info()->set_share_cgroups(false);

  task2.mutable_container()->CopyFrom(containerInfo);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Erase the memory limit so we can be sure what error message to look for
  // in the next test case.
  task1.mutable_limits()->erase("mem");
  task2.mutable_limits()->erase("mem");

  // Error: the agent does not have any isolators loaded, so it can't enforce
  // resource limits and does not launch the task group.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Status;
    Future<TaskStatus> task2Status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Status))
      .WillOnce(FutureArg<1>(&task2Status))
      .WillRepeatedly(Return()); // Ignore subsequent updates.

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Status);
    EXPECT_EQ(TASK_LOST, task1Status->state());
    EXPECT_EQ(
        "CPU limits can only be set on tasks launched in Mesos containers"
        " when the agent has loaded the 'cgroups/cpu' isolator",
        task1Status->message());

    AWAIT_READY(task2Status);
    EXPECT_EQ(TASK_LOST, task2Status->state());
    EXPECT_EQ(
        "CPU limits can only be set on tasks launched in Mesos containers"
        " when the agent has loaded the 'cgroups/cpu' isolator",
        task2Status->message());
  }
}


TEST_F(TaskGroupValidationTest, ShareCgroup)
{
  Clock::pause();

  master::Flags masterFlags = CreateMasterFlags();
  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  Owned<MasterDetector> detector = master.get()->createDetector();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get());
  ASSERT_SOME(slave);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

  EXPECT_CALL(sched, registered(&driver, _, _));

  Future<vector<Offer>> offers;
  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  driver.start();

  Clock::advance(masterFlags.allocation_interval);

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  const SlaveID slaveId = offers->at(0).slave_id();
  frameworkInfo.mutable_id()->CopyFrom(offers->at(0).framework_id());

  TaskInfo task1 = createTask(
      slaveId,
      Resources::parse("cpus:0.1;mem:32").get(),
      "exit 0",
      None(),
      "test-task-1",
      id::UUID::random().toString());

  TaskInfo task2 = createTask(
      slaveId,
      Resources::parse("cpus:0.1;mem:32").get(),
      "exit 0",
      None(),
      "test-task-2",
      id::UUID::random().toString());

  ContainerInfo containerInfoNestedCgroups;
  containerInfoNestedCgroups.set_type(ContainerInfo::MESOS);
  containerInfoNestedCgroups.mutable_linux_info()->set_share_cgroups(false);

  task2.mutable_container()->CopyFrom(containerInfoNestedCgroups);

  ExecutorInfo executor = createExecutorInfo(
      "test-executor",
      None(),
      Resources::parse("cpus:0.5;mem:32;disk:128").get(),
      ExecutorInfo::DEFAULT,
      frameworkInfo.id());

  // Use short filters so that resources are re-offered.
  Filters filters;
  filters.set_refuse_seconds(0);

  // Error: One task with 'share_cgroups==false', another task with no
  // 'LinuxInfo' set.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Status;
    Future<TaskStatus> task2Status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Status))
      .WillOnce(FutureArg<1>(&task2Status));

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Status);
    EXPECT_EQ(TASK_ERROR, task1Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
    EXPECT_TRUE(strings::contains(
          task1Status->message(),
          "If set, the value of 'share_cgroups' must be the same for all tasks "
          "in each task group and under a single executor"));

    AWAIT_READY(task2Status);
    EXPECT_EQ(TASK_ERROR, task2Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
    EXPECT_TRUE(strings::contains(
          task2Status->message(),
          "If set, the value of 'share_cgroups' must be the same for all tasks "
          "in each task group and under a single executor"));
  }

  ContainerInfo containerInfoSharedCgroups;
  containerInfoSharedCgroups.set_type(ContainerInfo::MESOS);
  containerInfoSharedCgroups.mutable_linux_info()->set_share_cgroups(true);

  task1.mutable_container()->CopyFrom(containerInfoSharedCgroups);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Error: Tasks with different values of 'share_cgroups'.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Status;
    Future<TaskStatus> task2Status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Status))
      .WillOnce(FutureArg<1>(&task2Status));

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Status);
    EXPECT_EQ(TASK_ERROR, task1Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
    EXPECT_TRUE(strings::contains(
          task1Status->message(),
          "If set, the value of 'share_cgroups' must be the same for all tasks "
          "in each task group and under a single executor"));

    AWAIT_READY(task2Status);
    EXPECT_EQ(TASK_ERROR, task2Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
    EXPECT_TRUE(strings::contains(
          task2Status->message(),
          "If set, the value of 'share_cgroups' must be the same for all tasks "
          "in each task group and under a single executor"));
  }

  task1.mutable_container()->CopyFrom(containerInfoNestedCgroups);

  executor.set_type(ExecutorInfo::CUSTOM);
  executor.mutable_container()->CopyFrom(containerInfoSharedCgroups);
  executor.mutable_command()->set_value("exit 0");

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Error: Executor with 'share_cgroups' set to 'true'.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Status;
    Future<TaskStatus> task2Status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Status))
      .WillOnce(FutureArg<1>(&task2Status));

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Status);
    EXPECT_EQ(TASK_ERROR, task1Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
    EXPECT_TRUE(strings::contains(
          task1Status->message(),
          "The 'share_cgroups' field cannot be set to 'true' on "
          "executor containers"));

    AWAIT_READY(task2Status);
    EXPECT_EQ(TASK_ERROR, task2Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
    EXPECT_TRUE(strings::contains(
          task2Status->message(),
          "The 'share_cgroups' field cannot be set to 'true' on "
          "executor containers"));
  }

  executor.mutable_container()->CopyFrom(containerInfoNestedCgroups);
  executor.set_type(ExecutorInfo::DEFAULT);
  executor.clear_command();

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Valid, no error.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Starting;
    Future<TaskStatus> task2Starting;
    Future<TaskStatus> task1Running;
    Future<TaskStatus> task2Running;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Starting))
      .WillOnce(FutureArg<1>(&task2Starting))
      .WillOnce(FutureArg<1>(&task1Running))
      .WillOnce(FutureArg<1>(&task2Running));

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Starting);
    EXPECT_EQ(TASK_STARTING, task1Starting->state());
    AWAIT_READY(task2Starting);
    EXPECT_EQ(TASK_STARTING, task2Starting->state());

    AWAIT_READY(task1Running);
    EXPECT_EQ(TASK_RUNNING, task1Running->state());
    AWAIT_READY(task2Running);
    EXPECT_EQ(TASK_RUNNING, task2Running->state());
  }

  task1 = createTask(
      slaveId,
      Resources::parse("cpus:0.1;mem:32").get(),
      "exit 0",
      None(),
      "test-task-1",
      id::UUID::random().toString());

  task2 = createTask(
      slaveId,
      Resources::parse("cpus:0.1;mem:32").get(),
      "exit 0",
      None(),
      "test-task-2",
      id::UUID::random().toString());

  task1.mutable_container()->CopyFrom(containerInfoSharedCgroups);
  task2.mutable_container()->CopyFrom(containerInfoSharedCgroups);

  EXPECT_CALL(sched, resourceOffers(&driver, _))
    .WillOnce(FutureArg<1>(&offers));

  Clock::advance(masterFlags.allocation_interval);
  Clock::settle();

  AWAIT_READY(offers);
  ASSERT_FALSE(offers->empty());

  // Error: The 'share_cgroups' field must have the same value for all tasks
  // under a single executor.
  {
    TaskGroupInfo taskGroup;
    taskGroup.add_tasks()->CopyFrom(task1);
    taskGroup.add_tasks()->CopyFrom(task2);

    Offer::Operation operation;
    operation.set_type(Offer::Operation::LAUNCH_GROUP);

    Offer::Operation::LaunchGroup* launchGroup =
      operation.mutable_launch_group();

    launchGroup->mutable_executor()->CopyFrom(executor);
    launchGroup->mutable_task_group()->CopyFrom(taskGroup);

    Future<TaskStatus> task1Status;
    Future<TaskStatus> task2Status;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&task1Status))
      .WillOnce(FutureArg<1>(&task2Status))
      .WillRepeatedly(Return()); // Ignore subsequent updates.

    driver.acceptOffers({offers->at(0).id()}, {operation}, filters);

    AWAIT_READY(task1Status);
    EXPECT_EQ(TASK_ERROR, task1Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task1Status->reason());
    EXPECT_TRUE(strings::contains(
          task1Status->message(),
          "If set, the value of 'share_cgroups' must be the same for all "
          "tasks in each task group and under a single executor"));

    AWAIT_READY(task2Status);
    EXPECT_EQ(TASK_ERROR, task2Status->state());
    EXPECT_EQ(TaskStatus::REASON_TASK_GROUP_INVALID, task2Status->reason());
    EXPECT_TRUE(strings::contains(
          task2Status->message(),
          "If set, the value of 'share_cgroups' must be the same for all "
          "tasks in each task group and under a single executor"));
  }
}


class FrameworkInfoValidationTest : public MesosTest {};


// This tests the role validation for FrameworkInfo.
TEST_F(FrameworkInfoValidationTest, ValidateRoles)
{
  // Not MULTI_ROLE, no 'role' (default to "*"), no 'roles'.
  {
    FrameworkInfo frameworkInfo;

    EXPECT_NONE(::framework::validate(frameworkInfo));
  }

  // Not MULTI_ROLE, no 'role' (default to "*"), has 'roles' (error!).
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.add_roles("bar");
    frameworkInfo.add_roles("qux");

    EXPECT_SOME(::framework::validate(frameworkInfo));
  }

  // Not MULTI_ROLE, has 'role', no 'roles'.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.set_role("foo");

    EXPECT_NONE(::framework::validate(frameworkInfo));
  }

  // Not MULTI_ROLE, has 'role', has 'roles' (error!).
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.add_roles("bar");
    frameworkInfo.add_roles("qux");
    frameworkInfo.set_role("foo");

    EXPECT_SOME(::framework::validate(frameworkInfo));
  }

  // Is MULTI_ROLE, no 'role', no 'roles'.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::MULTI_ROLE);

    EXPECT_NONE(::framework::validate(frameworkInfo));
  }

  // Is MULTI_ROLE, no 'role', has 'roles'.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::MULTI_ROLE);
    frameworkInfo.add_roles("bar");
    frameworkInfo.add_roles("qux");

    EXPECT_NONE(::framework::validate(frameworkInfo));
  }

  // Is MULTI_ROLE, has 'role' (error!), no 'roles'.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.set_role("foo");
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::MULTI_ROLE);

    EXPECT_SOME(::framework::validate(frameworkInfo));
  }

  // Is MULTI_ROLE, has 'role' (error!), has 'roles'.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.set_role("foo");
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::MULTI_ROLE);
    frameworkInfo.add_roles("bar");
    frameworkInfo.add_roles("qux");

    EXPECT_SOME(::framework::validate(frameworkInfo));
  }

  // Duplicate items in 'roles'.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::MULTI_ROLE);
    frameworkInfo.add_roles("bar");
    frameworkInfo.add_roles("qux");
    frameworkInfo.add_roles("bar");

    EXPECT_SOME(::framework::validate(frameworkInfo));
  }

  // Check invalid character in 'roles'.
  {
    FrameworkInfo frameworkInfo;
    frameworkInfo.add_roles("bar");
    frameworkInfo.add_roles("/x");
    frameworkInfo.add_capabilities()->set_type(
        FrameworkInfo::Capability::MULTI_ROLE);

    EXPECT_SOME(::framework::validate(frameworkInfo));
  }
}


// This tests the validation of the `FrameworkID`.
TEST_F(FrameworkInfoValidationTest, ValidateFrameworkID)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  // Unset framework IDs are used in an initial subscription and are valid.
  frameworkInfo.clear_id();
  EXPECT_NONE(::framework::validate(frameworkInfo));

  // We allow set but empty framework IDs, see MESOS-9481.
  frameworkInfo.mutable_id()->set_value("");
  EXPECT_NONE(::framework::validate(frameworkInfo));

  // Typical IDs the master would assign are valid.
  frameworkInfo.mutable_id()->set_value(id::UUID::random().toString());
  frameworkInfo.mutable_id()->set_value(
      strings::format("%s-4711", id::UUID::random().toString()).get());
  EXPECT_NONE(::framework::validate(frameworkInfo));

  // Framework IDs containing typical path separators are invalid.
  frameworkInfo.mutable_id()->set_value("foo/bar");
  EXPECT_SOME(::framework::validate(frameworkInfo));

  frameworkInfo.mutable_id()->set_value("foo/..");
  EXPECT_SOME(::framework::validate(frameworkInfo));
}


// This test validates that framework cannot configure negative
// resources in their minimal allocatable resources offer filters.
TEST_F(FrameworkInfoValidationTest, ValidateOfferFilters)
{
  Value::Scalar scalar;
  scalar.set_value(-2);

  OfferFilters offerFilters;
  offerFilters.mutable_min_allocatable_resources()
    ->add_quantities()
    ->mutable_quantities()
    ->insert({"cpus", scalar});

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ASSERT_FALSE(frameworkInfo.roles().empty());

  frameworkInfo.mutable_offer_filters()->insert(
      {frameworkInfo.roles(0), offerFilters});

  EXPECT_SOME_EQ(
      Error("Invalid resource quantity for 'cpus': "
        "Negative values not supported"),
      framework::validate(frameworkInfo));
}


// This tests validation of FrameworkInfo updates.
TEST_F(FrameworkInfoValidationTest, ValidateUpdate)
{
  {
    FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.add_roles("bar");

    EXPECT_NONE(framework::validateUpdate(
        DEFAULT_FRAMEWORK_INFO, frameworkInfo));
  }

  {
    FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    *frameworkInfo.mutable_principal() += "_foo";

    EXPECT_SOME(framework::validateUpdate(
        DEFAULT_FRAMEWORK_INFO, frameworkInfo));
  }

  {
    FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    *frameworkInfo.mutable_user() += "_foo";

    EXPECT_SOME(framework::validateUpdate(
        DEFAULT_FRAMEWORK_INFO, frameworkInfo));
  }

  {
    FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
    frameworkInfo.set_checkpoint(!frameworkInfo.checkpoint());

    EXPECT_SOME(framework::validateUpdate(
        DEFAULT_FRAMEWORK_INFO, frameworkInfo));
  }
}


// This test ensures that ia framework cannot use the
// `FrameworkInfo.roles` field without providing the
// MULTI_ROLE capability.
TEST_F(FrameworkInfoValidationTest, MissingMultiRoleCapability)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.clear_capabilities();
  framework.set_roles(0, "role");

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<string> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureArg<1>(&error));

  driver.start();

  AWAIT_READY(error);
}


// This test ensures that a multi-role framework can register.
TEST_F(FrameworkInfoValidationTest, AcceptMultiRoleFramework)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "role1");
  framework.add_roles("role2");
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<Nothing> registered;
  EXPECT_CALL(sched, registered(&driver, _, _))
    .WillOnce(FutureSatisfy(&registered));

  driver.start();

  AWAIT_READY(registered);
}


// This test ensures that a multi-role framework using
// a non-whitelisted role is denied registration.
TEST_F(FrameworkInfoValidationTest, MultiRoleWhitelist)
{
  master::Flags masterFlags = CreateMasterFlags();
  masterFlags.roles = "role1";

  Try<Owned<cluster::Master>> master = StartMaster(masterFlags);
  ASSERT_SOME(master);

  FrameworkInfo framework = DEFAULT_FRAMEWORK_INFO;
  framework.set_roles(0, "role1");
  framework.add_roles("role2");
  framework.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  MockScheduler sched;
  MesosSchedulerDriver driver(
      &sched, framework, master.get()->pid, DEFAULT_CREDENTIAL);

  Future<string> error;
  EXPECT_CALL(sched, error(&driver, _))
    .WillOnce(FutureArg<1>(&error));

  driver.start();

  AWAIT_READY(error);
}


// This test verifies that a not yet MULTI_ROLE capable framework can
// upgrade to be MULTI_ROLE capable, given that it does not change its
// roles, i.e., the previously used `FrameworkInfo.role` equals
// `FrameworkInfo.roles` (both set to the same single value; note
// that `FrameworkInfo.role` being unset is equivalent to being
// set to "*").
TEST_F(FrameworkInfoValidationTest, UpgradeToMultiRole)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.clear_capabilities();
  frameworkInfo.clear_roles();
  frameworkInfo.set_role("role");

  // Set a long failover timeout so the framework isn't immediately removed.
  frameworkInfo.set_failover_timeout(Weeks(1).secs());

  Future<FrameworkID> frameworkId;

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureArg<1>(&frameworkId));

    driver.start();

    Clock::settle();

    AWAIT_READY(frameworkId);

    driver.stop(true); // Failover.
    driver.join();
  }

  frameworkInfo.mutable_id()->CopyFrom(frameworkId.get());

  // Upgrade `frameworkInfo` to declare the MULTI_ROLE capability,
  // and migrate from `role` to `roles` field.
  frameworkInfo.add_roles(frameworkInfo.role());
  frameworkInfo.clear_role();
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> registered;
    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureSatisfy(&registered));

    driver.start();

    Clock::settle();

    AWAIT_READY(registered);

    driver.stop();
    driver.join();
  }
}


// This tests verifies that a multi-role capable framework is able
// to downgrade to remove multi-role capabilities and change roles
// the framework is subscribed to.
TEST_F(FrameworkInfoValidationTest, DowngradeFromMultipleRoles)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  // TODO(mpark): Remove this once `RESERVATION_REFINEMENT`
  // is removed from `DEFAULT_FRAMEWORK_INFO`.
  frameworkInfo.clear_capabilities();

  frameworkInfo.set_roles(0, "role1");
  frameworkInfo.add_roles("role2");
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  // Set a long failover timeout so the framework isn't immediately removed.
  frameworkInfo.set_failover_timeout(Weeks(1).secs());

  Future<FrameworkID> frameworkId;

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureArg<1>(&frameworkId));

    driver.start();

    Clock::settle();

    AWAIT_READY(frameworkId);

    driver.stop(true); // Failover.
    driver.join();
  }

  frameworkInfo.mutable_id()->CopyFrom(frameworkId.get());

  // Downgrade `frameworkInfo` to remove `MULTI_ROLE` capability, and
  // migrate from `roles` to `role` field.
  ASSERT_EQ(2, frameworkInfo.roles_size());
  frameworkInfo.set_role(frameworkInfo.roles(0));
  frameworkInfo.clear_roles();
  ASSERT_EQ(1, frameworkInfo.capabilities_size());
  frameworkInfo.clear_capabilities();

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> registered;
    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureSatisfy(&registered));

    driver.start();

    Clock::settle();

    AWAIT_READY(registered);

    driver.stop();
    driver.join();
  }
}


// This test verifies that a multi-role framework can change roles on failover.
TEST_F(FrameworkInfoValidationTest, RoleChangeWithMultiRole)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;

  ASSERT_FALSE(frameworkInfo.has_role());
  frameworkInfo.set_roles(0, "role1");

  // Set a long failover timeout so the framework isn't immediately removed.
  frameworkInfo.set_failover_timeout(Weeks(1).secs());

  Future<FrameworkID> frameworkId;

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureArg<1>(&frameworkId));

    driver.start();

    Clock::settle();

    AWAIT_READY(frameworkId);

    driver.stop(true); // Failover.
    driver.join();
  }

  frameworkInfo.mutable_id()->CopyFrom(frameworkId.get());
  frameworkInfo.add_roles("role2");

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> registered;
    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureSatisfy(&registered));

    driver.start();

    Clock::settle();

    AWAIT_READY(registered);

    driver.stop();
    driver.join();
  }
}


// This test checks that frameworks can change their `role` during master
// failover. The scenario tested here sets up a one-agent cluster with
// a single framework. On failover the master would first learn about
// the framework from the agent, and then from the framework.
TEST_F(FrameworkInfoValidationTest, RoleChangeWithMultiRoleMasterFailover)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  MockExecutor exec(DEFAULT_EXECUTOR_ID);
  TestContainerizer containerizer(&exec);

  StandaloneMasterDetector detector(master.get()->pid);
  Try<Owned<cluster::Slave>> agent = StartSlave(&detector, &containerizer);
  ASSERT_SOME(agent);

  // Set a role for the framework which we will change later. Also,
  // set a long framework failover timeout so the framework isn't
  // immediately cleaned up.
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  frameworkInfo.set_failover_timeout(Weeks(1).secs());
  frameworkInfo.clear_capabilities();
  frameworkInfo.clear_roles();
  frameworkInfo.set_role("role1");

  Future<FrameworkID> frameworkId;

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureArg<1>(&frameworkId));

    Future<vector<Offer>> offers;
    EXPECT_CALL(sched, resourceOffers(&driver, _))
      .WillOnce(FutureArg<1>(&offers))
      .WillRepeatedly(Return()); // Ignore subsequent offers.

    driver.start();

    AWAIT_READY(frameworkId);

    // Start a single task so the `FrameworkInfo` is known to the
    // agent, and communicated back to the master after master failover.
    AWAIT_READY(offers);
    ASSERT_FALSE(offers->empty());

    TaskInfo task;
    task.set_name("");
    task.mutable_task_id()->set_value("1");
    task.mutable_slave_id()->MergeFrom(offers.get()[0].slave_id());
    task.mutable_resources()->MergeFrom(offers.get()[0].resources());
    task.mutable_executor()->MergeFrom(DEFAULT_EXECUTOR_INFO);

    EXPECT_CALL(exec, registered(_, _, _, _));

    EXPECT_CALL(exec, launchTask(_, _))
      .WillOnce(SendStatusUpdateFromTask(TASK_RUNNING));

    Future<TaskStatus> runningStatus;
    EXPECT_CALL(sched, statusUpdate(&driver, _))
      .WillOnce(FutureArg<1>(&runningStatus));

    driver.launchTasks(offers.get()[0].id(), {task});

    AWAIT_READY(runningStatus);
    EXPECT_EQ(TASK_RUNNING, runningStatus->state());
    EXPECT_EQ(task.task_id(), runningStatus->task_id());

    // Since we launched a task, stopping the driver will cause the
    // task and its executor to be shutdown.
    EXPECT_CALL(exec, shutdown(_))
      .Times(AtMost(1));

    driver.stop();
    driver.join();
  }

  // Cause a master failover.
  detector.appoint(None());

  master->reset();
  master = StartMaster();
  ASSERT_SOME(master);

  // Make sure the agent registers before the framework resubscribes.
  // The master will learn about the framework from the agent.
  Future<SlaveReregisteredMessage> slaveReregisteredMessage =
    FUTURE_PROTOBUF(SlaveReregisteredMessage(), _, _);

  detector.appoint(master.get()->pid);

  AWAIT_READY(slaveReregisteredMessage);

  // Upgrade `frameworkInfo` to add `MULTI_ROLE` capability, and
  // migrate from `role1` to `role2`.
  frameworkInfo.mutable_id()->CopyFrom(frameworkId.get());
  frameworkInfo.add_roles("role2");
  frameworkInfo.clear_role();
  frameworkInfo.add_capabilities()->set_type(
      FrameworkInfo::Capability::MULTI_ROLE);

  {
    MockScheduler sched;
    MesosSchedulerDriver driver(
        &sched, frameworkInfo, master.get()->pid, DEFAULT_CREDENTIAL);

    Future<Nothing> registered;
    EXPECT_CALL(sched, registered(&driver, _, _))
      .WillOnce(FutureSatisfy(&registered));

    driver.start();

    AWAIT_READY(registered);

    driver.stop();
    driver.join();
  }
}


class RegisterSlaveValidationTest : public MesosTest {};


TEST_F(RegisterSlaveValidationTest, DropInvalidReregistration)
{
  Clock::pause();

  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  Future<SlaveRegisteredMessage> slaveRegisteredMessage =
    FUTURE_PROTOBUF(SlaveRegisteredMessage(), master.get()->pid, _);

  StandaloneMasterDetector detector(master.get()->pid);
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(&detector, slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);

  // Wait until the master acknowledges the slave registration.
  AWAIT_READY(slaveRegisteredMessage);

  // Drop and capture the slave's ReregisterSlaveMessage.
  Future<ReregisterSlaveMessage> reregisterSlaveMessage =
    DROP_PROTOBUF(ReregisterSlaveMessage(), slave.get()->pid, _);

  // Simulate a new master detected event on the slave,
  // so that the slave will do a re-registration.
  detector.appoint(master.get()->pid);

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(reregisterSlaveMessage);

  // Now that we have a valid ReregisterSlaveMessage, tweak it to
  // fail validation.
  ReregisterSlaveMessage message = reregisterSlaveMessage.get();

  Task* task = message.add_tasks();
  task->set_name(id::UUID::random().toString());
  task->mutable_slave_id()->set_value(id::UUID::random().toString());
  task->mutable_task_id()->set_value(id::UUID::random().toString());
  task->mutable_framework_id()->set_value(id::UUID::random().toString());
  task->mutable_executor_id()->set_value(id::UUID::random().toString());
  task->set_state(TASK_RUNNING);

  // We expect the master to drop the ReregisterSlaveMessage, so it
  // will not send any more SlaveReregisteredMessage responses.
  EXPECT_NO_FUTURE_PROTOBUFS(SlaveReregisteredMessage(), _, _);

  // Send the modified message to the master.
  process::post(slave.get()->pid, master->get()->pid, message);

  // Settle the clock to retire in-flight messages.
  Clock::settle();
}


TEST_F(RegisterSlaveValidationTest, DropInvalidRegistration)
{
  Try<Owned<cluster::Master>> master = StartMaster();
  ASSERT_SOME(master);

  // Drop and capture the slave's RegisterSlaveMessage.
  Future<RegisterSlaveMessage> registerSlaveMessage =
    DROP_PROTOBUF(RegisterSlaveMessage(), _, _);

  // We expect the master to drop the RegisterSlaveMessage, so it
  // will never send any SlaveRegisteredMessage responses.
  EXPECT_NO_FUTURE_PROTOBUFS(SlaveRegisteredMessage(), _, _);

  Clock::pause();

  Owned<MasterDetector> detector = master.get()->createDetector();
  slave::Flags slaveFlags = CreateSlaveFlags();
  Try<Owned<cluster::Slave>> slave = StartSlave(detector.get(), slaveFlags);
  ASSERT_SOME(slave);

  Clock::advance(slaveFlags.authentication_backoff_factor);
  Clock::advance(slaveFlags.registration_backoff_factor);

  AWAIT_READY(registerSlaveMessage);

  // Now that we have a valid RegisterSlaveMessage, tweak it to
  // fail validation by setting an invalid slave ID,
  RegisterSlaveMessage message = registerSlaveMessage.get();

  SlaveInfo* slaveInfo = message.mutable_slave();
  slaveInfo->mutable_id()->set_value(
      strings::join(
          "/../",
          id::UUID::random().toString(),
          id::UUID::random().toString(),
          id::UUID::random().toString()));

  // Send the modified message to the master.
  process::post(slave.get()->pid, master->get()->pid, message);

  // Settle the clock to retire in-flight messages.
  Clock::settle();
}


// Test that duplicate ExecutorIDs are correctly handled when
// validating the ReregisterSlaveMessage.
TEST_F(RegisterSlaveValidationTest, DuplicateExecutorID)
{
  ReregisterSlaveMessage message;

  SlaveInfo *slaveInfo = message.mutable_slave();
  slaveInfo->mutable_id()->set_value("agent-id");
  slaveInfo->mutable_resources()->CopyFrom(
      Resources::parse("cpus:2;mem:10").get());

  FrameworkInfo *framework = message.add_frameworks();
  framework->CopyFrom(DEFAULT_FRAMEWORK_INFO);
  framework->set_name("framework1");
  framework->mutable_id()->set_value("framework1");

  framework = message.add_frameworks();
  framework->CopyFrom(DEFAULT_FRAMEWORK_INFO);
  framework->set_name("framework2");
  framework->mutable_id()->set_value("framework2");

  ExecutorInfo *executor = message.add_executor_infos();
  executor->CopyFrom(DEFAULT_EXECUTOR_INFO);
  executor->mutable_framework_id()->set_value("framework1");

  executor = message.add_executor_infos();
  executor->CopyFrom(DEFAULT_EXECUTOR_INFO);
  executor->mutable_framework_id()->set_value("framework2");

  // Executors with the same ID in different frameworks are allowed.
  EXPECT_EQ(message.executor_infos(0).executor_id(),
            message.executor_infos(1).executor_id());
  EXPECT_NE(message.executor_infos(0).framework_id(),
            message.executor_infos(1).framework_id());
  EXPECT_NONE(master::validation::master::message::reregisterSlave(message));

  executor->mutable_framework_id()->set_value("framework1");

  // Executors with the same ID in in the same framework are not allowed.
  EXPECT_EQ(message.executor_infos(0).executor_id(),
            message.executor_infos(1).executor_id());
  EXPECT_EQ(message.executor_infos(0).framework_id(),
            message.executor_infos(1).framework_id());
  EXPECT_SOME(master::validation::master::message::reregisterSlave(message));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
