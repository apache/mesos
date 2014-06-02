#include <pthread.h>
#include <unistd.h>

#include <vector>

#include <gtest/gtest.h>

#include <stout/gtest.hpp>
#include <stout/os.hpp>

#include <stout/os/setns.hpp>

using std::set;
using std::string;


static void* child(void* arg)
{
  // Newly created threads have PTHREAD_CANCEL_ENABLE and
  // PTHREAD_CANCEL_DEFERRED so they can be cancelled.
  while (true) { os::sleep(Seconds(1)); }
}


TEST(OsSetnsTest, setns)
{
  if (os::user() != "root") {
    return;
  }

  // Get all the available namespaces.
  set<string> namespaces = os::namespaces();

  foreach (const string& ns, namespaces) {
    if (ns == "pid") {
      EXPECT_ERROR(os::setns(::getpid(), ns));
    } else {
      EXPECT_SOME(os::setns(::getpid(), ns));
    }
  }

  // Do not allow multi-threaded environment.
  pthread_t pthread;
  ASSERT_EQ(0, pthread_create(&pthread, NULL, child, NULL));

  foreach (const string& ns, namespaces) {
    EXPECT_ERROR(os::setns(::getpid(), ns));
  }

  // Terminate the threads.
  EXPECT_EQ(0, pthread_cancel(pthread));
  EXPECT_EQ(0, pthread_join(pthread, NULL));
}
