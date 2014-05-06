/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stout/dynamiclibrary.hpp>
#include <stout/gtest.hpp>
#include <stout/some.hpp>

// Test that we can dynamically load the library, that loads libraries.
TEST(DynamicLibraryTest, LoadKnownSymbol)
{
  DynamicLibrary dltest;

#ifdef __linux__
  Try<Nothing> result = dltest.open("libdl.so");
#else
  Try<Nothing> result = dltest.open("libdl.dylib");
#endif

  EXPECT_SOME(result);

  Try<void*> symbol = dltest.loadSymbol("dlopen");

  EXPECT_SOME(symbol);

  result = dltest.close();
  EXPECT_SOME(result);
}
