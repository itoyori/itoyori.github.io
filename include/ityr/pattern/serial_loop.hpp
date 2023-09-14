#pragma once

#include "ityr/common/util.hpp"
#include "ityr/ito/ito.hpp"
#include "ityr/ori/ori.hpp"
#include "ityr/pattern/root_exec.hpp"
#include "ityr/pattern/global_iterator.hpp"
#include "ityr/container/workhint_view.hpp"

namespace ityr {

namespace execution {

/**
 * @brief Serial execution policy for iterator-based loop functions.
 * @see [std::execution -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/execution_policy_tag_t)
 * @see `ityr::execution::seq`
 * @see `ityr::execution::sequenced_policy`
 * @see `ityr::for_each()`
 */
struct sequenced_policy {
  /**
   * @brief The maximum number of elements to check out at the same time if automatic checkout is enabled.
   */
  std::size_t checkout_count = 1;

  constexpr sequenced_policy() noexcept {}

  constexpr sequenced_policy(std::size_t checkout_count) noexcept
    : checkout_count(checkout_count) {}
};

/**
 * @brief Parallel execution policy for iterator-based loop functions.
 * @see [std::execution -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/execution_policy_tag_t)
 * @see `ityr::execution::par`
 * @see `ityr::execution::parallel_policy`
 * @see `ityr::for_each()`
 */
template <typename WorkHint = common::wallclock::wallclock_t>
struct parallel_policy {
  constexpr parallel_policy() noexcept {}

  constexpr parallel_policy(std::size_t cutoff_count) noexcept
    : cutoff_count(cutoff_count), checkout_count(cutoff_count) {}

  constexpr parallel_policy(std::size_t cutoff_count, std::size_t checkout_count) noexcept
    : cutoff_count(cutoff_count), checkout_count(checkout_count) {}

  parallel_policy(workhint_range_view<WorkHint> workhint) noexcept
    : workhint(workhint) {}

  parallel_policy(std::size_t cutoff_count, workhint_range_view<WorkHint> workhint) noexcept
    : cutoff_count(cutoff_count), checkout_count(cutoff_count), workhint(workhint) {}

  parallel_policy(std::size_t cutoff_count, std::size_t checkout_count, workhint_range_view<WorkHint> workhint) noexcept
    : cutoff_count(cutoff_count), checkout_count(checkout_count), workhint(workhint) {}

  /**
   * @brief The maximum number of elements to check out at the same time if automatic checkout is enabled.
   */
  std::size_t cutoff_count = 1;

  /**
   * @brief The number of elements for leaf tasks to stop parallel recursion.
   */
  std::size_t checkout_count = 1;

