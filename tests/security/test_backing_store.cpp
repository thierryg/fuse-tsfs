// =============================================================================
// TSFS - Time-aware Storage Filesystem for Linux
// -----------------------------------------------------------------------------
// File    : tests/security/test_backing_store.cpp
// Purpose : Path resolution security: traversal, symlink escapes, magic links (openat2 and fallback)
// Author  : Thierry Gayet <thierry.gayet@labworks.fr>
// Project : tsfs (version: VERSION)
// Copyright (c) 2026 Thierry Gayet. SPDX-License-Identifier: 0BSD
// =============================================================================
#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>

#include "fsutil/backing_store.hpp"
#include "fsutil/entry_stat.hpp"
#include "support/test_support.hpp"

namespace tsfs {
namespace {

namespace fs = std::filesystem;

/// Every test runs twice: kernel openat2 resolution and the portable fallback walk.
class BackingStoreTest : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        fs::create_directories(root_ / "dir/sub");
        fs::create_directories(outside_);
        test::write_file(root_ / "dir/sub/file", "inside");
        test::write_file(outside_ / "secret", "outside");
        fs::create_directory_symlink(outside_, root_ / "abs-dir-link");
        fs::create_directory_symlink("../outside", root_ / "rel-dir-link");
        fs::create_directory_symlink("sub", root_ / "dir/inner-link");
        fs::create_symlink(outside_ / "secret", root_ / "file-link");
        fs::create_directory_symlink("/proc/self/root", root_ / "magic");
        auto store = BackingStore::open(root_.string(), {true, false, /*force_fallback=*/GetParam()});
        ASSERT_TRUE(store.has_value()) << store.error().message;
        store_ = std::move(*store);
        if (!GetParam() && !store_->uses_openat2()) {
            GTEST_SKIP() << "kernel without openat2: covered by the fallback variant";
        }
    }

    test::TempDir dir_{"tsfs-sec"};
    fs::path root_{dir_ / "root"};
    fs::path outside_{dir_ / "outside"};
    std::unique_ptr<BackingStore> store_;
};

TEST_P(BackingStoreTest, ResolvesOrdinaryPaths) {
    auto parent = store_->open_parent("/dir/sub/file");
    ASSERT_TRUE(parent.has_value()) << parent.error().message;
    EXPECT_EQ(parent->name, "file");
    auto st = stat_entry(parent->directory.get(), parent->name.c_str(), false);
    ASSERT_TRUE(st.has_value());
    EXPECT_TRUE(st->is_regular());
    EXPECT_TRUE(store_->open_directory("/dir").has_value());
    EXPECT_TRUE(store_->open_directory("/").has_value());
    auto root = store_->open_parent("/");
    ASSERT_TRUE(root.has_value());
    EXPECT_EQ(root->name, ".");
}

TEST_P(BackingStoreTest, RejectsNonNormalizedPaths) {
    for (const char* bad : {"", "relative", "/dir/../..", "/dir/./sub", "//dir", "/dir/"}) {
        EXPECT_FALSE(store_->open_parent(bad).has_value()) << bad;
        EXPECT_FALSE(store_->open_directory(bad).has_value()) << bad;
    }
}

TEST_P(BackingStoreTest, NeverFollowsSymlinksInIntermediateComponents) {
    for (const char* through :
         {"/abs-dir-link/secret", "/rel-dir-link/secret", "/dir/inner-link/file", "/magic/etc/hostname"}) {
        auto parent = store_->open_parent(through);
        EXPECT_FALSE(parent.has_value()) << through;
        if (!parent) {
            EXPECT_TRUE(parent.error().code == ELOOP || parent.error().code == ENOTDIR || parent.error().code == EXDEV)
                << through << ": " << parent.error().message;
        }
    }
}

TEST_P(BackingStoreTest, NeverOpensASymlinkAsADirectory) {
    EXPECT_FALSE(store_->open_directory("/abs-dir-link").has_value());
    EXPECT_FALSE(store_->open_directory("/magic").has_value());
}

TEST_P(BackingStoreTest, FinalSymlinkIsSeenAsALinkNotItsTarget) {
    auto parent = store_->open_parent("/file-link");
    ASSERT_TRUE(parent.has_value());
    auto st = stat_entry(parent->directory.get(), parent->name.c_str(), false);
    ASSERT_TRUE(st.has_value());
    EXPECT_TRUE(S_ISLNK(st->mode));
}

TEST_P(BackingStoreTest, SwappedDirectoryIsDetectedAtResolutionTime) {
    auto before = store_->open_parent("/dir/sub/file");
    ASSERT_TRUE(before.has_value());
    fs::rename(root_ / "dir", root_ / "dir.moved");
    fs::create_directory_symlink(outside_, root_ / "dir");
    EXPECT_FALSE(store_->open_parent("/dir/sub/file").has_value());
}

INSTANTIATE_TEST_SUITE_P(Resolvers, BackingStoreTest, ::testing::Values(false, true),
                         [](const auto& param_info) { return param_info.param ? "fallback" : "openat2"; });

TEST(BackingStore, MissingRootIsReported) {
    const auto r = BackingStore::open("/nonexistent/tsfs/root", {});
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ENOENT);
}

TEST(BackingStore, KernelSupportProbe) {
    std::cout << "[ INFO     ] openat2 available: " << (kernel_supports_openat2() ? "yes" : "no") << '\n';
    SUCCEED();
}

}  // namespace
}  // namespace tsfs
