// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stout/dynamiclibrary.hpp>
#include <stout/gtest.hpp>
#include <stout/some.hpp>

using std::string;

#ifdef __linux__
static const string valid_library_path = "libdl.so";
#elif defined(__FreeBSD__)
static const string valid_library_path = "libc.so.7";
#elif defined(__WINDOWS__)
static const string valid_library_path = "ntdll.dll";
#else
static const string valid_library_path = "libdl.dylib";
#endif

#ifdef __WINDOWS__
static const string valid_symbol = "NtOpenProcess";
#else
static const string valid_symbol = "dlopen";
#endif

static const string invalid_symbol = "InvalidSymbol";
static const string invalid_library_path = "InvalidLibraryPath";


// Successful `open`, load symbol, `close`.
TEST(DynamicLibraryTest, LoadKnownSymbol)
{
  DynamicLibrary dltest;

  EXPECT_SOME(dltest.open(valid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));
  EXPECT_SOME(dltest.close());
}


// Successful `open`, fail to load nonsense symbol, `close`.
TEST(DynamicLibraryTest, FailToLoadInvalidSymbol)
{
  DynamicLibrary dltest;

  EXPECT_SOME(dltest.open(valid_library_path));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));
  EXPECT_SOME(dltest.close());
}


// Verify that `loadSymbol` and `close` fail if we don't call `open` first.
TEST(DynamicLibraryTest, CloseAndLoadSymbolFailWithoutOpeningLib)
{
  DynamicLibrary dltest;

  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));
  EXPECT_ERROR(dltest.close());
}


// Verify we can open a library and load a symbol. Then close, verify that we
// can't load the symbol. Then open and verify we can load symbol again, just
// to be safe.
TEST(DynamicLibraryTest, VerifyClose)
{
  DynamicLibrary dltest;

  EXPECT_SOME(dltest.open(valid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));
  EXPECT_SOME(dltest.close());

  EXPECT_ERROR(dltest.loadSymbol(valid_symbol));

  EXPECT_SOME(dltest.open(valid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));
  EXPECT_SOME(dltest.close());
}


// Attempt to load invalid lib path, verify failure, as well as failures when
// we try to call `loadSymbol` and `close`.
TEST(DynamicLibraryTest, FailToLoadInvalidLibPath)
{
  DynamicLibrary dltest;

  EXPECT_ERROR(dltest.open(invalid_library_path));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));
  EXPECT_ERROR(dltest.close());
}


// Attempt to `open` invalid lib path _twice_, verify failure, then verify
// failure when we call `loadSymbol` and `close`.
TEST(DynamicLibraryTest, DoubleFailOpen)
{
  DynamicLibrary dltest;

  EXPECT_ERROR(dltest.open(invalid_library_path));
  EXPECT_ERROR(dltest.open(invalid_library_path));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));
  EXPECT_ERROR(dltest.close());
}


// `open` valid library, load symbol, then verify we fail when we try to open
// the library again; verify we can still load symbols from, and `close`, the
// original library.
TEST(DynamicLibraryTest, OpenSuccessThenOpenFail)
{
  DynamicLibrary dltest;

  // `open`, successfully load symbol.
  EXPECT_SOME(dltest.open(valid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));

  // `open` same path again, fail, but successfully load known symbol; fail to
  // load nonsense symbol.
  EXPECT_ERROR(dltest.open(valid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));

  // `open` different (invalid) path, fail, but successfully load known symbol;
  // fail to load nonsense symbol.
  EXPECT_ERROR(dltest.open(invalid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));

  // Finally, successfully `close`.
  EXPECT_SOME(dltest.close());
}


// Attempt to `open` invalid lib, verify we can't load symbols from it, then
// open valid lib, and verify we can load symbols from, and `close` it.
TEST(DynamicLibraryTest, OpenFailThenOpenSuccess)
{
  DynamicLibrary dltest;

  // Fail to `open`, then fail load nonsense symbols.
  EXPECT_ERROR(dltest.open(invalid_library_path));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));

  // `open` valid path, succeed, then successfully load known symbol; fail to
  // load nonsense symbol.
  EXPECT_SOME(dltest.open(valid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));

  // `open` different (invalid) path, fail, but successfully load known symbol;
  // fail to load nonsense symbol.
  EXPECT_ERROR(dltest.open(invalid_library_path));
  EXPECT_SOME(dltest.loadSymbol(valid_symbol));
  EXPECT_ERROR(dltest.loadSymbol(invalid_symbol));

  // Finally, successfully `close`.
  EXPECT_SOME(dltest.close());
}
