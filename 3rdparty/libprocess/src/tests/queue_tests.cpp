// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#include <string>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/queue.hpp>

using process::Future;
using process::Queue;

using std::string;

TEST(QueueTest, Block)
{
  Queue<string> q;

  // A 'get' with an empty queue should block.
  Future<string> get = q.get();

  EXPECT_TRUE(get.isPending());

  // After putting something the 'get' should be completed.
  q.put("hello world");

  EXPECT_TRUE(get.isReady());
  EXPECT_EQ("hello world", get.get());
}


TEST(QueueTest, Noblock)
{
  Queue<string> q;

  // Doing a 'put' should cause a 'get' to be completed immediately.
  q.put("world hello");

  Future<string> get = q.get();

  EXPECT_TRUE(get.isReady());
  EXPECT_EQ("world hello", get.get());
}


TEST(QueueTest, Queue)
{
  Queue<string> q;

  // Multiple calls to 'get' should cause blocking until there have
  // been enough corresponding calls to 'put'.
  Future<string> get1 = q.get();
  Future<string> get2 = q.get();
  Future<string> get3 = q.get();

  EXPECT_TRUE(get1.isPending());
  EXPECT_TRUE(get2.isPending());
  EXPECT_TRUE(get3.isPending());

  q.put("hello");

  EXPECT_TRUE(get1.isReady());
  EXPECT_TRUE(get2.isPending());
  EXPECT_TRUE(get3.isPending());

  q.put("pretty");

  EXPECT_TRUE(get1.isReady());
  EXPECT_TRUE(get2.isReady());
  EXPECT_TRUE(get3.isPending());

  q.put("world");

  EXPECT_TRUE(get1.isReady());
  EXPECT_TRUE(get2.isReady());
  EXPECT_TRUE(get3.isReady());

  EXPECT_EQ("hello", get1.get());
  EXPECT_EQ("pretty", get2.get());
  EXPECT_EQ("world", get3.get());
}


TEST(QueueTest, Discard)
{
  Queue<string> q;

  Future<string> get1 = q.get();
  Future<string> get2 = q.get();

  EXPECT_TRUE(get1.isPending());
  EXPECT_TRUE(get2.isPending());

  get1.discard();

  EXPECT_TRUE(get1.isDiscarded());

  q.put("hello");

  EXPECT_TRUE(get2.isReady());
  EXPECT_EQ("hello", get2.get());
}
