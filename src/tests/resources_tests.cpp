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

#include <algorithm>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include <stout/bytes.hpp>
#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>

#include <mesos/resource_quantities.hpp>
#include <mesos/resources.hpp>

#include <mesos/v1/resources.hpp>

#include "common/resources_utils.hpp"

#include "internal/evolve.hpp"

#include "master/master.hpp"

#include "tests/mesos.hpp"
#include "tests/resources_utils.hpp"

using namespace mesos::internal::master;

using std::cout;
using std::endl;
using std::map;
using std::ostringstream;
using std::pair;
using std::set;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using mesos::internal::evolve;

using mesos::internal::protobuf::createLabel;

namespace mesos {
namespace internal {
namespace tests {


TEST(ResourcesTest, Parsing)
{
  Resource cpus = Resources::parse("cpus", "45.55", "*").get();

  ASSERT_EQ(Value::SCALAR, cpus.type());
  EXPECT_DOUBLE_EQ(45.55, cpus.scalar().value());

  Resource ports = Resources::parse(
      "ports", "[10000-20000, 30000-50000]", "*").get();

  ASSERT_EQ(Value::RANGES, ports.type());
  EXPECT_EQ(2, ports.ranges().range_size());

  Resource disks = Resources::parse("disks", "{sda1}", "*").get();

  ASSERT_EQ(Value::SET, disks.type());
  ASSERT_EQ(1, disks.set().item_size());
  EXPECT_EQ("sda1", disks.set().item(0));

  Resources r1 = Resources::parse(
      "cpus:45.55;ports:[10000-20000, 30000-50000];disks:{sda1}").get();

  Resources r2;
  r2 += cpus;
  r2 += ports;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, ParsingWithRoles)
{
  Resources parse1 = Resources::parse("cpus(role1):2;mem(role1):3").get();

  Resource cpus;
  cpus.set_name("cpus");
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(2);
  cpus.add_reservations()->CopyFrom(createStaticReservationInfo("role1"));

  Resource mem;
  mem.set_name("mem");
  mem.set_type(Value::SCALAR);
  mem.mutable_scalar()->set_value(3);
  mem.add_reservations()->CopyFrom(createStaticReservationInfo("role1"));

  Resources resources1;
  resources1 += cpus;
  resources1 += mem;

  EXPECT_EQ(parse1, resources1);

  Resources parse2 = Resources::parse(
      "cpus(role1):2.5;ports(role2):[0-100]").get();

  Resource cpus2;
  cpus2.set_name("cpus");
  cpus2.set_type(Value::SCALAR);
  cpus2.mutable_scalar()->set_value(2.5);
  cpus2.add_reservations()->CopyFrom(createStaticReservationInfo("role1"));

  Resource ports;
  ports.set_name("ports");
  ports.set_type(Value::RANGES);
  Value::Range* range = ports.mutable_ranges()->add_range();
  range->set_begin(0);
  range->set_end(100);
  ports.add_reservations()->CopyFrom(createStaticReservationInfo("role2"));

  Resources resources2;
  resources2 += ports;
  resources2 += cpus2;

  EXPECT_EQ(parse2, resources2);

  Resources parse3 = Resources::parse(
      "cpus:2.5;ports(role2):[0-100]", "role1").get();

  EXPECT_EQ(parse2, parse3);
}


TEST(ResourcesTest, ParseError)
{
  // Missing colon.
  EXPECT_ERROR(Resources::parse("cpus(role1)"));

  // Mismatched parentheses.
  EXPECT_ERROR(Resources::parse("cpus(role1:1"));
  EXPECT_ERROR(Resources::parse("cpus)(role1:1"));

  // Resources with the same name but different types are not
  // allowed.
  EXPECT_ERROR(Resources::parse("foo(role1):1;foo(role2):[0-1]"));
}


TEST(ResourcesTest, ParsingFromJSON)
{
  Resources resources = Resources::parse("cpus:2;mem:3").get();

  // Convert to an array of JSON objects.
  JSON::Array array =
    JSON::protobuf(static_cast<const RepeatedPtrField<Resource>&>(resources));

  // Parse JSON array into a collection of Resource messages and convert them
  // into Resources object.
  auto parse = ::protobuf::parse<RepeatedPtrField<Resource>>(array);
  ASSERT_SOME(parse);

  EXPECT_EQ(resources, Resources(parse.get()));

  // Use JSON strings both with and without newline characters.
  string jsonString =
    "[\n"
    "  {\n"
    "    \"name\": \"cpus\",\n"
    "    \"type\": \"SCALAR\",\n"
    "    \"scalar\": {\n"
    "      \"value\": 45.55\n"
    "    }\n"
    "  }\n"
    "]";

  Try<Resources> resourcesTry = Resources::parse(jsonString);
  ASSERT_SOME(resourcesTry);

  Resources cpuResources(resourcesTry.get());
  const Resource& cpus = *(cpuResources.begin());

  ASSERT_EQ(Value::SCALAR, cpus.type());
  EXPECT_EQ(45.55, cpus.scalar().value());

  EXPECT_EQ(1u, cpuResources.size());

  jsonString =
    "["
    "  {"
    "    \"name\": \"ports\","
    "    \"type\": \"RANGES\","
    "    \"ranges\": {"
    "      \"range\": ["
    "        {"
    "          \"begin\": 10000,"
    "          \"end\": 20000"
    "        },"
    "        {"
    "          \"begin\": 30000,"
    "          \"end\": 50000"
    "        }"
    "      ]"
    "    }"
    "  }"
    "]";

  resourcesTry = Resources::parse(jsonString);
  ASSERT_SOME(resourcesTry);

  Resources portResources(resourcesTry.get());
  const Resource& ports = *(portResources.begin());

  EXPECT_EQ(Value::RANGES, ports.type());
  EXPECT_EQ(2, ports.ranges().range_size());

  // Do not specify the ordering of ranges, only check the values.
  if (10000 != ports.ranges().range(0).begin()) {
    EXPECT_EQ(30000u, ports.ranges().range(0).begin());
    EXPECT_EQ(10000u, ports.ranges().range(1).begin());
  } else {
    EXPECT_EQ(30000u, ports.ranges().range(1).begin());
  }

  jsonString =
    "["
    "  {"
    "    \"name\": \"pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"lun_lun\","
    "        \"yang_yang\""
    "      ]"
    "    }"
    "  }"
    "]";

  resourcesTry = Resources::parse(jsonString);
  ASSERT_SOME(resourcesTry);

  Resources pandaResources(resourcesTry.get());
  const Resource& pandas = *(pandaResources.begin());

  EXPECT_EQ(Value::SET, pandas.type());
  EXPECT_EQ(2, pandas.set().item_size());
  EXPECT_EQ("pandas", pandas.name());

  // Do not specify the ordering of the set's items, only check the values.
  if ("lun_lun" != pandas.set().item(0)) {
    EXPECT_EQ("yang_yang", pandas.set().item(0));
    EXPECT_EQ("lun_lun", pandas.set().item(1));
  } else {
    EXPECT_EQ("yang_yang", pandas.set().item(1));
  }

  jsonString =
    "[\n"
    "  {\n"
    "    \"name\": \"cpus\",\n"
    "    \"type\": \"SCALAR\",\n"
    "    \"scalar\": {\n"
    "      \"value\": 45.55\n"
    "    }\n"
    "  },\n"
    "  {\n"
    "    \"name\": \"ports\",\n"
    "    \"type\": \"RANGES\",\n"
    "    \"ranges\": {\n"
    "      \"range\": [\n"
    "        {\n"
    "          \"begin\": 10000,\n"
    "          \"end\": 20000\n"
    "        },\n"
    "        {\n"
    "          \"begin\": 30000,\n"
    "          \"end\": 50000\n"
    "        }\n"
    "      ]\n"
    "    }\n"
    "  },\n"
    "  {\n"
    "    \"name\": \"pandas\",\n"
    "    \"type\": \"SET\",\n"
    "    \"set\": {\n"
    "      \"item\": [\n"
    "        \"lun_lun\",\n"
    "        \"yang_yang\"\n"
    "      ]\n"
    "    }\n"
    "  }\n"
    "]";

  resourcesTry = Resources::parse(jsonString);
  ASSERT_SOME(resourcesTry);

  Resources r1(resourcesTry.get());

  Resources r2;
  r2 += cpus;
  r2 += ports;
  r2 += pandas;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, ParsingFromJSONWithRoles)
{
  string jsonString =
    "[\n"
    "  {\n"
    "    \"name\": \"cpus\",\n"
    "    \"type\": \"SCALAR\",\n"
    "    \"scalar\": {\n"
    "      \"value\": 45.55\n"
    "    },\n"
    "    \"reservations\": [\n"
    "      {\n"
    "        \"type\": \"STATIC\",\n"
    "        \"role\": \"role1\"\n"
    "      }\n"
    "    ]\n"
    "  }\n"
    "]";

  Try<Resources> resourcesTry = Resources::parse(jsonString);
  ASSERT_SOME(resourcesTry);

  Resources cpuResources(resourcesTry.get());
  const Resource& cpus = *(cpuResources.begin());

  ASSERT_EQ(Value::SCALAR, cpus.type());
  EXPECT_EQ(45.55, cpus.scalar().value());
  EXPECT_EQ("role1", Resources::reservationRole(cpus));

  jsonString =
    "[\n"
    "  {\n"
    "    \"name\": \"cpus\",\n"
    "    \"type\": \"SCALAR\",\n"
    "    \"scalar\": {\n"
    "      \"value\": 54.44\n"
    "    },\n"
    "    \"reservations\": [\n"
    "      {\n"
    "        \"type\": \"STATIC\",\n"
    "        \"role\": \"role2\"\n"
    "      }\n"
    "    ]\n"
    "  }\n"
    "]";

  resourcesTry = Resources::parse(jsonString);
  ASSERT_SOME(resourcesTry);

  Resources cpuResources2(resourcesTry.get());
  const Resource& cpus2 = *(cpuResources2.begin());

  Resources resources;
  resources += cpus2;
  resources += cpus;
  resources += cpus;

  EXPECT_TRUE(resources.contains(Resources(cpus)));
  EXPECT_EQ(145.54, resources.cpus().get());

  foreach (const Resource& resource, resources) {
    if (Resources::reservationRole(resource) == "role1") {
      EXPECT_EQ(91.1, resource.scalar().value());
    } else {
      EXPECT_EQ(54.44, resource.scalar().value());
    }
  }

  jsonString =
    "["
    "  {"
    "    \"name\": \"ports\","
    "    \"type\": \"RANGES\","
    "    \"ranges\": {"
    "      \"range\": ["
    "        {"
    "          \"begin\": 10000,"
    "          \"end\": 20000"
    "        },"
    "        {"
    "          \"begin\": 30000,"
    "          \"end\": 50000"
    "        }"
    "      ]"
    "    },"
    "    \"reservations\": [\n"
    "      {\n"
    "        \"type\": \"STATIC\",\n"
    "        \"role\": \"role1\"\n"
    "      }\n"
    "    ]\n"
    "  },"
    "  {"
    "    \"name\": \"pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"lun_lun\","
    "        \"yang_yang\""
    "      ]"
    "    },"
    "    \"reservations\": [\n"
    "      {\n"
    "        \"type\": \"STATIC\",\n"
    "        \"role\": \"panda_liason\"\n"
    "      }\n"
    "    ]\n"
    "  }"
    "]";

  resourcesTry = Resources::parse(jsonString);
  ASSERT_SOME(resourcesTry);

  resources = resourcesTry.get();

  foreach (const Resource& resource, resources) {
    if (Resources::reservationRole(resource) == "role1") {
      EXPECT_EQ(Value::RANGES, resource.type());
      EXPECT_EQ(2, resource.ranges().range_size());
    } else {
      EXPECT_EQ(Value::SET, resource.type());
      EXPECT_EQ(2, resource.set().item_size());
    }
  }
}


TEST(ResourcesTest, ParsingFromJSONError)
{
  // A single JSON object, not an array.
  string jsonString =
    "{"
    "  \"name\": \"sad_pandas\","
    "  \"type\": \"SET\","
    "  \"set\": {"
    "    \"item\": ["
    "      \"bai_yun\","
    "      \"gao_gao\""
    "    ]"
    "  }"
    "}";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Missing comma in Resource array.
  jsonString =
    "["
    "  {"
    "    \"name\": \"sad_pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"bai_yun\","
    "        \"gao_gao\""
    "      ]"
    "    }"
    "  }"
    "  {"
    "    \"name\": \"happy_pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"yun_zi\","
    "        \"xiao_liwu\""
    "      ]"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Missing comma in Set list.
  jsonString =
    "["
    "  {"
    "    \"name\": \"sad_pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"bai_yun\","
    "        \"gao_gao\""
    "      ]"
    "    }"
    "  },"
    "  {"
    "    \"name\": \"happy_pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"yun_zi\","
    "        \"xiao_liwu\""
    "        \"xi_lan\""
    "      ]"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // An array of arrays.
  jsonString =
    "["
    "  ["
    "    {"
    "      \"name\": \"sad_pandas\","
    "      \"type\": \"SET\","
    "      \"set\": {"
    "        \"item\": ["
    "          \"bai_yun\","
    "          \"gao_gao\""
    "        ]"
    "      }"
    "    },"
    "    {"
    "      \"name\": \"happy_pandas\","
    "      \"type\": \"SET\","
    "      \"set\": {"
    "        \"item\": ["
    "          \"yun_zi\","
    "          \"xiao_liwu\""
    "        ]"
    "      }"
    "    }"
    "  ]"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Missing array brackets.
  jsonString =
    "{"
    "  \"name\": \"sad_pandas\","
    "  \"type\": \"SET\","
    "  \"set\": {"
    "    \"item\": ["
    "      \"bai_yun\","
    "      \"gao_gao\""
    "    ]"
    "  }"
    "},"
    "{"
    "  \"name\": \"happy_pandas\","
    "  \"type\": \"SET\","
    "  \"set\": {"
    "    \"item\": ["
    "      \"yun_zi\","
    "      \"xiao_liwu\""
    "    ]"
    "  }"
    "}";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Contains extraneous text.
  jsonString =
    "["
    "  {"
    "    \"name\": \"sad_pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"bai_yun\","
    "        \"gao_gao\""
    "      ]"
    "    }"
    "  }"
    "]"
    "once upon a time there was a sad panda...";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Wrong type of bracket.
  jsonString =
    "["
    "  {"
    "    \"name\": \"pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"lun_lun\","
    "        \"yang_yang\""
    "      }"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Missing 'type' field.
  jsonString =
    "["
    "  {"
    "    \"name\": \"pandas\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"lun_lun\","
    "        \"yang_yang\""
    "      ]"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Empty name.
  jsonString =
    "["
    "  {"
    "    \"name\": \"\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"lun_lun\","
    "        \"yang_yang\""
    "      ]"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Unrecognized Resource 'type'.
  jsonString =
    "["
    "  {"
    "    \"name\": \"pandas\","
    "    \"type\": \"FLOATINGPOINT\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"lun_lun\","
    "        \"yang_yang\""
    "      ]"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Negative Resources.
  jsonString =
    "["
    "  {"
    "    \"name\": \"panda_power\","
    "    \"type\": \"SCALAR\","
    "    \"scalar\": {"
    "      \"value\": -1"
    "    }"
    "  },"
    "  {"
    "    \"name\": \"panda_window\","
    "    \"type\": \"RANGES\","
    "    \"ranges\": {"
    "      \"range\": []"
    "    },"
    "    \"role\": \"role1\""
    "  },"
    "  {"
    "    \"name\": \"actual_pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": []"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Resources with the same name but different types.
  jsonString =
    "["
    "  {"
    "    \"name\": \"happy_pandas\","
    "    \"type\": \"SCALAR\","
    "    \"scalar\": {"
    "      \"value\": 14"
    "    }"
    "  },"
    "  {"
    "    \"name\": \"happy_pandas\","
    "    \"type\": \"SET\","
    "    \"set\": {"
    "      \"item\": ["
    "        \"yun_zi\","
    "        \"xiao_liwu\","
    "        \"xi_lan\""
    "      ]"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Attempts to specify a persistent volume.
  jsonString =
    "["
    "  {"
    "    \"name\" : \"disk\","
    "    \"type\" : \"SCALAR\","
    "    \"scalar\" : {"
    "      \"value\" : 2048"
    "    },"
    "    \"disk\": {"
    "      \"persistence\": {"
    "        \"id\" : \"persistent_volume_1\""
    "      },"
    "      \"volume\" : {"
    "        \"container_path\" : \"/var/lib/mesos/persist\","
    "        \"mode\" : \"RW\""
    "      }"
    "    }"
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Attempts to specify a dynamic reservation.
  jsonString =
    "["
    "  {"
    "    \"name\" : \"disk\","
    "    \"type\" : \"SCALAR\","
    "    \"scalar\" : {"
    "      \"value\" : 2048"
    "    },"
    "    \"reservations\" : [{"
    "      \"role\": \"new_role\""
    "      \"principal\" : \"default_principal\""
    "    }],"
    "    \"role\": \"*\""
    "  }"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));

