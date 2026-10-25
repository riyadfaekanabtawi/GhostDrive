#include "write_handle.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "async.hpp"
#include "ghostdrive/core/chunking.hpp"

namespace ghostdrive::vfs {

using core::Errc;
using core::Error;
using core::Status;

namespace {

// Reads exactly out.size() bytes; holes in the sparse staging file read as zeros, and bytes
// past its end (never expected) are zero-filled.
Status pread_full(const core::File& file, std::span<std::byte> out, std::uint64_t offset) {
    auto n = file.read_at(out, offset);
    if (!n) return n.error();
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(n.value()), out.end(), std::byte{0});
    return {};
}

// True if `current` carries a change from a device other than `local` that `base` lacks.
bool changed_elsewhere(const core::FileRecord& current, const core::FileRecord& base,
                       std::uint64_t local) {
    for (const auto& counter : current.version.counters()) {
        if (counter.device != local && counter.value > base.version.get(counter.device)) {
            return true;
        }
    }
    return false;
}

std::int64_t wall_clock_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

core::Result<std::shared_ptr<WriteHandle>> WriteHandle::open(Context context, std::string path,
                                                             std::optional<core::FileRecord> base,
                                                             std::uint32_t new_file_mode,
                                                             std::filesystem::path staging_file,
                                                             bool truncate) {
    auto file = core::File::open_scratch(staging_file);
    if (!file) return file.error();
    auto handle = std::make_shared<WriteHandle>(Private{}, std::move(context), std::move(path),
                                                std::move(base), new_file_mode,
                                                std::move(staging_file), std::move(file).value());
    if (truncate && handle->size() > 0) {
        // Emptying the file drops every block, so the block size is chosen afresh at commit.
        handle->block_ = 0;
        handle->valid_base_ = 0;
        handle->dirty_.clear();
        handle->size_ = 0;
        handle->dirty_any_ = true;
    }
    if (auto st = handle->file_.truncate(handle->size()); !st) return st.error();
    return handle;
}

WriteHandle::WriteHandle(Private, Context context, std::string path,
                         std::optional<core::FileRecord> base, std::uint32_t new_file_mode,
                         std::filesystem::path staging_file, core::File file)
    : ctx_(std::move(context)),
      path_(std::move(path)),
      base_(std::move(base)),
      new_file_mode_(new_file_mode),
      staging_file_(std::move(staging_file)),
      file_(std::move(file)) {
    if (base_ && base_->size > 0) {
        block_ = base_->block_size;
        valid_base_ = base_->chunks.size();
        size_ = base_->size;
    }
}

WriteHandle::~WriteHandle() {
    (void)file_.close();
    std::error_code ec;
    std::filesystem::remove(staging_file_, ec);
}

bool WriteHandle::from_base(std::uint64_t block) const noexcept {
    return block_ != 0 && base_ && block < valid_base_ && !(block < dirty_.size() && dirty_[block]);
}

void WriteHandle::mark_dirty(std::uint64_t block) {
    if (block >= dirty_.size()) dirty_.resize(block + 1, false);
    dirty_[block] = true;
    dirty_any_ = true;
}

std::uint32_t WriteHandle::base_chunk_size(std::uint64_t block) const noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(base_->block_size, base_->size - block * base_->block_size));
}

asio::awaitable<Status> WriteHandle::copy_up(std::uint64_t block) {
    auto chunk = co_await ctx_.fetcher.get(base_->chunks[block], base_chunk_size(block));
    if (!chunk) co_return chunk.error();
    auto data = chunk.value();
    auto st = co_await detail::run_on(ctx_.disk, [this, data, block] {
        // COPY: the block's original bytes enter the staging file so a partial write can land
        // on top of them.
        return file_.write_at(*data, block * block_);
    });
    if (!st) co_return st;
    mark_dirty(block);
    co_return Status{};
}

asio::awaitable<Status> WriteHandle::resize(std::uint64_t new_size) {
    std::uint64_t old_size = size();
    if (new_size == old_size) co_return Status{};
    if (block_ != 0) {
        if (new_size < old_size) {
            // The block that will hold the new end keeps only its head; bring it in first.
            std::uint64_t last = new_size == 0 ? 0 : (new_size - 1) / block_;
            if (new_size % block_ != 0 && from_base(last)) {
                if (auto st = co_await copy_up(last); !st) co_return st;
            }
            valid_base_ = std::min(valid_base_, core::chunk_count(new_size, block_));
            if (dirty_.size() > core::chunk_count(new_size, block_)) {
                dirty_.resize(core::chunk_count(new_size, block_));
            }
        } else {
            // The old last block gains zeros after its old end, so it changes.
            std::uint64_t last = (old_size - 1) / block_;
            if (old_size % block_ != 0 && from_base(last)) {
                if (auto st = co_await copy_up(last); !st) co_return st;
            }
            for (std::uint64_t b = core::chunk_count(old_size, block_);
                 b < core::chunk_count(new_size, block_); ++b) {
                mark_dirty(b);
            }
        }
    }
    auto st = co_await detail::run_on(ctx_.disk, [this, new_size]() -> Status {
        return file_.truncate(new_size);
    });
    if (!st) co_return st;
    size_ = new_size;
    dirty_any_ = true;
    co_return Status{};
}

asio::awaitable<Status> WriteHandle::truncate(std::uint64_t new_size) {
    co_return co_await resize(new_size);
}

