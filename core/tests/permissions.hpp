#pragma once

#include <gtest/gtest.h>

#ifdef _WIN32
// clang-format off
#include <windows.h>
#include <aclapi.h>
// clang-format on
#else
#include <sys/stat.h>
#endif

#include <filesystem>
#include <string>

#include "ghostdrive/core/file.hpp"

namespace ghostdrive::test {

// POSIX: the permission bits of `path` are exactly `mode`. Windows, where there are no mode
// bits: `path` is private to the current user (its owner and DACL; see core/file.hpp).
inline ::testing::AssertionResult has_private_mode(const std::filesystem::path& path,
                                                   unsigned mode) {
#ifdef _WIN32
    (void)mode;
    if (std::filesystem::is_directory(path)) {
        if (core::is_private_dir(path)) return ::testing::AssertionSuccess();
        return ::testing::AssertionFailure() << core::path_utf8(path) << " is not a private directory";
    }
    auto file = core::File::open_private(path);
    if (file) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure() << file.error().message;
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        return ::testing::AssertionFailure() << "cannot stat " << path.string();
    }
    const unsigned actual = st.st_mode & 0777;
    if (actual == mode) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << path.string() << " has mode " << std::oct << actual << ", not " << mode;
#endif
}

// Lets other users read `path`: chmod to `mode` on POSIX; on Windows, adds an entry granting
// Everyone read access to its DACL.
inline bool let_others_read(const std::filesystem::path& path, unsigned mode) {
#ifdef _WIN32
    (void)mode;
    PACL old_dacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr,
                                nullptr, &old_dacl, nullptr, &sd) != ERROR_SUCCESS) {
        return false;
    }
    SID_IDENTIFIER_AUTHORITY world = SECURITY_WORLD_SID_AUTHORITY;
    PSID everyone = nullptr;
    bool ok = false;
    if (::AllocateAndInitializeSid(&world, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone)) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = GENERIC_READ;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = NO_INHERITANCE;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_WELL_KNOWN_GROUP;
        access.Trustee.ptstrName = static_cast<LPWSTR>(everyone);
        PACL new_dacl = nullptr;
        if (::SetEntriesInAclW(1, &access, old_dacl, &new_dacl) == ERROR_SUCCESS) {
            std::wstring name = path.wstring();
            ok = ::SetNamedSecurityInfoW(name.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                                         nullptr, nullptr, new_dacl, nullptr) == ERROR_SUCCESS;
            ::LocalFree(new_dacl);
        }
        ::FreeSid(everyone);
    }
    ::LocalFree(sd);
    return ok;
#else
    return ::chmod(path.c_str(), static_cast<mode_t>(mode)) == 0;
#endif
}

}  // namespace ghostdrive::test