  // Attempts to specify revocable resources.
  jsonString =
    "[\n"
    "  {\n"
    "    \"name\": \"cpus\",\n"
    "    \"type\": \"SCALAR\",\n"
    "    \"scalar\": {\n"
    "      \"value\": 54.44\n"
    "    },\n"
    "    \"role\": \"role2\",\n"
    "    \"revocable\": {}\n"
    "  }\n"
    "]";

  EXPECT_ERROR(Resources::parse(jsonString));
}


TEST(ResourcesTest, Resources)
{
  Resources r = Resources::parse(
      "cpus:45.55;mem:1024;ports:[10000-20000, 30000-50000];disk:512").get();

  EXPECT_SOME(r.cpus());
  EXPECT_DOUBLE_EQ(45.55, r.cpus().get());
  EXPECT_SOME_EQ(Megabytes(1024), r.mem());
  EXPECT_SOME_EQ(Megabytes(512), r.disk());

  ASSERT_SOME(r.ports());

  ostringstream ports;
  ports << r.ports().get();

  EXPECT_EQ("[10000-20000, 30000-50000]", ports.str());

  r = Resources::parse("cpus:45.55;disk:512").get();

  EXPECT_SOME(r.cpus());
  EXPECT_DOUBLE_EQ(45.55, r.cpus().get());
  EXPECT_SOME_EQ(Megabytes(512), r.disk());
  EXPECT_NONE(r.mem());
  EXPECT_NONE(r.ports());
}


TEST(ResourcesTest, MoveConstruction)
{
  const Resources r = CHECK_NOTERROR(Resources::parse(
      "cpus:45.55;mem:1024;ports:[10000-20000, 30000-50000];disk:512"));

  // Move constructor for `Resources`.
  Resources r1 = r;
  Resources rr1{std::move(r1)};
  EXPECT_EQ(r, rr1);

  // Move constructor for `vector<Resource>`.
  vector<Resource> r2;
  foreach (const Resource& resource, r) {
    r2.push_back(resource);
  }
  Resources rr2{std::move(r2)};
  EXPECT_EQ(r, rr2);

  // Move constructor for `google::protobuf::RepeatedPtrField<Resource>`.
  google::protobuf::RepeatedPtrField<Resource> r3;
  foreach (const Resource& resource, r) {
    *r3.Add() = resource;
  }
  Resources rr3{std::move(r3)};
  EXPECT_EQ(r, rr3);

  // Move assignment for `Resources`.
  Resources r4 = r;
  Resources rr4;
  rr4 = std::move(r4);
  EXPECT_EQ(r, rr4);

  // Move constructor for `Resource`.
  const Resource resource = CHECK_NOTERROR(Resources::parse("cpu", "1.0", "*"));

  Resource r5 = resource;
  Resources rr5{std::move(r5)};
  EXPECT_EQ(Resources(resource), rr5);
}


TEST(ResourcesTest, Printing)
{
  Resources r = Resources::parse(
      "cpus:45.55;ports:[10000-20000, 30000-50000];disks:{sda1}").get();

  string output =
    "cpus:45.55; ports:[10000-20000, 30000-50000]; disks:{sda1}";

  ostringstream oss;
  oss << r;

  // TODO(benh): This test is a bit strict because it implies the
  // ordering of things (e.g., the ordering of resources and the
  // ordering of ranges). We should really just be checking for the
  // existence of certain substrings in the output.

  EXPECT_EQ(output, oss.str());
}


TEST(ResourcesTest, PrintingExtendedAttributes)
{
  Resource disk;
  disk.set_name("disk");
  disk.set_type(Value::SCALAR);
  disk.mutable_scalar()->set_value(1);

  // Standard resource.
  ostringstream stream;
  stream << disk;
  EXPECT_EQ("disk:1", stream.str());

  // Standard resource with role (statically reserved).
  stream.str("");
  disk.add_reservations()->CopyFrom(createStaticReservationInfo("alice"));
  stream << disk;
  EXPECT_EQ("disk(reservations: [(STATIC,alice)]):1", stream.str());

  // Allocated resource.
  stream.str("");
  disk.mutable_allocation_info()->set_role("role");
  stream << disk;
  EXPECT_EQ(
      "disk(allocated: role)(reservations: [(STATIC,alice)]):1", stream.str());
  disk.clear_allocation_info();

  // Standard revocable resource.
  stream.str("");
  disk.mutable_revocable();
  stream << disk;
  EXPECT_EQ("disk(reservations: [(STATIC,alice)]){REV}:1", stream.str());
  disk.clear_revocable();

  // Disk resource with persistent volume.
  stream.str("");
  disk.mutable_disk()->mutable_persistence()->set_id("hadoop");
  disk.mutable_disk()->mutable_volume()->set_container_path("/data");
  stream << disk;
  EXPECT_EQ(
      "disk(reservations: [(STATIC,alice)])[hadoop:/data]:1", stream.str());

  // Ensure {REV} comes after [disk].
  stream.str("");
  disk.mutable_revocable();
  stream << disk;
  EXPECT_EQ(
      "disk(reservations: [(STATIC,alice)])[hadoop:/data]{REV}:1",
      stream.str());
  disk.clear_revocable();

  // Disk resource with host path.
  stream.str("");
  disk.mutable_disk()->mutable_volume()->set_host_path("/hdfs");
  disk.mutable_disk()->mutable_volume()->set_mode(Volume::RW);
  stream << disk;
  EXPECT_EQ(
      "disk(reservations: [(STATIC,alice)])[hadoop:/hdfs:/data:rw]:1",
      stream.str());

  // Disk resource with MOUNT type.
  stream.str("");
  disk.mutable_disk()->mutable_source()->set_type(
      Resource::DiskInfo::Source::MOUNT);
  disk.mutable_disk()->mutable_source()->mutable_mount()->set_root("/mnt1");
  stream << disk;
  EXPECT_EQ(
      "disk(reservations: "
      "[(STATIC,alice)])[MOUNT:/mnt1,hadoop:/hdfs:/data:rw]:1",
      stream.str());
  disk.mutable_disk()->clear_source();

  // Disk resource with PATH type.
  stream.str("");
  disk.mutable_disk()->mutable_source()->set_type(
      Resource::DiskInfo::Source::PATH);
  disk.mutable_disk()->mutable_source()->mutable_path()->set_root("/mnt2");
  stream << disk;
  EXPECT_EQ(
      "disk(reservations: "
      "[(STATIC,alice)])[PATH:/mnt2,hadoop:/hdfs:/data:rw]:1",
      stream.str());
  disk.mutable_disk()->clear_source();

  // Disk resource with host path and dynamic reservation without labels.
  stream.str("");
  Resource::ReservationInfo* reservation = disk.add_reservations();
  reservation->CopyFrom(
      createDynamicReservationInfo("alice/refined", "hdfs-p"));
  stream << disk;
  EXPECT_EQ(
      "disk(reservations: [(STATIC,alice),(DYNAMIC,alice/refined,hdfs-p)])"
      "[hadoop:/hdfs:/data:rw]:1",
      stream.str());

  // Disk resource with host path and dynamic reservation with labels.
  stream.str("");
  Labels* labels = reservation->mutable_labels();
  labels->add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels->add_labels()->CopyFrom(createLabel("foo"));
  stream << disk;
  EXPECT_EQ(
      "disk(reservations: [(STATIC,alice),(DYNAMIC,alice/refined,hdfs-p,"
      "{foo: bar, foo})])[hadoop:/hdfs:/data:rw]:1",
      stream.str());
}


TEST(ResourcesTest, PrintingScalarPrecision)
{
  Resource scalar;
  scalar.set_name("cpus");
  scalar.set_type(Value::SCALAR);
  scalar.mutable_scalar()->set_value(1.234);

  // Three decimal digits of precision are supported.
  ostringstream stream;
  stream << scalar;
  EXPECT_EQ("cpus:1.234", stream.str());

  // Additional precision is discarded via rounding.
  scalar.mutable_scalar()->set_value(1.2345);
  stream.str("");
  stream << scalar;
  EXPECT_EQ("cpus:1.235", stream.str());

  scalar.mutable_scalar()->set_value(1.2344);
  stream.str("");
  stream << scalar;
  EXPECT_EQ("cpus:1.234", stream.str());

  // Trailing zeroes are not printed.
  scalar.mutable_scalar()->set_value(1.1);
  stream.str("");
  stream << scalar;
  EXPECT_EQ("cpus:1.1", stream.str());

  // Large integers are printed with all digits.
  scalar.mutable_scalar()->set_value(1000001);
  stream.str("");
  stream << scalar;
  EXPECT_EQ("cpus:1000001", stream.str());

  // Even larger value with precision in the fractional part limited
  // to 3 digits but full precision in the integral part preserved.
  scalar.mutable_scalar()->set_value(99999999999.9994);
  stream.str("");
  stream << scalar;
  EXPECT_EQ("cpus:99999999999.999", stream.str());
}


TEST(ResourcesTest, InitializedIsEmpty)
{
  Resources r;
  EXPECT_TRUE(r.empty());
}


TEST(ResourcesTest, BadResourcesNotAllocatable)
{
  Resource cpus;
  cpus.set_type(Value::SCALAR);
  cpus.mutable_scalar()->set_value(1);

  Resources r;
  r += cpus;

  EXPECT_TRUE(r.empty());

  cpus.set_name("cpus");
  cpus.mutable_scalar()->set_value(0);

  r += cpus;

  EXPECT_TRUE(r.empty());
}


TEST(ResourcesTest, ScalarEquals)
{
  Resource cpus = Resources::parse("cpus", "3", "*").get();
  Resource mem  = Resources::parse("mem", "3072", "*").get();

  Resources r1;
  r1 += cpus;
  r1 += mem;

  Resources r2;
  r2 += cpus;
  r2 += mem;

  EXPECT_FALSE(r1.empty());
  EXPECT_FALSE(r2.empty());
  EXPECT_EQ(r1, r2);

  Resources cpus1 = Resources::parse("cpus", "3", "role1").get();
  Resources cpus2 = Resources::parse("cpus", "3", "role2").get();

  EXPECT_NE(cpus1, cpus2);
}


TEST(ResourcesTest, ScalarSubset)
{
  Resource cpus1 = Resources::parse("cpus", "1", "*").get();
  Resource mem1  = Resources::parse("mem", "3072", "*").get();

  Resource cpus2 = Resources::parse("cpus", "1", "*").get();
  Resource mem2  = Resources::parse("mem", "4096", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  EXPECT_TRUE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));
}


TEST(ResourcesTest, ScalarSubset2)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "1", "role2").get();

  Resources r1;
  r1 += cpus1;

  Resources r2;
  r2 += cpus2;

  EXPECT_FALSE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));

  Resource cpus3 = Resources::parse("cpus", "3", "role1").get();

  Resources r3;
  r3 += cpus3;

  EXPECT_FALSE(r1.contains(r3));
  EXPECT_FALSE(r2.contains(r3));
  EXPECT_FALSE(r3.contains(r2));
  EXPECT_TRUE(r3.contains(r1));
}


TEST(ResourcesTest, ScalarAddition)
{
  Resource cpus1 = Resources::parse("cpus", "1", "*").get();
  Resource mem1 = Resources::parse("mem", "5", "*").get();

  Resource cpus2 = Resources::parse("cpus", "2", "*").get();
  Resource mem2 = Resources::parse("mem", "10", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  // Test +=(Resource&&).
  Resources r2;
  r2 += Resource(cpus2);
  r2 += Resource(mem2);

  Resources sum = r1 + r2;

  EXPECT_FALSE(sum.empty());
  EXPECT_EQ(2u, sum.size());
  EXPECT_EQ(3, sum.get<Value::Scalar>("cpus")->value());
  EXPECT_EQ(15, sum.get<Value::Scalar>("mem")->value());

  // Verify operator+ with rvalue references.
  Resources sum1 = Resources(r1) + r2;
  Resources sum2 = r1 + Resources(r2);
  Resources sum3 = Resources(r1) + Resources(r2);

  EXPECT_EQ(sum, sum1);
  EXPECT_EQ(sum, sum2);
  EXPECT_EQ(sum, sum3);

  Resources r = r1;
  r += r2;

  EXPECT_FALSE(r.empty());
  EXPECT_EQ(2u, r.size());
  EXPECT_EQ(3, r.get<Value::Scalar>("cpus")->value());
  EXPECT_EQ(15, r.get<Value::Scalar>("mem")->value());
}


TEST(ResourcesTest, ScalarAddition2)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "3", "role2").get();
  Resource cpus3 = Resources::parse("cpus", "5", "role1").get();

  Resources r1;
  r1 += cpus1;
  r1 += Resource(cpus2); // Test +=(Resource&&).

  Resources r2;
  r2 += cpus3;

  Resources sum = r1 + r2;

  EXPECT_FALSE(sum.empty());
  EXPECT_EQ(2u, sum.size());
  EXPECT_EQ(9, sum.cpus().get());
  EXPECT_EQ(sum, Resources::parse("cpus(role1):6;cpus(role2):3").get());

  // Verify operator+ with rvalue references.
  Resources sum1 = Resources(r1) + r2;
  Resources sum2 = r1 + Resources(r2);
  Resources sum3 = Resources(r1) + Resources(r2);

  EXPECT_EQ(sum, sum1);
  EXPECT_EQ(sum, sum2);
  EXPECT_EQ(sum, sum3);
}


TEST(ResourcesTest, ScalarSubtraction)
{
  Resource cpus1 = Resources::parse("cpus", "50", "*").get();
  Resource mem1 = Resources::parse("mem", "4096", "*").get();

  Resource cpus2 = Resources::parse("cpus", "0.5", "*").get();
  Resource mem2 = Resources::parse("mem", "1024", "*").get();

  Resources r1;
  r1 += cpus1;
  r1 += mem1;

  Resources r2;
  r2 += cpus2;
  r2 += mem2;

  Resources diff = r1 - r2;

  EXPECT_FALSE(diff.empty());
  EXPECT_EQ(49.5, diff.get<Value::Scalar>("cpus")->value());
  EXPECT_EQ(3072, diff.get<Value::Scalar>("mem")->value());

  Resources r = r1;
  r -= r2;

  EXPECT_EQ(49.5, diff.get<Value::Scalar>("cpus")->value());
  EXPECT_EQ(3072, diff.get<Value::Scalar>("mem")->value());

  r = r1;
  r -= r1;

  EXPECT_TRUE(r.empty());
}


TEST(ResourcesTest, ScalarSubtraction2)
{
  Resource cpus1 = Resources::parse("cpus", "5", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "3", "role2").get();
  Resource cpus3 = Resources::parse("cpus", "1", "role1").get();

  Resources r1;
  r1 += cpus1;
  r1 += cpus2;

  Resources r2;
  r2 += cpus3;

  Resources diff = r1 - r2;

  EXPECT_FALSE(diff.empty());
  EXPECT_EQ(2u, diff.size());
  EXPECT_EQ(7, diff.cpus().get());
  EXPECT_EQ(diff, Resources::parse("cpus(role1):4;cpus(role2):3").get());
}


TEST(ResourcesTest, RangesEquals)
{
  Resource ports1 = Resources::parse(
      "ports", "[20-40]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[20-30, 31-39, 40-40]", "*").get();

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, RangesSubset)
{
  Resource ports1 = Resources::parse("ports", "[2-2, 4-5]", "*").get();
  Resource ports2 = Resources::parse("ports", "[1-10]", "*").get();
  Resource ports3 = Resources::parse("ports", "[2-3]", "*").get();
  Resource ports4 = Resources::parse("ports", "[1-2, 4-6]", "*").get();
  Resource ports5 = Resources::parse("ports", "[1-4, 5-5]", "*").get();

  EXPECT_EQ(2, ports1.ranges().range_size());
  EXPECT_EQ(1, ports2.ranges().range_size());
  EXPECT_EQ(1, ports3.ranges().range_size());
  EXPECT_EQ(2, ports4.ranges().range_size());
  EXPECT_EQ(1, ports5.ranges().range_size());

  Resources r1;
  r1 += ports1;

  Resources r2;
  r2 += ports2;

  Resources r3;
  r3 += ports3;

  Resources r4;
  r4 += ports4;

  Resources r5;
  r5 += ports5;

  EXPECT_TRUE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));
  EXPECT_FALSE(r3.contains(r1));
  EXPECT_FALSE(r1.contains(r3));
  EXPECT_TRUE(r2.contains(r3));
  EXPECT_FALSE(r3.contains(r2));
  EXPECT_TRUE(r4.contains(r1));
  EXPECT_TRUE(r2.contains(r4));
  EXPECT_TRUE(r5.contains(r1));
  EXPECT_FALSE(r1.contains(r5));
}


