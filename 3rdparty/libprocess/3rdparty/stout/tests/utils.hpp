#include <gtest/gtest.h>

#include <string>

#include <stout/gtest.hpp>
#include <stout/os.hpp>
#include <stout/try.hpp>

class TemporaryDirectoryTest : public ::testing::Test
{
protected:
  virtual void SetUp()
  {
    // Save the current working directory.
    cwd = os::getcwd();

    // Create a temporary directory for the test.
    Try<std::string> directory = os::mkdtemp();

    ASSERT_SOME(directory) << "Failed to mkdtemp";

    sandbox = directory.get();

    // Run the test out of the temporary directory we created.
    ASSERT_TRUE(os::chdir(sandbox.get()))
      << "Failed to chdir into '" << sandbox.get() << "'";
  }

  virtual void TearDown()
  {
    // Return to previous working directory and cleanup the sandbox.
    ASSERT_TRUE(os::chdir(cwd));

    if (sandbox.isSome()) {
      ASSERT_SOME(os::rmdir(sandbox.get()));
    }
  }

private:
  std::string cwd;
  Option<std::string> sandbox;
};
