#include <string>

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/queue.hpp>

using namespace process;

using std::string;

TEST(Queue, Block)
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


TEST(Queue, Noblock)
{
  Queue<string> q;

  // Doing a 'put' should cause a 'get' to be completed immediately.
  q.put("world hello");

  Future<string> get = q.get();

  EXPECT_TRUE(get.isReady());
  EXPECT_EQ("world hello", get.get());
}


TEST(Queue, Queue)
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
