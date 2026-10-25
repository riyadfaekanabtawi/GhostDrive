#include "ghostdrive/core/file.hpp"

#include <system_error>

namespace ghostdrive::core {

Status write_private_file(const std::filesystem::path& path, std::span<const std::byte> data) {
    auto tmp = path;
    tmp += ".tmp";
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    auto file = File::create_private(tmp);
    if (!file) return file.error();
    auto st = file.value().write_at(data, 0);
    if (st) st = file.value().sync();
    if (st) st = file.value().close();
    if (st) st = replace_file(tmp, path);
    if (!st) {
        (void)file.value().close();
        std::filesystem::remove(tmp, ec);  // never leave a stray copy of the secret behind
    }
    return st;
}

Result<Bytes> read_private_file(const std::filesystem::path& path) {
    auto file = File::open_private(path);
    if (!file) return file.error();
    auto size = file.value().size();
    if (!size) return size.error();
    Bytes data(static_cast<std::size_t>(size.value()));
    auto got = file.value().read_at(data, 0);
    if (!got) return got.error();
    if (got.value() != data.size()) return Error{Errc::io, "short read of " + path_utf8(path)};
    return data;
}

}  // namespace ghostdrive::core
