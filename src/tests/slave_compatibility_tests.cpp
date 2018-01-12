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


// This set of tests checks whether the various settings of the
// --reconfiguration_policy flag behave as expected.

#include "slave/compatibility.hpp"

#include <mesos/attributes.hpp>
#include <mesos/resources.hpp>

#include "tests/mesos.hpp"

namespace mesos {
namespace internal {
namespace tests {

class SlaveCompatibilityTest : public MesosTest {};


SlaveInfo createSlaveInfo(
    const std::string& resources,
    const std::string& attributes)
{
  SlaveID id;
  id.set_value("agent");

  Attributes agentAttributes = Attributes::parse(attributes);
  Resources agentResources = Resources::parse(resources).get();

  SlaveInfo slave;
  *(slave.mutable_attributes()) = agentAttributes;
  *(slave.mutable_resources()) = agentResources;
  *(slave.mutable_id()) = id;
  slave.set_hostname(id.value());

  return slave;
}


TEST_F(SlaveCompatibilityTest, Equal)
{
  SlaveInfo original = createSlaveInfo("cpus:500", "foo:bar");

  SlaveInfo changedAttributes(original);
  SlaveInfo changedResources(original);
  ASSERT_SOME(slave::compatibility::equal(original, changedAttributes));
  ASSERT_SOME(slave::compatibility::equal(original, changedResources));

  *(changedAttributes.mutable_attributes()) = Attributes::parse("foo:baz");
  ASSERT_ERROR(slave::compatibility::equal(original, changedAttributes));

  *(changedResources.mutable_resources()) = Resources::parse("cpus:600").get();
  ASSERT_ERROR(slave::compatibility::equal(original, changedResources));

  *(changedResources.mutable_resources()) =
      Resources::parse("cpus:250;cpus:250").get();

  ASSERT_SOME(slave::compatibility::equal(original, changedResources));
}


TEST_F(SlaveCompatibilityTest, Additive)
{
  // Changing the hostname is not permitted.
  SlaveInfo originalHostname;
  originalHostname.set_hostname("host");
  SlaveInfo changedHostname(originalHostname);
  ASSERT_SOME(slave::compatibility::additive(
      originalHostname, changedHostname));

  changedHostname.set_hostname("another_host");
  ASSERT_ERROR(slave::compatibility::additive(
      originalHostname, changedHostname));

  // Changing the port is not permitted.
  SlaveInfo originalPort;
  originalPort.set_port(1234);
  SlaveInfo changedPort(originalPort);
  ASSERT_SOME(slave::compatibility::additive(originalPort, changedPort));

  changedPort.set_port(4321);
  ASSERT_ERROR(slave::compatibility::additive(originalPort, changedPort));

  // Resources.

  // Adding new resources is permitted.
  SlaveInfo originalResource = createSlaveInfo("cpus:50", "");
  SlaveInfo extendedResource = createSlaveInfo("cpus:50;mem:100", "");
  SlaveInfo modifiedResource = createSlaveInfo("cpus:[100-200]", "");
  ASSERT_SOME(slave::compatibility::additive(
      originalResource, extendedResource));

  // Removing existing resources is not permitted.
  ASSERT_ERROR(slave::compatibility::additive(
      extendedResource, originalResource));

  // Changing the type of a resource is not permitted.
  ASSERT_ERROR(slave::compatibility::additive(
      originalResource, modifiedResource));

  // Scalar resources can be increased but not decreased.
  SlaveInfo originalScalarResource = createSlaveInfo("cpus:50", "");
  SlaveInfo changedScalarResource = createSlaveInfo("cpus:100", "");
  ASSERT_SOME(slave::compatibility::additive(
      originalScalarResource, changedScalarResource));
  ASSERT_ERROR(slave::compatibility::additive(
      changedScalarResource, originalScalarResource));

  // Going from 50 to 30+30 is still an increase
  SlaveInfo changedScalarResource2 = createSlaveInfo("cpus:30;cpus:30", "");
  ASSERT_SOME(slave::compatibility::additive(
      originalScalarResource, changedScalarResource2));

  // Range attributes can be extended but not shrinked.
  SlaveInfo originalRangeResource = createSlaveInfo("range:[100-200]", "");
  SlaveInfo changedRangeResource = createSlaveInfo("range:[100-300]", "");
  ASSERT_SOME(slave::compatibility::additive(
      originalRangeResource, changedRangeResource));
  ASSERT_ERROR(slave::compatibility::additive(
      changedRangeResource, originalRangeResource));

  // Set attributes can be extended but not shrinked.
  SlaveInfo originalSetResource = createSlaveInfo("set:{}", "");
  SlaveInfo changedSetResource = createSlaveInfo("set:{a,b}", "");
  ASSERT_SOME(slave::compatibility::additive(
      originalSetResource, changedSetResource));
  ASSERT_ERROR(slave::compatibility::additive(
      changedSetResource, originalSetResource));

  // Attributes.

  // Adding new attributes is permitted.
  SlaveInfo originalAttribute = createSlaveInfo("", "os:lucid");
  SlaveInfo extendedAttribute = createSlaveInfo("", "os:lucid;dc:amsterdam");
  SlaveInfo modifiedAttribute = createSlaveInfo("", "os:4");
  ASSERT_SOME(slave::compatibility::additive(
      originalAttribute, extendedAttribute));

  // Removing existing attributes is not permitted.
  ASSERT_ERROR(slave::compatibility::additive(
      extendedAttribute, originalAttribute));

  // Changing the type of an attribute is not permitted.
  ASSERT_ERROR(slave::compatibility::additive(
      originalAttribute, modifiedAttribute));

  // Changing value of a text attribute is not permitted.
  SlaveInfo originalTextAttribute = createSlaveInfo("", "os:lucid");
  SlaveInfo changedTextAttribute = createSlaveInfo("", "os:trusty");
  ASSERT_ERROR(slave::compatibility::additive(
      originalTextAttribute, changedTextAttribute));

  // Changing the value of a scalar attribute is not permitted.
  SlaveInfo originalScalarAttribute = createSlaveInfo("", "rack:1");
  SlaveInfo changedScalarAttribute = createSlaveInfo("", "rack:2");
  ASSERT_ERROR(slave::compatibility::additive(
      originalScalarAttribute, changedScalarAttribute));

  // Range attributes can be extended but not shrinked.
  SlaveInfo originalRangeAttribute = createSlaveInfo("", "range:[100-200]");
  SlaveInfo changedRangeAttribute = createSlaveInfo("", "range:[100-300]");
  ASSERT_SOME(slave::compatibility::additive(
      originalRangeAttribute, changedRangeAttribute));
  ASSERT_ERROR(slave::compatibility::additive(
      changedRangeAttribute, originalRangeAttribute));
}


TEST_F(SlaveCompatibilityTest, AdditiveWithReservations)
{
  SlaveInfo originalReservations = createSlaveInfo("foo(A):10;foo(B):20", "");

  // Ok: Both roles have increased amounts of `foo`.
  SlaveInfo increasedReservations = createSlaveInfo("foo(A):20;foo(B):30", "");
  ASSERT_SOME(slave::compatibility::additive(
      originalReservations, increasedReservations));

  // Not ok: The total increases, but the amount of `foo` reserved for role A
  // decreased.
  SlaveInfo modifiedReservations = createSlaveInfo("foo(A):5;foo(B):50", "");
  ASSERT_ERROR(slave::compatibility::additive(
      originalReservations, modifiedReservations));
}


TEST_F(SlaveCompatibilityTest, Disks)
{
  const char* diskAsJson = R"_(
  [
    {
      "name": "disk",
      "type": "SCALAR",
      "scalar": {
        "value": 868000
      }
    }
  ]
  )_";

  const  char* diskAndMountAsJson = R"_(
   [
    {
      "name": "disk",
      "type": "SCALAR",
      "scalar": {
        "value": 868000
      }
    },
    {
      "name": "disk",
      "type": "SCALAR",
      "scalar": {
        "value": 1830000
      },
      "disk": {
        "source": {
          "type": "MOUNT",
          "mount": {
            "root" : "/srv/mesos/volumes/a"
          }
        }
      }
    }
  ]
  )_";



  SlaveInfo info1 = createSlaveInfo(diskAsJson, "");
  SlaveInfo info2 = createSlaveInfo(diskAndMountAsJson, "");

  ASSERT_SOME(slave::compatibility::additive(info1, info2));

  // MESOS-8410.
  ASSERT_SOME(slave::compatibility::additive(info2, info2));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
