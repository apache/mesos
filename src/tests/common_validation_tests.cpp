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

#include <limits>

#include <stout/gtest.hpp>

#include "common/validation.hpp"

namespace mesos {
namespace internal {
namespace tests {

TEST(CommonValidationTest, Volume)
{
  // One of the 'host_path', 'image' or 'source' must be set.
  {
    Volume volume;
    volume.set_container_path("container_path");
    volume.set_mode(Volume::RW);

    EXPECT_SOME_EQ(
        Error("Only one of them should be set: "
              "'host_path', 'image' and 'source'"),
        common::validation::validateVolume(volume));

    volume.set_host_path("host_path");
    volume.mutable_image();

    EXPECT_SOME_EQ(
        Error("Only one of them should be set: "
              "'host_path', 'image' and 'source'"),
        common::validation::validateVolume(volume));
  }

  // Source 'type' is compatible with the corresponding fields.
  {
    Volume volume;
    volume.set_container_path("container_path");
    volume.set_mode(Volume::RW);

    Volume::Source* source = volume.mutable_source();
    source->set_type(Volume::Source::DOCKER_VOLUME);

    EXPECT_SOME_EQ(
        Error("'source.docker_volume' is not set for "
              "DOCKER_VOLUME volume"),
        common::validation::validateVolume(volume));

    source->set_type(Volume::Source::HOST_PATH);

    EXPECT_SOME_EQ(
        Error("'source.host_path' is not set for "
              "HOST_PATH volume"),
        common::validation::validateVolume(volume));

    source->set_type(Volume::Source::SANDBOX_PATH);

    EXPECT_SOME_EQ(
        Error("'source.sandbox_path' is not set for "
              "SANDBOX_PATH volume"),
        common::validation::validateVolume(volume));

    source->set_type(Volume::Source::SECRET);

    EXPECT_SOME_EQ(
        Error("'source.secret' is not set for "
              "SECRET volume"),
        common::validation::validateVolume(volume));
  }
}


TEST(CommonValidationTest, OfferFilters)
{
  // Offer filters can be empty.
  EXPECT_NONE(common::validation::validateOfferFilters(OfferFilters()));

  {
    // Offer filters can have empty minimal allocatable resources to
    // communicate that any resource should be considered allocatable.
    OfferFilters offerFilters;
    offerFilters.mutable_min_allocatable_resources();
    EXPECT_NONE(common::validation::validateOfferFilters(offerFilters));
  }

  {
    // Minimal allocatable resources cannot have empty quantities.
    OfferFilters offerFilters;
    offerFilters.mutable_min_allocatable_resources()->add_quantities();
    EXPECT_SOME_EQ(
        Error("Resource quantities must contain at least one quantity"),
        common::validation::validateOfferFilters(offerFilters));
  }

  {
    // A resource quantity cannot be negative.
    Value::Scalar scalar;
    scalar.set_value(-2);

    OfferFilters offerFilters;
    offerFilters.mutable_min_allocatable_resources()
      ->add_quantities()
      ->mutable_quantities()
      ->insert({"cpus", scalar});
    EXPECT_SOME_EQ(
        Error(
            "Invalid resource quantity for 'cpus': "
            "Negative values not supported"),
        common::validation::validateOfferFilters(offerFilters));
  }

  {
    // A resource quantity must be a number.
    static_assert(
        std::numeric_limits<double>::has_quiet_NaN,
        "Expected double to have a quiet NaN representation");

    Value::Scalar scalar;
    scalar.set_value(std::numeric_limits<double>::quiet_NaN());

    OfferFilters offerFilters;
    offerFilters.mutable_min_allocatable_resources()
      ->add_quantities()
      ->mutable_quantities()
      ->insert({"cpus", scalar});
    EXPECT_SOME_EQ(
        Error("Invalid resource quantity for 'cpus': NaN not supported"),
        common::validation::validateOfferFilters(offerFilters));
  }

  {
    // A resource quantity cannot be (positive) infinite.
    static_assert(
        std::numeric_limits<double>::has_infinity,
        "Expected double to have a representation of infinity");

    Value::Scalar scalar;
    scalar.set_value(std::numeric_limits<double>::infinity());

    OfferFilters offerFilters;
    offerFilters.mutable_min_allocatable_resources()
      ->add_quantities()
      ->mutable_quantities()
      ->insert({"cpus", scalar});
    EXPECT_SOME_EQ(
        Error(
            "Invalid resource quantity for 'cpus': "
            "Infinite values not supported"),
        common::validation::validateOfferFilters(offerFilters));
  }

  {
    // A resource quantity can be zero.
    Value::Scalar scalar;
    scalar.set_value(0);

    OfferFilters offerFilters;
    offerFilters.mutable_min_allocatable_resources()
      ->add_quantities()
      ->mutable_quantities()
      ->insert({"cpus", scalar});
    EXPECT_NONE(common::validation::validateOfferFilters(offerFilters));
  }

  {
    // A resource quantity can be positive.
    Value::Scalar scalar;
    scalar.set_value(2);

    OfferFilters offerFilters;
    offerFilters.mutable_min_allocatable_resources()
      ->add_quantities()
      ->mutable_quantities()
      ->insert({"cpus", scalar});
    EXPECT_NONE(common::validation::validateOfferFilters(offerFilters));
  }
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
