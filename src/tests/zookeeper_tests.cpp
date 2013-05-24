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

#include <zookeeper.h>

#include <gmock/gmock.h>

#include <string>

#include <stout/strings.hpp>

#include "zookeeper/authentication.hpp"

#include "tests/zookeeper.hpp"

using namespace mesos::internal;
using namespace mesos::internal::tests;


TEST_F(ZooKeeperTest, Auth)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  authenticatedZk.create("/test",
                         "42",
                         zookeeper::EVERYONE_READ_CREATOR_ALL,
                         0,
                         NULL);
  ASSERT_ZK_GET("42", &authenticatedZk, "/test");

  ZooKeeper unauthenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  ASSERT_ZK_GET("42", &unauthenticatedZk, "/test");
  ASSERT_EQ(ZNOAUTH, unauthenticatedZk.set("/test", "", -1));

  ZooKeeper nonOwnerZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  nonOwnerZk.authenticate("digest", "non-owner:non-owner");
  ASSERT_ZK_GET("42", &nonOwnerZk, "/test");
  ASSERT_EQ(ZNOAUTH, nonOwnerZk.set("/test", "", -1));
}


TEST_F(ZooKeeperTest, Create)
{
  ZooKeeperTest::TestWatcher watcher;

  ZooKeeper authenticatedZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  authenticatedZk.authenticate("digest", "creator:creator");
  EXPECT_EQ(ZOK, authenticatedZk.create("/foo/bar",
                                        "",
                                        zookeeper::EVERYONE_READ_CREATOR_ALL,
                                        0,
                                        NULL,
                                        true));
  authenticatedZk.create("/foo/bar/baz",
                         "43",
                         zookeeper::EVERYONE_CREATE_AND_READ_CREATOR_ALL,
                         0,
                         NULL);
  ASSERT_ZK_GET("43", &authenticatedZk, "/foo/bar/baz");

  ZooKeeper nonOwnerZk(server->connectString(), NO_TIMEOUT, &watcher);
  watcher.awaitSessionEvent(ZOO_CONNECTED_STATE);
  nonOwnerZk.authenticate("digest", "non-owner:non-owner");
  EXPECT_EQ(ZNODEEXISTS, nonOwnerZk.create("/foo/bar/baz",
                                           "",
                                           zookeeper::EVERYONE_READ_CREATOR_ALL,
                                           0,
                                           NULL,
                                           true));
  EXPECT_EQ(ZOK, nonOwnerZk.create("/foo/bar/baz/bam",
                                   "44",
                                   zookeeper::EVERYONE_READ_CREATOR_ALL,
                                   0,
                                   NULL,
                                   true));
  ASSERT_ZK_GET("44", &nonOwnerZk, "/foo/bar/baz/bam");

  std::string result;
  EXPECT_EQ(ZOK, nonOwnerZk.create("/foo/bar/baz/",
                                   "",
                                   zookeeper::EVERYONE_READ_CREATOR_ALL,
                                   ZOO_SEQUENCE | ZOO_EPHEMERAL,
                                   &result,
                                   true));
  EXPECT_TRUE(strings::startsWith(result, "/foo/bar/baz/0"));
}
