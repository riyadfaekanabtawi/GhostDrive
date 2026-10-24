#include "ghostdrive/vfs/tree.hpp"

#include <algorithm>
#include <chrono>
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

std::string_view last_component(std::string_view path) {
    auto slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
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

std::optional<Ino> Tree::parent_for_locked(std::string_view path, bool create) {
    auto parts = split(path);
    if (parts.empty()) return std::nullopt;
    // Check the whole way first so a rejected path leaves no new directories behind.
    Ino cursor = kRootIno;
    std::size_t existing = 0;
    for (; existing + 1 < parts.size(); ++existing) {
        const auto& node = nodes_.at(cursor);
        auto it = node.children.find(parts[existing]);
        if (it == node.children.end()) break;
        if (nodes_.at(it->second).type != NodeType::directory) return std::nullopt;
        cursor = it->second;
    }
    if (existing + 1 < parts.size() && !create) return std::nullopt;
    for (std::size_t i = existing; i + 1 < parts.size(); ++i) {
        Ino ino = next_ino_++;
        nodes_[ino] =
            Node{.parent = cursor, .name = std::string(parts[i]), .type = NodeType::directory};
        nodes_.at(cursor).children.emplace(std::string(parts[i]), ino);
        entries_changed_locked(cursor);
        entries_changed_locked(ino);
        cursor = ino;
    }
    return cursor;
}

void Tree::upsert_locked(const core::FileRecord& record) {
    // The index only holds validated paths; this is a second line of defence for the kernel.
    if (!core::is_valid_drive_path(record.path)) return;
    NodeType type = is_directory_record(record) ? NodeType::directory : NodeType::file;
    auto name = last_component(record.path);

    // Reject a type collision before parent_for_locked creates anything.
    if (auto existing = resolve_locked(record.path)) {
        if (nodes_.at(*existing).type != type) return;
    }
    auto parent = parent_for_locked(record.path, true);
    if (!parent) return;

    auto& siblings = nodes_.at(*parent).children;
    Ino ino;
    if (auto it = siblings.find(name); it != siblings.end()) {
        ino = it->second;
    } else {
        ino = next_ino_++;
        nodes_[ino] = Node{.parent = *parent, .name = std::string(name), .type = type};
        siblings.emplace(std::string(name), ino);
        entries_changed_locked(*parent);
    }
    auto& node = nodes_.at(ino);
    node.record = record;
    if (type == NodeType::file) {
        node.mtime_ns = record.mtime_ns;
    } else {
        node.mtime_ns = std::max(node.mtime_ns, record.mtime_ns);
    }
}

void Tree::entries_changed_locked(Ino dir) {
    // A directory's mtime is when its entries last changed here, not any file's mtime: a
    // renamed or deleted file keeps its old mtime, and NFS clients (FUSE-T) keep serving a
    // cached listing while the directory's mtime stays the same.
    auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
    auto& node = nodes_.at(dir);
    node.mtime_ns = std::max<std::int64_t>(now, node.mtime_ns + 1);
}

void Tree::remove_locked(std::string_view path) {
    auto ino = resolve_locked(path);
    if (!ino || *ino == kRootIno) return;
    auto& node = nodes_.at(*ino);
    Ino parent = node.parent;
    if (node.type == NodeType::directory) {
        // The directory record is gone; it stays while entries still imply it.
        node.record.reset();
        prune_locked(*ino);
        return;
    }
    nodes_.at(parent).children.erase(node.name);
    nodes_.erase(*ino);
    entries_changed_locked(parent);
    prune_locked(parent);
}

void Tree::prune_locked(Ino dir) {
    // Implied directories exist only to hold entries, so drop the ones left empty.
    while (dir != kRootIno) {
        auto& node = nodes_.at(dir);
        if (!node.children.empty() || node.record) return;
        Ino parent = node.parent;
        nodes_.at(parent).children.erase(node.name);
        nodes_.erase(dir);
        entries_changed_locked(parent);
        dir = parent;
    }
}

void Tree::erase_subtree_locked(Ino ino) {
    std::vector<Ino> stack{ino};
    while (!stack.empty()) {
        Ino current = stack.back();
        stack.pop_back();
        for (const auto& [name, child] : nodes_.at(current).children) stack.push_back(child);
        nodes_.erase(current);
    }
}

bool Tree::move(std::string_view from, std::string_view to) {
    std::unique_lock lock(mutex_);
    if (!core::is_valid_drive_path(from) || !core::is_valid_drive_path(to)) return false;
    auto src = resolve_locked(from);
    if (!src || *src == kRootIno) return false;
    if (from == to) return true;
    // A directory cannot move into itself.
    if (to.size() > from.size() && to.substr(0, from.size()) == from && to[from.size()] == '/') {
        return false;
    }
    if (auto dst = resolve_locked(to)) {
        if (nodes_.at(*dst).type == NodeType::directory) return false;
        if (nodes_.at(*src).type == NodeType::directory) return false;
        Ino dst_parent = nodes_.at(*dst).parent;
        nodes_.at(dst_parent).children.erase(nodes_.at(*dst).name);
        erase_subtree_locked(*dst);
        entries_changed_locked(dst_parent);
    }
    auto new_parent = parent_for_locked(to, true);
    if (!new_parent) return false;

    auto& node = nodes_.at(*src);
    Ino old_parent = node.parent;
    nodes_.at(old_parent).children.erase(node.name);
    node.parent = *new_parent;
    node.name = std::string(last_component(to));
    nodes_.at(*new_parent).children.emplace(node.name, *src);
    entries_changed_locked(old_parent);
    entries_changed_locked(*new_parent);
    prune_locked(old_parent);

    // Keep the stored records' paths in step with where they now live.
    std::vector<Ino> stack{*src};
    while (!stack.empty()) {
        Ino current = stack.back();
        stack.pop_back();
        auto& n = nodes_.at(current);
        if (n.record) n.record->path = path_locked(current);
        for (const auto& [name, child] : n.children) stack.push_back(child);
    }
    return true;
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

std::string Tree::path_locked(Ino ino) const {
    std::vector<const std::string*> names;
    for (Ino cursor = ino; cursor != kRootIno; cursor = nodes_.at(cursor).parent) {
        names.push_back(&nodes_.at(cursor).name);
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!path.empty()) path += '/';
        path += **it;
    }
    return path;
}

Attr Tree::attr_locked(Ino ino, const Node& node) const {
    Attr attr;
    attr.ino = ino;
    attr.type = node.type;
    attr.mtime_ns = node.mtime_ns;
    if (node.type == NodeType::directory) {
        // Set-ID and sticky bits from peers are dropped: they mean nothing on a synced drive.
        attr.perm = node.record ? (node.record->mode & 0777) : kDirPerm;
        if (attr.perm == 0) attr.perm = kDirPerm;
        std::uint32_t subdirs = 0;
        for (const auto& [name, child] : node.children) {
            if (nodes_.at(child).type == NodeType::directory) ++subdirs;
        }
        attr.nlink = 2 + subdirs;
    } else {
        attr.perm = node.record->mode & 0777;
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

std::optional<std::string> Tree::path_of(Ino ino) const {
    std::shared_lock lock(mutex_);
    if (!nodes_.contains(ino)) return std::nullopt;
    return path_locked(ino);
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
    if (it == nodes_.end()) return std::nullopt;
    return it->second.record;
}

std::vector<std::string> Tree::record_paths_under(Ino ino) const {
    std::shared_lock lock(mutex_);
    std::vector<std::string> paths;
    if (!nodes_.contains(ino)) return paths;
    std::vector<Ino> stack{ino};
    while (!stack.empty()) {
        Ino current = stack.back();
        stack.pop_back();
        const auto& node = nodes_.at(current);
        if (node.record) paths.push_back(path_locked(current));
        for (const auto& [name, child] : node.children) stack.push_back(child);
    }
    return paths;
}

std::uint64_t Tree::applied_sequence() const {
    std::shared_lock lock(mutex_);
    return applied_sequence_;
}

}  // namespace ghostdrive::vfs
