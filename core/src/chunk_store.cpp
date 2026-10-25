#include "ghostdrive/core/chunk_store.hpp"

#include <system_error>

#include "ghostdrive/core/chunking.hpp"
#include "ghostdrive/core/file.hpp"

namespace ghostdrive::core {

namespace fs = std::filesystem;

namespace {

Error fs_error(std::string_view what, const fs::path& path, const std::error_code& ec) {
    return Error{Errc::io, std::string(what) + " " + path_utf8(path) + ": " + ec.message()};
}

}  // namespace

ChunkStore::ChunkStore(fs::path root, ChunkHasher hasher)
    : root_(std::move(root)), hasher_(std::move(hasher)) {}

Result<ChunkStore> ChunkStore::open(fs::path root, ChunkHasher hasher) {
    std::error_code ec;
    for (const fs::path& dir : {root / "objects", root / "tmp"}) {
        fs::create_directories(dir, ec);
        if (ec) return fs_error("create_directories", dir, ec);
    }
    if (auto st = make_private_dir(root); !st) return st.error();
    // Leftover temp files are from interrupted writes and were never renamed into place.
    for (const auto& entry : fs::directory_iterator(root / "tmp", ec)) {
        fs::remove(entry.path(), ec);
    }
    return ChunkStore(std::move(root), std::move(hasher));
}

fs::path ChunkStore::object_path(const ChunkId& id) const {
    const std::string hex = id.to_hex();
    return root_ / "objects" / hex.substr(2, 2) / hex;
}

Status ChunkStore::write_object(const ChunkId& id, std::span<const std::byte> data) {
    const fs::path target = object_path(id);
    std::error_code ec;
    if (fs::exists(target, ec)) return {};

    std::array<std::byte, 16> nonce{};
    if (auto st = random_bytes(nonce); !st) return st;
    const fs::path temp = root_ / "tmp" / to_hex(nonce);
    auto file = File::create_private(temp);
    if (!file) return file.error();
    auto st = file.value().write_at(data, 0);
    if (st) st = file.value().sync();
    if (st) st = file.value().close();
    if (!st) {
        (void)file.value().close();
        fs::remove(temp, ec);
        return st;
    }
    fs::create_directories(target.parent_path(), ec);
    if (ec) {
        fs::remove(temp, ec);
        return fs_error("create_directories", target.parent_path(), ec);
    }
    fs::rename(temp, target, ec);
    if (ec) {
        const Error err = fs_error("rename", target, ec);
        fs::remove(temp, ec);
        return err;
    }
    return {};
}

Result<ChunkId> ChunkStore::put(std::span<const std::byte> data) {
    const ChunkId id = hasher_.hash(data);
    if (auto st = write_object(id, data); !st) return st.error();
    return id;
}

Status ChunkStore::put_verified(const ChunkId& expected, std::span<const std::byte> data) {
    if (hasher_.hash(data) != expected) {
        return Error{Errc::corrupt, "chunk data does not match id " + expected.short_hex()};
    }
    return write_object(expected, data);
}

bool ChunkStore::contains(const ChunkId& id) const {
    std::error_code ec;
    return fs::exists(object_path(id), ec);
}

Result<std::uint64_t> ChunkStore::size(const ChunkId& id) const {
    const fs::path path = object_path(id);
    std::error_code ec;
    const auto bytes = fs::file_size(path, ec);
    if (ec) {
        if (ec == std::errc::no_such_file_or_directory) {
            return Error{Errc::not_found, "chunk " + id.short_hex() + " not found"};
        }
        return fs_error("stat", path, ec);
    }
    return static_cast<std::uint64_t>(bytes);
}

Result<std::size_t> ChunkStore::read(const ChunkId& id, std::uint64_t offset,
                                     std::span<std::byte> out) const {
    const fs::path path = object_path(id);
    auto file = File::open_read(path);
    if (!file) return file.error();
    // COPY: chunk file -> caller buffer (copy point C1 in docs/ARCHITECTURE.md section 4.1).
    return file.value().read_at(out, offset);
}

Result<Bytes> ChunkStore::read_verified(const ChunkId& id) const {
    const fs::path path = object_path(id);
    auto file = File::open_read(path);
    if (!file) return file.error();
    auto bytes = file.value().size();
    if (!bytes) return bytes.error();
    Bytes data(static_cast<std::size_t>(bytes.value()));
    // COPY: chunk file -> owned buffer, so the bytes that were hashed are the bytes returned.
    auto n = file.value().read_at(data, 0);
    if (!n) return n.error();
    data.resize(n.value());
    if (hasher_.hash(data) != id) {
        return Error{Errc::corrupt, "chunk " + id.short_hex() + " failed verification"};
    }
    return data;
}

Status ChunkStore::remove(const ChunkId& id) {
    const fs::path path = object_path(id);
    std::error_code ec;
    if (!fs::remove(path, ec)) {
        if (ec) return fs_error("remove", path, ec);
        return Error{Errc::not_found, "chunk " + id.short_hex() + " not found"};
    }
    return {};
}

Result<ImportedFile> ChunkStore::import_file(const fs::path& file) {
    auto source = File::open_read(file);
    if (!source) return source.error();
    auto bytes = source.value().size();
    if (!bytes) return bytes.error();

    ImportedFile imported;
    imported.size = bytes.value();
    imported.block_size = block_size_for(imported.size);
    imported.chunks.reserve(
        static_cast<std::size_t>(chunk_count(imported.size, imported.block_size)));

    Bytes buffer(imported.block_size);
    for (std::uint64_t offset = 0; offset < imported.size; offset += imported.block_size) {
        // COPY: source file -> chunk buffer, needed to hash and store the chunk.
        auto n = source.value().read_at(buffer, offset);
        if (!n) return n.error();
        if (n.value() == 0) {
            return Error{Errc::io, "file " + path_utf8(file) + " shrank while importing"};
        }
        auto id = put(std::span(buffer).first(n.value()));
        if (!id) return id.error();
        imported.chunks.push_back(id.value());
    }
    return imported;
}

}  // namespace ghostdrive::core
