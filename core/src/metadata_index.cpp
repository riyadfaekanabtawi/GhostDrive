#include "ghostdrive/core/metadata_index.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <mutex>
#include <tuple>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::core {

namespace {

constexpr int kSchemaVersion = 1;

constexpr const char* kSchema = R"sql(
CREATE TABLE meta (
    key TEXT PRIMARY KEY,
    value INTEGER NOT NULL
) WITHOUT ROWID;
CREATE TABLE files (
    path TEXT PRIMARY KEY,
    size INTEGER NOT NULL,
    mode INTEGER NOT NULL,
    mtime_ns INTEGER NOT NULL,
    hlc INTEGER NOT NULL,
    version BLOB NOT NULL,
    modified_by INTEGER NOT NULL,
    block_size INTEGER NOT NULL,
    chunking INTEGER NOT NULL,
    deleted INTEGER NOT NULL,
    sequence INTEGER NOT NULL UNIQUE
) WITHOUT ROWID;
CREATE TABLE chunks (
    path TEXT NOT NULL REFERENCES files(path) ON DELETE CASCADE,
    idx INTEGER NOT NULL,
    chunk BLOB NOT NULL,
    PRIMARY KEY (path, idx)
) WITHOUT ROWID;
CREATE INDEX chunks_by_id ON chunks(chunk);
CREATE TABLE devices (
    key BLOB PRIMARY KEY,
    short_id INTEGER NOT NULL UNIQUE,
    name TEXT NOT NULL,
    peer_index_id INTEGER NOT NULL DEFAULT 0,
    peer_max_sequence INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;
CREATE TABLE holdings (
    chunk BLOB NOT NULL,
    device BLOB NOT NULL REFERENCES devices(key) ON DELETE CASCADE,
    PRIMARY KEY (chunk, device)
) WITHOUT ROWID;
)sql";

// SQLite integers are signed 64-bit; unsigned values are stored by bit pattern.
sqlite3_int64 to_db(std::uint64_t v) noexcept {
    return static_cast<sqlite3_int64>(v);
}
std::uint64_t from_db(sqlite3_int64 v) noexcept {
    return static_cast<std::uint64_t>(v);
}

struct DbCloser {
    void operator()(sqlite3* db) const noexcept { sqlite3_close_v2(db); }
};
using Db = std::unique_ptr<sqlite3, DbCloser>;

struct StmtFinalizer {
    void operator()(sqlite3_stmt* stmt) const noexcept { sqlite3_finalize(stmt); }
};

Error db_error(sqlite3* db, std::string_view what) {
    return Error{Errc::database, std::string(what) + ": " + sqlite3_errmsg(db)};
}

class Stmt {
public:
    Stmt(sqlite3* db, sqlite3_stmt* stmt) : db_(db), stmt_(stmt) {}

    Stmt& bind_i64(int index, std::int64_t value) {
        check(sqlite3_bind_int64(stmt_.get(), index, value));
        return *this;
    }
    Stmt& bind_u64(int index, std::uint64_t value) { return bind_i64(index, to_db(value)); }
    Stmt& bind_text(int index, std::string_view text) {
        check(sqlite3_bind_text64(stmt_.get(), index, text.data(), text.size(), SQLITE_TRANSIENT,
                                  SQLITE_UTF8));
        return *this;
    }
    Stmt& bind_blob(int index, std::span<const std::byte> blob) {
        // A zero-length blob with a null pointer binds as NULL; use a non-null pointer instead.
        static const std::byte kEmpty{};
        check(sqlite3_bind_blob64(stmt_.get(), index, blob.empty() ? &kEmpty : blob.data(),
                                  blob.size(), SQLITE_TRANSIENT));
        return *this;
    }

    // true when a row is available, false when the statement is done.
    Result<bool> step() {
        if (bind_failed_) return db_error(db_, "bind");
        const int rc = sqlite3_step(stmt_.get());
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        return db_error(db_, "step");
    }
    Status run() {
        auto r = step();
        if (!r) return r.error();
        return {};
    }

