#include "ghostdrive/core/file_record.hpp"

#include <array>
#include <cstdio>
#include <ctime>

#include "ghostdrive/core/bytes.hpp"
#include "ghostdrive/core/chunking.hpp"

namespace ghostdrive::core {

bool is_valid_drive_path(std::string_view path) noexcept {
    if (path.empty() || path.front() == '/' || path.back() == '/') return false;
    if (path.find('\0') != std::string_view::npos) return false;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

Status validate(const FileRecord& record) {
    if (!is_valid_drive_path(record.path)) {
        return Error{Errc::invalid_argument, "invalid drive path: " + record.path};
    }
    if (!record.origin_path.empty() &&
        (!is_valid_drive_path(record.origin_path) ||
         record.path != conflict_copy_path(record.origin_path, record.mtime_ns,
                                           record.modified_by))) {
        return Error{Errc::invalid_argument, "conflict copy at the wrong path: " + record.path};
    }
    if (record.deleted) {
        if (!record.chunks.empty()) {
            return Error{Errc::invalid_argument, "deleted record has chunks: " + record.path};
        }
        return {};
    }
    if (record.size > 0) {
        // Fixed power-of-two chunking (ARCHITECTURE.md section 11.3). Anything else would let a
        // peer's record dictate allocation sizes.
        const auto b = record.block_size;
        if (b < kMinBlockSize || b > kMaxBlockSize || (b & (b - 1)) != 0) {
            return Error{Errc::invalid_argument, "record has an invalid block size: " + record.path};
        }
    }
    if (record.chunks.size() != chunk_count(record.size, record.block_size)) {
        return Error{Errc::invalid_argument, "chunk list does not match size: " + record.path};
    }
    return {};
}

std::string conflict_copy_path(std::string_view path, std::int64_t mtime_ns, std::uint64_t device) {
    const std::size_t slash = path.rfind('/');
    const std::string_view dir = slash == std::string_view::npos ? "" : path.substr(0, slash + 1);
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    // A leading dot is a hidden file, not an extension.
    const std::size_t dot = name.rfind('.');
    const bool has_ext = dot != std::string_view::npos && dot != 0 && dot + 1 < name.size();
    const std::string_view stem = has_ext ? name.substr(0, dot) : name;
    const std::string_view ext = has_ext ? name.substr(dot) : "";

    const std::time_t seconds = static_cast<std::time_t>(mtime_ns / 1'000'000'000);
    std::tm utc{};
    gmtime_r(&seconds, &utc);
    std::array<char, 32> stamp{};
    std::snprintf(stamp.data(), stamp.size(), "%04d%02d%02d-%02d%02d%02d", utc.tm_year + 1900,
                  utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);

    std::array<std::byte, 8> id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::byte>(device >> (8 * (7 - i)));
    }
    const std::string device_tag = base32_encode(id).substr(0, 7);

    std::string out;
    out.reserve(path.size() + 40);
    out.append(dir).append(stem).append(".sync-conflict-").append(stamp.data());
    out.append("-").append(device_tag).append(ext);
    return out;
}

Bytes record_signed_bytes(const DriveId& drive, const FileRecord& record) {
    constexpr std::string_view kContext = "ghostdrive-file-record-v1";
    const std::string& path = record.origin_path.empty() ? record.path : record.origin_path;
    const Bytes version = record.version.serialize();
    Bytes out;
    out.reserve(kContext.size() + drive.size() + path.size() + version.size() + 64 +
                record.chunks.size() * kChunkIdSize);
    auto put = [&out](std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out.push_back(static_cast<std::byte>(v >> (8 * i)));
    };
    const auto context = as_bytes(kContext);
    out.insert(out.end(), context.begin(), context.end());
    out.insert(out.end(), drive.begin(), drive.end());
    put(path.size(), 4);
    const auto path_bytes = as_bytes(path);
    out.insert(out.end(), path_bytes.begin(), path_bytes.end());
    put(record.size, 8);
    put(record.mode, 4);
    put(static_cast<std::uint64_t>(record.mtime_ns), 8);
    put(record.hlc, 8);
    out.insert(out.end(), version.begin(), version.end());
    put(record.modified_by, 8);
    put(record.block_size, 4);
    put(static_cast<std::uint8_t>(record.chunking), 1);
    put(record.deleted ? 1 : 0, 1);
    put(record.chunks.size(), 4);
    for (const auto& id : record.chunks) {
        const auto bytes = id.to_bytes();  // algorithm tag, then digest
        out.insert(out.end(), bytes.begin(), bytes.end());
    }
    return out;
}

Bytes countersign_bytes(const DriveId& drive, const FileRecord& record) {
    constexpr std::string_view kContext = "ghostdrive-file-record-adoption-v1";
    const auto context = as_bytes(kContext);
    Bytes out(context.begin(), context.end());
    const Bytes signed_bytes = record_signed_bytes(drive, record);
    out.insert(out.end(), signed_bytes.begin(), signed_bytes.end());
    const Signature signature = record.signature.value_or(Signature{});
    out.insert(out.end(), signature.begin(), signature.end());
    return out;
}

}  // namespace ghostdrive::core
