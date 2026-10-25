#include "ghostdrive/core/metadata_index.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#include <tuple>

#include "ghostdrive/core/bytes.hpp"

namespace ghostdrive::core {

namespace {

constexpr int kSchemaVersion = 2;

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

// Version 2 adds record signatures. A new index runs it right after kSchema, so new and
// migrated indexes have the same tables. Rows of signers are never deleted: a removed device's
// key stays known, and its short ID is never bound to another key.
constexpr const char* kSchemaV2 = R"sql(
ALTER TABLE files ADD COLUMN signature BLOB;
ALTER TABLE files ADD COLUMN origin_path TEXT NOT NULL DEFAULT '';
ALTER TABLE files ADD COLUMN effective_version BLOB;
ALTER TABLE files ADD COLUMN signer INTEGER NOT NULL DEFAULT 0;
UPDATE files SET effective_version = version, signer = modified_by;
CREATE TABLE signers (
    short_id INTEGER PRIMARY KEY,
    key BLOB NOT NULL UNIQUE,
    retired_seq INTEGER
);
INSERT INTO signers(short_id, key) SELECT short_id, key FROM devices;
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
    Stmt& bind_null(int index) {
        check(sqlite3_bind_null(stmt_.get(), index));
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
    bool col_null(int col) const { return sqlite3_column_type(stmt_.get(), col) == SQLITE_NULL; }
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

Result<std::uint64_t> random_u64() {
    std::array<std::byte, 8> random{};
    if (auto st = random_bytes(random); !st) return st.error();
    std::uint64_t value = 0;
    for (std::byte b : random) value = (value << 8) | std::to_integer<std::uint64_t>(b);
    return value;
}

bool same_content(const FileRecord& a, const FileRecord& b) {
    return a.size == b.size && a.mode == b.mode && a.mtime_ns == b.mtime_ns &&
           a.deleted == b.deleted && a.chunks == b.chunks;
}

}  // namespace

struct MetadataIndex::Impl {
    struct Signer {
        PublicKey key{};
        bool retired = false;
        bool pinned = false;  // in the devices table, or this device
    };

    Db db;
    const DeviceKey* key = nullptr;
    DriveId drive{};
    std::uint64_t local_device = 0;
    std::uint64_t index_id = 0;
    std::map<std::uint64_t, Signer> signers;  // by short ID, mirrors the signers table
    mutable std::mutex mutex;