    std::int64_t col_i64(int col) const { return sqlite3_column_int64(stmt_.get(), col); }
    std::uint64_t col_u64(int col) const { return from_db(col_i64(col)); }
    std::string col_text(int col) const {
        const auto* text = sqlite3_column_text(stmt_.get(), col);
        const int len = sqlite3_column_bytes(stmt_.get(), col);
        return text
                   ? std::string(reinterpret_cast<const char*>(text), static_cast<std::size_t>(len))
                   : std::string();
    }
    std::span<const std::byte> col_blob(int col) const {
        const void* data = sqlite3_column_blob(stmt_.get(), col);
        const int len = sqlite3_column_bytes(stmt_.get(), col);
        if (!data) return {};
        return {static_cast<const std::byte*>(data), static_cast<std::size_t>(len)};
    }

private:
    void check(int rc) {
        if (rc != SQLITE_OK) bind_failed_ = true;
    }

    sqlite3* db_;
    std::unique_ptr<sqlite3_stmt, StmtFinalizer> stmt_;
    bool bind_failed_ = false;
};

Result<Stmt> prepare(sqlite3* db, std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr) !=
        SQLITE_OK) {
        return db_error(db, "prepare");
    }
    return Stmt(db, stmt);
}

Status exec(sqlite3* db, const char* sql) {
    char* message = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &message) != SQLITE_OK) {
        Error err{Errc::database, std::string("exec: ") + (message ? message : "unknown error")};
        sqlite3_free(message);
        return err;
    }
    return {};
}

class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (active_) static_cast<void>(exec(db_, "ROLLBACK"));
    }

    Status begin() {
        auto st = exec(db_, "BEGIN IMMEDIATE");
        active_ = st.ok();
        return st;
    }
    Status commit() {
        auto st = exec(db_, "COMMIT");
        if (st) active_ = false;
        return st;
    }

private:
    sqlite3* db_;
    bool active_ = false;
};

Result<std::uint64_t> read_meta(sqlite3* db, std::string_view key) {
    auto stmt = prepare(db, "SELECT value FROM meta WHERE key = ?");
    if (!stmt) return stmt.error();
    stmt.value().bind_text(1, key);
    auto row = stmt.value().step();
    if (!row) return row.error();
    if (!row.value()) return Error{Errc::corrupt, "index meta key missing: " + std::string(key)};
    return stmt.value().col_u64(0);
}

Status write_meta(sqlite3* db, std::string_view key, std::uint64_t value) {
    auto stmt = prepare(db, "INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)");
    if (!stmt) return stmt.error();
    return stmt.value().bind_text(1, key).bind_u64(2, value).run();
}

}  // namespace

struct MetadataIndex::Impl {
    Db db;
    std::uint64_t local_device = 0;
    std::uint64_t index_id = 0;
    mutable std::mutex mutex;

    Result<std::optional<FileRecord>> read_record(std::string_view path) const;
    Result<FileRecord> write_record(FileRecord record);
    Result<FileRecord> commit(FileRecord record, HybridClock& clock);
    Result<ApplyOutcome> resolve_concurrent(const FileRecord& local, const FileRecord& remote);
};

Result<std::optional<FileRecord>> MetadataIndex::Impl::read_record(std::string_view path) const {
    auto stmt = prepare(db.get(),
                        "SELECT size, mode, mtime_ns, hlc, version, modified_by, block_size, "
                        "chunking, deleted, sequence FROM files WHERE path = ?");
    if (!stmt) return stmt.error();
    Stmt& s = stmt.value();
    s.bind_text(1, path);
    auto row = s.step();
    if (!row) return row.error();
    if (!row.value()) return std::optional<FileRecord>();

    FileRecord record;
    record.path = std::string(path);
    record.size = s.col_u64(0);
    record.mode = static_cast<std::uint32_t>(s.col_i64(1));
    record.mtime_ns = s.col_i64(2);
    record.hlc = s.col_u64(3);
    auto version = VersionVector::parse(s.col_blob(4));
    if (!version) return version.error();
    record.version = std::move(version).value();
    record.modified_by = s.col_u64(5);
    record.block_size = static_cast<std::uint32_t>(s.col_i64(6));
    record.chunking = static_cast<Chunking>(s.col_i64(7));
    record.deleted = s.col_i64(8) != 0;
    record.sequence = s.col_u64(9);

    auto chunks = prepare(db.get(), "SELECT chunk FROM chunks WHERE path = ? ORDER BY idx");
    if (!chunks) return chunks.error();
    chunks.value().bind_text(1, path);
    for (;;) {
        auto more = chunks.value().step();
        if (!more) return more.error();
        if (!more.value()) break;
        auto id = ChunkId::from_bytes(chunks.value().col_blob(0));
        if (!id) return id.error();
        record.chunks.push_back(id.value());
    }
    return std::optional<FileRecord>(std::move(record));
}