TEST(ResourcesTest, RangesAddition)
{
  Resource ports1 = Resources::parse(
      "ports", "[20000-40000]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[30000-50000, 10000-20000]", "*").get();

  Resources r;
  r += ports1;
  r += Resource(ports2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[10000-50000]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesAddition2)
{
  Resource ports1 = Resources::parse("ports", "[1-10, 5-30, 50-60]", "*").get();
  Resource ports2 = Resources::parse("ports", "[1-65, 70-80]", "*").get();

  Resources r;
  r += ports1;
  r += Resource(ports2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-65, 70-80]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesAddition3)
{
  Resource ports1 = Resources::parse("ports", "[1-2]", "*").get();
  Resource ports2 = Resources::parse("ports", "[3-4]", "*").get();
  Resource ports3 = Resources::parse("ports", "[7-8]", "*").get();
  Resource ports4 = Resources::parse("ports", "[5-6]", "*").get();

  Resources r1;
  r1 += ports1;
  r1 += Resource(ports2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r1.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-4]")->ranges(),
      r1.get<Value::Ranges>("ports"));

  Resources r2;
  r2 += ports3;
  r2 += Resource(ports4); // Test operator+=(Resource&&).

  EXPECT_FALSE(r2.empty());

  EXPECT_SOME_EQ(
      values::parse("[5-8]")->ranges(),
      r2.get<Value::Ranges>("ports"));

  r2 += r1;

  EXPECT_FALSE(r2.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-8]")->ranges(),
      r2.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesAddition4)
{
  Resource ports1 = Resources::parse(
      "ports", "[1-4, 9-10, 20-22, 26-30]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[5-8, 23-25]", "*").get();

  Resources r;
  r += ports1;
  r += Resource(ports2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-10, 20-30]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction)
{
  Resource ports1 = Resources::parse(
      "ports", "[20000-40000]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[10000-20000, 30000-50000]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[20001-29999]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction1)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]", "*").get();
  Resource ports2 = Resources::parse("ports", "[50000-50001]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[50002-60000]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction2)
{
  Resource ports1 = Resources::parse("ports", "[50000-60000]", "*").get();
  Resource ports2 = Resources::parse("ports", "[50000-50000]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[50001-60000]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction3)
{
  Resources resources = Resources::parse("ports:[50000-60000]").get();

  Resources resourcesOffered = Resources::parse("").get();
  Resources resourcesInUse = Resources::parse("ports:[50000-50001]").get();

  Resources resourcesFree = resources - (resourcesOffered + resourcesInUse);

  EXPECT_FALSE(resourcesFree.empty());

  EXPECT_SOME_EQ(
      values::parse("[50002-60000]")->ranges(),
      resourcesFree.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction4)
{
  Resources resources = Resources::parse("ports:[50000-60000]").get();

  Resources resourcesOffered;
  resourcesOffered += resources;
  resourcesOffered -= resources;

  EXPECT_TRUE(resourcesOffered.empty());
  EXPECT_NONE(resourcesOffered.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction5)
{
  Resource ports1 = Resources::parse(
      "ports", "[1-10, 20-30, 40-50]", "*").get();

  Resource ports2 = Resources::parse(
      "ports", "[2-9, 15-45, 48-50]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-1, 10-10, 46-47]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, RangesSubtraction6)
{
  Resource ports1 = Resources::parse("ports", "[1-10]", "*").get();
  Resource ports2 = Resources::parse("ports", "[11-20]", "*").get();

  Resources r;
  r += ports1;
  r -= ports2;

  EXPECT_FALSE(r.empty());

  EXPECT_SOME_EQ(
      values::parse("[1-10]")->ranges(),
      r.get<Value::Ranges>("ports"));
}


TEST(ResourcesTest, SetEquals)
{
  Resource disks = Resources::parse("disks", "{sda1}", "*").get();

  Resources r1;
  r1 += disks;

  Resources r2;
  r2 += disks;

  EXPECT_EQ(r1, r2);
}


TEST(ResourcesTest, SetSubset)
{
  Resource disks1 = Resources::parse(
      "disks", "{sda1,sda2}", "*").get();

  Resource disks2 = Resources::parse(
      "disks", "{sda1,sda3,sda4,sda2}", "*").get();

  Resources r1;
  r1 += disks1;

  Resources r2;
  r2 += disks2;

  EXPECT_FALSE(r1.empty());
  EXPECT_FALSE(r2.empty());
  EXPECT_TRUE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));
}


TEST(ResourcesTest, SetAddition)
{
  Resource disks1 = Resources::parse(
      "disks", "{sda1,sda2,sda3}", "*").get();

  Resource disks2 = Resources::parse(
      "disks", "{sda1,sda2,sda3,sda4}", "*").get();

  Resources r;
  r += disks1;
  r += Resource(disks2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r.empty());

  Option<Value::Set> set = r.get<Value::Set>("disks");

  ASSERT_SOME(set);
  EXPECT_EQ(4, set->item_size());
}


TEST(ResourcesTest, SetSubtraction)
{
  Resource disks1 = Resources::parse(
      "disks", "{sda1,sda2,sda3,sda4}", "*").get();

  Resource disks2 = Resources::parse(
      "disks", "{sda2,sda3,sda4}", "*").get();

  Resources r;
  r += disks1;
  r -= disks2;

  EXPECT_FALSE(r.empty());

  Option<Value::Set> set = r.get<Value::Set>("disks");

  ASSERT_SOME(set);
  EXPECT_EQ(1, set->item_size());
  EXPECT_EQ("sda1", set->item(0));
}


TEST(ResourcesTest, EmptyUnequal)
{
  Resources empty = Resources::parse("").get();
  Resources cpus2 = Resources::parse("cpus:2").get();

  EXPECT_FALSE(empty == cpus2);
}


TEST(ResourcesTest, Reservations)
{
  Resources unreserved = Resources::parse(
      "cpus:1;mem:2;disk:4").get();
  Resources role1 = Resources::parse(
      "cpus(role1):2;mem(role1):4;disk(role1):8").get();
  Resources role2 = Resources::parse(
      "cpus(role2):4;mem(role2):8;disk(role2):6").get();

  Resources resources = unreserved + role1 + role2;

  hashmap<string, Resources> reserved = resources.reservations();

  EXPECT_EQ(2u, reserved.size());
  EXPECT_EQ(role1, reserved["role1"]);
  EXPECT_EQ(role2, reserved["role2"]);

  EXPECT_EQ(role1, resources.reserved("role1"));
  EXPECT_EQ(role2, resources.reserved("role2"));

  // Resources with role "*" are not considered reserved.
  EXPECT_EQ(Resources(), resources.reserved("*"));

  EXPECT_EQ(unreserved, resources.unreserved());

  EXPECT_EQ(role1 + role2, resources.reserved());
}


// This test verifies that we can get all resources allocatable to a role,
// including reservations of itself, its ancestors, and unreserved resources.
TEST(ResourcesTest, HierarchicalReservations)
{
  Resources unreserved = Resources::parse(
      "cpus:1;mem:2;disk:4").get();
  Resources grandfather = Resources::parse(
      "cpus(a):2;mem(a):4;disk(a):8").get();
  Resources father = Resources::parse(
      "cpus(a/bx):4;mem(a/bx):8;disk(a/bx):16").get();
  Resources uncle = Resources::parse(
      "cpus(a/b):4;mem(a/b):8;disk(a/b):16").get();
  Resources child = Resources::parse(
      "cpus(a/bx/c):8;mem(a/bx/c):16;disk(a/bx/c):32").get();

  Resources family = unreserved + grandfather + father + uncle + child;

  EXPECT_EQ(unreserved, family.allocatableTo("*"));

  EXPECT_EQ(grandfather + unreserved, family.allocatableTo("a"));

  EXPECT_EQ(grandfather + father + unreserved, family.allocatableTo("a/bx"));

  EXPECT_EQ(grandfather + father + child + unreserved,
            family.allocatableTo("a/bx/c"));
}


TEST(ResourceProviderIDTest, Addition)
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value("RESOURCE_PROVIDER_ID");

  Resource cpus = Resources::parse("cpus", "4", "*").get();
  cpus.mutable_provider_id()->CopyFrom(resourceProviderId);

  Resource disk1 = createDiskResource("1", "*", None(), None());

  Resource disk2 = disk1;
  disk2.mutable_provider_id()->CopyFrom(resourceProviderId);

  Resources r1;
  r1 += cpus;
  r1 += Resource(disk2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r1.empty());
  EXPECT_EQ(2u, r1.size());
  EXPECT_TRUE(r1.contains(cpus));
  EXPECT_TRUE(r1.contains(disk2));
  EXPECT_FALSE(r1.contains(disk1));
  EXPECT_EQ(4, r1.get<Value::Scalar>("cpus")->value());
  EXPECT_EQ(1, r1.get<Value::Scalar>("disk")->value());

  Resources r2;
  r2 += disk2;
  r2 += Resource(disk2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r2.empty());
  EXPECT_EQ(1u, r2.size());
  EXPECT_TRUE(r2.contains(disk2));
  EXPECT_EQ(2, r2.get<Value::Scalar>("disk")->value());

  EXPECT_EQ(Resources(disk2) + disk2, r2);

  Resources r3;
  r3 += disk1;
  r3 += Resources(disk2); // Test operator+=(Resource&&).

  EXPECT_FALSE(r3.empty());
  EXPECT_EQ(2u, r3.size());
  EXPECT_TRUE(r3.contains(disk1));
  EXPECT_TRUE(r3.contains(disk2));
  EXPECT_EQ(2, r3.get<Value::Scalar>("disk")->value());

  EXPECT_EQ(Resources(disk1) + disk2, r3);
}


TEST(ResourceProviderIDTest, Subtraction)
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value("RESOURCE_PROVIDER_ID");

  Resource cpus = Resources::parse("cpus", "4", "*").get();
  cpus.mutable_provider_id()->CopyFrom(resourceProviderId);

  Resource disk1 = createDiskResource("1", "*", None(), None());

  Resource disk2 = disk1;
  disk2.mutable_provider_id()->CopyFrom(resourceProviderId);

  ASSERT_TRUE(Resources(cpus).contains(cpus));
  EXPECT_TRUE((Resources(cpus) - cpus).empty());

  EXPECT_FALSE(Resources(cpus).contains(disk1));
  EXPECT_FALSE(Resources(cpus).contains(disk2));

  Resources r0;
  r0 += cpus;
  r0 += disk1;

  ASSERT_TRUE(r0.contains(cpus));
  ASSERT_TRUE(r0.contains(disk1));
  EXPECT_EQ(Resources(cpus), r0 - disk1);
  EXPECT_EQ(Resources(disk1), r0 - cpus);

  Resources r1;
  r1 += cpus;
  r1 += disk2;

  ASSERT_TRUE(r1.contains(cpus));
  ASSERT_TRUE(r1.contains(disk2));
  EXPECT_EQ(Resources(cpus), r1 - disk2);
  EXPECT_EQ(Resources(disk2), r1 - cpus);

  Resources r2;
  r2 += disk2;
  r2 += disk2;

  ASSERT_TRUE(r2.contains(disk2));
  EXPECT_EQ(Resources(disk2), r2 - disk2);
}


TEST(ResourceProviderIDTest, Equals)
{
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value("RESOURCE_PROVIDER_ID");

  Resource cpus = Resources::parse("cpus", "1", "*").get();
  cpus.mutable_provider_id()->CopyFrom(resourceProviderId);

  Resource disk1 = createDiskResource("1", "*", None(), None());

  Resource disk2 = disk1;
  disk2.mutable_provider_id()->CopyFrom(resourceProviderId);

  Resources r1 = cpus;
  Resources r2 = disk1;
  Resources r3 = disk2;
  Resources r4 = r1 + disk1;
  Resources r5 = r1 + disk2;

  EXPECT_EQ(r1, r1);
  EXPECT_NE(r1, r2);
  EXPECT_NE(r1, r3);
  EXPECT_NE(r2, r3);
  EXPECT_EQ(r2, r2);
  EXPECT_EQ(r3, r3);

  EXPECT_NE(r4, r5);
  EXPECT_EQ(r4, r4);
  EXPECT_EQ(r5, r5);
}


TEST(ResourceProviderIDTest, Contains) {
  ResourceProviderID resourceProviderId;
  resourceProviderId.set_value("RESOURCE_PROVIDER_ID");

  Resource cpus = Resources::parse("cpus", "1", "*").get();

  Resource disk1 = createDiskResource("1", "*", None(), None());

  Resource disk2 = disk1;
  disk2.mutable_provider_id()->CopyFrom(resourceProviderId);

  Resources r1 = disk1;
  Resources r2 = disk2;
  Resources r3 = Resources(cpus) + disk1;
  Resources r4 = Resources(cpus) + disk2;

  EXPECT_FALSE(r1.contains(r2));
  EXPECT_FALSE(r2.contains(r1));
  EXPECT_TRUE(r2.contains(r2));

  EXPECT_TRUE(r3.contains(r1));
  EXPECT_FALSE(r3.contains(r2));
  EXPECT_FALSE(r4.contains(r1));
  EXPECT_TRUE(r4.contains(r2));

  EXPECT_FALSE(r3.contains(r4));
  EXPECT_TRUE(r4.contains(r4));
}


TEST(ResourcesTest, ToUnreserved)
{
  Resource cpus1 = Resources::parse("cpus", "1", "role1").get();
  Resource cpus2 = Resources::parse("cpus", "2", "role2").get();
  Resource mem1 = Resources::parse("mem", "5", "role1").get();

  Resources r;
  r += cpus1;
  r += cpus2;
  r += mem1;

  EXPECT_EQ(r.toUnreserved(), Resources::parse("cpus:3;mem:5").get());
}


TEST(ResourcesTest, Find)
{
  Resources resources1 = Resources::parse(
      "cpus(role1):2;mem(role1):10;cpus:4;mem:20").get();

  Resources targets1 = Resources::parse(
      "cpus(role1):3;mem(role1):15").get();

  EXPECT_SOME_EQ(
      Resources::parse("cpus(role1):2;mem(role1):10;cpus:1;mem:5").get(),
      resources1.find(targets1));

  Resources resources2 = Resources::parse(
      "cpus(role1):1;mem(role1):5;cpus(role2):2;"
      "mem(role2):8;cpus:1;mem:7").get();

  Resources targets2 = Resources::parse(
      "cpus(role1):3;mem(role1):15").get();

  EXPECT_SOME_EQ(
      Resources::parse(
        "cpus(role1):1;mem(role1):5;cpus:1;mem:7;"
        "cpus(role2):1;mem(role2):3").get(),
      resources2.find(targets2));

  Resources resources3 = Resources::parse(
      "cpus(role1):5;mem(role1):5;cpus:5;mem:5").get();

  Resources targets3 = Resources::parse("cpus:6;mem:6").get();

  EXPECT_SOME_EQ(
      Resources::parse("cpus:5;mem:5;cpus(role1):1;mem(role1):1").get(),
      resources3.find(targets3));

  Resources resources4 = Resources::parse("cpus(role1):1;mem(role1):1").get();
  Resources targets4 = Resources::parse("cpus(role1):2;mem(role1):2").get();

  EXPECT_NONE(resources4.find(targets4));
}


// This test verifies the correctness of shrinking resources to the target
// resource quantities.
TEST(ResourcesTest, ShrinkToQuantities)
{
  auto shrink = [](const Resources& resources, const string& quantitiesString) {
    ResourceQuantities quantities =
      CHECK_NOTERROR(ResourceQuantities::fromString(quantitiesString));

    return shrinkResources(resources, quantities);
  };

  // Emptiness.
  Resources empty;
  EXPECT_EQ(empty, shrink(empty, ""));
  EXPECT_EQ(empty, shrink(empty, "cpus:1"));

  // Simple shrink.
  Resources cpu1 = CHECK_NOTERROR(Resources::parse("cpus:1"));
  EXPECT_EQ(empty, shrink(cpu1, ""));

  Resources cpu2 = CHECK_NOTERROR(Resources::parse("cpus:2"));
  EXPECT_EQ(cpu1, shrink(cpu2, "cpus:1"));

  // Multiple resources.
  Resources cpu2mem20 = CHECK_NOTERROR(Resources::parse("cpus:2;mem:20"));
  Resources cpu1mem10 = CHECK_NOTERROR(Resources::parse("cpus:1;mem:10"));
  EXPECT_EQ(cpu1mem10, shrink(cpu2mem20, "cpus:1;mem:10"));
  EXPECT_EQ(cpu1mem10, shrink(cpu2mem20, "cpus:1;mem:10;disk:10"));
  EXPECT_EQ(cpu1, shrink(cpu2mem20, "cpus:1"));

  // Non-choppable resources.
  Resources mountDisk20 = createDiskResource(
      "20", "role", None(), None(), createDiskSourceMount("mnt"));
  EXPECT_EQ(Resources(), shrink(mountDisk20, "disk:10"));

  // Random chopping.
  //
  // We construct 20 disk resources consisted of 10 regular disk and
  // 10 mount disk. A chopping of 10 disk should make a random choice
  // of picking either the regular disk or the mount disk.
  Resources regularDisk10 = CHECK_NOTERROR(Resources::parse("disk:10"));
  Resources mountDisk10 = createDiskResource(
      "10", "role", None(), None(), createDiskSourceMount("mnt"));
  Resources combinedDisk20 = regularDisk10 + mountDisk10;

  // A chopping of 10 disk could return either 10 regular disk or
  // 10 mount disk.
  Resources shrunk10 = shrink(combinedDisk20, "disk:10");

  // Repeat the same chopping for 1000 times, expecting the chopping
  // result to be different from the first time at least once.
  const size_t count = 1000u;
  bool atLeastDifferentOnce = false;

  for (size_t i = 0; i < count; i++) {
    if (shrink(combinedDisk20, "disk:10") != shrunk10) {
      atLeastDifferentOnce = true;
      break;
    }
  }
  CHECK(atLeastDifferentOnce);
}


// This test verifies the correctness of shrinking resources to the target
// resource limits.
TEST(ResourcesTest, ShrinkToLimits)
{
  auto shrink = [](const Resources& resources, const string& limitsString) {
    ResourceLimits limits =
      CHECK_NOTERROR(ResourceLimits::fromString(limitsString));

    return shrinkResources(resources, limits);
  };

  // Emptiness.
  Resources empty;
  EXPECT_EQ(empty, shrink(empty, ""));
  EXPECT_EQ(empty, shrink(empty, "cpus:1"));

  // Simple shrink.
  Resources cpu1 = CHECK_NOTERROR(Resources::parse("cpus:1"));
  EXPECT_EQ(cpu1, shrink(cpu1, ""));

  Resources cpu2 = CHECK_NOTERROR(Resources::parse("cpus:2"));
  EXPECT_EQ(cpu1, shrink(cpu2, "cpus:1"));

  // Multiple resources.
  Resources cpu2mem20 = CHECK_NOTERROR(Resources::parse("cpus:2;mem:20"));
  Resources cpu1mem10 = CHECK_NOTERROR(Resources::parse("cpus:1;mem:10"));
  EXPECT_EQ(cpu1mem10, shrink(cpu2mem20, "cpus:1;mem:10"));
  EXPECT_EQ(cpu1mem10, shrink(cpu2mem20, "cpus:1;mem:10;disk:10"));

  Resources cpu1mem20 = CHECK_NOTERROR(Resources::parse("cpus:1;mem:20"));
  EXPECT_EQ(cpu1mem20, shrink(cpu2mem20, "cpus:1"));

  // Non-choppable resources.
  Resources mountDisk20 = createDiskResource(
      "20", "role", None(), None(), createDiskSourceMount("mnt"));
  EXPECT_EQ(Resources(), shrink(mountDisk20, "disk:10"));

  // Random chopping.
  //
  // We construct 20 disk resources consisted of 10 regular disk and
  // 10 mount disk. A chopping of 10 disk should make a random choice
  // of picking either the regular disk or the mount disk.
  Resources regularDisk10 = CHECK_NOTERROR(Resources::parse("disk:10"));
  Resources mountDisk10 = createDiskResource(
      "10", "role", None(), None(), createDiskSourceMount("mnt"));
  Resources combinedDisk20 = regularDisk10 + mountDisk10;

  // A chopping of 10 disk could return either 10 regular disk or
  // 10 mount disk.
  Resources shrunk10 = shrink(combinedDisk20, "disk:10");

  // Repeat the same chopping for 1000 times, expecting the chopping
  // result to be different from the first time at least once.
  const size_t count = 1000u;
  bool atLeastDifferentOnce = false;

  for (size_t i = 0; i < count; i++) {
    if (shrink(combinedDisk20, "disk:10") != shrunk10) {
      atLeastDifferentOnce = true;
      break;
    }
  }
  CHECK(atLeastDifferentOnce);
}


// This test verifies that we can filter resources of a given name
// from Resources.
TEST(ResourcesTest, Get)
{
  Resources cpus = Resources::parse("cpus(role1):2;cpus:4").get();
  Resources mem = Resources::parse("mem(role1):10;mem:10").get();

  // Filter "cpus" resources.
  EXPECT_EQ(cpus, (cpus + mem).get("cpus"));

  // Filter "mem" resources.
  EXPECT_EQ(mem, (cpus + mem).get("mem"));
}


// This test verifies that we can get the set of unique names from
// Resources.
TEST(ResourcesTest, Names)
{
  Resources resources =
    Resources::parse("cpus(role1):2;cpus:4;mem(role1):10;mem:10").get();

  set<string> names = {"cpus", "mem"};
  ASSERT_EQ(names, resources.names());
}


TEST(ResourcesTest, Types)
{
  Resources resources =
    Resources::parse("cpus(role1):2;cpus:4;ports:[1-10];ports:[11-20]").get();

  map<string, Value_Type> types{
    {"cpus", Value::SCALAR},
    {"ports", Value::RANGES}
  };
  ASSERT_EQ(types, resources.types());
}


TEST(ResourcesTest, PrecisionSimple)
{
  Resources cpu = Resources::parse("cpus:1.001").get();
  EXPECT_EQ(1.001, cpu.cpus().get());

  Resources r1 = cpu + cpu + cpu - cpu - cpu;

  EXPECT_EQ(cpu, r1);
  EXPECT_EQ(1.001, r1.cpus().get());

  Resources zero = Resources::parse("cpus:0").get();

  EXPECT_EQ(cpu, cpu - zero);
  EXPECT_EQ(cpu, cpu + zero);
}


TEST(ResourcesTest, PrecisionManyOps)
{
  Resources start = Resources::parse("cpus:1.001").get();
  Resources current = start;
  Resources next;

  for (int i = 0; i < 2500; i++) {
    next = current + current + current - current - current;
    EXPECT_EQ(1.001, next.cpus().get());
    EXPECT_EQ(current, next);
    EXPECT_EQ(start, next);
    current = next;
  }
}


TEST(ResourcesTest, PrecisionManyConsecutiveOps)
{
  Resources start = Resources::parse("cpus:1.001").get();
  Resources increment = start;
  Resources current = start;

  for (int i = 0; i < 100000; i++) {
    current += increment;
  }

  for (int i = 0; i < 100000; i++) {
    current -= increment;
  }

  EXPECT_EQ(start, current);
}


TEST(ResourcesTest, PrecisionLost)
{
  Resources cpu = Resources::parse("cpus:1.5011").get();
  EXPECT_EQ(1.501, cpu.cpus().get());

  Resources r1 = cpu + cpu + cpu - cpu - cpu;

  EXPECT_EQ(cpu, r1);
  EXPECT_EQ(1.501, r1.cpus().get());
}


TEST(ResourcesTest, PrecisionRounding)
{
  // Round up (away from zero) at the half-way point.
  Resources cpu = Resources::parse("cpus:1.5015").get();
  EXPECT_EQ(1.502, cpu.cpus().get());

  Resources r1 = cpu + cpu + cpu - cpu - cpu;

  EXPECT_EQ(cpu, r1);
  EXPECT_EQ(1.502, r1.cpus().get());
}


TEST(ResourcesTest, VerySmallValue)
{
  Try<Resources> resources = Resources::parse("cpus:0.00001");
  EXPECT_ERROR(resources);
}


TEST(ResourcesTest, AbsentResources)
{
  Try<Resources> resources = Resources::parse("gpus:0");
  ASSERT_SOME(resources);

  EXPECT_EQ(0u, resources->size());
}


TEST(ResourcesTest, ContainsResourceQuantities)
{
  auto resources = [](const string& s) {
    return CHECK_NOTERROR(Resources::parse(s));
  };

  auto quantities = [](const string& s) {
      return CHECK_NOTERROR(ResourceQuantities::fromString(s));
  };

  // Empty case tests.

  Resources emptyResources;
  ResourceQuantities emptyQuantities;

  EXPECT_TRUE(emptyResources.contains(emptyQuantities));
  EXPECT_FALSE(emptyResources.contains(quantities("cpus:1")));
  EXPECT_TRUE(resources("cpus:1").contains(emptyQuantities));

  // Single scalar resource tests.

  EXPECT_TRUE(resources("cpus:2").contains(quantities("cpus:1")));

  EXPECT_TRUE(resources("cpus:1").contains(quantities("cpus:1")));

  EXPECT_FALSE(resources("cpus:0.5").contains(quantities("cpus:1")));

  // Single range resource tests.

  EXPECT_TRUE(resources("ports:[1-3]").contains(quantities("ports:2")));

  EXPECT_TRUE(resources("ports:[1-2]").contains(quantities("ports:2")));

  EXPECT_FALSE(resources("ports:[1-1]").contains(quantities("ports:2")));

  // Single set resources tests.

  EXPECT_TRUE(resources("features:{a,b,c}").contains(quantities("features:2")));

  EXPECT_TRUE(resources("features:{a,b}").contains(quantities("features:2")));

  EXPECT_FALSE(resources("features:{a}").contains(quantities("features:2")));

  // Multiple resources tests.

  EXPECT_TRUE(resources("cpus:3;ports:[1-3];features:{a,b,c};mem:10")
                .contains(quantities("cpus:3;ports:3;features:3")));

  EXPECT_TRUE(resources("cpus:3;ports:[1-3];features:{a,b,c}")
                .contains(quantities("cpus:3;ports:3;features:3")));

  EXPECT_FALSE(resources("cpus:1;ports:[1-3];features:{a,b,c}")
                 .contains(quantities("cpus:3;ports:3;features:3")));

  EXPECT_FALSE(resources("cpus:3;ports:[1-3]")
                 .contains(quantities("cpus:3;ports:3;features:3")));

  // Duplicate names.

  EXPECT_FALSE(resources("cpus(role1):2").contains(quantities("cpus:3")));

  EXPECT_TRUE(resources("cpus(role1):2;cpus:1").contains(quantities("cpus:3")));

  Resource::ReservationInfo reservation =
    createDynamicReservationInfo("role", "principal");
  Resources resources_ = createReservedResource("ports", "[1-10]", reservation);

  EXPECT_FALSE(resources_.contains(quantities("ports:12")));

  resources_ +=
    CHECK_NOTERROR(Resources::parse("ports:[20-25]")); // 15 ports in total.

  EXPECT_TRUE(resources_.contains(quantities("ports:12")));

  resources_ = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      "principal1",
      true); // Shared.

  EXPECT_FALSE(resources_.contains(quantities("disk:128")));

  resources_ +=
    CHECK_NOTERROR(Resources::parse("disk:64")); // 128M disk in total.

  EXPECT_TRUE(resources_.contains(quantities("disk:128")));
}


TEST(ReservedResourcesTest, Validation)
{
  // Unreserved.
  EXPECT_NONE(Resources::validate(createReservedResource("cpus", "8")));

  // Statically reserved to "role".
  EXPECT_NONE(Resources::validate(createReservedResource(
      "cpus", "8", createStaticReservationInfo("role"))));

  // Dynamically reserved without labels.
  EXPECT_NONE(Resources::validate(createReservedResource(
      "cpus", "8", createDynamicReservationInfo("role", "principal2"))));

  // Dynamically reserved with labels.
  Labels labels;
  labels.add_labels()->CopyFrom(createLabel("foo", "bar"));
  EXPECT_NONE(Resources::validate(createReservedResource(
      "cpus",
      "8",
      createDynamicReservationInfo("role", "principal2", labels))));
}


TEST(ReservedResourcesTest, Equals)
{
  Labels labels1;
  labels1.add_labels()->CopyFrom(createLabel("foo", "bar"));

  Labels labels2;
  labels2.add_labels()->CopyFrom(createLabel("foo", "baz"));

  vector<Resources> unique = {
    // Unreserved.
    createReservedResource("cpus", "8"),
    // Statically reserved for role.
    createReservedResource("cpus", "8", createStaticReservationInfo("role1")),
    createReservedResource("cpus", "8", createStaticReservationInfo("role2")),
    // Dynamically reserved for role.
    createReservedResource(
        "cpus", "8", createDynamicReservationInfo("role1", "principal1")),
    createReservedResource(
        "cpus", "8", createDynamicReservationInfo("role1", "principal2")),
    createReservedResource(
        "cpus", "8", createDynamicReservationInfo("role2", "principal1")),
    createReservedResource(
        "cpus", "8", createDynamicReservationInfo("role2", "principal2")),
    // Dynamically reserved with labels.
    createReservedResource(
        "cpus",
        "8",
        createDynamicReservationInfo("role1", "principal2", labels1)),
    createReservedResource(
        "cpus",
        "8",
        createDynamicReservationInfo("role1", "principal2", labels2))};

  // Test that all resources in 'unique' are considered different.
  foreach (const Resources& left, unique) {
    foreach (const Resources& right, unique) {
      if (&left == &right) {
        continue;
      }
      EXPECT_NE(left, right);
    }
  }
}


TEST(ReservedResourcesTest, AdditionStaticallyReserved)
{
  Resources left =
    createReservedResource("cpus", "8", createStaticReservationInfo("role"));
  Resources right =
    createReservedResource("cpus", "4", createStaticReservationInfo("role"));
  Resources expected =
    createReservedResource("cpus", "12", createStaticReservationInfo("role"));

  EXPECT_EQ(expected, left + right);

  // Test operator+ with rvalue references.
  EXPECT_EQ(expected, Resources(left) + right);
  EXPECT_EQ(expected, left + Resources(right));
  EXPECT_EQ(expected, Resources(left) + Resources(right));
}


TEST(ReservedResourcesTest, AdditionDynamicallyReservedWithoutLabels)
{
  Resource::ReservationInfo reservation =
    createDynamicReservationInfo("role", "principal");

  Resources left = createReservedResource("cpus", "8", reservation);
  Resources right = createReservedResource("cpus", "4", reservation);
  Resources expected = createReservedResource("cpus", "12", reservation);

  EXPECT_EQ(expected, left + right);

  // Test operator+ with rvalue references.
  EXPECT_EQ(expected, left + Resources(right));
  EXPECT_EQ(expected, Resources(left) + right);
  EXPECT_EQ(expected, Resources(left) + Resources(right));
}


TEST(ReservedResourcesTest, AdditionDynamicallyReservedWithSameLabels)
{
  Labels labels;
  labels.add_labels()->CopyFrom(createLabel("foo", "bar"));

  Resource::ReservationInfo reservation =
    createDynamicReservationInfo("role", "principal", labels);

  Resources left = createReservedResource("cpus", "8", reservation);
  Resources right = createReservedResource("cpus", "4", reservation);
  Resources expected = createReservedResource("cpus", "12", reservation);

  EXPECT_EQ(expected, left + right);

  // Test operator+ with rvalue references.
  EXPECT_EQ(expected, left + Resources(right));
  EXPECT_EQ(expected, Resources(left) + right);
  EXPECT_EQ(expected, Resources(left) + Resources(right));
}


TEST(ReservedResourcesTest, AdditionDynamicallyReservedWithDistinctLabels)
{
  Labels labels1;
  Labels labels2;

  labels1.add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels2.add_labels()->CopyFrom(createLabel("foo", "baz"));

  Resource::ReservationInfo reservation1 =
    createDynamicReservationInfo("role", "principal", labels1);
  Resource::ReservationInfo reservation2 =
    createDynamicReservationInfo("role", "principal", labels2);

  Resources r1 = createReservedResource("cpus", "6", reservation1);
  Resources r2 = createReservedResource("cpus", "6", reservation2);
  Resources sum = r1 + r2;

  EXPECT_EQ(2u, sum.size());
  EXPECT_FALSE(sum == r1 + r1);
  EXPECT_FALSE(sum == r2 + r2);

  // Test operator+ with rvalue references.
  Resources sum1 = Resources(r1) + r2;
  Resources sum2 = r1 + Resources(r2);
  Resources sum3 = Resources(r1) + Resources(r2);

  EXPECT_EQ(sum, sum1);
  EXPECT_EQ(sum, sum2);
  EXPECT_EQ(sum, sum3);
}


TEST(ReservedResourcesTest, Subtraction)
{
  Labels labels1;
  Labels labels2;

  labels1.add_labels()->CopyFrom(createLabel("foo", "bar"));
  labels2.add_labels()->CopyFrom(createLabel("foo", "baz"));

  Resource::ReservationInfo reservation1 =
    createDynamicReservationInfo("role", "principal", labels1);

  Resource::ReservationInfo reservation2 =
    createDynamicReservationInfo("role", "principal", labels2);

  Resources r1 =
    createReservedResource("cpus", "8", createStaticReservationInfo("role"));

  Resources r2 = createReservedResource("cpus", "8", reservation1);

  EXPECT_TRUE((r1 - r1).empty());
  EXPECT_TRUE((r2 - r2).empty());
  EXPECT_FALSE((r2 - r1).empty());
  EXPECT_FALSE((r1 - r2).empty());
  EXPECT_EQ(r1, r1 - r2);
  EXPECT_EQ(r2, r2 - r1);

  Resources total = r1 + r2;

  Resources r3 =
    createReservedResource("cpus", "6", createStaticReservationInfo("role"));
  Resources r4 = createReservedResource("cpus", "4", reservation1);

  Resources expected = r3 + r4;

  Resources r5 =
    createReservedResource("cpus", "2", createStaticReservationInfo("role"));
  Resources r6 = createReservedResource("cpus", "4", reservation1);

  EXPECT_EQ(expected, total - r5 - r6);

  // Distinct labels
  Resources r7 = createReservedResource("cpus", "8", reservation1);
  Resources r8 = createReservedResource("cpus", "8", reservation2);

  EXPECT_FALSE((r2 - r1).empty());
  EXPECT_FALSE((r1 - r2).empty());
  EXPECT_EQ(r1, r1 - r2);
  EXPECT_EQ(r2, r2 - r1);
}


TEST(ReservedResourcesTest, Contains)
{
  Resources r1 =
    createReservedResource("cpus", "8", createStaticReservationInfo("role"));

  Resources r2 = createReservedResource(
      "cpus", "12", createDynamicReservationInfo("role", "principal"));

  EXPECT_TRUE(r1.contains(r1));
  EXPECT_TRUE(r2.contains(r2));

  EXPECT_FALSE(r1.contains(r2));
  EXPECT_FALSE(r2.contains(r1));

  EXPECT_FALSE(r1.contains(r1 + r2));
  EXPECT_FALSE(r2.contains(r1 + r2));
  EXPECT_TRUE((r1 + r2).contains(r1));
  EXPECT_TRUE((r1 + r2).contains(r2));
  EXPECT_TRUE((r1 + r2).contains(r1 + r2));
}


TEST(DiskResourcesTest, Validation)
{
  Resource cpus = Resources::parse("cpus", "2", "*").get();
  cpus.mutable_disk()->CopyFrom(createDiskInfo("1", "path"));

  Option<Error> error = Resources::validate(cpus);
  ASSERT_SOME(error);
  EXPECT_EQ(
      "DiskInfo should not be set for cpus resource",
      error->message);

  EXPECT_NONE(
      Resources::validate(createDiskResource("10", "role", "1", "path")));

  EXPECT_NONE(
      Resources::validate(createDiskResource("10", "*", None(), "path")));

  EXPECT_NONE(
      Resources::validate(createDiskResource(
          "10",
          "role",
          "1",
          "path",
          createDiskSourcePath("mnt"))));

  EXPECT_NONE(
      Resources::validate(createDiskResource(
          "10",
          "role",
          "1",
          "path",
          createDiskSourceMount("mnt"))));
}


TEST(DiskResourcesTest, Equals)
{
  Resources r1 = createDiskResource("10", "*", None(), None());
  Resources r2 = createDiskResource("10", "*", None(), "path1");
  Resources r3 = createDiskResource("10", "*", None(), "path2");
  Resources r4 = createDiskResource("10", "role", None(), "path2");
  Resources r5 = createDiskResource("10", "role", "1", "path1");
  Resources r6 = createDiskResource("10", "role", "1", "path2");
  Resources r7 = createDiskResource("10", "role", "2", "path2");

  EXPECT_NE(r1, r2);

  EXPECT_EQ(r2, r3);
  EXPECT_EQ(r5, r6);

  EXPECT_NE(r6, r7);
  EXPECT_NE(r4, r7);
}


TEST(DiskResourcesTest, DiskSourceEquals)
{
  Resource::DiskInfo::Source s1 = createDiskSourcePath("mnt");
  Resource::DiskInfo::Source s2 = createDiskSourcePath("mnt2");
  Resource::DiskInfo::Source s3 = createDiskSourceMount("mnt");
  Resource::DiskInfo::Source s4 = createDiskSourceMount("mnt2");

  Resources r1 = createDiskResource("10", "*", None(), None(), s1);
  Resources r2 = createDiskResource("10", "*", None(), "path1", s1);
  Resources r3 = createDiskResource("10", "*", None(), "path2", s1);
  Resources r4 = createDiskResource("10", "role", None(), "path2", s1);
  Resources r5 = createDiskResource("10", "role", "1", "path1", s1);
  Resources r6 = createDiskResource("10", "role", "1", "path2", s1);
  Resources r7 = createDiskResource("10", "role", "2", "path2", s1);

  EXPECT_EQ(r1, r2);
  EXPECT_EQ(r2, r3);
  EXPECT_EQ(r5, r6);

  EXPECT_NE(r6, r7);
  EXPECT_NE(r4, r7);

  Resources r8 = createDiskResource("10", "*", None(), None(), s2);
  Resources r9 = createDiskResource("10", "*", None(), "path1", s2);

  EXPECT_EQ(r8, r9);

  EXPECT_NE(r8, r1);
  EXPECT_NE(r9, r2);

  Resources r10 = createDiskResource("10", "*", None(), "path1", s3);

  EXPECT_EQ(r10, r10);
  EXPECT_NE(r3, r10);

  Resources r11 = createDiskResource("10", "*", None(), None(), s1);
  Resources r12 = createDiskResource("10", "*", None(), None(), s2);
  Resources r13 = createDiskResource("10", "*", None(), None(), s3);
  Resources r14 = createDiskResource("10", "*", None(), None(), s4);

  EXPECT_EQ(r11, r11);
  EXPECT_EQ(r13, r13);

  EXPECT_NE(r11, r12);
  EXPECT_NE(r11, r13);
  EXPECT_NE(r13, r14);
}


class DiskResourcesSourceTest
  : public ::testing::Test,
    public ::testing::WithParamInterface<std::tuple<
        Resource::DiskInfo::Source::Type,
        bool, // Whether the disk has the `vendor` field set.
        bool, // Whether the disk has the `id` field set.
        bool>> {}; // Whether the disk has the `profile` field set.


INSTANTIATE_TEST_CASE_P(
    TypeIdentityProfile,
    DiskResourcesSourceTest,
    ::testing::Combine(
        // We test all source types.
        ::testing::Values(
            Resource::DiskInfo::Source::RAW,
            Resource::DiskInfo::Source::PATH,
            Resource::DiskInfo::Source::BLOCK,
            Resource::DiskInfo::Source::MOUNT),
        // We test the cases where the source has a vendor (i.e., has the
        // `vendor` field set) and where not.
        ::testing::Bool(),
        // We test the cases where the source has an identity (i.e., has the
        // `id` field set) and where not.
        ::testing::Bool(),
        // We test the cases where the source has a profile (i.e., has the
        // `profile` field set) and where not.
        ::testing::Bool()));


TEST_P(DiskResourcesSourceTest, SourceIdentity)
{
  auto parameters = GetParam();

  Resource::DiskInfo::Source::Type type = std::get<0>(parameters);
  bool hasVendor = std::get<1>(parameters);
  bool hasIdentity = std::get<2>(parameters);
  bool hasProfile = std::get<3>(parameters);

  // Create a disk, possibly with an id to signify identity.
  Resource::DiskInfo::Source source;
  source.set_type(type);

  if (hasVendor) {
    source.set_vendor("vendor");
  }

  if (hasIdentity) {
    source.set_id("id");
  }

  if (hasProfile) {
    source.set_profile("profile");
  }

  // Create two disk resources with the created source.
  Resource disk1 = Resources::parse("disk", "1", "*").get();
  disk1.mutable_disk()->mutable_source()->CopyFrom(source);
  const Resources r1 = disk1;

  EXPECT_TRUE(r1.contains(r1));

  Resource disk2 = Resources::parse("disk", "2", "*").get();
  disk2.mutable_disk()->mutable_source()->CopyFrom(source);
  const Resources r2 = disk2;

  // We perform three checks here: checks involving `r1` and `r2`
  // test subtraction semantics while tests of the size of the
  // resources test addition semantics.
  switch (type) {
    case Resource::DiskInfo::Source::RAW: {
      if (hasIdentity) {
        // `RAW` resources with source identity cannot be added or split.
        EXPECT_FALSE(r2.contains(r1));
        EXPECT_NE(r2, r1 + r1);
        EXPECT_EQ(2u, (r1 + r1).size());
      } else {
        // `RAW` resources without source identity can be added and split.
        EXPECT_TRUE(r2.contains(r1));
        EXPECT_EQ(r2, r1 + r1);
        EXPECT_EQ(1u, (r1 + r1).size());
      }
      break;
    }
    case Resource::DiskInfo::Source::BLOCK:
    case Resource::DiskInfo::Source::MOUNT: {
      // `BLOCK` or `MOUNT` resources cannot be added or split,
      // regardless of identity.
      EXPECT_FALSE(r2.contains(r1));
      EXPECT_NE(r2, r1 + r1);
      EXPECT_EQ(2u, (r1 + r1).size());
      break;
    }
    case Resource::DiskInfo::Source::PATH: {
      // `PATH` resources can be added and split, regardless of identity.
      EXPECT_TRUE(r2.contains(r1));
      EXPECT_EQ(r2, r1 + r1);
      EXPECT_EQ(1u, (r1 + r1).size());
      break;
    }
    case Resource::DiskInfo::Source::UNKNOWN: {
      FAIL() << "Unexpected source type";
      break;
    }
  }
}


TEST(DiskResourcesTest, Addition)
{
  Resources r1 = createDiskResource("10", "role", None(), "path");
  Resources r2 = createDiskResource("10", "role", None(), None());
  Resources r3 = createDiskResource("20", "role", None(), "path");

  EXPECT_EQ(r3, r1 + r1);
  EXPECT_NE(r3, r1 + r2);

  Resources r4 = createDiskResource("10", "role", "1", "path");
  Resources r5 = createDiskResource("10", "role", "2", "path");
  Resources r6 = createDiskResource("20", "role", "1", "path");

  Resources sum = r4 + r5;

  EXPECT_TRUE(sum.contains(r4));
  EXPECT_TRUE(sum.contains(r5));
  EXPECT_FALSE(sum.contains(r3));
  EXPECT_FALSE(sum.contains(r6));

  // Test operator+ with rvalue references.
  Resources sum1 = Resources(r4) + r5;
  Resources sum2 = r4 + Resources(r5);
  Resources sum3 = Resources(r4) + Resources(r5);

  EXPECT_EQ(sum, sum1);
  EXPECT_EQ(sum, sum2);
  EXPECT_EQ(sum, sum3);
}


TEST(DiskResourcesTest, DiskSourceAddition)
{
  Resource::DiskInfo::Source s1 = createDiskSourcePath("mnt");
  Resource::DiskInfo::Source s2 = createDiskSourcePath("mnt2");
  Resource::DiskInfo::Source s3 = createDiskSourceMount("mnt");
  Resource::DiskInfo::Source s4 = createDiskSourceMount("mnt2");

  Resources r1 = createDiskResource("10", "role", None(), None(), s1);
  Resources r2 = createDiskResource("20", "role", None(), None(), s2);
  Resources r3 = createDiskResource("10", "role", None(), None(), s2);
  Resources r4 = createDiskResource("20", "role", None(), None(), s1);

  EXPECT_NE(r2, r1 + r1);
  EXPECT_NE(r2, r1 + r3);

  EXPECT_EQ(r4, r1 + r1);

  EXPECT_TRUE(r4.contains(r1));

  Resources r5 = createDiskResource("10", "role", None(), None(), s3);
  Resources r6 = createDiskResource("20", "role", None(), None(), s4);
  Resources r7 = createDiskResource("10", "role", None(), None(), s4);
  Resources r8 = createDiskResource("20", "role", None(), None(), s3);

  EXPECT_NE(r6, r5 + r5);
  EXPECT_NE(r6, r5 + r7);

  EXPECT_FALSE(r8.contains(r5));
  EXPECT_TRUE(r8.contains(r8));

  Resources sum = r1 + r5;
  EXPECT_NE(r4, sum);

  // Test operator+ with rvalue references.
  Resources sum1 = Resources(r1) + r5;
  Resources sum2 = r1 + Resources(r5);
  Resources sum3 = Resources(r1) + Resources(r5);

  EXPECT_EQ(sum, sum1);
  EXPECT_EQ(sum, sum2);
  EXPECT_EQ(sum, sum3);
}


TEST(DiskResourcesTest, Subtraction)
{
  Resources r1 = createDiskResource("10", "role", None(), "path");
  Resources r2 = createDiskResource("10", "role", None(), None());

  EXPECT_TRUE((r1 - r1).empty());
  EXPECT_TRUE((r2 - r2).empty());
  EXPECT_FALSE((r1 - r2).empty());

  Resources r3 = createDiskResource("10", "role", "1", "path");
  Resources r4 = createDiskResource("10", "role", "2", "path");
  Resources r5 = createDiskResource("10", "role", "2", "path2");

  EXPECT_EQ(r3, r3 - r4);
  EXPECT_TRUE((r3 - r3).empty());
  EXPECT_TRUE((r4 - r5).empty());
}


TEST(DiskResourcesTest, DiskSourceSubtraction)
{
  Resource::DiskInfo::Source s1 = createDiskSourcePath("mnt");
  Resource::DiskInfo::Source s2 = createDiskSourcePath("mnt2");
  Resource::DiskInfo::Source s3 = createDiskSourceMount("mnt");
  Resource::DiskInfo::Source s4 = createDiskSourceMount("mnt2");

  Resources r1 = createDiskResource("10", "role", None(), None(), s1);
  Resources r2 = createDiskResource("20", "role", None(), None(), s2);
  Resources r3 = createDiskResource("10", "role", None(), None(), s2);
  Resources r4 = createDiskResource("20", "role", None(), None(), s1);

  EXPECT_TRUE((r1 - r1).empty());
  EXPECT_TRUE((r4 - r1 - r1).empty());

  EXPECT_FALSE((r3 - r1).empty());

  EXPECT_EQ(r3, r3 - r1);
  EXPECT_EQ(r1, r4 - r1);

  Resources r5 = createDiskResource("10", "role", None(), None(), s3);
  Resources r6 = createDiskResource("20", "role", None(), None(), s4);
  Resources r7 = createDiskResource("10", "role", None(), None(), s4);
  Resources r8 = createDiskResource("20", "role", None(), None(), s3);

  EXPECT_TRUE((r5 - r5).empty());

  EXPECT_FALSE((r8 - r5 - r5).empty());
  EXPECT_FALSE((r7 - r5).empty());

  EXPECT_EQ(r7, r7 - r5);
  EXPECT_EQ(r8, r8 - r5);

  EXPECT_FALSE((r5 - r1).empty());
}


TEST(DiskResourcesTest, Contains)
{
  Resources r1 = createDiskResource("10", "role", "1", "path");
  Resources r2 = createDiskResource("10", "role", "1", "path");

  EXPECT_FALSE(r1.contains(r1 + r2));
  EXPECT_FALSE(r2.contains(r1 + r2));
  EXPECT_TRUE((r1 + r2).contains(r1 + r2));

  Resources r3 = createDiskResource("20", "role", "2", "path");

  EXPECT_TRUE((r1 + r3).contains(r1));
  EXPECT_TRUE((r1 + r3).contains(r3));
}


TEST(DiskResourcesTest, SourceContains)
{
  Resource::DiskInfo::Source s1 = createDiskSourcePath("mnt");
  Resource::DiskInfo::Source s2 = createDiskSourceMount("mnt");

  Resources r1 = createDiskResource("10", "role", "1", "path", s1);
  Resources r2 = createDiskResource("20", "role", "1", "path", s1);
  Resources r3 = createDiskResource("10", "role", "1", "path", s2);
  Resources r4 = createDiskResource("20", "role", "1", "path", s2);

  EXPECT_TRUE(r1.contains(r1));
  EXPECT_TRUE(r3.contains(r3));
  EXPECT_TRUE(r4.contains(r4));

  EXPECT_FALSE(r2.contains(r1));
  EXPECT_FALSE(r2.contains(r1 + r1));

  EXPECT_FALSE(r4.contains(r3));
  EXPECT_FALSE(r4.contains(r3 + r3));

  Resources r5 = createDiskResource("10", "role", None(), None(), s1);
  Resources r6 = createDiskResource("20", "role", None(), None(), s1);
  Resources r7 = createDiskResource("10", "role", None(), None(), s2);
  Resources r8 = createDiskResource("20", "role", None(), None(), s2);

  EXPECT_TRUE(r5.contains(r5));
  EXPECT_TRUE(r7.contains(r7));
  EXPECT_TRUE(r8.contains(r8));

  EXPECT_TRUE(r6.contains(r5));
  EXPECT_TRUE(r6.contains(r5 + r5));

  EXPECT_FALSE(r8.contains(r7));
  EXPECT_FALSE(r8.contains(r7 + r7));
}


TEST(DiskResourcesTest, FilterPersistentVolumes)
{
  Resources resources = Resources::parse("cpus:1;mem:512;disk:1000").get();

  Resources r1 = createDiskResource("10", "role1", "1", "path");
  Resources r2 = createDiskResource("20", "role2", None(), None());

  resources += r1;
  resources += r2;

  EXPECT_EQ(r1, resources.persistentVolumes());
}


TEST(ResourcesOperationTest, ReserveResources)
{
  Resources unreservedCpus = Resources::parse("cpus:1").get();
  Resources unreservedMem = Resources::parse("mem:512").get();

  Resources unreserved = unreservedCpus + unreservedMem;

  Resources reservedCpus1 = unreservedCpus.pushReservation(
      createDynamicReservationInfo("role", "principal"));

  EXPECT_SOME_EQ(unreservedMem + reservedCpus1,
                 unreserved.apply(RESERVE(reservedCpus1)));

  // Check the case of insufficient unreserved resources.
  Resources reservedCpus2 = createReservedResource(
      "cpus", "2", createDynamicReservationInfo("role", "principal"));

  EXPECT_ERROR(unreserved.apply(RESERVE(reservedCpus2)));
}


TEST(ResourcesOperationTest, UnreserveResources)
{
  Resources reservedCpus = createReservedResource(
      "cpus", "1", createDynamicReservationInfo("role", "principal"));

  Resources reservedMem = createReservedResource(
      "mem", "512", createDynamicReservationInfo("role", "principal"));

  Resources reserved = reservedCpus + reservedMem;

  Resources unreservedCpus1 = reservedCpus.toUnreserved();

  EXPECT_SOME_EQ(reservedMem + unreservedCpus1,
                 reserved.apply(UNRESERVE(reservedCpus)));

  // Check the case of insufficient unreserved resources.
  Resource reservedCpus2 = createReservedResource(
      "cpus", "2", createDynamicReservationInfo("role", "principal"));

  EXPECT_ERROR(reserved.apply(UNRESERVE(reservedCpus2)));
}


TEST(ResourcesOperationTest, CreatePersistentVolume)
{
  Resources total = Resources::parse("cpus:1;mem:512;disk(role):1000").get();

  Resource volume1 = createDiskResource("200", "role", "1", "path");

  Offer::Operation create1;
  create1.set_type(Offer::Operation::CREATE);
  create1.mutable_create()->add_volumes()->CopyFrom(volume1);

  EXPECT_SOME_EQ(
      Resources::parse("cpus:1;mem:512;disk(role):800").get() + volume1,
      total.apply(create1));

  // Check the case of insufficient disk resources.
  Resource volume2 = createDiskResource("2000", "role", "1", "path");

  Offer::Operation create2;
  create2.set_type(Offer::Operation::CREATE);
  create2.mutable_create()->add_volumes()->CopyFrom(volume2);

  EXPECT_ERROR(total.apply(create2));
}


TEST(ResourcesOperationTest, StrippedResourcesVolume)
{
  Resources volume = createDiskResource("200", "role", "1", "path");
  Resources stripped = volume.createStrippedScalarQuantity();

  EXPECT_TRUE(stripped.persistentVolumes().empty());
  EXPECT_TRUE(stripped.reserved().empty());
  EXPECT_EQ(Megabytes(200), stripped.disk().get());

  Resource strippedVolume = *(stripped.begin());

  ASSERT_EQ(Value::SCALAR, strippedVolume.type());
  EXPECT_DOUBLE_EQ(200, strippedVolume.scalar().value());
  EXPECT_EQ("disk", strippedVolume.name());
  EXPECT_FALSE(strippedVolume.has_disk());
  EXPECT_FALSE(Resources::isPersistentVolume(strippedVolume));
}


TEST(ResourcesOperationTest, StrippedResourcesAllocated)
{
  Resources allocated = Resources::parse("cpus:1;mem:512").get();
  allocated.allocate("role");

  Resources stripped = allocated.createStrippedScalarQuantity();

  // Allocation info should be stripped when
  // converting to a quantity.
  foreach (const Resource& resource, stripped) {
    EXPECT_FALSE(resource.has_allocation_info());
  }
}


TEST(ResourcesOperationTest, StrippedResourcesReserved)
{
  Resources unreserved = Resources::parse("cpus:1;mem:512").get();
  Resources dynamicallyReserved = unreserved.pushReservation(
      createDynamicReservationInfo("role", "principal"));

  Resources stripped = dynamicallyReserved.createStrippedScalarQuantity();

  EXPECT_TRUE(stripped.reserved("role").empty());

  foreach (const Resource& resource, stripped) {
    EXPECT_FALSE(Resources::isDynamicallyReserved(resource));
    EXPECT_TRUE(Resources::isUnreserved(resource));
  }
}


TEST(ResourcesOperationTest, StrippedResourcesResourceProvider)
{
  Resource plain = Resources::parse("cpus", "1", "*").get();

  Resource provided = plain;
  provided.mutable_provider_id()->set_value("RESOURCE_PROVIDER_ID");

  Resources stripped = Resources(provided).createStrippedScalarQuantity();

  EXPECT_EQ(Resources(plain), stripped);
}


TEST(ResourcesOperationTest, StrippedResourcesNonScalar)
{
  Resources ports = Resources::parse("ports:[10000-20000, 30000-50000]").get();

  EXPECT_TRUE(ports.createStrippedScalarQuantity().empty());

  Resources names = Resources::parse("names:{foo,bar}").get();

  EXPECT_TRUE(names.createStrippedScalarQuantity().empty());
}


TEST(ResourceOperationTest, StrippedResourcesRevocable)
{
  Resource plain = Resources::parse("cpus", "1", "*").get();

  Resource revocable = plain;
  revocable.mutable_revocable();

  Resources stripped = Resources(revocable).createStrippedScalarQuantity();

  EXPECT_EQ(Resources(plain), stripped);
}


TEST(ResourcesOperationTest, CreatePersistentVolumeFromMount)
{
  Resource::DiskInfo::Source source = createDiskSourceMount("mnt");
  Resources total = createDiskResource("200", "role", None(), None(), source);

  Resource volume1 = createDiskResource("200", "role", "1", "path", source);

  Offer::Operation create1;
  create1.set_type(Offer::Operation::CREATE);
  create1.mutable_create()->add_volumes()->CopyFrom(volume1);

  EXPECT_SOME(total.apply(create1));

  // Check the case of sufficient (but subset of) disk resources from
  // an exclusive mount.
  Resource volume2 = createDiskResource("50", "role", "1", "path", source);

  Offer::Operation create2;
  create2.set_type(Offer::Operation::CREATE);
  create2.mutable_create()->add_volumes()->CopyFrom(volume2);

  EXPECT_ERROR(total.apply(create2));
}


TEST(ResourcesOperationTest, CreateSharedPersistentVolume)
{
  Resources total = Resources::parse("cpus:1;mem:512;disk(role):1000").get();

  Resource volume1 = createDiskResource(
      "200", "role", "1", "path", None(), true);

  Offer::Operation create1;
  create1.set_type(Offer::Operation::CREATE);
  create1.mutable_create()->add_volumes()->CopyFrom(volume1);

  EXPECT_SOME_EQ(
      Resources::parse("cpus:1;mem:512;disk(role):800").get() + volume1,
      total.apply(create1));

  // Apply a pair of CREATE and DESTROY of the same volume the result
  // should be the original `total`.
  Offer::Operation destroy1;
  destroy1.set_type(Offer::Operation::DESTROY);
  destroy1.mutable_destroy()->add_volumes()->CopyFrom(volume1);

  EXPECT_SOME_EQ(total, total.apply(create1)->apply(destroy1));

  // Check the case of insufficient disk resources.
  Resource volume2 = createDiskResource(
      "2000", "role", "1", "path", None(), true);

  Offer::Operation create2;
  create2.set_type(Offer::Operation::CREATE);
  create2.mutable_create()->add_volumes()->CopyFrom(volume2);

  EXPECT_ERROR(total.apply(create2));
}


TEST(ResourcesOperationTest, DestroySharedPersistentVolumeMultipleCopies)
{
  Resources total = Resources::parse("cpus:1;mem:512;disk(role):800").get();
  Resource volume1 = createDiskResource(
      "200", "role", "1", "path", None(), true);

  // Add 2 copies of the shared volume.
  total += volume1;
  total += volume1;

  // DESTROY of the shared volume should fail since there are multiple
  // shared copies in `total`.
  Offer::Operation destroy1;
  destroy1.set_type(Offer::Operation::DESTROY);
  destroy1.mutable_destroy()->add_volumes()->CopyFrom(volume1);

  EXPECT_ERROR(total.apply(destroy1));
}


TEST(ResourcesOperationTest, FlattenResources)
{
  Resources unreservedCpus = Resources::parse("cpus:1").get();
  Resources unreservedMem = Resources::parse("mem:512").get();

  Resources unreserved = unreservedCpus + unreservedMem;

  Resources reservedCpus = unreservedCpus.pushReservation(
      createDynamicReservationInfo("role", "principal"));

  EXPECT_SOME_EQ(unreservedMem + reservedCpus,
                 unreserved.apply(RESERVE(reservedCpus)));
}


// Helper for creating a revocable resource.
static Resource createRevocableResource(
    const string& name,
    const string& value,
    const string& role,
    bool revocable)
{
  Resource resource = Resources::parse(name, value, role).get();

  if (revocable) {
    resource.mutable_revocable();
  }

  return resource;
}


// This test verifies that revocable and non-revocable resources are
// not considered equal.
TEST(RevocableResourceTest, Equals)
{
  Resources resources1 = createRevocableResource("cpus", "1", "*", true);

  Resources resources2 = resources1;
  EXPECT_EQ(resources1, resources2);

  Resources resources3 = createRevocableResource("cpus", "1", "*", false);
  EXPECT_NE(resources1, resources3);
}


// This test verifies that adding revocable resources to revocable
// resources will merge them but adding to non-revocable resources
// will not merge.
TEST(RevocableResourceTest, Addition)
{
  Resources r1 = createRevocableResource("cpus", "1", "*", true);
  Resources r2 = createRevocableResource("cpus", "1", "*", true);
  Resources r3 = createRevocableResource("cpus", "2", "*", true);

  // Adding revocable resources will merge them.
  EXPECT_EQ(r3, r1 + r2);

  Resources r4 = createRevocableResource("cpus", "1", "*", true);
  Resources r5 = createRevocableResource("cpus", "1", "*", false);
  Resources r6 = createRevocableResource("cpus", "2", "*", false);

  Resources sum = r4 + r5;

  // Adding revocable and non-revocable resources will not merge them.
  EXPECT_TRUE(sum.contains(r4));
  EXPECT_TRUE(sum.contains(r5));
  EXPECT_FALSE(sum.contains(r3));
  EXPECT_FALSE(sum.contains(r6));

  // Test operator+ with rvalue references.
  Resources sum1 = Resources(r4) + r5;
  Resources sum2 = r4 + Resources(r5);
  Resources sum3 = Resources(r4) + Resources(r5);

  EXPECT_EQ(sum, sum1);
  EXPECT_EQ(sum, sum2);
  EXPECT_EQ(sum, sum3);
}


// This test verifies that subtracting revocable resources from
// revocable resources will merge them but subtracting from
// non-revocable resources will not merge.
TEST(RevocableResourceTest, Subtraction)
{
  Resources r1 = createRevocableResource("cpus", "1", "*", true);
  Resources r2 = createRevocableResource("cpus", "1", "*", true);

  // Subtracting revocable resources will merge them.
  EXPECT_TRUE((r1 - r2).empty());

  Resources r3 = createRevocableResource("cpus", "1", "*", true);
  Resources r4 = createRevocableResource("cpus", "1", "*", false);

  // Subtracting non-revocable resources from revocable resources is
  // a no-op.
  EXPECT_EQ(r3, r3 - r4);
  EXPECT_TRUE((r3 - r3).empty());
}


// This test verifies that adding revocable resources to revocable or
// non-revocable resources respects the 'contains' logic.
TEST(RevocableResourceTest, Contains)
{
  Resources r1 = createRevocableResource("cpus", "1", "*", true);
  Resources r2 = createRevocableResource("cpus", "1", "*", true);

  EXPECT_FALSE(r1.contains(r1 + r2));
  EXPECT_FALSE(r2.contains(r1 + r2));
  EXPECT_TRUE((r1 + r2).contains(r1));
  EXPECT_TRUE((r1 + r2).contains(r2));
  EXPECT_TRUE((r1 + r2).contains(r1 + r2));

  Resources r3 = createRevocableResource("cpus", "1", "*", false);

  EXPECT_TRUE((r1 + r3).contains(r1));
  EXPECT_TRUE((r1 + r3).contains(r3));
}


// This test verifies that revocable and non-revocable resources
// can be filtered.
TEST(RevocableResourceTest, Filter)
{
  Resources r1 = createRevocableResource("cpus", "1", "*", true);
  EXPECT_EQ(r1, r1.revocable());
  EXPECT_TRUE(r1.nonRevocable().empty());

  Resources r2 = createRevocableResource("cpus", "1", "*", false);
  EXPECT_EQ(r2, r2.nonRevocable());
  EXPECT_TRUE(r2.revocable().empty());

  EXPECT_EQ(r1, (r1 + r2).revocable());
  EXPECT_EQ(r2, (r1 + r2).nonRevocable());
}


// This test verifies that `Resources::find()` correctly distinguishes
// between revocable and non-revocable resources.
TEST(RevocableResourceTest, Find)
{
  Resources r1 = createRevocableResource("cpus", "1", "*", true);
  EXPECT_EQ(r1, r1.revocable());
  EXPECT_TRUE(r1.nonRevocable().empty());

  Resources r2 = Resources::parse("cpus:1").get();
  EXPECT_EQ(r2, r2.nonRevocable());
  EXPECT_TRUE(r2.revocable().empty());

  EXPECT_SOME_EQ(r1, (r1 + r2).find(r1));
  EXPECT_SOME_EQ(r2, (r1 + r2).find(r2));

  EXPECT_NONE(r1.find(r2));
  EXPECT_NONE(r2.find(r1));
}


// This test checks that the resources in the "pre-reservation-refinement"
// format are valid. In the "pre-reservation-refinement" format, the reservation
// state is represented by `Resource.role` and `Resource.reservation` fields.
TEST(ResourceFormatTest, PreReservationRefinement)
{
  Resource resource;
  resource.set_name("cpus");
  resource.set_type(Value::SCALAR);
  resource.mutable_scalar()->set_value(555.5);

  // Unreserved resource.
  EXPECT_NONE(Resources::validate(resource));

  resource.set_role("*");
  EXPECT_NONE(Resources::validate(resource));

  // Statically reserved resource.
  resource.set_role("foo");
  EXPECT_NONE(Resources::validate(resource));

  // Dynamically reserved resource.
  Resource::ReservationInfo* reservation = resource.mutable_reservation();
  reservation->set_principal("principal1");

  EXPECT_NONE(Resources::validate(resource));
}


// This test checks that the resources in the "post-reservation-refinement"
// format are valid. In the "post-reservation-refinement" format,
// the reservation state is represented by the `Resource.reservations` field.
TEST(ResourceFormatTest, PostReservationRefinement)
{
  Resource resource;
  resource.set_name("cpus");
  resource.set_type(Value::SCALAR);
  resource.mutable_scalar()->set_value(555.5);

  // Unreserved resource.
  EXPECT_NONE(Resources::validate(resource));

  Resource::ReservationInfo* reservation = resource.add_reservations();
  reservation->set_role("foo");
  reservation->set_principal("principal1");

  // Statically reserved resource.
  reservation->set_type(Resource::ReservationInfo::STATIC);
  EXPECT_NONE(Resources::validate(resource));

  // Dynamically reserved resource.
  reservation->set_type(Resource::ReservationInfo::DYNAMIC);
  EXPECT_NONE(Resources::validate(resource));

  // Refined static reservation is invalid.
  reservation->set_type(Resource::ReservationInfo::STATIC);
  Resource::ReservationInfo* refinedReservation = resource.add_reservations();
  refinedReservation->set_type(Resource::ReservationInfo::STATIC);
  refinedReservation->set_role("foo/bar");
  refinedReservation->set_principal("principal2");
  EXPECT_SOME(Resources::validate(resource));

  // Refined dynamic reservation on top of static reservation.
  refinedReservation->set_type(Resource::ReservationInfo::DYNAMIC);
  EXPECT_NONE(Resources::validate(resource));

  // Refined dynamic reservation on top of dynamic reservation.
  reservation->set_type(Resource::ReservationInfo::DYNAMIC);
  EXPECT_NONE(Resources::validate(resource));
}


// This test checks that the resources in the "endpoint" format are valid.
// In the "endpoint" format, both the fields for both pre-refinement and
// post-refinement reservations are set, but they must be set to mutually
// equivalent values.
TEST(ResourceFormatTest, Endpoint)
{
  Resource resource;
  resource.set_name("cpus");
  resource.set_type(Value::SCALAR);
  resource.mutable_scalar()->set_value(555.5);
  resource.set_role("r1");

  // Dynamically reserved, pre-refinement format.
  Resource::ReservationInfo* unrefinedReservation =
    resource.mutable_reservation();
  unrefinedReservation->set_principal("principal1");

  // Set `reservations` field as well (post-refinement format).
  Resource::ReservationInfo* refinedReservation = resource.add_reservations();
  refinedReservation->set_type(Resource::ReservationInfo::DYNAMIC);
  refinedReservation->set_role("r1");
  refinedReservation->set_principal("principal1");

  // Should validate now that all fields are set to equivalent values.
  EXPECT_NONE(Resources::validate(resource));

  // Should not validate if pre- and post-refinement fields are inconsistent.
  {
    refinedReservation->set_role("r2");
    EXPECT_SOME(Resources::validate(resource));
    refinedReservation->set_role("r1");

    refinedReservation->set_type(Resource::ReservationInfo::STATIC);
    EXPECT_SOME(Resources::validate(resource));
    refinedReservation->set_type(Resource::ReservationInfo::DYNAMIC);

    unrefinedReservation->set_principal("principal2");
    EXPECT_SOME(Resources::validate(resource));
    unrefinedReservation->set_principal("principal1");

    // Sanity check that the resource is still valid.
    EXPECT_NONE(Resources::validate(resource));
  }

  // Should not validate if the post-refinement format contains a
  // reservation refinement.
  Resource::ReservationInfo* refinedReservation2 = resource.add_reservations();
  refinedReservation2->set_type(Resource::ReservationInfo::DYNAMIC);
  refinedReservation2->set_role("r1/r2");
  refinedReservation2->set_principal("principal1");

  EXPECT_SOME(Resources::validate(resource));
}


TEST(ResourceFormatTest, DowngradeWithoutResources)
{
  FrameworkInfo frameworkInfo = DEFAULT_FRAMEWORK_INFO;
  EXPECT_SOME(downgradeResources(&frameworkInfo));
  EXPECT_EQ(
      DEFAULT_FRAMEWORK_INFO.SerializeAsString(),
      frameworkInfo.SerializeAsString());
}


TEST(ResourceFormatTest, DowngradeWithResourcesWithoutRefinedReservations)
{
  SlaveID slaveId;
  slaveId.set_value("agent");

  TaskInfo actual;
  {
    actual.set_name("task");
    actual.mutable_task_id()->set_value("task_id");
    actual.mutable_slave_id()->CopyFrom(slaveId);

    Resource resource;
    resource.set_name("cpus");
    resource.set_type(Value::SCALAR);
    resource.mutable_scalar()->set_value(555.5);

    // Add "post-reservation-refinement" resources.

    // Unreserved resource.
    actual.add_resources()->CopyFrom(resource);

    Resource::ReservationInfo* reservation = resource.add_reservations();

    // Statically reserved resource.
    reservation->set_type(Resource::ReservationInfo::STATIC);
    reservation->set_role("foo");
    actual.add_resources()->CopyFrom(resource);

    // Dynamically reserved resource.
    reservation->set_type(Resource::ReservationInfo::DYNAMIC);
    reservation->set_role("bar");
    reservation->set_principal("principal1");
    actual.add_resources()->CopyFrom(resource);
  }

  TaskInfo expected;
  {
    expected.set_name("task");
    expected.mutable_task_id()->set_value("task_id");
    expected.mutable_slave_id()->CopyFrom(slaveId);

    Resource resource;
    resource.set_name("cpus");
    resource.set_type(Value::SCALAR);
    resource.mutable_scalar()->set_value(555.5);

    // Add "pre-reservation-refinement" resources.

    // Unreserved resource.
    resource.set_role("*");
    expected.add_resources()->CopyFrom(resource);

    // Statically reserved resource.
    resource.set_role("foo");
    expected.add_resources()->CopyFrom(resource);

    // Dynamically reserved resource.
    resource.set_role("bar");
    Resource::ReservationInfo* reservation = resource.mutable_reservation();
    reservation->set_principal("principal1");
    expected.add_resources()->CopyFrom(resource);
  }

  EXPECT_SOME(downgradeResources(&actual));
  EXPECT_EQ(expected, actual);
}


TEST(ResourceFormatTest, DowngradeWithResourcesWithRefinedReservations)
{
  SlaveID slaveId;
  slaveId.set_value("agent");

  TaskInfo actual;
  {
    actual.set_name("task");
    actual.mutable_task_id()->set_value("task_id");
    actual.mutable_slave_id()->CopyFrom(slaveId);

    Resource resource;
    resource.set_name("cpus");
    resource.set_type(Value::SCALAR);
    resource.mutable_scalar()->set_value(555.5);

    // Add "post-reservation-refinement" resources.

    // Unreserved resource.
    actual.add_resources()->CopyFrom(resource);

    Resource::ReservationInfo* reservation = resource.add_reservations();

    // Statically reserved resource.
    reservation->set_type(Resource::ReservationInfo::STATIC);
    reservation->set_role("foo");
    actual.add_resources()->CopyFrom(resource);

    // Dynamically reserved resource.
    reservation->set_type(Resource::ReservationInfo::DYNAMIC);
    reservation->set_role("bar");
    reservation->set_principal("principal1");
    actual.add_resources()->CopyFrom(resource);

    // Dynamically refined reservation on top of dynamic reservation.
    Resource::ReservationInfo* refinedReservation = resource.add_reservations();
    refinedReservation->set_type(Resource::ReservationInfo::DYNAMIC);
    refinedReservation->set_role("bar/baz");
    refinedReservation->set_principal("principal2");
    actual.add_resources()->CopyFrom(resource);
  }

  TaskInfo expected;
  {
    expected.set_name("task");
    expected.mutable_task_id()->set_value("task_id");
    expected.mutable_slave_id()->CopyFrom(slaveId);

    Resource resource;
    resource.set_name("cpus");
    resource.set_type(Value::SCALAR);
    resource.mutable_scalar()->set_value(555.5);

    // Add "pre-reservation-refinement" resources.

    // Unreserved resource.
    resource.set_role("*");
    expected.add_resources()->CopyFrom(resource);

    // Statically reserved resource.
    resource.set_role("foo");
    expected.add_resources()->CopyFrom(resource);

    // Dynamically reserved resource.
    resource.set_role("bar");
    Resource::ReservationInfo* reservation = resource.mutable_reservation();
    reservation->set_principal("principal1");
    expected.add_resources()->CopyFrom(resource);

    // Add non-downgradable resources. Note that the non-downgradable
    // resources remain in "post-reservation-refinement" format.

    // Dynamically refined reservation on top of dynamic reservation.
    resource.clear_role();
    resource.clear_reservation();

    Resource::ReservationInfo* dynamicReservation = resource.add_reservations();
    dynamicReservation->set_type(Resource::ReservationInfo::DYNAMIC);
    dynamicReservation->set_role("bar");
    dynamicReservation->set_principal("principal1");

    Resource::ReservationInfo* refinedReservation = resource.add_reservations();
    refinedReservation->set_type(Resource::ReservationInfo::DYNAMIC);
    refinedReservation->set_role("bar/baz");
    refinedReservation->set_principal("principal2");

    expected.add_resources()->CopyFrom(resource);
  }

  EXPECT_ERROR(downgradeResources(&actual));
  EXPECT_EQ(expected, actual);
}


TEST(ResourcesTest, Count)
{
  // The summation of identical shared resources is valid and
  // the result is reflected in the count.
  Resource sharedDisk = createDiskResource(
      "100", "role1", "1", "path1", None(), true);
  EXPECT_EQ(1u, (Resources(sharedDisk)).count(sharedDisk));
  EXPECT_EQ(2u, (Resources(sharedDisk) + sharedDisk).count(sharedDisk));

  // The summation is invalid and a no-op for non-shared disks so the
  // count remains 1.
  Resource nonSharedDisk = createDiskResource("100", "role1", "1", "path1");
  EXPECT_EQ(1u, (Resources(nonSharedDisk)).count(nonSharedDisk));
  EXPECT_EQ(
      1u, (Resources(nonSharedDisk) + nonSharedDisk).count(nonSharedDisk));

  // After the summation the scalar changes so the count is 0.
  Resource cpus = Resources::parse("cpus", "1", "*").get();
  EXPECT_EQ(1u, Resources(cpus).count(cpus));
  EXPECT_EQ(0u, (Resources(cpus) + cpus).count(cpus));
}


TEST(ResourcesTest, Evolve)
{
  string resourcesString = "cpus(role1):2;mem(role1):10;cpus:4;mem:20";
  Resources resources = Resources::parse(resourcesString).get();

  v1::Resources evolved = evolve(resources);

  EXPECT_EQ(v1::Resources::parse(resourcesString).get(), evolved);
}


TEST(ResourcesTest, ReservationAncestor)
{
  Resources baseResources = *Resources::parse("cpus:2;mem:10");

  EXPECT_EQ(
      baseResources,
      Resources::getReservationAncestor(baseResources, baseResources));

  Resources resources1 = baseResources.pushReservation(
      createDynamicReservationInfo("foo"));

  EXPECT_EQ(
      resources1,
      Resources::getReservationAncestor(resources1, resources1));

  EXPECT_EQ(
      baseResources,
      Resources::getReservationAncestor(resources1, baseResources));

  EXPECT_EQ(
      baseResources,
      Resources::getReservationAncestor(baseResources, resources1));

  resources1 = resources1.pushReservation(
      createDynamicReservationInfo("foo/bar"));

  EXPECT_EQ(
      resources1,
      Resources::getReservationAncestor(resources1, resources1));

  EXPECT_EQ(
      baseResources,
      Resources::getReservationAncestor(resources1, baseResources));

  EXPECT_EQ(
      baseResources,
      Resources::getReservationAncestor(baseResources, resources1));

  Resources resources2 = baseResources.pushReservation(
      createDynamicReservationInfo("foo"));

  EXPECT_EQ(
      resources2,
      Resources::getReservationAncestor(resources1, resources2));

  EXPECT_EQ(
      resources2,
      Resources::getReservationAncestor(resources2, resources1));

  EXPECT_NE(
      resources1,
      Resources::getReservationAncestor(baseResources, resources2));

  EXPECT_EQ(
      baseResources,
      Resources::getReservationAncestor(baseResources, resources2));

  Resources resources3 = resources2.pushReservation(
      createDynamicReservationInfo("foo/baz"));

  EXPECT_EQ(
      resources2,
      Resources::getReservationAncestor(resources1, resources3));
}


TEST(SharedResourcesTest, Printing)
{
  Resources volume = createPersistentVolume(
      Megabytes(64),
      "role1",
      "id1",
      "path1",
      None(),
      None(),
      "principal1",
      true); // Shared.

  {
    ostringstream oss;

    oss << volume;
    EXPECT_EQ(
        "disk(reservations: [(STATIC,role1)])[id1:path1]<SHARED>:64<1>",
        oss.str());
  }

  {
    ostringstream oss;

    oss << volume + volume;
    EXPECT_EQ(
        "disk(reservations: [(STATIC,role1)])[id1:path1]<SHARED>:64<2>",
        oss.str());
  }
}


TEST(SharedResourcesTest, ScalarAdditionShared)
{
  // Shared persistent volume.
  Resource disk = createDiskResource(
      "50", "role1", "1", "path", None(), true);

  Resources r1;
  r1 += Resources::parse("cpus", "1", "*").get();
  r1 += Resources::parse("mem", "5", "*").get();
  r1 += disk;

  EXPECT_EQ(1u, r1.count(disk));

  Resources r2 = Resources::parse("cpus:2;mem:10").get() + disk;

  EXPECT_EQ(1u, r2.count(disk));

  // Verify addition (operator+) on Resources.
  Resources sum = r1 + r2;

  EXPECT_FALSE(sum.empty());
  EXPECT_EQ(3u, sum.size());
  EXPECT_EQ(3, sum.get<Value::Scalar>("cpus")->value());
  EXPECT_EQ(15, sum.get<Value::Scalar>("mem")->value());
  EXPECT_EQ(50, sum.get<Value::Scalar>("disk")->value());
  EXPECT_EQ(2u, sum.count(disk));

  // Test operator+ with rvalue references.
  Resources sum1 = Resources(r1) + r2;
  Resources sum2 = r1 + Resources(r2);
  Resources sum3 = Resources(r1) + Resources(r2);

  EXPECT_EQ(sum, sum1);
  EXPECT_EQ(sum, sum2);
  EXPECT_EQ(sum, sum3);

  // Verify operator+= on Resources is the same as operator+.
  Resources r = r1;
  r += r2;
  EXPECT_EQ(r, sum);
}


TEST(SharedResourcesTest, ScalarSubtractionShared)
{
  // Shared persistent volume.
  Resource disk = createDiskResource(
      "8192", "role1", "1", "path", None(), true);

  Resources r1 = Resources::parse("cpus:40;mem:4096").get() + disk + disk;
  Resources r2 = Resources::parse("cpus:5;mem:512").get() + disk;

  // Verify subtraction (operator-) on Resources.
  Resources diff = r1 - r2;

  EXPECT_FALSE(diff.empty());
  EXPECT_EQ(3u, diff.size());
  EXPECT_EQ(35, diff.get<Value::Scalar>("cpus")->value());
  EXPECT_EQ(3584, diff.get<Value::Scalar>("mem")->value());
  EXPECT_EQ(8192, diff.get<Value::Scalar>("disk")->value());
  EXPECT_EQ(1u, diff.count(disk));
  EXPECT_TRUE(diff.contains(disk));

  // Verify operator-= on Resources is the same as operator-.
  Resources r = r1;
  r -= r2;
  EXPECT_EQ(diff, r);

  // Verify that when all copies of shared resource is removed, that specific
  // shared resource is no longer contained in the Resources object.
  EXPECT_EQ(2u, r1.count(disk));
  EXPECT_TRUE(r1.contains(disk));
  EXPECT_EQ(1u, r2.count(disk));
  EXPECT_TRUE(r2.contains(disk));

  EXPECT_EQ(0u, (r1 - r2 - r2).count(disk));
  EXPECT_FALSE((r1 - r2 - r2).contains(disk));
  EXPECT_EQ(0u, (r2 - r1).count(disk));
  EXPECT_FALSE((r2 - r1).contains(disk));
}


TEST(SharedResourcesTest, ScalarSharedCompoundExpressions)
{
  // Shared persistent volume.
  Resource disk = createDiskResource(
      "50", "role1", "1", "path", None(), true);

  Resources r1 = Resources::parse("cpus:2;mem:10").get() +
    disk + disk + disk + disk;
  Resources r2 = Resources::parse("cpus:2;mem:10").get() + disk + disk + disk;

  EXPECT_EQ(4u, r1.count(disk));
  EXPECT_EQ(3u, r2.count(disk));

  // Verify multiple arithmetic operations on shared resources.
  EXPECT_EQ(r1 + r1 - r1, r1);
  EXPECT_EQ(r1 + r2 - r1, r2);
  EXPECT_EQ(r2 + r1 - r2, r1);
  EXPECT_EQ(r2 + r1 - r1, r2);
  EXPECT_EQ(r2 - r1 + r1, r1);
  EXPECT_EQ(r1 - r2 + r2, r1);

  // Verify subtraction of Resources when only shared counts vary.
  EXPECT_TRUE((r2 - r1).empty());
  EXPECT_FALSE((r1 - r2).empty());
}


// Verify shared counts on addition and subtraction of shared
// resources which differ in their scalar values.
TEST(SharedResourcesTest, ScalarNonEqualSharedOperations)
{
  // Shared persistent volumes.
  Resource disk1 = createDiskResource(
      "50", "role1", "1", "path1", None(), true);
  Resource disk2 = createDiskResource(
      "100", "role1", "2", "path2", None(), true);

  Resources r1 = Resources(disk1) + disk2;

  EXPECT_EQ(1u, r1.count(disk1));
  EXPECT_EQ(1u, r1.count(disk2));

  Resources r2 = Resources(disk1) + disk2 - disk1;

  EXPECT_EQ(0u, r2.count(disk1));
  EXPECT_EQ(1u, r2.count(disk2));

  // Cannot subtract nonequal shared resources.
  Resources r3 = Resources(disk1) - disk2;

  EXPECT_EQ(1u, r3.count(disk1));
  EXPECT_EQ(0u, r3.count(disk2));
}


// Verify addition and subtraction of similar resources which differ in
// their sharedness only.
TEST(SharedResourcesTest, ScalarSharedAndNonSharedOperations)
{
  Resource sharedDisk = createDiskResource(
      "100", "role1", "1", "path", None(), true);

  Resource nonSharedDisk = createDiskResource("100", "role1", "1", "path");

  Resources r1 = Resources::parse("cpus:1;mem:5").get() + sharedDisk;
  Resources r2 = Resources::parse("cpus:1;mem:5").get() + nonSharedDisk;

  // r1 and r2 don't contain each other because of sharedDisk and
  // nonSharedDisk's different sharedness.
  EXPECT_FALSE(r2.contains(r1));
  EXPECT_FALSE(r1.contains(r2));

  // Additions of resources with non-matching sharedness.
  Resources r3 = sharedDisk;
  r3 += sharedDisk;
  r3 += nonSharedDisk;

  EXPECT_FALSE(r3.empty());
  EXPECT_EQ(2u, r3.size());
  EXPECT_EQ(200, r3.get<Value::Scalar>("disk")->value());
  EXPECT_EQ(2u, r3.count(sharedDisk));
  EXPECT_EQ(1u, r3.count(nonSharedDisk));

  // Cannot subtract resources with non-matching sharedness.
  Resources r4 = nonSharedDisk;
  r4 -= sharedDisk;

  EXPECT_EQ(r4, nonSharedDisk);
}


// This test verifies that shared resources can be filtered.
TEST(SharedResourcesTest, Filter)
{
  Resources r1 = createDiskResource("10", "role1", "1", "path", None(), true);
  EXPECT_EQ(r1, r1.shared());
  EXPECT_TRUE(r1.nonShared().empty());

  Resources r2 = createDiskResource(
      "20", "role2", None(), None(), None(), false);

  EXPECT_TRUE(r2.shared().empty());
  EXPECT_EQ(r2, r2.nonShared());

  EXPECT_EQ(r1, (r1 + r2).shared());
  EXPECT_EQ(r2, (r1 + r2).nonShared());

  Resources resources = Resources::parse("cpus:1;mem:512;disk:1000").get();
  Resources sum = resources + r1 + r2;

  EXPECT_EQ(r1, sum.shared());
  EXPECT_EQ(resources + r2, sum.nonShared());
}


TEST(AllocatedResourcesTest, Equality)
{
  Resources cpus1 = Resources::parse("cpus", "1", "*").get();
  Resources cpus2 = Resources::parse("cpus", "1", "*").get();

  cpus1.allocate("role1");
  cpus2.allocate("role2");

  EXPECT_EQ(cpus1, cpus1);
  EXPECT_NE(cpus1, cpus2);
}


TEST(AllocatedResourcesTest, Contains)
{
  Resources cpus1 = Resources::parse("cpus", "1", "*").get();
  Resources cpus2 = Resources::parse("cpus", "1", "*").get();

  cpus1.allocate("role1");
  cpus2.allocate("role2");

  EXPECT_TRUE((cpus1 + cpus2).contains(cpus1));
  EXPECT_TRUE((cpus1 + cpus2).contains(cpus2));
}


TEST(AllocatedResourcesTest, Addition)
{
  Resources cpus1 = Resources::parse("cpus", "1", "*").get();
  Resources cpus2 = Resources::parse("cpus", "1", "*").get();

  cpus1.allocate("role1");
  cpus2.allocate("role2");

  EXPECT_EQ(2u, (cpus1 + cpus2).size());
  EXPECT_SOME_EQ(2.0, (cpus1 + Resources(cpus2)).cpus());
}


TEST(AllocatedResourcesTest, Subtraction)
{
  Resources cpus1 = Resources::parse("cpus", "1", "*").get();
  Resources cpus2 = Resources::parse("cpus", "1", "*").get();

  cpus1.allocate("role1");
  cpus2.allocate("role2");

  EXPECT_TRUE((cpus1 - cpus1).empty());
  EXPECT_TRUE((cpus2 - cpus2).empty());

  EXPECT_EQ(cpus1, cpus1 - cpus2);
  EXPECT_EQ(cpus2, cpus2 - cpus1);
}


TEST(AllocatedResourcesTest, Allocations)
{
  // Unreserved resources can be allocated to any role (including *).
  Resources cpus1 = Resources::parse("cpus", "1", "*").get();
  Resources mem1 = Resources::parse("mem", "1024", "*").get();

  cpus1.allocate("*");
  mem1.allocate("*");

  Resources cpus2 = Resources::parse("cpus", "2", "*").get();
  Resources mem2 = Resources::parse("mem", "2048", "*").get();

  cpus2.allocate("role1");
  mem2.allocate("role1");

  // Reserved resources are allocated to the reserved role.
  Resources cpus3 = Resources::parse("cpus", "3", "role2").get();
  Resources mem3 = Resources::parse("mem", "3096", "role2").get();

  cpus3.allocate("role2");
  mem3.allocate("role2");

  Resources resources = cpus1 + cpus2 + cpus3 + mem1 + mem2 + mem3;

  hashmap<string, Resources> allocations = resources.allocations();

  EXPECT_EQ(3u, allocations.size());
  EXPECT_EQ(cpus1 + mem1, allocations["*"]);
  EXPECT_EQ(cpus2 + mem2, allocations["role1"]);
  EXPECT_EQ(cpus3 + mem3, allocations["role2"]);

  // Test unallocation.
  cpus1.unallocate();

  Resource r = *cpus1.begin();
  EXPECT_FALSE(r.has_allocation_info());
}


struct ScalarArithmeticParameter
{
  Resources resources;
  size_t totalOperations;
};


class Resources_Scalar_Arithmetic_BENCHMARK_Test
  : public ::testing::Test,
    public ::testing::WithParamInterface<int>
{
protected:
  static vector<ScalarArithmeticParameter> parameters_;

public:
  static void SetUpTestCase()
  {
    // Test a typical vector of scalars.
    ScalarArithmeticParameter scalars;
    scalars.resources =
      Resources::parse("cpus:1;gpus:1;mem:128;disk:256").get();
    scalars.totalOperations = 50000;

    // Note that the benchmark incorrectly sums together
    // identity-based resources, because the allocator
    // incorrectly sums resources across slaves. In
    // particular, for identity based resources like sets
    // and range, this means that a+a = a rather than 2a.
    //
    // TODO(bmahler): As we introduce a notion of a
    // ResourceQuantity, we can disallow summation
    // of identical resources and update this benchmark
    // accordingly.

    // Test a large amount of unique reservations. This can
    // occur when aggregating across agents in a cluster.
    ScalarArithmeticParameter reservations;
    for (int i = 0; i < 1000; ++i) {
      Labels labels;

      Label* label = labels.add_labels();
      label->set_key("key_" + stringify(i));
      label->set_value("value_" + stringify(i));

      reservations.resources +=
        scalars.resources.pushReservation(createDynamicReservationInfo(
            stringify(i), "principal_" + stringify(i), labels));
    }
    reservations.totalOperations = 10;

    // Test a typical vector of scalars which include shared resources
    // (viz, shared persistent volumes).
    Resource disk = createDiskResource(
        "256", "test", "persistentId", "/volume", None(), true);

    ScalarArithmeticParameter shared;
    shared.resources = Resources::parse("cpus:1;mem:128").get() + disk;
    shared.totalOperations = 50000;

    parameters_.push_back(std::move(scalars));
    parameters_.push_back(std::move(reservations));
    parameters_.push_back(std::move(shared));
  }

  // Returns the 'Resources' parameters to run the benchmarks against.
  static Try<ScalarArithmeticParameter> parameters(int n)
  {
    if (n < 0 || n >= static_cast<int>(parameters_.size())) {
      return Error("Invalid parameter set");
    }

    return parameters_.at(n);
  }
};


vector<ScalarArithmeticParameter>
  Resources_Scalar_Arithmetic_BENCHMARK_Test::parameters_;


// The Resources benchmark tests are parameterized by the
// 'Resources' object to apply operations to, and the number
// of times to run the operation.
INSTANTIATE_TEST_CASE_P(
    ResourcesScalarArithmeticOperators,
    Resources_Scalar_Arithmetic_BENCHMARK_Test,
    ::testing::Range(0, 3));


static string abbreviate(string s, size_t max)
{
  string ellipses = "...";

  if (s.size() > max) {
    return s.substr(0, max-ellipses.size()) + "...";
  } else {
    return s;
  }
}


TEST_P(Resources_Scalar_Arithmetic_BENCHMARK_Test, Arithmetic)
{
  const ScalarArithmeticParameter parameter =
    CHECK_NOTERROR(parameters(GetParam()));

  const Resources& resources = parameter.resources;
  size_t totalOperations = parameter.totalOperations;

  Resources total;
  Stopwatch watch;

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    total += resources;
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'total += r' operations"
       << " on " << abbreviate(stringify(resources), 50) << endl;

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    total -= resources;
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'total -= r' operations"
       << " on " << abbreviate(stringify(resources), 50) << endl;

  ASSERT_TRUE(total.empty()) << total;

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    total = total + resources;
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'total = total + r' operations"
       << " on " << abbreviate(stringify(resources), 50) << endl;

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    total = total - resources;
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'total = total - r' operations"
       << " on " << abbreviate(stringify(resources), 50) << endl;

  ASSERT_TRUE(total.empty()) << total;
}


class Resources_Filter_BENCHMARK_Test : public ::testing::Test {};


TEST_F(Resources_Filter_BENCHMARK_Test, Filters)
{
  size_t totalOperations = 50000u;

  Resources nonRevocable =
    Resources::parse("cpus:1;gpus:1;mem:128;disk:256").get();

  Stopwatch watch;

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    nonRevocable.nonRevocable();
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'r.nonRevocable()' operations"
       << " on " << stringify(nonRevocable) << endl;

  Resources revocable = createRevocableResource("cpus", "1", "*", true);

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    revocable.revocable();
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'r.revocable()' operations"
       << " on " << stringify(revocable) << endl;

  Resources unReserved = nonRevocable;

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    unReserved.unreserved();
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'r.unreserved()' operations"
       << " on " << stringify(unReserved) << endl;

  Resources reserved = Resources::parse(
    "cpus(role):1;gpus(role):1;mem(role):128;disk(role):256").get();

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    reserved.reserved("role");
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations << " 'r.reserved(role)' operations"
       << " on " << stringify(reserved) << endl;
}


struct ContainsParameter
{
  Resources subset;
  Resources superset;
  size_t totalOperations;
};


class Resources_Contains_BENCHMARK_Test
  : public ::testing::Test,
    public ::testing::WithParamInterface<int>
{
public:
  static vector<ContainsParameter> parameters_;

  // Returns the 'Resources' parameters to run the `contains`
  // benchmarks against. This test will include three kind of
  // 'Resources' parameters: scalar, ranges and mixed
  // (scalar and ranges).
  //
  // NOTE: We do not execute this expensive function at test
  // instantiation time, but at test execution time so we only pay for
  // its cost when actually executing the test and not during global
  // test registration.
  //
  // TODO(bbannier): Break this function down into orthogonal pieces.
  static void SetUpTestCase()
  {
    // Test a typical vector of scalars, the superset contains
    // the subset for this case.
    ContainsParameter scalars1;
    scalars1.subset = Resources::parse("cpus:1;mem:128").get();
    scalars1.superset =
      Resources::parse("cpus:1;gpus:1;mem:128;disk:256").get();

    scalars1.totalOperations = 5000;

    // Test a typical vector of scalars, the superset does not
    // contains the subset for this case.
    ContainsParameter scalars2;
    scalars2.subset = scalars1.superset;
    scalars2.superset = scalars1.subset;
    scalars2.totalOperations = 5000;

    // Test a typical vector of scalars, the superset is same
    // as the subset for this case.
    ContainsParameter scalars3;
    scalars3.subset = scalars1.subset;
    scalars3.superset = scalars1.subset;
    scalars3.totalOperations = 5000;

    // TODO(bmahler): Increase the port range to [1-64,000] once
    // performance is improved such that this doesn't take a
    // long time to run.

    // Create a fragmented range for ports resources.
    Try<::mesos::Value::Ranges> range_ =
      fragment(createRange(1, 16000), 16000/2);

    // Test a typical vector of a fragment range of ports, the superset
    // contains the subset for this case.
    ContainsParameter range1;
    range1.subset = createPorts(range_.get());
    range1.superset = Resources::parse("ports", "[1-16000]", "*").get();
    range1.totalOperations = 100;

    // Test a typical vector of a fragment range of ports, the superset
    // does not contain the subset for this case.
    ContainsParameter range2;
    range2.subset = range1.superset;
    range2.superset = range1.subset;
    range2.totalOperations = 50;

    // Test a typical vector of a fragment range of ports, the superset
    // is same as the subset for this case.
    ContainsParameter range3;
    range3.subset = range1.subset;
    range3.superset = range1.subset;
    range3.totalOperations = 1;

    // Test mixed resources including both scalar and ports resources,
    // the superset contains the subset for this case.
    ContainsParameter mixed1;
    mixed1.subset = scalars1.subset + range1.subset;
    mixed1.superset = scalars1.superset + range1.superset;
    mixed1.totalOperations = 100;

    // Test mixed resources including both scalar and ports resources,
    // the superset contains the subset for this case.
    ContainsParameter mixed2;
    mixed2.subset = mixed1.superset;
    mixed2.superset = mixed1.subset;
    mixed2.totalOperations = 50;

    // Test mixed resources including both scalar and ports resources,
    // the superset is same as the subset for this case.
    ContainsParameter mixed3;
    mixed3.subset = mixed1.subset;
    mixed3.superset = mixed1.subset;
    mixed3.totalOperations = 1;

    parameters_.push_back(std::move(scalars1));
    parameters_.push_back(std::move(scalars2));
    parameters_.push_back(std::move(scalars3));
    parameters_.push_back(std::move(range1));
    parameters_.push_back(std::move(range2));
    parameters_.push_back(std::move(range3));
    parameters_.push_back(std::move(mixed1));
    parameters_.push_back(std::move(mixed2));
    parameters_.push_back(std::move(mixed3));
  }

  static Try<ContainsParameter> parameters(int n)
  {
    if (n < 0 || n >= static_cast<int>(parameters_.size())) {
      return Error("Invalid parameter set");
    }

    return parameters_.at(n);
  }
};


vector<ContainsParameter> Resources_Contains_BENCHMARK_Test::parameters_;


// The Resources `contains` benchmark tests are parameterized by
// the 'Resources' object to apply operations to.
INSTANTIATE_TEST_CASE_P(
    ResourcesContains,
    Resources_Contains_BENCHMARK_Test,
    ::testing::Range(0, 9));


TEST_P(Resources_Contains_BENCHMARK_Test, Contains)
{
  const ContainsParameter parameter = CHECK_NOTERROR(parameters(GetParam()));

  const Resources& subset = parameter.subset;
  const Resources& superset = parameter.superset;
  size_t totalOperations = parameter.totalOperations;

  Stopwatch watch;

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    superset.contains(subset);
  }
  watch.stop();

  cout << "Took " << watch.elapsed()
       << " to perform " << totalOperations
       << " 'superset.contains(subset)' operations on superset resources "
       << abbreviate(stringify(superset), 50)
       << " contains subset resources " << abbreviate(stringify(subset), 50)
       << endl;
}


class Resources_Parse_BENCHMARK_Test
  : public MesosTest,
    public ::testing::WithParamInterface<size_t> {};


INSTANTIATE_TEST_CASE_P(
    Resources_Parse,
    Resources_Parse_BENCHMARK_Test,
    ::testing::Values(1000U, 10000U, 50000U));


TEST_P(Resources_Parse_BENCHMARK_Test, Parse)
{
  const size_t iterationCount = GetParam();
  const size_t resourcesCount = 100;

  vector<string> rawResources;

  for (size_t i = 0; i < resourcesCount; i++) {
    rawResources.push_back("res" + stringify(i) + ":" + stringify(i));
  }

  string inputString = strings::join(";", rawResources);

  for (size_t i = 0; i < iterationCount; i++) {
    Try<Resources> resource = Resources::parse(inputString);
    EXPECT_SOME(resource);
  }
}


class Resources_Ranges_BENCHMARK_Test
  : public MesosTest,
    public ::testing::WithParamInterface<size_t> {};


// Size "100" here means 100 sub-ranges. We choose to parameterize on number of
// subranges because it's a dominant factor in the performance of range
// arithmetic operations.
INSTANTIATE_TEST_CASE_P(
    ResourcesRangesSizes,
    Resources_Ranges_BENCHMARK_Test,
    ::testing::Values(10U, 100U, 1000U));


// This test benchmarks the range arithmetic performance when the two
// range operands have partial overlappings.
TEST_P(Resources_Ranges_BENCHMARK_Test, ArithmeticOverlapping)
{
  const size_t totalOperations = 1000;

  // We construct `ports1` and `ports2` such that each of their
  // intervals partially overlaps with the other:
  // ports1 = [1-6, 11-16, 21-26, ..., 991-996] (100 sub-ranges of [1-996])
  // ports2 = [3-8, 13-18, 23-28, ..., 993-998] (100 sub-ranges of [1-998])
  Value::Ranges ranges1, ranges2;
  for (size_t i = 0, port1Index = 1, port2Index = 3, stride = 5; i < GetParam();
       i++) {
    *ranges1.add_range() = createRange(port1Index, port1Index + stride);
    *ranges2.add_range() = createRange(port2Index, port2Index + stride);

    port1Index += stride * 2;
    port2Index += stride * 2;
  }

  Resources ports1 = createPorts(ranges1);
  Resources ports2 = createPorts(ranges2);

  auto printResult = [&](const string& operation, const Duration& elapsedTime) {
    cout << "Took " << elapsedTime << " to perform " << totalOperations << " '"
         << operation << "' operations on " << abbreviate(stringify(ports1), 27)
         << ranges1.range(GetParam() - 1).begin() << "-"
         << ranges1.range(GetParam() - 1).end() << "] and "
         << abbreviate(stringify(ports2), 27) << ", "
         << ranges2.range(GetParam() - 1).begin() << "-"
         << ranges2.range(GetParam() - 1).end() << "] with " << GetParam()
         << " sub-ranges" << endl;
  };

  Resources result;

  Stopwatch watch;

  Duration elapsedTime;

  for (size_t i = 0; i < totalOperations; i++) {
    result = ports1;

    watch.start();
    result += ports2;
    watch.stop();

    elapsedTime += watch.elapsed();
  }

  printResult("a += b", elapsedTime);

  elapsedTime = Duration::zero();

  for (size_t i = 0; i < totalOperations; i++) {
    result = ports1;

    watch.start();
    result -= ports2;
    watch.stop();

    elapsedTime += watch.elapsed();
  }

  printResult("a -= b", elapsedTime);

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    result = ports1 + ports2;
  }
  watch.stop();

  printResult("a + b", watch.elapsed());

  watch.start();
  for (size_t i = 0; i < totalOperations; i++) {
    result = ports1 - ports2;
  }
  watch.stop();

  printResult("a - b", watch.elapsed());
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
