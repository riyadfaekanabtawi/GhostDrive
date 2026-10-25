#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace ghostdrive::vfs::detail {

// The key two names share when Windows treats them as the same name: uppercase by file system
// rules (LCMapStringEx with LCMAP_UPPERCASE and without LCMAP_LINGUISTIC_CASING, documented as
// the file system casing, for `locale`, which is LOCALE_NAME_INVARIANT except in tests). Null
// when the mapping fails: the raw name would not compare the way the kernel does, so callers
// treat it as a name that matches nothing.
std::optional<std::wstring> fold_name(std::wstring_view name, const wchar_t* locale);

}  // namespace ghostdrive::vfs::detail
