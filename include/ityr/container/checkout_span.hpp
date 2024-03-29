#pragma once

#include "ityr/common/util.hpp"
#include "ityr/ori/ori.hpp"
#include "ityr/container/global_span.hpp"

namespace ityr {

namespace checkout_mode {

/** @brief See `ityr::checkout_mode::read`. */
using read_t = ori::mode::read_t;

/**
 * @brief Read-only checkout mode.
 * @see `ityr::make_checkout()`.
 * @see `ityr::make_global_iterator()`.
 */
inline constexpr read_t read;

/** @brief See `ityr::checkout_mode::write`. */
using write_t = ori::mode::write_t;

/**
 * @brief Write-only checkout mode.
 * @see `ityr::make_checkout()`.
 * @see `ityr::make_global_iterator()`.
 */
inline constexpr write_t write;

/** @brief See `ityr::checkout_mode::read_write`. */
using read_write_t = ori::mode::read_write_t;

/**
 * @brief Read+Write checkout mode.
 * @see `ityr::make_checkout()`.
 * @see `ityr::make_global_iterator()`.
 */
inline constexpr read_write_t read_write;

/** @brief See `ityr::checkout_mode::no_access`. */
struct no_access_t {};

/**
 * @brief Checkout mode to disable automatic checkout.
 * @see `ityr::make_global_iterator()`.
 */
inline constexpr no_access_t no_access;

}

/**
 * @brief Checkout span to automatically manage the lifetime of checked-out memory.
 *
 * A global memory region can be checked out at the constructor and checked in at the destructor.
 * The checkout span can be moved but cannot be copied, in order to ensure the checkin operation is
 * performed only once.
 * The checkout span can be used as in `std::span` (C++20) to access elements in the checked-out
 * memory region.
 *
 * `ityr::make_checkout()` is a helper function to create the checkout span.
 *
 * @see `ityr::make_checkout()`.
 */
template <typename T, typename Mode>
class checkout_span {
public:
  using element_type           = T;
  using value_type             = std::remove_cv_t<element_type>;
  using size_type              = std::size_t;
  using pointer                = element_type*;
  using const_pointer          = std::add_const_t<element_type>*;
  using difference_type        = typename std::iterator_traits<pointer>::difference_type;
  using reference              = typename std::iterator_traits<pointer>::reference;
  using const_reference        = typename std::iterator_traits<const_pointer>::reference;
  using iterator               = pointer;
  using const_iterator         = const_pointer;
  using reverse_iterator       = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  checkout_span() {}

  /**
   * @brief Construct a checkout span by checking out a global memory region.
   */
  explicit checkout_span(ori::global_ptr<T> gptr, std::size_t n, Mode)
    : ptr_((gptr && n > 0) ? ori::checkout(gptr, n, Mode{}) : nullptr),
      n_(n) {
    ITYR_CHECK(((ptr_ && n_ > 0) || (!ptr_ && n == 0)));
  }

  /**
   * @brief Perform the checkin operation when destroyed.
   */
  ~checkout_span() { destroy(); }

  checkout_span(const checkout_span&) = delete;
  checkout_span& operator=(const checkout_span&) = delete;

  checkout_span(checkout_span&& cs) : ptr_(cs.ptr_), n_(cs.n_) { cs.ptr_ = nullptr; cs.n_ = 0; }
  checkout_span& operator=(checkout_span&& cs) {
    destroy();
    ptr_ = cs.ptr_;
    n_   = cs.n_;
    cs.ptr_ = nullptr;
    cs.n_   = 0;
    return *this;
  }

  constexpr pointer data() const noexcept { return ptr_; }
  constexpr size_type size() const noexcept { return n_; }

  constexpr iterator begin() const noexcept { return ptr_; }
  constexpr iterator end() const noexcept { return ptr_ + n_; }

  constexpr const_iterator cbegin() const noexcept { return ptr_; }
  constexpr const_iterator cend() const noexcept { return ptr_ + n_; }

  constexpr reverse_iterator rbegin() const noexcept { return std::make_reverse_iterator(end()); }
  constexpr reverse_iterator rend() const noexcept { return std::make_reverse_iterator(begin()); }

  constexpr const_reverse_iterator crbegin() const noexcept { return std::make_reverse_iterator(cend()); }
  constexpr const_reverse_iterator crend() const noexcept { return std::make_reverse_iterator(begin()); }

  constexpr reference operator[](size_type i) const { assert(i <= n_); return ptr_[i]; }

  constexpr reference front() const { return *ptr_; }
  constexpr reference back() const { return *(ptr_ + n_ - 1); }

  constexpr bool empty() const noexcept { return n_ == 0; }

  /**
   * @brief Manually perform the checkout operation by checking in the previous span.
   */
  void checkout(ori::global_ptr<T> gptr, std::size_t n, Mode) {
    checkin();
    ptr_ = ori::checkout(gptr, n, Mode{});
    n_ = n;
  }

  /**
   * @brief Manually perform the nonblocking checkout operation by checking in the previous span.
   */
  void checkout_nb(ori::global_ptr<T> gptr, std::size_t n, Mode) {
    checkin();
    ptr_ = ori::checkout_nb(gptr, n, Mode{}).first;
    n_ = n;
  }

  /**
   * @brief Manually perform the checkin operation by discarding the current checkout span.
   */
  void checkin() {
    if (ptr_) {
      ori::checkin(ptr_, n_, Mode{});
      ptr_ = nullptr;
      n_   = 0;
    }
  }

private:
  void destroy() {
    if (ptr_) {
      ori::checkin(ptr_, n_, Mode{});
    }
  }

