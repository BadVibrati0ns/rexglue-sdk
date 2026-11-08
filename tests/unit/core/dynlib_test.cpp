/**
 * @file        tests/unit/core/dynlib_test.cpp
 * @brief       Unit tests for rex::platform::DynamicLibrary.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_LINUX

#include <catch2/catch_test_macros.hpp>

#include <rex/platform/dynlib.h>

TEST_CASE("LoadIfAlreadyLoaded finds only libraries already in the process", "[dynlib]") {
  rex::platform::DynamicLibrary library;
  CHECK(library.LoadIfAlreadyLoaded("libc.so.6"));
  CHECK(library.GetRawSymbol("malloc") != nullptr);
  CHECK_FALSE(library.LoadIfAlreadyLoaded("librex_unit_test_not_a_library.so"));
}

#endif  // REX_PLATFORM_LINUX
