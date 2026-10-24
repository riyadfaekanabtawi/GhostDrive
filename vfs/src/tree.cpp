#include "ghostdrive/vfs/tree.hpp"

#include <mutex>

namespace ghostdrive::vfs {

namespace {

constexpr std::size_t kRefreshBatch = 1000;
constexpr std::uint32_t kDirPerm = 0755;
constexpr std::uint32_t kDefaultFilePerm = 0644;

std::vector<std::string_view> split(std::string_view path) {
    std::vector<std::string_view> parts;
    while (!path.empty()) {
        auto slash = path.find('/');
        parts.push_back(path.substr(0, slash));
        if (slash == std::string_view::npos) break;
        path.remove_prefix(slash + 1);
    }
    return parts;
}

}  // namespace

Tree::Tree() {
    nodes_[kRootIno] = Node{.parent = kRootIno, .type = NodeType::directory};
}

core::Result<std::size_t> Tree::refresh(const core::MetadataIndex& index) {
    std::size_t applied = 0;
    for (;;) {
        auto records = index.records_since(applied_sequence(), kRefreshBatch);
        if (!records) return records.error();
        if (records.value().empty()) return applied;
        std::unique_lock lock(mutex_);
        for (const auto& record : records.value()) {
            if (record.deleted) {
                remove_locked(record.path);
            } else {
                upsert_locked(record);
            }
            applied_sequence_ = std::max(applied_sequence_, record.sequence);
            ++applied;
        }
    }
}

void Tree::apply(const core::FileRecord& record) {
    std::unique_lock lock(mutex_);
    if (record.deleted) {
        remove_locked(record.path);
    } else {
        upsert_locked(record);
    }
    applied_sequence_ = std::max(applied_sequence_, record.sequence);
}

void Tree::upsert_locked(const core::FileRecord& record) {
    // The index only holds validated paths; this is a second line of defence for the kernel.
    if (!core::is_valid_drive_path(record.path)) return;
    auto parts = split(record.path);

    // Check the whole path first so a rejected record leaves no new directories behind.
    Ino cursor = kRootIno;
    std::size_t existing = 0;
    for (; existing < parts.size(); ++existing) {
        const auto& node = nodes_.at(cursor);
        auto it = node.children.find(parts[existing]);
        if (it == node.children.end()) break;
        const auto& child = nodes_.at(it->second);
        bool last = existing + 1 == parts.size();
        if (last ? child.type != NodeType::file : child.type != NodeType::directory) return;
        cursor = it->second;
    }

    for (std::size_t i = existing; i < parts.size(); ++i) {
        bool last = i + 1 == parts.size();
        Ino ino = next_ino_++;
        nodes_[ino] = Node{.parent = cursor,
                           .name = std::string(parts[i]),
                           .type = last ? NodeType::file : NodeType::directory};
        nodes_.at(cursor).children.emplace(std::string(parts[i]), ino);
        cursor = ino;
    }

    auto& file = nodes_.at(cursor);
    file.record = record;
    file.mtime_ns = record.mtime_ns;
    for (Ino dir = file.parent;; dir = nodes_.at(dir).parent) {
        auto& node = nodes_.at(dir);
        node.mtime_ns = std::max(node.mtime_ns, record.mtime_ns);
        if (dir == kRootIno) break;
    }
}

void Tree::remove_locked(std::string_view path) {
    auto ino = resolve_locked(path);
    if (!ino || nodes_.at(*ino).type != NodeType::file) return;
    Ino parent = nodes_.at(*ino).parent;
    nodes_.at(parent).children.erase(nodes_.at(*ino).name);
    nodes_.erase(*ino);
    // Directories exist only to hold files, so drop the ones this leaves empty.
    while (parent != kRootIno && nodes_.at(parent).children.empty()) {
        Ino grandparent = nodes_.at(parent).parent;
        nodes_.at(grandparent).children.erase(nodes_.at(parent).name);
        nodes_.erase(parent);
        parent = grandparent;
    }
}

std::optional<Ino> Tree::resolve_locked(std::string_view path) const {
    Ino cursor = kRootIno;
    if (path.empty()) return cursor;
    for (auto part : split(path)) {
        const auto& node = nodes_.at(cursor);
        auto it = node.children.find(part);
        if (it == node.children.end()) return std::nullopt;
        cursor = it->second;
    }
    return cursor;
}

Attr Tree::attr_locked(Ino ino, const Node& node) const {
    Attr attr;
    attr.ino = ino;
    attr.type = node.type;
    attr.mtime_ns = node.mtime_ns;
    if (node.type == NodeType::directory) {
        attr.perm = kDirPerm;
        std::uint32_t subdirs = 0;
        for (const auto& [name, child] : node.children) {
            if (nodes_.at(child).type == NodeType::directory) ++subdirs;
        }
        attr.nlink = 2 + subdirs;
    } else {
        attr.perm = node.record->mode & 07777;
        if (attr.perm == 0) attr.perm = kDefaultFilePerm;
        attr.size = node.record->size;
        attr.nlink = 1;
    }
    return attr;
}

std::optional<Ino> Tree::lookup(Ino parent, std::string_view name) const {
    std::shared_lock lock(mutex_);
    auto it = nodes_.find(parent);
    if (it == nodes_.end() || it->second.type != NodeType::directory) return std::nullopt;
    auto child = it->second.children.find(name);
    if (child == it->second.children.end()) return std::nullopt;
    return child->second;
}

std::optional<Ino> Tree::resolve(std::string_view path) const {
    std::shared_lock lock(mutex_);
    return resolve_locked(path);
}

std::optional<Attr> Tree::getattr(Ino ino) const {
    std::shared_lock lock(mutex_);
    auto it = nodes_.find(ino);
    if (it == nodes_.end()) return std::nullopt;
    return attr_locked(ino, it->second);
}

std::optional<std::vector<DirEntry>> Tree::readdir(Ino ino) const {
    std::shared_lock lock(mutex_);
    auto it = nodes_.find(ino);
    if (it == nodes_.end() || it->second.type != NodeType::directory) return std::nullopt;
    std::vector<DirEntry> entries;
    entries.reserve(it->second.children.size());
    for (const auto& [name, child] : it->second.children) {
        entries.push_back(DirEntry{name, child, nodes_.at(child).type});
    }
    return entries;
}

std::optional<core::FileRecord> Tree::file_record(Ino ino) const {
    std::shared_lock lock(mutex_);
    auto it = nodes_.find(ino);
    if (it == nodes_.end() || it->second.type != NodeType::file) return std::nullopt;
    return it->second.record;
}

std::uint64_t Tree::applied_sequence() const {
    std::shared_lock lock(mutex_);
    return applied_sequence_;
}

}  // namespace ghostdrive::vfs
