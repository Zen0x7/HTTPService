#pragma once

#include "arena.hpp"

#include <cassert>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace httpservice
{
namespace detail
{

// C++11 allocator backed by a thread-local arena.
//
// Copies share the same arena. deallocate() is a no-op: memory is reclaimed
// in bulk by arena::reset(). Propagation traits are enabled so containers
// keep pointing at the originating arena across moves and swaps.
template <class T>
class arena_allocator
{
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    arena_allocator() noexcept
        : arena_(current_arena)
    {
    }

    explicit arena_allocator(arena* a) noexcept
        : arena_(a)
    {
    }

    template <class U>
    arena_allocator(arena_allocator<U> const& other) noexcept
        : arena_(other.arena_)
    {
    }

    template <class U>
    struct rebind
    {
        using other = arena_allocator<U>;
    };

    T*
    allocate(std::size_t n)
    {
        assert(arena_ != nullptr && "arena_allocator used outside a connection thread");
        void* p = arena_->allocate(n * sizeof(T), alignof(T));
        return static_cast<T*>(p);
    }

    void
    deallocate(T*, std::size_t) noexcept
    {
        // Reclaimed by arena::reset().
    }

    arena_allocator
    select_on_container_copy_construction() const noexcept
    {
        return *this;
    }

    arena* arena_;

    friend bool
    operator==(arena_allocator const& lhs, arena_allocator const& rhs) noexcept
    {
        return lhs.arena_ == rhs.arena_;
    }

    friend bool
    operator!=(arena_allocator const& lhs, arena_allocator const& rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

template <class T, class U>
bool
operator==(arena_allocator<T> const& lhs, arena_allocator<U> const& rhs) noexcept
{
    return lhs.arena_ == rhs.arena_;
}

template <class T, class U>
bool
operator!=(arena_allocator<T> const& lhs, arena_allocator<U> const& rhs) noexcept
{
    return !(lhs == rhs);
}

} // namespace detail
} // namespace httpservice