    Result<std::optional<FileRecord>> read_record(std::string_view path) const;
    Result<std::vector<std::string>> paths_where(const char* sql, std::uint64_t arg) const;
    Result<FileRecord> write_record(FileRecord record);
    Status sign(FileRecord& record) const;
    Status verify(const FileRecord& record) const;
    Status load_signers();
    Status migrate_to_v2();
    // The caller holds a transaction.
    Result<FileRecord> commit_in_tx(FileRecord record, HybridClock& clock);
    Result<FileRecord> commit(FileRecord record, HybridClock& clock);
    Result<ApplyOutcome> resolve_concurrent(const FileRecord& local, const FileRecord& remote);
};

Result<std::optional<FileRecord>> MetadataIndex::Impl::read_record(std::string_view path) const {
    auto stmt = prepare(db.get(),
                        "SELECT size, mode, mtime_ns, hlc, version, modified_by, block_size, "
                        "chunking, deleted, sequence, signature, origin_path, effective_version, "
                        "signer FROM files WHERE path = ?");
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
    if (!s.col_null(10)) {
        const auto signature = s.col_blob(10);
        if (signature.size() != kSignatureSize) return Error{Errc::corrupt, "bad stored signature"};
        Signature sig{};
        std::copy(signature.begin(), signature.end(), sig.begin());
        record.signature = sig;
    }
    record.origin_path = s.col_text(11);
    auto effective = VersionVector::parse(s.col_blob(12));
    if (!effective) return effective.error();
    record.effective_version = std::move(effective).value();
    record.signer = s.col_u64(13);

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
                       "block_size, chunking, deleted, sequence, signature, origin_path, "
                       "effective_version, signer) "
                       "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    if (!ins) return ins.error();
    const Bytes version = record.version.serialize();
    const Bytes effective = record.effective_version.serialize();
    if (record.signature) {
        ins.value().bind_blob(12, *record.signature);
    } else {
        ins.value().bind_null(12);
    }
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
                  .bind_text(13, record.origin_path)
                  .bind_blob(14, effective)
                  .bind_u64(15, record.signer)
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

Result<std::vector<std::string>> MetadataIndex::Impl::paths_where(const char* sql,
                                                                  std::uint64_t arg) const {
    auto stmt = prepare(db.get(), sql);
    if (!stmt) return stmt.error();
    stmt.value().bind_u64(1, arg);
    std::vector<std::string> paths;
    for (;;) {
        auto row = stmt.value().step();
        if (!row) return row.error();
        if (!row.value()) break;
        paths.push_back(stmt.value().col_text(0));
    }
    return paths;
}

Status MetadataIndex::Impl::sign(FileRecord& record) const {
    record.signer = local_device;
    auto signature = key->sign(record_signed_bytes(drive, record));
    if (!signature) return signature.error();
    record.signature = signature.value();
    return {};
}

Status MetadataIndex::Impl::verify(const FileRecord& record) const {
    // not_found: the record may become valid once a device is pinned or retired here.
    auto it = signers.find(record.signer);
    if (it == signers.end()) {
        return Error{Errc::not_found, "record signed by an unknown device: " + record.path};
    }
    if (it->second.retired) {
        return Error{Errc::invalid_argument, "record signed by a removed device: " + record.path};
    }
    // Only a removed device's records may be signed by another: a member that adopted them.
    if (record.signer != record.modified_by) {
        auto author = signers.find(record.modified_by);
        if (!it->second.pinned || author == signers.end()) {
            return Error{Errc::not_found, "adopted record not verifiable yet: " + record.path};
        }
        if (!author->second.retired) {
            return Error{Errc::invalid_argument,
                         "record signed by a device other than its author: " + record.path};
        }
    }
    if (!record.signature ||
        !verify_signature(it->second.key, record_signed_bytes(drive, record), *record.signature)) {
        return Error{Errc::invalid_argument, "record signature does not verify: " + record.path};
    }
    return {};
}

Status MetadataIndex::Impl::load_signers() {
    auto stmt = prepare(db.get(), "SELECT short_id, key, retired_seq FROM signers");
    if (!stmt) return stmt.error();
    std::map<std::uint64_t, Signer> loaded;
    for (;;) {
        auto row = stmt.value().step();
        if (!row) return row.error();
        if (!row.value()) break;
        const Stmt& s = stmt.value();
        const auto blob = s.col_blob(1);
        if (blob.size() != kPublicKeySize) return Error{Errc::corrupt, "signer key has wrong size"};
        Signer signer;
        std::copy(blob.begin(), blob.end(), signer.key.begin());
        signer.retired = !s.col_null(2);
        loaded[s.col_u64(0)] = signer;
    }
    auto pinned = prepare(db.get(), "SELECT short_id FROM devices");
    if (!pinned) return pinned.error();
    for (;;) {
        auto row = pinned.value().step();
        if (!row) return row.error();
        if (!row.value()) break;
        if (auto it = loaded.find(pinned.value().col_u64(0)); it != loaded.end()) {
            it->second.pinned = true;
        }
    }
    loaded[local_device].pinned = true;
    signers = std::move(loaded);
    return {};
}

Result<FileRecord> MetadataIndex::Impl::commit_in_tx(FileRecord record, HybridClock& clock) {
    if (record.deleted) {
        record.size = 0;
        record.chunks.clear();
    }
    // Changing a conflict copy here makes it an ordinary file at its own path.
    record.origin_path.clear();
    if (auto st = validate(record); !st) return st.error();
    auto existing = read_record(record.path);
    if (!existing) return existing.error();
    record.version = existing.value() ? existing.value()->effective_version : VersionVector{};
    record.version.bump(local_device);
    record.effective_version = record.version;
    record.hlc = clock.now();
    record.modified_by = local_device;
    if (auto st = sign(record); !st) return st.error();
    return write_record(std::move(record));
}

Result<FileRecord> MetadataIndex::Impl::commit(FileRecord record, HybridClock& clock) {
    Transaction tx(db.get());
    if (auto st = tx.begin(); !st) return st.error();
    auto written = commit_in_tx(std::move(record), clock);
    if (!written) return written.error();
    if (auto st = tx.commit(); !st) return st.error();
    return written;
}

Status MetadataIndex::Impl::migrate_to_v2() {
    if (auto st = exec(db.get(), kSchemaV2); !st) return st;
    auto self = prepare(db.get(), "INSERT INTO signers(short_id, key) VALUES(?, ?)");
    if (!self) return self.error();
    if (auto st = self.value().bind_u64(1, local_device).bind_blob(2, key->public_key()).run();
        !st) {
        return st;
    }
    // Records this device wrote before signing are signed as they are, version unchanged.
    // Peers holding the same version store the signature when it reaches them.
    auto own = paths_where("SELECT path FROM files WHERE modified_by = ?", local_device);
    if (!own) return own.error();
    for (const auto& path : own.value()) {
        auto record = read_record(path);
        if (!record) return record.error();
        if (!record.value()) continue;
        if (auto st = sign(*record.value()); !st) return st;
        auto update = prepare(db.get(), "UPDATE files SET signature = ? WHERE path = ?");
        if (!update) return update.error();
        auto st = update.value().bind_blob(1, *record.value()->signature).bind_text(2, path).run();
        if (!st) return st;
    }
    // Other devices' records stay unsigned: nothing here can vouch for them. They are filled in
    // when their author's signature arrives, and are never sent or adopted until then.
    // Peers already passed these records unsigned; a new index_id makes them fetch them again.
    auto id = random_u64();
    if (!id) return id.error();
    if (auto st = write_meta(db.get(), "index_id", id.value()); !st) return st;
    const std::string pragma = "PRAGMA user_version = " + std::to_string(kSchemaVersion);
    return exec(db.get(), pragma.c_str());
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
    // The winner keeps its signed version and is relayed unchanged. Only the local effective
    // version absorbs both histories, so the loser is ignored here from now on; every device
    // picks the same winner, so they converge without anyone signing the merge.
    winner.effective_version = local.effective_version.merged(remote.version);
    if (auto w = write_record(std::move(winner)); !w) return w.error();

    // Deletes lose to edits outright. A loser that is itself a conflict copy cannot be copied
    // again: its signature covers only its origin.
    if (local.deleted || remote.deleted || !loser.origin_path.empty()) {
        return ApplyOutcome{
            remote_wins ? ApplyOutcome::Kind::applied : ApplyOutcome::Kind::kept_local, {}};
    }

    // Built only from the loser, so both devices create an identical record (same path, same
    // version, the loser's signature) and it converges without a second conflict.
    FileRecord copy = loser;
    copy.origin_path = loser.path;
    copy.path = conflict_copy_path(loser.path, loser.mtime_ns, loser.modified_by);
    copy.effective_version = copy.version;
    auto existing = read_record(copy.path);
    if (!existing) return existing.error();
    if (!existing.value() ||
        existing.value()->effective_version.compare(copy.version) == VersionVector::Order::before) {
        if (auto c = write_record(copy); !c) return c.error();
    }
    return ApplyOutcome{ApplyOutcome::Kind::conflict, copy.path};
}

MetadataIndex::MetadataIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetadataIndex::MetadataIndex(MetadataIndex&&) noexcept = default;
MetadataIndex& MetadataIndex::operator=(MetadataIndex&&) noexcept = default;
MetadataIndex::~MetadataIndex() = default;

Result<MetadataIndex> MetadataIndex::open(const std::filesystem::path& db_path,
                                          const DriveId& drive, const DeviceKey& key) {
    const std::uint64_t local_device = short_id(key.public_key());
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

    std::int64_t schema = 0;
    {
        auto version_stmt = prepare(db.get(), "PRAGMA user_version");
        if (!version_stmt) return version_stmt.error();
        auto version_row = version_stmt.value().step();
        if (!version_row) return version_row.error();
        if (version_row.value()) schema = version_stmt.value().col_i64(0);
    }
    if (schema != 0 && schema != 1 && schema != kSchemaVersion) {
        return Error{Errc::corrupt, "unsupported index schema version " + std::to_string(schema)};
    }

    auto impl = std::make_unique<Impl>();
    impl->db = std::move(db);
    impl->key = &key;
    impl->drive = drive;
    impl->local_device = local_device;
    sqlite3* handle = impl->db.get();
    auto check_device = [&]() -> Status {
        auto stored_device = read_meta(handle, "local_device");
        if (!stored_device) return stored_device.error();
        if (stored_device.value() != local_device) {
            return Error{Errc::invalid_argument,
                         "index " + db_path.string() + " belongs to a different device"};
        }
        return {};
    };
    if (schema != kSchemaVersion) {
        Transaction tx(handle);
        if (auto st = tx.begin(); !st) return st.error();
        if (schema == 0) {
            if (auto st = exec(handle, kSchema); !st) return st.error();
            if (auto st = write_meta(handle, "sequence", 0); !st) return st.error();
            if (auto st = write_meta(handle, "local_device", local_device); !st) return st.error();
        }
        // Before migrating, which signs records as this device.
        if (auto st = check_device(); !st) return st.error();
        if (auto st = impl->migrate_to_v2(); !st) return st.error();
        if (auto st = tx.commit(); !st) return st.error();
    }
    if (auto st = check_device(); !st) return st.error();
    auto index_id = read_meta(handle, "index_id");
    if (!index_id) return index_id.error();
    impl->index_id = index_id.value();
    if (auto st = impl->load_signers(); !st) return st.error();
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

Result<std::vector<FileRecord>> MetadataIndex::records_since(std::uint64_t after, std::size_t limit,
                                                             bool signed_only) const {
    std::lock_guard lock(impl_->mutex);
    auto stmt = prepare(impl_->db.get(),
                        "SELECT path FROM files WHERE sequence > ? AND "
                        "(? = 0 OR signature IS NOT NULL) ORDER BY sequence LIMIT ?");
    if (!stmt) return stmt.error();
    stmt.value()
        .bind_u64(1, after)
        .bind_i64(2, signed_only ? 1 : 0)
        .bind_i64(3, static_cast<std::int64_t>(limit));
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
    // A peer must not push our clock or a version counter to the end of its range: both wrap,
    // which would freeze the file (our edits would compare as older) or lose every conflict.
    constexpr std::uint64_t kMaxCounter = std::uint64_t{1} << 62;
    constexpr std::uint64_t kMaxClockSkewMs = 24ull * 60 * 60 * 1000;
    for (const auto& c : record.version.counters()) {
        if (c.value >= kMaxCounter) {
            return Error{Errc::invalid_argument, "remote record has an out-of-range version"};
        }
    }
    const auto wall_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    if (HybridClock::physical_ms(record.hlc) > wall_ms + kMaxClockSkewMs) {
        return Error{Errc::invalid_argument, "remote record is timestamped too far in the future"};
    }
    std::lock_guard lock(impl_->mutex);

    Transaction tx(impl_->db.get());
    if (auto st = tx.begin(); !st) return st.error();
    auto local = impl_->read_record(record.path);
    if (!local) return local.error();

    // Only records that change something here are verified; ignored ones cost no signature
    // check. The signed version is stored as sent, beside the local effective version.
    FileRecord incoming = record;
    incoming.effective_version = record.version;
    ApplyOutcome outcome{ApplyOutcome::Kind::ignored, {}};
    if (!local.value()) {
        if (auto st = impl_->verify(incoming); !st) return st.error();
        if (auto w = impl_->write_record(std::move(incoming)); !w) return w.error();
        outcome.kind = ApplyOutcome::Kind::applied;
    } else {
        const FileRecord& stored = *local.value();
        switch (record.version.compare(stored.effective_version)) {
            case VersionVector::Order::equal:
                if (stored.signature || record.version != stored.version) return outcome;
                if (auto st = impl_->verify(incoming); !st) return st.error();
                // A record stored unsigned before signing takes the author's signature, but
                // only if it says the same. An unsigned row has no provenance, so different
                // content at the same version is treated as a concurrent change.
                if (same_content(stored, incoming) && stored.modified_by == incoming.modified_by &&
                    stored.hlc == incoming.hlc) {
                    incoming.effective_version = stored.effective_version;
                    if (auto w = impl_->write_record(std::move(incoming)); !w) return w.error();
                } else {
                    auto resolved = impl_->resolve_concurrent(stored, incoming);
                    if (!resolved) return resolved.error();
                    outcome = std::move(resolved).value();
                }
                break;
            case VersionVector::Order::before:
                return outcome;
            case VersionVector::Order::after:
                if (auto st = impl_->verify(incoming); !st) return st.error();
                if (auto w = impl_->write_record(std::move(incoming)); !w) return w.error();
                outcome.kind = ApplyOutcome::Kind::applied;
                break;
            case VersionVector::Order::concurrent: {
                if (auto st = impl_->verify(incoming); !st) return st.error();
                auto resolved = impl_->resolve_concurrent(stored, incoming);
                if (!resolved) return resolved.error();
                outcome = std::move(resolved).value();
                break;
            }
        }
    }
    if (auto st = tx.commit(); !st) return st.error();
    clock.update(record.hlc);
    return outcome;
}

Status MetadataIndex::add_device(const PublicKey& key, std::string_view name) {
    std::lock_guard lock(impl_->mutex);
    sqlite3* db = impl_->db.get();
    const std::uint64_t id = short_id(key);
    const Error clash{Errc::already_exists, "device already pinned: " + fingerprint(key)};
    auto known = impl_->signers.find(id);
    if (known != impl_->signers.end() && known->second.key != key) return clash;

    Transaction tx(db);
    if (auto st = tx.begin(); !st) return st;
    auto stmt = prepare(db, "INSERT INTO devices(key, short_id, name) VALUES(?, ?, ?)");
    if (!stmt) return stmt.error();
    auto st = stmt.value().bind_blob(1, key).bind_u64(2, id).bind_text(3, name).run();
    // Either the key or its 64-bit short ID is already present.
    if (!st && sqlite3_errcode(db) == SQLITE_CONSTRAINT) return clash;
    if (!st) return st;
    if (known == impl_->signers.end()) {
        auto signer = prepare(db, "INSERT INTO signers(short_id, key) VALUES(?, ?)");
        if (!signer) return signer.error();
        if (auto sst = signer.value().bind_u64(1, id).bind_blob(2, key).run(); !sst) return sst;
    }
    if (auto cst = tx.commit(); !cst) return cst;
    // A retired key stays retired even if pinned again: removal is permanent.
    auto& entry = impl_->signers[id];
    entry.key = key;
    entry.pinned = true;
    return {};
}

Result<std::size_t> MetadataIndex::retire_signer(const PublicKey& key) {
    std::lock_guard lock(impl_->mutex);
    sqlite3* db = impl_->db.get();
    const std::uint64_t id = short_id(key);
    if (id == impl_->local_device) {
        return Error{Errc::invalid_argument, "this device cannot retire its own key"};
    }
    auto known = impl_->signers.find(id);
    // Records under this short ID were signed by another key; they are not this key's to give up.
    if (known != impl_->signers.end() && known->second.key != key) return std::size_t{0};
    // Retiring and adopting happen in one transaction, so a retired key is done.
    if (known != impl_->signers.end() && known->second.retired) return std::size_t{0};

    Transaction tx(db);
    if (auto st = tx.begin(); !st) return st.error();
    auto sequence = read_meta(db, "sequence");
    if (!sequence) return sequence.error();
    auto signer = prepare(db,
                          "INSERT INTO signers(short_id, key, retired_seq) VALUES(?, ?, ?) "
                          "ON CONFLICT(short_id) DO UPDATE SET retired_seq = excluded.retired_seq");
    if (!signer) return signer.error();
    auto st = signer.value().bind_u64(1, id).bind_blob(2, key).bind_u64(3, sequence.value()).run();
    if (!st) return st.error();

    // Adoption: a device that joins later cannot check the retired key's signatures, so this
    // device signs each verified record it signed as the record's signer. Content, version and
    // modified_by stay as they were, so adoptions by several members compare equal and a newer
    // version by the removed device still supersedes an older one, whoever adopted each.
    // Unsigned rows were never verified and are not adopted.
    auto paths = impl_->paths_where(
        "SELECT path FROM files WHERE modified_by = ?1 AND signer = ?1 AND signature IS NOT NULL",
        id);
    if (!paths) return paths.error();
    for (const auto& path : paths.value()) {
        auto record = impl_->read_record(path);
        if (!record) return record.error();
        if (!record.value()) continue;
        if (auto sst = impl_->sign(*record.value()); !sst) return sst.error();
        if (auto w = impl_->write_record(std::move(*record.value())); !w) return w.error();
    }
    if (auto cst = tx.commit(); !cst) return cst.error();
    auto& entry = impl_->signers[id];
    entry.key = key;
    entry.retired = true;
    return paths.value().size();
}

bool MetadataIndex::is_active_signer(std::uint64_t id) const {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->signers.find(id);
    return it != impl_->signers.end() && !it->second.retired;
}

bool MetadataIndex::is_retired_signer(std::uint64_t id) const {
    std::lock_guard lock(impl_->mutex);
    auto it = impl_->signers.find(id);
    return it != impl_->signers.end() && it->second.retired;
}

Status MetadataIndex::remove_device(const PublicKey& key) {
    std::lock_guard lock(impl_->mutex);
    auto stmt = prepare(impl_->db.get(), "DELETE FROM devices WHERE key = ?");
    if (!stmt) return stmt.error();
    if (auto st = stmt.value().bind_blob(1, key).run(); !st) return st;
    if (sqlite3_changes(impl_->db.get()) == 0) {
        return Error{Errc::not_found, "device not pinned: " + fingerprint(key)};
    }
    if (auto it = impl_->signers.find(short_id(key)); it != impl_->signers.end()) {
        it->second.pinned = false;
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