  /**
   * @brief Work hints for ADWS.
   */
  workhint_range_view<WorkHint> workhint;
};

/**
 * @brief Default serial execution policy for iterator-based loop functions.
 * @see `ityr::execution::sequenced_policy`
 */
inline constexpr sequenced_policy seq;

/**
 * @brief Default parallel execution policy for iterator-based loop functions.
 * @see `ityr::execution::sequenced_policy`
 */
inline constexpr parallel_policy par;

namespace internal {

inline constexpr sequenced_policy to_sequenced_policy(const sequenced_policy& opts) noexcept {
  return opts;
}

template <typename WorkHint>
inline constexpr sequenced_policy to_sequenced_policy(const parallel_policy<WorkHint>& opts) noexcept {
  return sequenced_policy(opts.checkout_count);
}

inline void assert_policy(const sequenced_policy& opts) {
  ITYR_CHECK(0 < opts.checkout_count);
}

template <typename WorkHint>
inline void assert_policy(const parallel_policy<WorkHint>& opts) {
  ITYR_CHECK(0 < opts.checkout_count);
  ITYR_CHECK(opts.checkout_count <= opts.cutoff_count);
}

}
}

namespace internal {

inline auto checkout_global_iterators_aux(std::size_t) {
  return std::make_tuple(std::make_tuple(), std::make_tuple());
}

template <typename ForwardIterator, typename... ForwardIterators>
inline auto checkout_global_iterators_aux(std::size_t n, ForwardIterator it, ForwardIterators... rest) {
  ITYR_CHECK(n > 0);
  if constexpr (is_global_iterator_v<ForwardIterator>) {
    auto&& [cs, it_] = it.checkout_nb(n);
    auto&& [css, its] = checkout_global_iterators_aux(n, rest...);
    return std::make_tuple(std::tuple_cat(std::make_tuple(std::move(cs)), std::move(css)),
                           std::tuple_cat(std::make_tuple(it_), its));
  } else {
    auto&& [css, its] = checkout_global_iterators_aux(n, rest...);
    return std::make_tuple(std::move(css),
                           std::tuple_cat(std::make_tuple(it), its));
  }
}

template <typename... ForwardIterators>
inline auto checkout_global_iterators(std::size_t n, ForwardIterators... its) {
  auto ret = checkout_global_iterators_aux(n, its...);
  ori::checkout_complete();
  return ret;
}

template <typename Op, typename... ForwardIterators>
inline void apply_iterators(Op                  op,
                            std::size_t         n,
                            ForwardIterators... its) {
  for (std::size_t i = 0; i < n; (++i, ..., ++its)) {
    op(*its...);
  }
}

template <typename Op, typename ForwardIterator, typename... ForwardIterators>
inline void for_each_aux(const execution::sequenced_policy& policy,
                         Op                                 op,
                         ForwardIterator                    first,
                         ForwardIterator                    last,
                         ForwardIterators...                firsts) {
  if constexpr ((is_global_iterator_v<ForwardIterator> || ... ||
                 is_global_iterator_v<ForwardIterators>)) {
    // perform automatic checkout for global iterators
    std::size_t n = std::distance(first, last);
    std::size_t c = policy.checkout_count;

    for (std::size_t d = 0; d < n; d += c) {
      auto n_ = std::min(n - d, c);

      auto [css, its] = checkout_global_iterators(n_, first, firsts...);
      std::apply([&](auto&&... args) {
        apply_iterators(op, n_, std::forward<decltype(args)>(args)...);
      }, its);

      ((first = std::next(first, n_)), ..., (firsts = std::next(firsts, n_)));
    }

  } else {
    for (; first != last; (++first, ..., ++firsts)) {
      op(*first, *firsts...);
    }
  }
}

template <typename Iterator, typename Mode>
inline auto convert_to_global_iterator(Iterator it, Mode mode) {
  if constexpr (is_global_iterator_v<Iterator>) {
    static_assert(std::is_same_v<typename Iterator::mode, Mode> ||
                  std::is_same_v<typename Iterator::mode, checkout_mode::no_access_t>);
    return it;
  } else if constexpr (ori::is_global_ptr_v<Iterator>) {
    return make_global_iterator(it, mode);
  } else {
    return it;
  }
}

}

template <typename BidirectionalIterator1, typename BidirectionalIteratorD>
inline BidirectionalIteratorD
move_backward(const execution::sequenced_policy& policy,
              BidirectionalIterator1             first1,
              BidirectionalIterator1             last1,
              BidirectionalIteratorD             first_d) {
  if constexpr (ori::is_global_ptr_v<BidirectionalIterator1> ||
                ori::is_global_ptr_v<BidirectionalIteratorD>) {
    using value_type1  = typename std::iterator_traits<BidirectionalIterator1>::value_type;
    using value_type_d = typename std::iterator_traits<BidirectionalIteratorD>::value_type;
    return move_backward(
        policy,
        internal::convert_to_global_iterator(first1 , internal::src_checkout_mode_t<value_type1>{}),
        internal::convert_to_global_iterator(last1  , internal::src_checkout_mode_t<value_type1>{}),
        internal::convert_to_global_iterator(first_d, internal::dest_checkout_mode_t<value_type_d>{}));

  } else {
    using std::make_move_iterator;
    using std::make_reverse_iterator;
    internal::for_each_aux(
        policy,
        [&](auto&& r1, auto&& d) {
          d = std::move(r1);
        },
        make_reverse_iterator(make_move_iterator(last1)),
        make_reverse_iterator(make_move_iterator(first1)),
        make_reverse_iterator(first_d));

    return std::prev(first_d, std::distance(first1, last1));
  }
}

ITYR_TEST_CASE("[ityr::pattern::serial_loop] move_backward") {
  ito::init();
  ori::init();

  long n = 100000;
  ori::global_ptr<common::move_only_t> p = ori::malloc_coll<common::move_only_t>(n);

  root_exec([=] {
    internal::for_each_aux(
        execution::sequenced_policy(128),
        [&](common::move_only_t& mo, long i) {
          mo = common::move_only_t{i};
        },
        make_global_iterator(p    , checkout_mode::read_write),
        make_global_iterator(p + n, checkout_mode::read_write),
        count_iterator<long>(0));

    long offset = 1000;
    move_backward(
        execution::sequenced_policy(128),
        p, p + n - offset, p + n);

    internal::for_each_aux(
        execution::sequenced_policy(128),
        [&](const common::move_only_t& mo, long i) {
          if (i < offset) {
            ITYR_CHECK(mo.value() == -1);
          } else {
            ITYR_CHECK(mo.value() == i - offset);
          }
        },
        make_global_iterator(p    , checkout_mode::read),
        make_global_iterator(p + n, checkout_mode::read),
        count_iterator<long>(0));
  });

  ori::free_coll(p);

  ori::fini();
  ito::fini();
}

}
