#include <iostream>
#include <string>

#include <stout/os.hpp>

// This helper program takes a an expected user.
// Returns 0 if the current username equals the expected username.
// Returns 1 otherwise.
int main(int argc, char **argv) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <expected username>" << std::endl;
    return 1;
  }

  const std::string expected(argv[1]);

  Result<std::string> user = os::user();
  if (user.isSome() && user.get() == expected) {
      return 0;
  }

  return 1;
}
