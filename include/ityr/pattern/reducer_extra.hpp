#pragma once

#include "ityr/common/util.hpp"
#include "ityr/pattern/parallel_loop.hpp"
#include "ityr/container/global_vector.hpp"
#include "ityr/container/global_span.hpp"

namespace ityr::reducer {

template <typename T, typename Counter = std::size_t>
struct histogram {
  static_assert(std::is_arithmetic_v<T>);

  using value_type            = T;
  using accumulator_type      = global_vector<Counter>;
  using accumulator_view_type = global_span<Counter>;

  static constexpr bool direct_accumulation = false;

  histogram(std::size_t n_bins) : n_bins_(n_bins) {}
  histogram(std::size_t n_bins, const value_type& lowest, const value_type& highest)
    : n_bins_(n_bins), lowest_(lowest), highest_(highest) {}

  void foldl(accumulator_view_type& acc, const value_type& x) const {
    if (lowest_ <= x && x <= highest_) {
      auto delta = (highest_ - lowest_) / n_bins_;
      std::size_t key = (x - lowest_) / delta;
      ITYR_CHECK(0 <= key);
      ITYR_CHECK(key < n_bins_);
      acc[key]++;
    }
  }

  void foldl(accumulator_view_type& acc_l, const accumulator_view_type& acc_r) const {
    transform(
        execution::parallel_policy{.cutoff_count = 128, .checkout_count = 128},
        acc_l.begin(), acc_l.end(), acc_r.begin(), acc_l.begin(),
        [](const Counter& c1, const Counter& c2) { return c1 + c2; });
  }

  void foldr(const accumulator_view_type& acc_l, accumulator_view_type& acc_r) const {
    // commutative
    foldl(acc_r, acc_l);
  }

  accumulator_type identity() const {
    return global_vector<Counter>(n_bins_, 0);
  }

  accumulator_view_type view(const accumulator_type& x) const {
    return global_span<Counter>(x.begin(), x.end());
  }

  accumulator_type clone(const accumulator_view_type& v) const {
    return global_vector<Counter>(v.begin(), v.end());
  }

private:
  std::size_t n_bins_;
  value_type  lowest_  = std::numeric_limits<value_type>::lowest();
  value_type  highest_ = std::numeric_limits<value_type>::max();
};

ITYR_TEST_CASE("[ityr::reducer] extra reducer test") {
  ito::init();
  ori::init();

  ITYR_SUBCASE("histogram") {
    root_exec([=] {
      int n_samples = 100000;
      int n_bins = 1000;
      global_vector<double> v({.collective = true}, n_samples);

      transform(
          execution::parallel_policy{.cutoff_count = 128, .checkout_count = 128},
          count_iterator<int>(0), count_iterator<int>(n_samples), v.begin(),
          [=](int i) {
            double x = (static_cast<double>(i) + 0.5) / n_bins;
            return x - static_cast<int>(x); // within [0.0, 1.0)
          });

      auto bins = reduce(
          execution::parallel_policy{.cutoff_count = 128, .checkout_count = 128},
          v.begin(), v.end(), histogram<double>(n_bins, 0.0, 1.0));
      ITYR_CHECK(bins.size() == n_bins);

      auto count_sum = reduce(execution::par, bins.begin(), bins.end());
      ITYR_CHECK(count_sum == n_samples);

      for_each(
          execution::par,
          make_global_iterator(bins.begin(), checkout_mode::read),
          make_global_iterator(bins.end()  , checkout_mode::read),
          [=](auto count) {
            ITYR_CHECK(count == n_samples / n_bins);
          });
    });
  }

  ori::fini();
  ito::fini();
}

}
