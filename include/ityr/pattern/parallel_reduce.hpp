#pragma once

#include "ityr/common/util.hpp"
#include "ityr/common/topology.hpp"
#include "ityr/ito/ito.hpp"
#include "ityr/ori/ori.hpp"
#include "ityr/pattern/count_iterator.hpp"
#include "ityr/pattern/global_iterator.hpp"
#include "ityr/pattern/serial_loop.hpp"
#include "ityr/pattern/parallel_loop.hpp"
#include "ityr/pattern/reducer.hpp"

namespace ityr {

namespace internal {

template <typename AccumulateOp, typename CombineOp, typename Reducer,
          typename ReleaseHandler, typename ForwardIterator, typename... ForwardIterators,
          typename = std::enable_if_t<!Reducer::direct_accumulation>>
inline void
parallel_reduce_generic(const execution::parallel_policy&       policy,
                        AccumulateOp                            accumulate_op,
                        CombineOp                               combine_op,
                        Reducer                                 reducer,
                        typename Reducer::accumulator_view_type acc_view,
                        ReleaseHandler                          rh,
                        ForwardIterator                         first,
                        ForwardIterator                         last,
                        ForwardIterators...                     firsts) {
  ori::poll();

  // for immediately executing cross-worker tasks in ADWS
  ito::poll([] { return ori::release_lazy(); },
            [&](ori::release_handler rh_) { ori::acquire(rh); ori::acquire(rh_); });

  auto d = std::distance(first, last);
  if (static_cast<std::size_t>(d) <= policy.cutoff_count) {
    for_each_aux(
        execution::internal::to_sequenced_policy(policy),
        [&](auto&&... refs) {
          accumulate_op(acc_view, std::forward<decltype(refs)>(refs)...);
        },
        first, last, firsts...);
    return;
  }

  auto mid = std::next(first, d / 2);

  auto tgdata = ito::task_group_begin();

  ito::thread<void> th(
      ito::with_callback, [=] { ori::acquire(rh); }, [] { ori::release(); },
      ito::with_workhint, 1, 1,
      [=] {
        parallel_reduce_generic(policy, accumulate_op, combine_op, reducer,
                                acc_view, rh, first, mid, firsts...);
      });

  if (th.serialized()) {
    th.join();

    parallel_reduce_generic(policy, accumulate_op, combine_op, reducer,
                            acc_view, rh, mid, last, std::next(firsts, d / 2)...);

    ito::task_group_end(tgdata, [] { ori::release(); }, [] { ori::acquire(); });

  } else {
    auto acc_r = reducer.identity();
    auto acc_view_r = reducer.view(acc_r);
    rh = ori::release_lazy();

    parallel_reduce_generic(policy, accumulate_op, combine_op, reducer,
                            acc_view_r, rh, mid, last, std::next(firsts, d / 2)...);

    ori::release();

    th.join();

    ito::task_group_end(tgdata, [] { ori::release(); }, [] { ori::acquire(); });

    ori::acquire();

    combine_op(acc_view, acc_view_r, first, mid, last, firsts...);
  }
}

template <typename AccumulateOp, typename CombineOp, typename Reducer,
          typename ReleaseHandler, typename ForwardIterator, typename... ForwardIterators>
inline typename Reducer::accumulator_type
parallel_reduce_generic(execution::parallel_policy         policy,
                        AccumulateOp                       accumulate_op,
                        CombineOp                          combine_op,
                        Reducer                            reducer,
                        typename Reducer::accumulator_type acc,
                        ReleaseHandler                     rh,
                        ForwardIterator                    first,
                        ForwardIterator                    last,
                        ForwardIterators...                firsts) {
  using acc_t = typename Reducer::accumulator_type;

  ori::poll();

  // for immediately executing cross-worker tasks in ADWS
  ito::poll([] { return ori::release_lazy(); },
            [&](ori::release_handler rh_) { ori::acquire(rh); ori::acquire(rh_); });

  auto d = std::distance(first, last);
  if (static_cast<std::size_t>(d) <= policy.cutoff_count) {
    for_each_aux(
        execution::internal::to_sequenced_policy(policy),
        [&](auto&&... refs) {
          accumulate_op(acc, std::forward<decltype(refs)>(refs)...);
        },
        first, last, firsts...);
    return acc;
  }

  auto mid = std::next(first, d / 2);

  auto tgdata = ito::task_group_begin();

  ito::thread<acc_t> th(
      ito::with_callback, [=] { ori::acquire(rh); }, [] { ori::release(); },
      ito::with_workhint, 1, 1,
      [=] {
        return parallel_reduce_generic(policy, accumulate_op, combine_op, reducer,
                                       acc, rh, first, mid, firsts...);
      });

  if (th.serialized()) {
    acc = parallel_reduce_generic(policy, accumulate_op, combine_op, reducer,
                                  th.join(), rh, mid, last, std::next(firsts, d / 2)...);

    ito::task_group_end(tgdata, [] { ori::release(); }, [] { ori::acquire(); });

  } else {
    acc_t new_acc = reducer.identity();
    rh = ori::release_lazy();

    acc_t acc_r = parallel_reduce_generic(policy, accumulate_op, combine_op, reducer,
                                          std::move(new_acc), rh, mid, last, std::next(firsts, d / 2)...);

    ori::release();

    acc = th.join();

    ito::task_group_end(tgdata, [] { ori::release(); }, [] { ori::acquire(); });

    ori::acquire();

    combine_op(acc, std::move(acc_r), first, mid, last, firsts...);
  }

  return acc;
}

template <typename AccumulateOp, typename CombineOp, typename Reducer, typename AccView,
          typename ForwardIterator, typename... ForwardIterators>
inline auto reduce_generic(const execution::sequenced_policy& policy,
                           AccumulateOp                       accumulate_op,
                           CombineOp                          combine_op [[maybe_unused]],
                           Reducer                            reducer [[maybe_unused]],
                           AccView                            acc,
                           ForwardIterator                    first,
                           ForwardIterator                    last,
                           ForwardIterators...                firsts) {
  execution::internal::assert_policy(policy);
  for_each_aux(
      execution::internal::to_sequenced_policy(policy),
      [&](auto&&... refs) {
        accumulate_op(acc, std::forward<decltype(refs)>(refs)...);
      },
      first, last, firsts...);
  if constexpr (Reducer::direct_accumulation) {
    return acc;
  }
}

template <typename AccumulateOp, typename CombineOp, typename Reducer, typename AccView,
          typename ForwardIterator, typename... ForwardIterators>
inline auto reduce_generic(const execution::parallel_policy& policy,
                           AccumulateOp                      accumulate_op,
                           CombineOp                         combine_op,
                           Reducer                           reducer,
                           AccView                           acc,
                           ForwardIterator                   first,
                           ForwardIterator                   last,
                           ForwardIterators...               firsts) {
  execution::internal::assert_policy(policy);
  auto rh = ori::release_lazy();
  return parallel_reduce_generic(policy, accumulate_op, combine_op, reducer, acc,
                                 rh, first, last, firsts...);
}

}

/**
 * @brief Calculate reduction while transforming each element.
 *
 * @param policy             Execution policy (`ityr::execution`).
 * @param first              Begin iterator.
 * @param last               End iterator.
 * @param reducer            Reducer object (`ityr::reducer`).
 * @param unary_transform_op Unary operator to transform each element.
 *
 * @return The reduced result.
 *
 * This function applies `unary_transform_op` to each element in the range `[first, last)` and
 * performs reduction over them.
 *
 * If global pointers are provided as iterators, they are automatically checked out with the read-only
 * mode in the specified granularity (`ityr::execution::sequenced_policy::checkout_count` if serial,
 * or `ityr::execution::parallel_policy::checkout_count` if parallel) without explicitly passing them
 * as global iterators.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * int r = ityr::transform_reduce(ityr::execution::par, v1.begin(), v1.end(), ityr::reducer::plus<int>{},
 *                                [](int x) { return x * x; });
 * // r = 55
 * ```
 *
 * @see [std::transform_reduce -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/transform_reduce)
 * @see `ityr::reduce()`
 * @see `ityr::transform()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator,
          typename Reducer, typename UnaryTransformOp>
inline typename Reducer::accumulator_type
transform_reduce(const ExecutionPolicy& policy,
                 ForwardIterator        first,
                 ForwardIterator        last,
                 Reducer                reducer,
                 UnaryTransformOp       unary_transform_op) {
  if constexpr (ori::is_global_ptr_v<ForwardIterator>) {
    return transform_reduce(
        policy,
        internal::convert_to_global_iterator(first, checkout_mode::read),
        internal::convert_to_global_iterator(last , checkout_mode::read),
        reducer,
        unary_transform_op);

  } else {
    auto accumulate_op = [=](auto&& acc, const auto& r) {
      reducer.foldl(std::forward<decltype(acc)>(acc), unary_transform_op(r));
    };

    auto combine_op = [=](auto&& acc1, auto&& acc2,
                          ForwardIterator, ForwardIterator, ForwardIterator) {
      reducer.foldl(std::forward<decltype(acc1)>(acc1), std::forward<decltype(acc2)>(acc2));
    };

    if constexpr (Reducer::direct_accumulation) {
      return internal::reduce_generic(policy, accumulate_op, combine_op, reducer,
                                      reducer.identity(), first, last);
    } else {
      auto acc = reducer.identity();
      internal::reduce_generic(policy, accumulate_op, combine_op, reducer,
                               reducer.view(acc), first, last);
      return acc;
    }
  }
}

/**
 * @brief Calculate reduction by transforming each element.
 *
 * @param policy              Execution policy (`ityr::execution`).
 * @param first1              1st begin iterator.
 * @param last1               1st end iterator.
 * @param first2              2nd begin iterator.
 * @param reducer             Reducer object (`ityr::reducer`).
 * @param binary_transform_op Binary operator to transform a pair of each element.
 *
 * @return The reduced result.
 *
 * This function applies `binary_transform_op` to a pair of each element in the range `[first1, last1)`
 * and `[first2, first2 + (last1 - first1)]` and performs reduction over them.
 *
 * If global pointers are provided as iterators, they are automatically checked out with the read-only
 * mode in the specified granularity (`ityr::execution::sequenced_policy::checkout_count` if serial,
 * or `ityr::execution::parallel_policy::checkout_count` if parallel) without explicitly passing them
 * as global iterators.
 * The specified regions can be overlapped.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * bool r = ityr::transform_reduce(ityr::execution::par, v1.begin(), v1.end() - 1, v1.begin() + 1,
 *                                 ityr::reducer::logical_and<>{}, [](int x, int y) { return x <= y; });
 * // r = true (v1 is sorted)
 * ```
 *
 * @see [std::transform_reduce -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/transform_reduce)
 * @see `ityr::reduce()`
 * @see `ityr::transform()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIterator2,
          typename Reducer, typename BinaryTransformOp>
inline typename Reducer::accumulator_type
transform_reduce(const ExecutionPolicy& policy,
                 ForwardIterator1       first1,
                 ForwardIterator1       last1,
                 ForwardIterator2       first2,
                 Reducer                reducer,
                 BinaryTransformOp      binary_transform_op) {
  if constexpr (ori::is_global_ptr_v<ForwardIterator1> ||
                ori::is_global_ptr_v<ForwardIterator2>) {
    return transform_reduce(
        policy,
        internal::convert_to_global_iterator(first1, checkout_mode::read),
        internal::convert_to_global_iterator(last1 , checkout_mode::read),
        internal::convert_to_global_iterator(first2, checkout_mode::read),
        reducer,
        binary_transform_op);

  } else {
    auto accumulate_op = [=](auto&& acc, const auto& r1, const auto& r2) {
      reducer.foldl(std::forward<decltype(acc)>(acc), binary_transform_op(r1, r2));
    };

    auto combine_op = [=](auto&& acc1, auto&& acc2,
                          ForwardIterator1, ForwardIterator1, ForwardIterator1, ForwardIterator2) {
      reducer.foldl(std::forward<decltype(acc1)>(acc1), std::forward<decltype(acc2)>(acc2));
    };

    if constexpr (Reducer::direct_accumulation) {
      return internal::reduce_generic(policy, accumulate_op, combine_op, reducer,
                                      reducer.identity(), first1, last1, first2);
    } else {
      auto acc = reducer.identity();
      internal::reduce_generic(policy, accumulate_op, combine_op, reducer,
                               reducer.view(acc), first1, last1, first2);
      return acc;
    }
  }
}

/**
 * @brief Calculate a dot product.
 *
 * @param policy Execution policy (`ityr::execution`).
 * @param first1 1st begin iterator.
 * @param last1  1st end iterator.
 * @param first2 2nd begin iterator.
 *
 * @return The reduced result.
 *
 * Equivalent to `ityr::transform_reduce(policy, first1, last1, first2, ityr::reducer::plus<T>{}, std::multiplies<>{})`,
 * where `T` is the type of the expression `(*first1) * (*first2)`.
 * This corresponds to calculating a dot product of two vectors.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<int> v2 = {2, 3, 4, 5, 6};
 * int dot = ityr::transform_reduce(ityr::execution::par, v1.begin(), v1.end(), v2.begin());
 * // dot = 70
 * ```
 *
 * @see [std::transform_reduce -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/transform_reduce)
 * @see `ityr::reduce()`
 * @see `ityr::transform()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIterator2>
inline auto transform_reduce(const ExecutionPolicy& policy,
                             ForwardIterator1       first1,
                             ForwardIterator1       last1,
                             ForwardIterator2       first2) {
  using T = decltype((*first1) * (*first2));
  return transform_reduce(policy, first1, last1, first2, reducer::plus<T>{}, std::multiplies<>{});
}

/**
 * @brief Calculate reduction.
 *
 * @param policy  Execution policy (`ityr::execution`).
 * @param first   Begin iterator.
 * @param last    End iterator.
 * @param reducer Reducer object (`ityr::reducer`).
 *
 * @return The reduced result.
 *
 * This function performs reduction over the elements in the given range `[first, last)`.
 *
 * If global pointers are provided as iterators, they are automatically checked out with the read-only
 * mode in the specified granularity (`ityr::execution::sequenced_policy::checkout_count` if serial,
 * or `ityr::execution::parallel_policy::checkout_count` if parallel) without explicitly passing them
 * as global iterators.
 *
 * Itoyori's reduce operation resembles the standard `std::reduce()`, but it differs from the standard
 * one in that `ityr::reduce()` receives a `reducer`. A reducer offers an *associative* binary operator
 * that satisfies `op(x, op(y, z)) == op(op(x, y), z)`, and an *identity* element that satisfies
 * `op(identity, x) = x` and `op(x, identity) = x`. Note that *commucativity* is not required unlike
 * the standard `std::reduce()`.
 *
 * TODO: How to define a reducer is to be documented.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v = {1, 2, 3, 4, 5};
 * int product = ityr::reduce(ityr::execution::par, v.begin(), v.end(), ityr::reducer::multiplies<int>{});
 * // product = 120
 * ```
 *
 * @see [std::reduce -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/reduce)
 * @see `ityr::transform_reduce()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator, typename Reducer>
inline typename Reducer::accumulator_type
reduce(const ExecutionPolicy& policy,
       ForwardIterator        first,
       ForwardIterator        last,
       Reducer                reducer) {
  return transform_reduce(policy, first, last, reducer,
                          [](auto&& r) { return std::forward<decltype(r)>(r); });
}

/**
 * @brief Calculate reduction.
 *
 * @param policy Execution policy (`ityr::execution`).
 * @param first  Begin iterator.
 * @param last   End iterator.
 *
 * @return The reduced result.
 *
 * Equivalent to `ityr::reduce(policy, first, last, ityr::reducer::plus<T>{})`, where type `T` is
 * the value type of given iterators (`ForwardIterator`).
 */
template <typename ExecutionPolicy, typename ForwardIterator>
inline typename std::iterator_traits<ForwardIterator>::value_type
reduce(const ExecutionPolicy& policy,
       ForwardIterator        first,
       ForwardIterator        last) {
  using T = typename std::iterator_traits<ForwardIterator>::value_type;
  return reduce(policy, first, last, reducer::plus<T>{});
}

/**
 * @brief Calculate a prefix sum (inclusive scan) while transforming each element.
 *
 * @param policy             Execution policy (`ityr::execution`).
 * @param first1             Input begin iterator.
 * @param last1              Input end iterator.
 * @param first_d            Output begin iterator.
 * @param reducer            Reducer object (`ityr::reducer`).
 * @param unary_transform_op Unary operator to transform each element.
 * @param init               Initial value for the prefix sum.
 *
 * @return The end iterator of the output range (`first_d + (last1 - first1)`).
 *
 * This function applies `unary_transform_op` to each element in the range `[first1, last1)` and
 * calculates a prefix sum over them. The prefix sum is inclusive, which means that the i-th element
 * of the prefix sum includes the i-th element in the input range. That is, the i-th element of the
 * prefix sum is: `init + f(*first1) + ... + f(*(first1 + i))`, where `+` is the associative binary
 * operator (provided by `reducer`) and `f()` is the transform operator (`unary_transform_op`).
 * The calculated prefix sum is stored in the output range `[first_d, first_d + (last1 - first1))`.
 *
 * If given iterators are global pointers, they are automatically checked out in the specified
 * granularity (`ityr::execution::sequenced_policy::checkout_count` if serial,
 * or `ityr::execution::parallel_policy::checkout_count` if parallel) without explicitly passing them
 * as global iterators.
 * Input global pointers (`first1` and `last1`) are automatically checked out with the read-only mode.
 * Similarly, output global iterator (`first_d`) are checked out with the write-only mode if their
 * value type is *trivially copyable*; otherwise, they are checked out with the read-write mode.
 *
 * Overlapping regions can be specified for the input and output ranges.
 *
 * Unlike the standard `std::transform_inclusive_scan()`, Itoyori's `ityr::transform_inclusive_scan()`
 * requires a `reducer` as `ityr::reduce()` does.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<double> v2(v1.size());
 * ityr::transform_inclusive_scan(ityr::execution::par, v1.begin(), v1.end(), v2.begin(),
 *                                ityr::reducer::multiplies<double>{},
 *                                [](int x) { return static_cast<double>(x); }, 0.01);
 * // v2 = {0.01, 0.02, 0.06, 0.24, 1.2}
 * ```
 *
 * @see [std::transform_inclusive_scan -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/transform_inclusive_scan)
 * @see `ityr::inclusive_scan()`
 * @see `ityr::reduce()`
 * @see `ityr::transform()`
 * @see `ityr::transform_reduce()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIteratorD,
          typename Reducer, typename UnaryTransformOp>
inline ForwardIteratorD
transform_inclusive_scan(const ExecutionPolicy&                    policy,
                         ForwardIterator1                          first1,
                         ForwardIterator1                          last1,
                         ForwardIteratorD                          first_d,
                         Reducer                                   reducer,
                         UnaryTransformOp                          unary_transform_op,
                         const typename Reducer::accumulator_type& init) {
  if constexpr (ori::is_global_ptr_v<ForwardIterator1> ||
                ori::is_global_ptr_v<ForwardIteratorD>) {
    using value_type_d = typename std::iterator_traits<ForwardIteratorD>::value_type;
    return transform_inclusive_scan(
        policy,
        internal::convert_to_global_iterator(first1 , checkout_mode::read),
        internal::convert_to_global_iterator(last1  , checkout_mode::read),
        internal::convert_to_global_iterator(first_d, internal::dest_checkout_mode_t<value_type_d>{}),
        reducer,
        unary_transform_op,
        init);

  } else {
    auto accumulate_op = [=](auto&& acc, const auto& r1, auto&& d) {
      reducer.foldl(acc, unary_transform_op(r1));
      d = reducer.clone(acc);
    };

    // TODO: more efficient scan implementation
    auto combine_op = [=](auto&&           acc1,
                          auto&&           acc2,
                          ForwardIterator1 first_,
                          ForwardIterator1 mid_,
                          ForwardIterator1 last_,
                          ForwardIteratorD first_d_) {
      // Add the left accumulator `acc1` to the right half of the region
      auto dm = std::distance(first_, mid_);
      auto dl = std::distance(first_, last_);
      if constexpr (!is_global_iterator_v<ForwardIteratorD>) {
        for_each(policy, std::next(first_d_, dm), std::next(first_d_, dl),
                 [=](auto&& acc_r) { reducer.foldr(acc1, acc_r); });
      } else if constexpr (std::is_same_v<typename ForwardIteratorD::mode, checkout_mode::no_access_t>) {
        for_each(policy, std::next(first_d_, dm), std::next(first_d_, dl),
                 [=](auto&& acc_r) { reducer.foldr(acc1, acc_r); });
      } else {
        // &*: convert global_iterator -> global_ref -> global_ptr
        auto fd = make_global_iterator(&*first_d_, checkout_mode::read_write);
        for_each(policy, std::next(fd, dm), std::next(fd, dl),
                 [=](auto&& acc_r) { reducer.foldr(acc1, acc_r); });
      }
      reducer.foldl(std::forward<decltype(acc1)>(acc1), std::forward<decltype(acc2)>(acc2));
    };

    internal::reduce_generic(policy, accumulate_op, combine_op, reducer,
                             reducer.view(init), first1, last1, first_d);

    return std::next(first_d, std::distance(first1, last1));
  }
}

/**
 * @brief Calculate a prefix sum (inclusive scan) while transforming each element.
 *
 * @param policy             Execution policy (`ityr::execution`).
 * @param first1             Input begin iterator.
 * @param last1              Input end iterator.
 * @param first_d            Output begin iterator.
 * @param reducer            Reducer object (`ityr::reducer`).
 * @param unary_transform_op Unary operator to transform each element.
 *
 * @return The end iterator of the output range (`first_d + (last1 - first1)`).
 *
 * Equivalent to `ityr::transform_inclusive_reduce(policy, first1, last1, first_d, reducer, unary_transform_op, reducer.identity())`.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<double> v2(v1.size());
 * ityr::transform_inclusive_scan(ityr::execution::par, v1.begin(), v1.end(), v2.begin(),
 *                                ityr::reducer::multiplies<double>{}, [](int x) { return 0.1 * x; });
 * // v2 = {0.1, 0.02, 0.006, 0.0024, 0.0012}
 * ```
 *
 * @see [std::transform_inclusive_scan -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/transform_inclusive_scan)
 * @see `ityr::inclusive_scan()`
 * @see `ityr::reduce()`
 * @see `ityr::transform()`
 * @see `ityr::transform_reduce()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIteratorD,
          typename Reducer, typename UnaryTransformOp>
inline ForwardIteratorD transform_inclusive_scan(const ExecutionPolicy& policy,
                                                 ForwardIterator1       first1,
                                                 ForwardIterator1       last1,
                                                 ForwardIteratorD       first_d,
                                                 Reducer                reducer,
                                                 UnaryTransformOp       unary_transform_op) {
  return transform_inclusive_scan(policy, first1, last1, first_d, reducer,
                                  unary_transform_op, reducer.identity());
}

/**
 * @brief Calculate a prefix sum (inclusive scan).
 *
 * @param policy  Execution policy (`ityr::execution`).
 * @param first1  Input begin iterator.
 * @param last1   Input end iterator.
 * @param first_d Output begin iterator.
 * @param reducer Reducer object (`ityr::reducer`).
 * @param init    Initial value for the prefix sum.
 *
 * @return The end iterator of the output range (`first_d + (last1 - first1)`).
 *
 * This function calculates a prefix sum over the elements in the input range `[first1, last1)`.
 * The prefix sum is inclusive, which means that the i-th element of the prefix sum includes the
 * i-th element in the input range. That is, the i-th element of the prefix sum is:
 * `init + *first1 + ... + *(first1 + i)`, where `+` is the associative binary operator (provided
 * by `reducer`).
 * The calculated prefix sum is stored in the output range `[first_d, first_d + (last1 - first1))`.
 *
 * If given iterators are global pointers, they are automatically checked out in the specified
 * granularity (`ityr::execution::sequenced_policy::checkout_count` if serial,
 * or `ityr::execution::parallel_policy::checkout_count` if parallel) without explicitly passing them
 * as global iterators.
 * Input global pointers (`first1` and `last1`) are automatically checked out with the read-only mode.
 * Similarly, output global iterator (`first_d`) are checked out with the write-only mode if their
 * value type is *trivially copyable*; otherwise, they are checked out with the read-write mode.
 *
 * Overlapping regions can be specified for the input and output ranges.
 *
 * Unlike the standard `std::inclusive_scan()`, Itoyori's `ityr::inclusive_scan()`
 * requires a `reducer` as `ityr::reduce()` does.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<int> v2(v1.size());
 * ityr::inclusive_scan(ityr::execution::par, v1.begin(), v1.end(), v2.begin(),
 *                      ityr::reducer::multiplies<int>{}, 10);
 * // v2 = {10, 20, 60, 240, 1200}
 * ```
 *
 * @see [std::inclusive_scan -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/inclusive_scan)
 * @see `ityr::transform_inclusive_scan()`
 * @see `ityr::reduce()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIteratorD,
          typename Reducer>
inline ForwardIteratorD
inclusive_scan(const ExecutionPolicy&                    policy,
               ForwardIterator1                          first1,
               ForwardIterator1                          last1,
               ForwardIteratorD                          first_d,
               Reducer                                   reducer,
               const typename Reducer::accumulator_type& init) {
  return transform_inclusive_scan(policy, first1, last1, first_d, reducer,
                                  [](auto&& r) { return std::forward<decltype(r)>(r); }, init);
}

/**
 * @brief Calculate a prefix sum (inclusive scan).
 *
 * @param policy  Execution policy (`ityr::execution`).
 * @param first1  Input begin iterator.
 * @param last1   Input end iterator.
 * @param first_d Output begin iterator.
 * @param reducer Reducer object (`ityr::reducer`).
 *
 * @return The end iterator of the output range (`first_d + (last1 - first1)`).
 *
 * Equivalent to `ityr::inclusive_scan(policy, first1, last1, first_d, reducer, reducer.identity())`.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<int> v2(v1.size());
 * ityr::inclusive_scan(ityr::execution::par, v1.begin(), v1.end(), v2.begin(),
 *                      ityr::reducer::multiplies<int>{});
 * // v2 = {1, 2, 6, 24, 120}
 * ```
 *
 * @see [std::inclusive_scan -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/inclusive_scan)
 * @see `ityr::transform_inclusive_scan()`
 * @see `ityr::reduce()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIteratorD,
          typename Reducer>
inline ForwardIteratorD inclusive_scan(const ExecutionPolicy& policy,
                                       ForwardIterator1       first1,
                                       ForwardIterator1       last1,
                                       ForwardIteratorD       first_d,
                                       Reducer                reducer) {
  return inclusive_scan(policy, first1, last1, first_d, reducer, reducer.identity());
}

/**
 * @brief Calculate a prefix sum (inclusive scan).
 *
 * @param policy  Execution policy (`ityr::execution`).
 * @param first1  Input begin iterator.
 * @param last1   Input end iterator.
 * @param first_d Output begin iterator.
 *
 * @return The end iterator of the output range (`first_d + (last1 - first1)`).
 *
 * Equivalent to `ityr::inclusive_scan(policy, first1, last1, first_d, ityr::reducer::plus<T>{})`, where
 * `T` is the value type of the input iterator.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<int> v2(v1.size());
 * ityr::inclusive_scan(ityr::execution::par, v1.begin(), v1.end(), v2.begin());
 * // v2 = {1, 3, 6, 10, 15}
 * ```
 *
 * @see [std::inclusive_scan -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/inclusive_scan)
 * @see `ityr::transform_inclusive_scan()`
 * @see `ityr::reduce()`
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIteratorD>
inline ForwardIteratorD inclusive_scan(const ExecutionPolicy& policy,
                                       ForwardIterator1       first1,
                                       ForwardIterator1       last1,
                                       ForwardIteratorD       first_d) {
  using T = typename std::iterator_traits<ForwardIterator1>::value_type;
  return inclusive_scan(policy, first1, last1, first_d, reducer::plus<T>{});
}

/**
 * @brief Check if two ranges have equal values.
 *
 * @param policy Execution policy (`ityr::execution`).
 * @param first1 1st begin iterator.
 * @param last1  1st end iterator.
 * @param first2 2nd begin iterator.
 * @param pred   Binary predicate operator.
 *
 * @return Returns true if `pred` returns true for all pairs of elements in the input ranges
 *         (`[first1, last1)` and `[first2, first2 + (last1 - first1))`).
 *
 * If global pointers are provided as iterators, they are automatically checked out with the read-only
 * mode in the specified granularity (`ityr::execution::sequenced_policy::checkout_count` if serial,
 * or `ityr::execution::parallel_policy::checkout_count` if parallel) without explicitly passing them
 * as global iterators.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<double> v2 = {1.0, 2.0, 3.0, 4.0, 5.0};
 * bool r = ityr::equal(ityr::execution::par, v1.begin(), v1.end(), v2.begin(),
 *                      [](int x, double y) { return x == static_cast<int>(y); });
 * // r = true
 * ```
 *
 * @see [std::equal -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/equal)
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIterator2,
          typename BinaryPredicate>
inline bool equal(const ExecutionPolicy& policy,
                  ForwardIterator1       first1,
                  ForwardIterator1       last1,
                  ForwardIterator2       first2,
                  BinaryPredicate        pred) {
  return transform_reduce(policy, first1, last1, first2, reducer::logical_and{}, pred);
}

/**
 * @brief Check if two ranges have equal values.
 *
 * @param policy Execution policy (`ityr::execution`).
 * @param first1 1st begin iterator.
 * @param last1  1st end iterator.
 * @param first2 2nd begin iterator.
 * @param last2  2nd end iterator.
 * @param pred   Binary predicate operator.
 *
 * @return Returns true if the input ranges are of the same size (`last1 - first1 == last2 - first2`)
 *         and `pred` returns true for all pairs of elements in the input ranges.
 *
 * If global pointers are provided as iterators, they are automatically checked out with the read-only
 * mode in the specified granularity (`ityr::execution::sequenced_policy::checkout_count` if serial,
 * or `ityr::execution::parallel_policy::checkout_count` if parallel) without explicitly passing them
 * as global iterators.
 *
 * Example:
 * ```
 * ityr::global_vector<int> v1 = {1, 2, 3, 4, 5};
 * ityr::global_vector<double> v2 = {1.0, 2.0, 3.0, 4.0, 5.0};
 * bool r = ityr::equal(ityr::execution::par, v1.begin(), v1.end(), v2.begin(), v2.end(),
 *                      [](int x, double y) { return x == static_cast<int>(y); });
 * // r = true
 * ```
 *
 * @see [std::equal -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/equal)
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIterator2,
          typename BinaryPredicate>
inline bool equal(const ExecutionPolicy& policy,
                  ForwardIterator1       first1,
                  ForwardIterator1       last1,
                  ForwardIterator2       first2,
                  ForwardIterator2       last2,
                  BinaryPredicate        pred) {
  return std::distance(first1, last1) == std::distance(first2, last2) &&
         equal(policy, first1, last1, first2, pred);
}

/**
 * @brief Check if two ranges have equal values.
 *
 * @param policy Execution policy (`ityr::execution`).
 * @param first1 1st begin iterator.
 * @param last1  1st end iterator.
 * @param first2 2nd begin iterator.
 *
 * Equivalent to `ityr::equal(policy, first1, last1, first2, std::equal_to<>{})`.
 *
 * @see [std::equal -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/equal)
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIterator2>
inline bool equal(const ExecutionPolicy& policy,
                  ForwardIterator1       first1,
                  ForwardIterator1       last1,
                  ForwardIterator2       first2) {
  return equal(policy, first1, last1, first2, std::equal_to<>{});
}

/**
 * @brief Check if two ranges have equal values.
 *
 * @param policy Execution policy (`ityr::execution`).
 * @param first1 1st begin iterator.
 * @param last1  1st end iterator.
 * @param first2 2nd begin iterator.
 * @param last2  2nd end iterator.
 *
 * Equivalent to `ityr::equal(policy, first1, last1, first2, last2, std::equal_to<>{})`.
 *
 * @see [std::equal -- cppreference.com](https://en.cppreference.com/w/cpp/algorithm/equal)
 * @see `ityr::execution::sequenced_policy`, `ityr::execution::seq`,
 *      `ityr::execution::parallel_policy`, `ityr::execution::par`
 */
template <typename ExecutionPolicy, typename ForwardIterator1, typename ForwardIterator2>
inline bool equal(const ExecutionPolicy& policy,
                  ForwardIterator1       first1,
                  ForwardIterator1       last1,
                  ForwardIterator2       first2,
                  ForwardIterator2       last2) {
  return equal(policy, first1, last1, first2, last2, std::equal_to<>{});
}

ITYR_TEST_CASE("[ityr::pattern::parallel_loop] reduce and transform_reduce") {
  ito::init();
  ori::init();

  ITYR_SUBCASE("default cutoff") {
    long n = 10000;
    long r = ito::root_exec([=] {
      return reduce(
          execution::par,
          count_iterator<long>(0),
          count_iterator<long>(n));
    });
    ITYR_CHECK(r == n * (n - 1) / 2);
  }

  ITYR_SUBCASE("custom cutoff") {
    long n = 100000;
    long r = ito::root_exec([=] {
      return reduce(
          execution::parallel_policy{.cutoff_count = 100},
          count_iterator<long>(0),
          count_iterator<long>(n));
    });
    ITYR_CHECK(r == n * (n - 1) / 2);
  }

  ITYR_SUBCASE("transform unary") {
    long n = 100000;
    long r = ito::root_exec([=] {
      return transform_reduce(
          execution::parallel_policy{.cutoff_count = 100},
          count_iterator<long>(0),
          count_iterator<long>(n),
          reducer::plus<long>{},
          [](long x) { return x * x; });
    });
    ITYR_CHECK(r == n * (n - 1) * (2 * n - 1) / 6);
  }

  ITYR_SUBCASE("transform binary") {
    long n = 100000;
    long r = ito::root_exec([=] {
      return transform_reduce(
          execution::parallel_policy{.cutoff_count = 100},
          count_iterator<long>(0),
          count_iterator<long>(n),
          count_iterator<long>(0),
          reducer::plus<long>{},
          [](long x, long y) { return x * y; });
    });
    ITYR_CHECK(r == n * (n - 1) * (2 * n - 1) / 6);
  }

  ITYR_SUBCASE("zero elements") {
    long r = ito::root_exec([=] {
      return reduce(
          execution::parallel_policy{.cutoff_count = 100},
          count_iterator<long>(0),
          count_iterator<long>(0));
    });
    ITYR_CHECK(r == 0);
  }

  ori::fini();
  ito::fini();
}

ITYR_TEST_CASE("[ityr::pattern::parallel_loop] parallel reduce with global_ptr") {
  ito::init();
  ori::init();

  long n = 100000;
  ori::global_ptr<long> p = ori::malloc_coll<long>(n);

  ito::root_exec([=] {
    long count = 0;
    for_each(
        execution::sequenced_policy{.checkout_count = 100},
        make_global_iterator(p    , checkout_mode::write),
        make_global_iterator(p + n, checkout_mode::write),
        [&](long& v) { v = count++; });
  });

  ITYR_SUBCASE("default cutoff") {
    long r = ito::root_exec([=] {
      return reduce(
          execution::par,
          p, p + n);
    });
    ITYR_CHECK(r == n * (n - 1) / 2);
  }

  ITYR_SUBCASE("custom cutoff and checkout count") {
    long r = ito::root_exec([=] {
      return reduce(
          execution::parallel_policy{.cutoff_count = 100, .checkout_count = 50},
          p, p + n);
    });
    ITYR_CHECK(r == n * (n - 1) / 2);
  }

  ITYR_SUBCASE("without auto checkout") {
    long r = ito::root_exec([=] {
      return transform_reduce(
          execution::par,
          make_global_iterator(p    , checkout_mode::no_access),
          make_global_iterator(p + n, checkout_mode::no_access),
          reducer::plus<long>{},
          [](ori::global_ref<long> gref) {
            return gref.get();
          });
    });
    ITYR_CHECK(r == n * (n - 1) / 2);
  }

  ITYR_SUBCASE("serial") {
    long r = ito::root_exec([=] {
      return reduce(
          execution::sequenced_policy{.checkout_count = 100},
          p, p + n);
    });
    ITYR_CHECK(r == n * (n - 1) / 2);
  }

  ori::free_coll(p);

  ori::fini();
  ito::fini();
}

ITYR_TEST_CASE("[ityr::pattern::parallel_loop] inclusive scan") {
  ito::init();
  ori::init();

  long n = 100000;
  ori::global_ptr<long> p1 = ori::malloc_coll<long>(n);
  ori::global_ptr<long> p2 = ori::malloc_coll<long>(n);

  ito::root_exec([=] {
    fill(execution::parallel_policy{.cutoff_count = 100, .checkout_count = 100},
         p1, p1 + n, 1);

    inclusive_scan(
        execution::parallel_policy{.cutoff_count = 100, .checkout_count = 100},
        p1, p1 + n, p2);

    ITYR_CHECK(p2[0].get() == 1);
    ITYR_CHECK(p2[n - 1].get() == n);

    auto sum = reduce(
        execution::parallel_policy{.cutoff_count = 100, .checkout_count = 100},
        p2, p2 + n);

    ITYR_CHECK(sum == n * (n + 1) / 2);

    inclusive_scan(
        execution::parallel_policy{.cutoff_count = 100, .checkout_count = 100},
        p1, p1 + n, p2, reducer::multiplies<long>{}, 10);

    ITYR_CHECK(p2[0].get() == 10);
    ITYR_CHECK(p2[n - 1].get() == 10);

    transform_inclusive_scan(
        execution::parallel_policy{.cutoff_count = 100, .checkout_count = 100},
        p1, p1 + n, p2, reducer::plus<long>{}, [](long x) { return x + 1; }, 10);

    ITYR_CHECK(p2[0].get() == 12);
    ITYR_CHECK(p2[n - 1].get() == 10 + n * 2);
  });

  ori::free_coll(p1);
  ori::free_coll(p2);

  ori::fini();
  ito::fini();
}

}