Result<FileRecord> MetadataIndex::Impl::write_record(FileRecord record) {
    auto sequence = read_meta(db.get(), "sequence");
    if (!sequence) return sequence.error();
    record.sequence = sequence.value() + 1;
    if (auto st = write_meta(db.get(), "sequence", record.sequence); !st) return st.error();

    auto del = prepare(db.get(), "DELETE FROM files WHERE path = ?");
    if (!del) return del.error();
    if (auto st = del.value().bind_text(1, record.path).run(); !st) return st.error();

    auto ins = prepare(db.get(),
                       "INSERT INTO files(path, size, mode, mtime_ns, hlc, version, modified_by, "
                       "block_size, chunking, deleted, sequence) "
                       "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!ins) return ins.error();
    const Bytes version = record.version.serialize();
    auto st = ins.value()
                  .bind_text(1, record.path)
                  .bind_u64(2, record.size)
                  .bind_i64(3, record.mode)
                  .bind_i64(4, record.mtime_ns)
                  .bind_u64(5, record.hlc)
                  .bind_blob(6, version)
                  .bind_u64(7, record.modified_by)
                  .bind_i64(8, record.block_size)
                  .bind_i64(9, static_cast<std::int64_t>(record.chunking))
                  .bind_i64(10, record.deleted ? 1 : 0)
                  .bind_u64(11, record.sequence)
                  .run();
    if (!st) return st.error();

    for (std::size_t i = 0; i < record.chunks.size(); ++i) {
        auto chunk = prepare(db.get(), "INSERT INTO chunks(path, idx, chunk) VALUES(?, ?, ?)");
        if (!chunk) return chunk.error();
        const auto id = record.chunks[i].to_bytes();
        auto cst = chunk.value()
                       .bind_text(1, record.path)
                       .bind_i64(2, static_cast<std::int64_t>(i))
                       .bind_blob(3, id)
                       .run();
        if (!cst) return cst.error();
    }
    return record;
}

Result<FileRecord> MetadataIndex::Impl::commit(FileRecord record, HybridClock& clock) {
    if (record.deleted) {
        record.size = 0;
        record.chunks.clear();
    }
    if (auto st = validate(record); !st) return st.error();
    Transaction tx(db.get());
    if (auto st = tx.begin(); !st) return st.error();
    auto existing = read_record(record.path);
    if (!existing) return existing.error();
    record.version = existing.value() ? existing.value()->version : VersionVector{};
    record.version.bump(local_device);
    record.hlc = clock.now();
    record.modified_by = local_device;
    auto written = write_record(std::move(record));
    if (!written) return written.error();
    if (auto st = tx.commit(); !st) return st.error();
    return written;
}

Result<ApplyOutcome> MetadataIndex::Impl::resolve_concurrent(const FileRecord& local,
                                                             const FileRecord& remote) {
    // Edits beat deletes so data is never lost; otherwise the later (hlc, device) wins.
    bool remote_wins;
    if (local.deleted != remote.deleted) {
        remote_wins = local.deleted;
    } else {
        remote_wins =
            std::tie(remote.hlc, remote.modified_by) > std::tie(local.hlc, local.modified_by);
    }
    const FileRecord& loser = remote_wins ? local : remote;
    FileRecord winner = remote_wins ? remote : local;
    // Absorbing both histories makes the winner dominate the loser on every device.
    winner.version = local.version.merged(remote.version);
    if (auto w = write_record(std::move(winner)); !w) return w.error();

    if (local.deleted || remote.deleted) {
        return ApplyOutcome{
            remote_wins ? ApplyOutcome::Kind::applied : ApplyOutcome::Kind::kept_local, {}};
    }

    // Built only from the loser, so both devices create an identical record (same path, same
    // version) and it converges without a second conflict.
    FileRecord copy = loser;
    copy.path = conflict_copy_path(loser.path, loser.mtime_ns, loser.modified_by);
    auto existing = read_record(copy.path);
    if (!existing) return existing.error();
    if (!existing.value() ||
        existing.value()->version.compare(copy.version) == VersionVector::Order::before) {
        if (auto c = write_record(copy); !c) return c.error();
    }
    return ApplyOutcome{ApplyOutcome::Kind::conflict, copy.path};
}

MetadataIndex::MetadataIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetadataIndex::MetadataIndex(MetadataIndex&&) noexcept = default;
MetadataIndex& MetadataIndex::operator=(MetadataIndex&&) noexcept = default;
MetadataIndex::~MetadataIndex() = default;

Result<MetadataIndex> MetadataIndex::open(const std::filesystem::path& db_path,
                                          std::uint64_t local_device) {
    sqlite3* raw = nullptr;
    const int rc =
        sqlite3_open_v2(db_path.c_str(), &raw,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
    Db db(raw);
    if (rc != SQLITE_OK) {
        return Error{Errc::database, "open " + db_path.string() + ": " +
                                         (raw ? sqlite3_errmsg(raw) : "out of memory")};
    }
    if (auto st = exec(db.get(),
                       "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; "
                       "PRAGMA foreign_keys = ON;");
        !st) {
        return st.error();
    }

    auto version_stmt = prepare(db.get(), "PRAGMA user_version");
    if (!version_stmt) return version_stmt.error();
    auto version_row = version_stmt.value().step();
    if (!version_row) return version_row.error();
    const std::int64_t schema = version_row.value() ? version_stmt.value().col_i64(0) : 0;

    auto impl = std::make_unique<Impl>();
    if (schema == 0) {
        std::array<std::byte, 8> random{};
        if (auto st = random_bytes(random); !st) return st.error();
        std::uint64_t index_id = 0;
        for (std::byte b : random) index_id = (index_id << 8) | std::to_integer<std::uint64_t>(b);

        Transaction tx(db.get());
        if (auto st = tx.begin(); !st) return st.error();
        if (auto st = exec(db.get(), kSchema); !st) return st.error();
        if (auto st = write_meta(db.get(), "index_id", index_id); !st) return st.error();
        if (auto st = write_meta(db.get(), "sequence", 0); !st) return st.error();
        if (auto st = write_meta(db.get(), "local_device", local_device); !st) return st.error();
        const std::string pragma = "PRAGMA user_version = " + std::to_string(kSchemaVersion);
        if (auto st = exec(db.get(), pragma.c_str()); !st) return st.error();
        if (auto st = tx.commit(); !st) return st.error();
    } else if (schema != kSchemaVersion) {
        return Error{Errc::corrupt, "unsupported index schema version " + std::to_string(schema)};
    }

    auto stored_device = read_meta(db.get(), "local_device");
    if (!stored_device) return stored_device.error();
    if (stored_device.value() != local_device) {
        return Error{Errc::invalid_argument,
                     "index " + db_path.string() + " belongs to a different device"};
    }
    auto index_id = read_meta(db.get(), "index_id");
    if (!index_id) return index_id.error();

    impl->db = std::move(db);
    impl->local_device = local_device;
    impl->index_id = index_id.value();
    return MetadataIndex(std::move(impl));
}

std::uint64_t MetadataIndex::local_device() const noexcept {
    return impl_->local_device;
}

std::uint64_t MetadataIndex::index_id() const noexcept {
    return impl_->index_id;
}

Result<std::uint64_t> MetadataIndex::max_sequence() const {
    std::lock_guard lock(impl_->mutex);
    return read_meta(impl_->db.get(), "sequence");
}

Result<std::optional<FileRecord>> MetadataIndex::get(std::string_view path) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->read_record(path);
}

Result<std::vector<FileRecord>> MetadataIndex::records_since(std::uint64_t after,
                                                             std::size_t limit) const {
    std::lock_guard lock(impl_->mutex);
    auto stmt = prepare(impl_->db.get(),
                        "SELECT path FROM files WHERE sequence > ? ORDER BY sequence LIMIT ?");
    if (!stmt) return stmt.error();
    stmt.value().bind_u64(1, after).bind_i64(2, static_cast<std::int64_t>(limit));
    std::vector<std::string> paths;
    for (;;) {
        auto row = stmt.value().step();
        if (!row) return row.error();
        if (!row.value()) break;
        paths.push_back(stmt.value().col_text(0));
    }
    std::vector<FileRecord> records;
    records.reserve(paths.size());
    for (const std::string& path : paths) {
        auto record = impl_->read_record(path);
        if (!record) return record.error();
        if (record.value()) records.push_back(std::move(*record.value()));
    }
    return records;
}

Result<FileRecord> MetadataIndex::commit_local(FileRecord record, HybridClock& clock) {
    std::lock_guard lock(impl_->mutex);
    return impl_->commit(std::move(record), clock);
}

Result<FileRecord> MetadataIndex::delete_local(std::string_view path, HybridClock& clock) {
    std::lock_guard lock(impl_->mutex);
    auto existing = impl_->read_record(path);
    if (!existing) return existing.error();
    if (!existing.value() || existing.value()->deleted) {
        return Error{Errc::not_found, "no such file: " + std::string(path)};
    }
    FileRecord record = std::move(*existing.value());
    record.deleted = true;
    return impl_->commit(std::move(record), clock);
}

Result<ApplyOutcome> MetadataIndex::apply_remote(const FileRecord& record, HybridClock& clock) {
    if (auto st = validate(record); !st) return st.error();
    std::lock_guard lock(impl_->mutex);
    clock.update(record.hlc);

    Transaction tx(impl_->db.get());
    if (auto st = tx.begin(); !st) return st.error();
    auto local = impl_->read_record(record.path);
    if (!local) return local.error();

    ApplyOutcome outcome{ApplyOutcome::Kind::ignored, {}};
    if (!local.value()) {
        if (auto w = impl_->write_record(record); !w) return w.error();
        outcome.kind = ApplyOutcome::Kind::applied;
    } else {
        switch (record.version.compare(local.value()->version)) {
            case VersionVector::Order::equal:
            case VersionVector::Order::before:
                return outcome;
            case VersionVector::Order::after:
                if (auto w = impl_->write_record(record); !w) return w.error();
                outcome.kind = ApplyOutcome::Kind::applied;
                break;
            case VersionVector::Order::concurrent: {
                auto resolved = impl_->resolve_concurrent(*local.value(), record);
                if (!resolved) return resolved.error();
                outcome = std::move(resolved).value();
                break;
            }
        }
    }
    if (auto st = tx.commit(); !st) return st.error();
    return outcome;
}

Status MetadataIndex::add_device(const PublicKey& key, std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    auto stmt =
        prepare(impl_->db.get(), "INSERT INTO devices(key, short_id, name) VALUES(?, ?, ?)");
    if (!stmt) return stmt.error();
    auto st = stmt.value().bind_blob(1, key).bind_u64(2, short_id(key)).bind_text(3, name).run();
    // Either the key or its 64-bit short ID is already present.
    if (!st && sqlite3_errcode(impl_->db.get()) == SQLITE_CONSTRAINT) {
        return Error{Errc::already_exists, "device already pinned: " + fingerprint(key)};
    }
    return st;
}

Status MetadataIndex::remove_device(const PublicKey& key) {
    std::lock_guard lock(impl_->mutex);
    auto stmt = prepare(impl_->db.get(), "DELETE FROM devices WHERE key = ?");
    if (!stmt) return stmt.error();
    if (auto st = stmt.value().bind_blob(1, key).run(); !st) return st;
    if (sqlite3_changes(impl_->db.get()) == 0) {
        return Error{Errc::not_found, "device not pinned: " + fingerprint(key)};
    }
    return {};
}

Result<bool> MetadataIndex::is_pinned(const PublicKey& key) const {
    std::lock_guard lock(impl_->mutex);
    auto stmt = prepare(impl_->db.get(), "SELECT 1 FROM devices WHERE key = ?");
    if (!stmt) return stmt.error();
    stmt.value().bind_blob(1, key);
    return stmt.value().step();
}

Result<std::vector<DeviceRecord>> MetadataIndex::devices() const {
    std::lock_guard lock(impl_->mutex);
    auto stmt = prepare(impl_->db.get(),
                        "SELECT key, short_id, name, peer_index_id, peer_max_sequence FROM devices "
                        "ORDER BY short_id");
    if (!stmt) return stmt.error();
    std::vector<DeviceRecord> out;
    for (;;) {
        auto row = stmt.value().step();
        if (!row) return row.error();
        if (!row.value()) break;
        const Stmt& s = stmt.value();
        const auto key = s.col_blob(0);
        if (key.size() != kPublicKeySize) return Error{Errc::corrupt, "device key has wrong size"};
        DeviceRecord device;
        std::copy(key.begin(), key.end(), device.key.begin());
        device.short_id = s.col_u64(1);
        device.name = s.col_text(2);
        device.peer_index_id = s.col_u64(3);
        device.peer_max_sequence = s.col_u64(4);
        out.push_back(std::move(device));
    }
    return out;
}

Status MetadataIndex::set_peer_progress(const PublicKey& key, std::uint64_t peer_index_id,
                                        std::uint64_t peer_max_sequence) {
    std::lock_guard lock(impl_->mutex);
    auto stmt =
        prepare(impl_->db.get(),
                "UPDATE devices SET peer_index_id = ?, peer_max_sequence = ? WHERE key = ?");
    if (!stmt) return stmt.error();
    auto st = stmt.value()
                  .bind_u64(1, peer_index_id)
                  .bind_u64(2, peer_max_sequence)
                  .bind_blob(3, key)
                  .run();
    if (!st) return st;
    if (sqlite3_changes(impl_->db.get()) == 0) {
        return Error{Errc::not_found, "device not pinned: " + fingerprint(key)};
    }
    return {};
}

Status MetadataIndex::set_holds(const PublicKey& device, const ChunkId& chunk, bool holds) {
    std::lock_guard lock(impl_->mutex);
    const auto id = chunk.to_bytes();
    auto stmt = prepare(impl_->db.get(),
                        holds ? "INSERT OR IGNORE INTO holdings(chunk, device) VALUES(?, ?)"
                              : "DELETE FROM holdings WHERE chunk = ? AND device = ?");
    if (!stmt) return stmt.error();
    auto st = stmt.value().bind_blob(1, id).bind_blob(2, device).run();
    if (!st && sqlite3_extended_errcode(impl_->db.get()) == SQLITE_CONSTRAINT_FOREIGNKEY) {
        return Error{Errc::not_found, "device not pinned: " + fingerprint(device)};
    }
    return st;
}

Result<std::vector<PublicKey>> MetadataIndex::holders(const ChunkId& chunk) const {
    std::lock_guard lock(impl_->mutex);
    const auto id = chunk.to_bytes();
    auto stmt =
        prepare(impl_->db.get(), "SELECT device FROM holdings WHERE chunk = ? ORDER BY device");
    if (!stmt) return stmt.error();
    stmt.value().bind_blob(1, id);
    std::vector<PublicKey> out;
    for (;;) {
        auto row = stmt.value().step();
        if (!row) return row.error();
        if (!row.value()) break;
        const auto key = stmt.value().col_blob(0);
        if (key.size() != kPublicKeySize) return Error{Errc::corrupt, "holder key has wrong size"};
        PublicKey pk{};
        std::copy(key.begin(), key.end(), pk.begin());
        out.push_back(pk);
    }
    return out;
}

}  // namespace ghostdrive::core
