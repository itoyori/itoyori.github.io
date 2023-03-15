#pragma once

#include <cassert>

#include "ityr/common/util.hpp"
#include "ityr/ori/ori.hpp"

namespace ityr {

template <typename T>
class global_span {
  using this_t = global_span<T>;

public:
  using element_type = T;
  using value_type   = std::remove_cv_t<T>;
  using size_type    = std::size_t;
  using pointer      = ori::global_ptr<T>;
  using iterator     = pointer;
  using reference    = typename std::iterator_traits<pointer>::reference;

  global_span() {}
  template <typename ContiguousIterator>
  global_span(ContiguousIterator first, size_type n)
    : ptr_(&(*first)), n_(n) {}
  template <typename ContiguousIterator>
  global_span(ContiguousIterator first, ContiguousIterator last)
    : ptr_(&(*first)), n_(last - first) {}
  template <typename U>
  global_span(global_span<U> s) : ptr_(s.data()), n_(s.size() * sizeof(U) / sizeof(T)) {}

  constexpr pointer data() const noexcept { return ptr_; }
  constexpr size_type size() const noexcept { return n_; }

  constexpr iterator begin() const noexcept { return ptr_; }
  constexpr iterator end() const noexcept { return ptr_ + n_; }

  constexpr reference operator[](size_type i) const { assert(i <= n_); return ptr_[i]; }

  constexpr reference front() const { return *ptr_; }
  constexpr reference back() const { return *(ptr_ + n_ - 1); }

  constexpr bool empty() const noexcept { return n_ == 0; }

  constexpr this_t subspan(size_type offset, size_type count) const {
    assert(offset + count <= n_);
    return {ptr_ + offset, count};
  }

private:
  pointer   ptr_ = nullptr;
  size_type n_   = 0;
};

template <typename T>
inline constexpr auto data(const global_span<T>& s) noexcept {
  return s.data();
}

template <typename T>
inline constexpr auto size(const global_span<T>& s) noexcept {
  return s.size();
}

template <typename T>
inline constexpr auto begin(const global_span<T>& s) noexcept {
  return s.begin();
}

template <typename T>
inline constexpr auto end(const global_span<T>& s) noexcept {
  return s.end();
}

}
