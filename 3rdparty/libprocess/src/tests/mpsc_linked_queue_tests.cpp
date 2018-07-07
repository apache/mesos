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

#include <thread>

#include <stout/gtest.hpp>
#include <stout/stringify.hpp>

#include "mpsc_linked_queue.hpp"


TEST(MpscLinkedQueueTest, EnqueueDequeue)
{
  process::MpscLinkedQueue<std::string> q;
  std::string* s = new std::string("test");
  q.enqueue(s);
  std::string* s2 = q.dequeue();
  ASSERT_EQ(s, s2);
  delete s2;
}


TEST(MpscLinkedQueueTest, EnqueueDequeueMultiple)
{
  process::MpscLinkedQueue<std::string> q;
  for (int i = 0; i < 20; i++) {
    q.enqueue(new std::string(stringify(i)));
  }

  for (int i = 0; i < 20; i++) {
    std::string* s = q.dequeue();
    ASSERT_EQ(*s, stringify(i));
    delete s;
  }
}


TEST(MpscLinkedQueueTest, EnqueueDequeueMultithreaded)
{
  process::MpscLinkedQueue<std::string> q;
  std::vector<std::thread> threads;
  for (int t = 0; t < 5; t++) {
    threads.push_back(
        std::thread([t, &q]() {
          int start = t * 1000;
          int end = start + 1000;
          for (int i = start; i < end; i++) {
            q.enqueue(new std::string(stringify(i)));
          }
        }));
  }

  std::for_each(threads.begin(), threads.end(), [](std::thread& t) {
    t.join();
  });

  std::set<std::string> elements;

  std::string* s = nullptr;
  while ((s = q.dequeue()) != nullptr) {
    elements.insert(*s);
  }

  ASSERT_EQ(5000UL, elements.size());

  for (int i = 0; i < 5000; i++) {
    ASSERT_NE(elements.end(), elements.find(stringify(i)));
  }
}


TEST(MpscLinkedQueueTest, ForEach)
{
  process::MpscLinkedQueue<std::string> q;
  for (int i = 0; i < 20; i++) {
    q.enqueue(new std::string(stringify(i)));
  }
  int i = 0;
  q.for_each([&](std::string* s) {
    ASSERT_EQ(*s, stringify(i++));
  });
}


TEST(MpscLinkedQueueTest, Empty)
{
  process::MpscLinkedQueue<std::string> q;
  ASSERT_TRUE(q.empty());
  std::string* s = new std::string("test");
  q.enqueue(s);
  ASSERT_FALSE(q.empty());
  q.dequeue();
  ASSERT_TRUE(q.empty());
  delete s;
}
