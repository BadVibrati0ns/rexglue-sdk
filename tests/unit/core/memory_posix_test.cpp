/**
 * @file        tests/unit/core/memory_posix_test.cpp
 * @brief       Unit tests for POSIX page-protection queries and file mappings
 *              (rex::memory::QueryProtect, Protect, CreateFileMappingHandle).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/platform.h>

#if REX_PLATFORM_LINUX

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include <rex/memory/utils.h>

using rex::memory::PageAccess;

TEST_CASE("QueryProtect reports the protection of the mapping holding an address",
          "[memory][posix]") {
  const size_t page = rex::memory::page_size();
  auto* base = static_cast<uint8_t*>(
      mmap(nullptr, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  REQUIRE(base != MAP_FAILED);
  REQUIRE(mprotect(base + page, page, PROT_READ) == 0);

  size_t length = 0;
  PageAccess access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::QueryProtect(base + page + 16, length, access));
  CHECK(access == PageAccess::kReadOnly);
  CHECK(length == page - 16);

  PageAccess old_access = PageAccess::kNoAccess;
  REQUIRE(rex::memory::Protect(base, page, PageAccess::kNoAccess, &old_access));
  CHECK(old_access == PageAccess::kReadWrite);
  REQUIRE(rex::memory::QueryProtect(base, length, access));
  CHECK(access == PageAccess::kNoAccess);

  munmap(base, page * 3);
  CHECK_FALSE(rex::memory::QueryProtect(base + page, length, access));
}

TEST_CASE("A file mapping leaves no name in /dev/shm", "[memory][posix]") {
  const std::filesystem::path path =
      "rex_unit_test_mapping_" + std::to_string(static_cast<long>(getpid()));
  auto handle = rex::memory::CreateFileMappingHandle(path, 1 << 16, PageAccess::kReadWrite, true);
  REQUIRE(handle != rex::memory::kFileMappingHandleInvalid);
  CHECK_FALSE(std::filesystem::exists("/dev/shm/" + path.string()));
  void* view = rex::memory::MapFileView(handle, nullptr, 1 << 16, PageAccess::kReadWrite, 0);
  REQUIRE(view != nullptr);
  static_cast<uint8_t*>(view)[100] = 42;
  CHECK(static_cast<uint8_t*>(view)[100] == 42);
  rex::memory::UnmapFileView(handle, view, 1 << 16);
  rex::memory::CloseFileMappingHandle(handle, path);
}

#endif  // REX_PLATFORM_LINUX
