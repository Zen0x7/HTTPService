#pragma once

#include <cassert>
#include <cstddef>
#include <memory>
#include <new>
#include <vector>

namespace httpservice
{
namespace detail
{

// Thread-local pool of fixed-size chunks shared by the sessions of one
// connection thread. Not thread-safe by design: every access happens on the
// owning thread.
//
// `total_bytes` is the preallocated reserve; when it is exhausted the pool
// falls back to a heap allocation. Chunks are released back on arena
// destruction so the reserve is reused across connections.
class chunk_pool
{
public:
    chunk_pool(std::size_t total_bytes, std::size_t chunk_size)
        : chunk_size_(std::max(chunk_size, std::size_t{4096}))
    {
        std::size_t const n = std::max<std::size_t>(1, total_bytes / chunk_size_);
        chunks_.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            chunks_.push_back(raw_alloc(chunk_size_));
        }
    }

    ~chunk_pool()
    {
        for (std::byte* p : chunks_)
        {
            ::operator delete(p, std::align_val_t(alignof(std::max_align_t)));
        }
    }

    chunk_pool(chunk_pool const&) = delete;
    chunk_pool& operator=(chunk_pool const&) = delete;

    std::size_t
    chunk_size() const noexcept
    {
        return chunk_size_;
    }

    // Returns a chunk with at least `min_bytes` bytes. Prefers a pooled chunk;
    // if the pool is empty it allocates a dedicated chunk from the heap.
    std::byte*
    acquire(std::size_t min_bytes)
    {
        if (min_bytes <= chunk_size_ && !chunks_.empty())
        {
            std::byte* p = chunks_.back();
            chunks_.pop_back();
            return p;
        }
        return raw_alloc(std::max(min_bytes, chunk_size_));
    }

    void
    release(std::byte* p) noexcept
    {
        chunks_.push_back(p);
    }

private:
    static std::byte*
    raw_alloc(std::size_t n)
    {
        return static_cast<std::byte*>(::operator new(n, std::align_val_t(alignof(std::max_align_t))));
    }

    std::size_t chunk_size_;
    std::vector<std::byte*> chunks_;
};

// Bump allocator for ONE session, borrowing chunks from a thread-local pool.
// Resetting rewinds this session's own chunks, so a request cycle can reuse
// the arena without touching other sessions' in-flight data.
class arena
{
public:
    explicit arena(chunk_pool& pool)
        : pool_(pool)
    {
    }

    ~arena()
    {
        release_all();
    }

    arena(arena const&) = delete;
    arena& operator=(arena const&) = delete;

    // Allocates `n` bytes aligned to `alignment` (power of two).
    // Auto-grows by borrowing another chunk when current ones are exhausted.
    void*
    allocate(std::size_t n, std::size_t alignment)
    {
        assert((alignment & (alignment - 1)) == 0 && "alignment must be a power of two");
        if (n == 0)
        {
            n = 1;
        }

        if (chunks_.empty())
        {
            grow(std::max(n + alignment, pool_.chunk_size()), alignment);
        }

        for (auto& chunk : chunks_)
        {
            void* base = chunk.data + chunk.offset;
            std::size_t space = chunk.size - chunk.offset;
            if (void* p = std::align(alignment, n, base, space))
            {
                chunk.offset = static_cast<std::byte*>(p) - chunk.data + n;
                return p;
            }
        }

        grow(n + alignment, alignment);
        chunk& back = chunks_.back();
        void* base = back.data + back.offset;
        std::size_t space = back.size - back.offset;
        void* p = std::align(alignment, n, base, space);
        assert(p != nullptr && "growth chunk must satisfy any allocation");
        back.offset = static_cast<std::byte*>(p) - back.data + n;
        return p;
    }

    // Frees every allocation made since the last reset; the session's own
    // chunks are reused.
    void
    reset()
    {
        for (auto& chunk : chunks_)
        {
            chunk.offset = 0;
        }
    }

    std::size_t
    capacity() const
    {
        std::size_t total = 0;
        for (auto const& chunk : chunks_)
        {
            total += chunk.size;
        }
        return total;
    }

private:
    struct chunk
    {
        std::byte* data;
        std::size_t size;
        std::size_t offset;
    };

    void
    grow(std::size_t min_bytes, std::size_t alignment)
    {
        std::byte* raw = pool_.acquire(min_bytes);
        std::size_t size = pool_.chunk_size();
        if (min_bytes > pool_.chunk_size())
        {
            size = min_bytes;
        }
        // align the returned memory; the pool/heap chunks are max_align aligned
        void* p = raw;
        std::size_t space = size;
        p = std::align(alignment, 1, p, space);
        std::byte* data = static_cast<std::byte*>(p);
        chunks_.push_back({data, size, 0});
    }

    void
    release_all()
    {
        for (auto& chunk : chunks_)
        {
            pool_.release(chunk.data);
        }
        chunks_.clear();
    }

    chunk_pool& pool_;
    std::vector<chunk> chunks_;
};

// The arena active on the current thread. Session handlers set this to the
// running session's own arena; handlers on a thread are serialized, so the
// pointer is always unambiguous.
inline thread_local arena* current_arena = nullptr;

} // namespace detail
} // namespace httpservice
