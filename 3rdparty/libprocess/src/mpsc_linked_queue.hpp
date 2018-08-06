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

#ifndef __MPSC_LINKED_QUEUE_HPP__
#define __MPSC_LINKED_QUEUE_HPP__

#include <atomic>
#include <functional>

#include <glog/logging.h>

namespace process {

// This queue is a C++ port of the MpscLinkedQueue of JCTools, but limited to
// the core methods:
// https://github.com/JCTools/JCTools/blob/master/jctools-core/src/main/java/org/jctools/queues/MpscLinkedQueue.java
//
// which is a Java port of the MPSC algorithm as presented in following article:
// http://www.1024cores.net/home/lock-free-algorithms/queues/non-intrusive-mpsc-node-based-queue
//
// The queue has following properties:
//   Producers are wait-free (one atomic exchange per enqueue)
//   Consumer is
//     - lock-free
//     - mostly wait-free, except when consumer reaches the end of the queue
//       and producer enqueued a new node, but did not update the next pointer
//       on the old node, yet
template <typename T>
class MpscLinkedQueue
{
private:
  template <typename E>
  struct Node
  {
  public:
    explicit Node(E* element = nullptr) : element(element) {}

    E* element;
    std::atomic<Node<E>*> next = ATOMIC_VAR_INIT(nullptr);
  };

public:
  MpscLinkedQueue()
  {
    tail = new Node<T>();
    head.store(tail);
  }

  ~MpscLinkedQueue()
  {
    while (auto element = dequeue()) {
      delete element;
    }

    delete tail;
  }

  // Multi producer safe.
  void enqueue(T* element)
  {
    // A `nullptr` is used to denote an empty queue when doing a
    // `dequeue()` so producers can't use it as an element.
    CHECK_NOTNULL(element);

    auto newNode = new Node<T>(element);

    // Exchange is guaranteed to only give the old value to one
    // producer, so this is safe and wait-free.
    auto oldhead = head.exchange(newNode, std::memory_order_acq_rel);

    // At this point if this thread context switches out we may block
    // the consumer from doing a dequeue (see below). Eventually we'll
    // unblock the consumer once we run again and execute the next
    // line of code.
    oldhead->next.store(newNode, std::memory_order_release);
  }

  // Single consumer only.
  T* dequeue()
  {
    auto currentTail = tail;

    // Check and see if there is an actual element linked from `tail`
    // since we use `tail` as a "stub" rather than the actual element.
    auto nextTail = currentTail->next.load(std::memory_order_acquire);

    // There are three possible cases here:
    //
    // (1) The queue is empty.
    // (2) The queue appears empty but a producer is still enqueuing
    //     so let's wait for it and then dequeue.
    // (3) We have something to dequeue.
    //
    // Start by checking if the queue is or appears empty.
    if (nextTail == nullptr) {
      // Now check if the queue is actually empty or just appears
      // empty. If it's actually empty then return `nullptr` to denote
      // emptiness.
      if (head.load(std::memory_order_relaxed) == tail) {
        return nullptr;
      }

      // Another thread already inserted a new node, but did not
      // connect it to the tail, yet, so we spin-wait. At this point
      // we are not wait-free anymore.
      do {
        nextTail = currentTail->next.load(std::memory_order_acquire);
      } while (nextTail == nullptr);
    }

    CHECK_NOTNULL(nextTail);

    // Set next pointer of current tail to null to disconnect it
    // from the queue.
    currentTail->next.store(nullptr, std::memory_order_release);

    auto element = nextTail->element;
    nextTail->element = nullptr;

    tail = nextTail;
    delete currentTail;

    return element;
  }

  // Single consumer only.
  //
  // TODO(drexin): Provide C++ style iteration so someone can just use
  // the `std::for_each()`.
  template <typename F>
  void for_each(F&& f)
  {
    auto end = head.load();
    auto node = tail;

    for (;;) {
      node = node->next.load();

      // We are following the linked structure until we reach the end
      // node. There is a race with new nodes being added, so we limit
      // the traversal to the last node at the time we started.
      if (node == nullptr) {
        return;
      }

      f(node->element);

      if (node == end) {
        return;
      }
    }
  }

  // Single consumer only.
  bool empty()
  {
    return tail->next.load(std::memory_order_relaxed) == nullptr &&
      head.load(std::memory_order_relaxed) == tail;
  }

private:
  // TODO(drexin): Programatically get the cache line size.
  //
  // We align head to 64 bytes (x86 cache line size) to guarantee
  // it to be put on a new cache line. This is to prevent false
  // sharing with other objects that could otherwise end up on
  // the same cache line as this queue. We also align tail to
  // avoid false sharing with head and add padding after tail
  // to avoid false sharing with other objects.
  alignas(64) std::atomic<Node<T>*> head;
  alignas(64) Node<T>* tail;
  char pad[64 - sizeof(Node<T>*)];
};

} // namespace process {

#endif // __MPSC_LINKED_QUEUE_HPP__
