#pragma once

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

namespace ghostdrive::test {

// Per-test scratch directory under the build tree, removed when the test ends.
class TempDir {
public:
    TempDir() {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        path_ = std::filesystem::path(GHOSTDRIVE_TEST_TMPDIR) /
                (std::string(info->test_suite_name()) + "." + info->name());
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace ghostdrive::test