asio::awaitable<Status> WriteHandle::write(std::uint64_t offset, core::Bytes data) {
    if (data.empty()) co_return Status{};
    if (offset > size()) {
        if (auto st = co_await resize(offset); !st) co_return st;
    }
    std::uint64_t end = offset + data.size();
    if (block_ != 0) {
        std::uint64_t first = offset / block_;
        std::uint64_t last = (end - 1) / block_;
        for (std::uint64_t b : {first, last}) {
            if (!from_base(b)) continue;
            std::uint64_t block_start = b * block_;
            std::uint64_t block_end = std::min<std::uint64_t>(block_start + block_, size());
            bool covered = offset <= block_start && end >= block_end;
            if (!covered) {
                if (auto st = co_await copy_up(b); !st) co_return st;
            }
        }
    }
    auto shared = std::make_shared<core::Bytes>(std::move(data));
    auto st = co_await detail::run_on(ctx_.disk, [this, shared, offset] {
        return file_.write_at(*shared, offset);
    });
    if (!st) co_return st;
    if (block_ != 0) {
        for (std::uint64_t b = offset / block_; b <= (end - 1) / block_; ++b) mark_dirty(b);
    }
    dirty_any_ = true;
    if (end > size()) size_ = end;
    co_return Status{};
}

asio::awaitable<core::Result<ReadReply>> WriteHandle::read(std::uint64_t offset, std::size_t size) {
    ReadReply reply;
    std::uint64_t file_size = this->size();
    if (size == 0 || offset >= file_size) co_return reply;
    std::uint64_t end = std::min<std::uint64_t>(offset + size, file_size);

    // Contiguous runs of staged bytes are read in one pread; untouched blocks come from the
    // fetcher, so a write handle sees its own writes and the base version everywhere else.
    std::uint64_t pos = offset;
    while (pos < end) {
        std::uint64_t block = block_ == 0 ? 0 : pos / block_;
        std::uint64_t run_end = end;
        if (block_ != 0) run_end = std::min(end, (block + 1) * block_);
        if (block_ != 0 && from_base(block)) {
            auto chunk = co_await ctx_.fetcher.get(base_->chunks[block], base_chunk_size(block));
            if (!chunk) co_return chunk.error();
            std::uint64_t start = block * block_;
            reply.slices.push_back(std::span<const std::byte>(*chunk.value())
                                       .subspan(static_cast<std::size_t>(pos - start),
                                                static_cast<std::size_t>(run_end - pos)));
            reply.chunks.push_back(std::move(chunk).value());
        } else {
            auto buffer = std::make_shared<core::Bytes>(static_cast<std::size_t>(run_end - pos));
            auto st = co_await detail::run_on(ctx_.disk, [this, buffer, pos] {
                return pread_full(file_, *buffer, pos);
            });
            if (!st) co_return st.error();
            reply.slices.push_back(*buffer);
            reply.chunks.push_back(std::move(buffer));
        }
        pos = run_end;
    }
    co_return reply;
}

void WriteHandle::set_mode(std::uint32_t perm) {
    mode_override_ = perm & 07777;
    dirty_any_ = true;
}

void WriteHandle::set_mtime(std::int64_t mtime_ns) {
    mtime_override_ = mtime_ns;
    dirty_any_ = true;
}

asio::awaitable<Status> WriteHandle::commit() {
    if (!dirty_any_ || orphaned_) co_return Status{};
    std::uint64_t file_size = size();
    std::uint32_t block =
        block_ != 0 ? block_ : (file_size > 0 ? core::block_size_for(file_size) : 0);
    std::uint64_t count = core::chunk_count(file_size, block);

    auto committed = co_await detail::run_on(
        ctx_.disk, [this, file_size, block, count]() -> core::Result<core::FileRecord> {
            std::vector<core::ChunkId> chunks(count);
            core::Bytes buffer;
            for (std::uint64_t i = 0; i < count; ++i) {
                if (block_ != 0 && from_base(i)) {
                    chunks[i] = base_->chunks[i];
                    continue;
                }
                buffer.resize(static_cast<std::size_t>(
                    std::min<std::uint64_t>(block, file_size - i * block)));
                if (auto st = pread_full(file_, buffer, i * block); !st) {
                    return st.error();
                }
                auto id = ctx_.store.put(buffer);
                if (!id) return id.error();
                chunks[i] = id.value();
            }

            std::lock_guard lock(ctx_.mutation_mutex);
            if (orphaned_) return Error{Errc::not_found, "file was removed while open"};
            core::FileRecord record;
            if (base_) record = *base_;
            record.path = path_;
            // commit_local supersedes whatever version is stored. If another device changed
            // the file since this writer opened it, keep both: ours becomes a conflict copy.
            if (base_) {
                auto current = ctx_.index.get(path_);
                if (!current) return current.error();
                if (current.value() && changed_elsewhere(*current.value(), *base_,
                                                         ctx_.index.local_device())) {
                    path_ = core::conflict_copy_path(path_, wall_clock_ns(),
                                                     ctx_.index.local_device());
                    record.path = path_;
                    record.version = {};
                }
            }
            record.deleted = false;
            record.size = file_size;
            record.mode = mode_override_ ? *mode_override_ : (base_ ? base_->mode : new_file_mode_);
            record.mtime_ns = mtime_override_ ? *mtime_override_ : wall_clock_ns();
            record.block_size = count > 0 ? block : 0;
            record.chunking = core::Chunking::fixed;
            record.chunks = std::move(chunks);
            auto result = ctx_.index.commit_local(std::move(record), ctx_.clock);
            if (result && ctx_.after_commit) ctx_.after_commit();
            return result;
        });
    if (!committed) {
        if (orphaned_) co_return Status{};
        co_return committed.error();
    }
    base_ = std::move(committed).value();
    block_ = base_->block_size;
    valid_base_ = base_->chunks.size();
    dirty_.assign(static_cast<std::size_t>(valid_base_), false);
    dirty_any_ = false;
    mode_override_.reset();
    mtime_override_.reset();
    co_return Status{};
}

}  // namespace ghostdrive::vfs