  pointer   ptr_ = nullptr;
  size_type n_   = 0;
};

template <typename T, typename Mode>
inline constexpr auto data(const checkout_span<T, Mode>& cs) noexcept {
  return cs.data();
}

template <typename T, typename Mode>
inline constexpr auto size(const checkout_span<T, Mode>& cs) noexcept {
  return cs.size();
}

template <typename T, typename Mode>
inline constexpr auto begin(const checkout_span<T, Mode>& cs) noexcept {
  return cs.begin();
}

template <typename T, typename Mode>
inline constexpr auto end(const checkout_span<T, Mode>& cs) noexcept {
  return cs.end();
}

/**
 * @brief Checkout a global memory region.
 *
 * @param gptr Starting global pointer.
 * @param n    Number of elements to be checked out.
 * @param mode Checkout mode (`ityr::checkout_mode`).
 *
 * @return The checkout span `ityr::checkout_span` for the specified range.
 *
 * This function checks out the global memory range `[gptr, gptr + n)`.
 * After this call, this virtual memory region becomes directly accessible by CPU load/store
 * instructions. In programs, it is recommended to access the memory via the returned checkout span.
 *
 * The checkout mode `mode` can be either `read`, `read_write`, or `write`.
 * - If `read` or `read_write`, the checked-out region has the latest data.
 * - If `read_write` or `write`, the entire checked-out region is considered modified.
 *
 * The checkout span automatically performs a checkin operation when destroyed (e.g., when exiting
 * the scope). The lifetime of the checkout span cannot overlap with any fork/join call, because threads
 * can be migrated and a pair of checkout and checkin calls must be performed in the same process.
 *
 * Overlapping regions can be checked out by multiple processes at the same time, as long as no data
 * race occurs (i.e., all regions are checked out with `ityr::checkout_mode::read`).
 * Within each process, multiple regions can be simultaneously checked out with an arbitrary mode,
 * and the memory ordering to the checked-out region is determined to the program order (because
 * the same memory view is exposed to the process).
 *
 * Example:
 * ```
 * ityr::global_vector<int> v = {1, 2, 3, 4, 5};
 * {
 *   auto cs = ityr::make_checkout(v.data(), v.size(), ityr::checkout_mode::read);
 *   for (int i : cs) {
 *     std::cout << i << " ";
 *   }
 *   std::cout << std::endl;
 *   // automatic checkin when `cs` is destroyed
 * }
 * // Output: 1 2 3 4 5
 * ```
 *
 * @see [std::span -- cppreference.com](https://en.cppreference.com/w/cpp/container/span)
 * @see `ityr::checkout_mode::read`, `ityr::checkout_mode::read_write`, `ityr::checkout_mode::write`
 * @see `ityr::make_global_iterator()`
 */
template <typename T, typename Mode>
inline checkout_span<T, Mode> make_checkout(ori::global_ptr<T> gptr, std::size_t n, Mode mode) {
  return checkout_span<T, Mode>{gptr, n, mode};
}

/**
 * @brief Checkout a global memory region.
 *
 * @param gspan Global span to be checked out.
 * @param mode  Checkout mode (`ityr::checkout_mode`).
 *
 * @return The checkout span `ityr::checkout_span` for the specified range.
 *
 * Equivalent to `ityr::make_checkout(gspan.data(), gspan.size(), mode)`.
 */
template <typename T, typename Mode>
inline checkout_span<T, Mode> make_checkout(global_span<T> gspan, Mode mode) {
  return checkout_span<T, Mode>{gspan.data(), gspan.size(), mode};
}

namespace internal {

inline auto make_checkouts_aux() {
  return std::make_tuple();
}

template <typename T, typename Mode, typename... Rest>
inline auto make_checkouts_aux(ori::global_ptr<T> gptr, std::size_t n, Mode mode, Rest&&... rest) {
  checkout_span<T, Mode> cs;
  cs.checkout_nb(gptr, n, mode);
  return std::tuple_cat(std::make_tuple(std::move(cs)),
                        make_checkouts_aux(std::forward<Rest>(rest)...));
}

template <typename T, typename Mode, typename... Rest>
inline auto make_checkouts_aux(global_span<T> gspan, Mode mode, Rest&&... rest) {
  checkout_span<T, Mode> cs;
  cs.checkout_nb(gspan.data(), gspan.size(), mode);
  return std::tuple_cat(std::make_tuple(std::move(cs)),
                        make_checkouts_aux(std::forward<Rest>(rest)...));
}

}

/**
 * @brief Checkout multiple global memory regions.
 *
 * @param args... Sequence of checkout requests. Each checkout request should be in the form of
 *                `<global_ptr>, <num_elems>, <checkout_mode>` or `<global_span>, <checkout_mode>`.
 *
 * @return A tuple collecting the checkout spans for each checkout request.
 *
 * This function performs multiple checkout operations at the same time.
 * This may improve performance by overlapping communication to fetch remote data, compared to
 * checking out one by one.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<int> v2 = {2, 3, 4, 5, 6};
 * ityr::global_vector<int> v3(10);
 *
 * ityr::global_span<int> s2(v2);
 *
 * auto [cs1, cs2, cs3] =
 *   ityr::make_checkouts(
 *       v1.data(), v1.size(), ityr::checkout_mode::read,
 *       s2, ityr::checkout_mode::read_write,
 *       v3.data() + 2, 3, ityr::checkout_mode::write);
 * ```
 *
 * @see `ityr::make_checkout()`
 */
template <typename... Args>
inline auto make_checkouts(Args&&... args) {
  auto css = internal::make_checkouts_aux(std::forward<Args>(args)...);
  ori::checkout_complete();
  return css;
}

}
