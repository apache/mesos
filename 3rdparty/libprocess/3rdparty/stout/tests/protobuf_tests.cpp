#include <string>

#include <gtest/gtest.h>

#include <gmock/gmock.h>

#include <stout/gtest.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>

#include "protobuf_tests.pb.h"

using std::string;

TEST(ProtobufTest, JSON)
{
  tests::Message message;
  message.set_b(true);
  message.set_str("string");
  message.set_bytes("bytes");
  message.set_int32(-1);
  message.set_int64(-1);
  message.set_uint32(1);
  message.set_uint64(1);
  message.set_sint32(-1);
  message.set_sint64(-1);
  message.set_f(1.0);
  message.set_d(1.0);
  message.set_e(tests::ONE);
  message.mutable_nested()->set_str("nested");
  message.add_repeated_bool(true);
  message.add_repeated_string("repeated_string");
  message.add_repeated_bytes("repeated_bytes");
  message.add_repeated_int32(-2);
  message.add_repeated_int64(-2);
  message.add_repeated_uint32(2);
  message.add_repeated_uint64(2);
  message.add_repeated_sint32(-2);
  message.add_repeated_sint64(-2);
  message.add_repeated_float(1.0);
  message.add_repeated_double(1.0);
  message.add_repeated_double(2.0);
  message.add_repeated_enum(tests::TWO);
  message.add_repeated_nested()->set_str("repeated_nested");

  // TODO(bmahler): To dynamically generate a protobuf message,
  // see the commented-out code below.
//  DescriptorProto proto;
//
//  proto.set_name("Message");
//
//  FieldDescriptorProto* field = proto.add_field();
//  field->set_name("str");
//  field->set_type(FieldDescriptorProto::TYPE_STRING);
//
//  const Descriptor* descriptor = proto.descriptor();
//
//  DynamicMessageFactory factory;
//  Message* message = factory.GetPrototype(descriptor);
//
//  Reflection* message.getReflection();

  // The keys are in alphabetical order.
  string expected = strings::remove(
      "{"
      "  \"b\": true,"
      "  \"bytes\": \"bytes\","
      "  \"d\": 1,"
      "  \"e\": \"ONE\","
      "  \"f\": 1,"
      "  \"int32\": -1,"
      "  \"int64\": -1,"
      "  \"nested\": { \"str\": \"nested\"},"
      "  \"optional_default\": 42,"
      "  \"repeated_bool\": [true],"
      "  \"repeated_bytes\": [\"repeated_bytes\"],"
      "  \"repeated_double\": [1, 2],"
      "  \"repeated_enum\": [\"TWO\"],"
      "  \"repeated_float\": [1],"
      "  \"repeated_int32\": [-2],"
      "  \"repeated_int64\": [-2],"
      "  \"repeated_nested\": [ { \"str\": \"repeated_nested\" } ],"
      "  \"repeated_sint32\": [-2],"
      "  \"repeated_sint64\": [-2],"
      "  \"repeated_string\": [\"repeated_string\"],"
      "  \"repeated_uint32\": [2],"
      "  \"repeated_uint64\": [2],"
      "  \"sint32\": -1,"
      "  \"sint64\": -1,"
      "  \"str\": \"string\","
      "  \"uint32\": 1,"
      "  \"uint64\": 1"
      "}",
      " ");

  JSON::Object object = JSON::Protobuf(message);

  EXPECT_EQ(expected, stringify(object));

  // Test parsing too.
  Try<tests::Message> parse = protobuf::parse<tests::Message>(object);
  ASSERT_SOME(parse);

  EXPECT_EQ(object, JSON::Protobuf(parse.get()));
}
