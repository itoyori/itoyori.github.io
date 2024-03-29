#pragma once

#include "ityr/common/util.hpp"
#include "ityr/ori/util.hpp"

namespace ityr::ori::mem_mapper {

struct segment {
  int         owner;
  std::size_t offset_b;
  std::size_t offset_e;
  std::size_t pm_offset;

  bool operator==(const segment& b) const {
    return owner == b.owner && offset_b == b.offset_b && offset_e == b.offset_e && pm_offset == b.pm_offset;
  }
  bool operator!=(const segment& b) const {
    return !(*this == b);
  }
};

struct numa_segment {
  int         owner; // -1: interleave
  std::size_t pm_offset_b;
  std::size_t pm_offset_e;

  bool operator==(const numa_segment& b) const {
    return owner == b.owner && pm_offset_b == b.pm_offset_b && pm_offset_e == b.pm_offset_e;
  }
  bool operator!=(const numa_segment& b) const {
    return !(*this == b);
  }
};

class base {
public:
  base(std::size_t size, int n_inter_ranks, int n_intra_ranks)
    : size_(size), n_inter_ranks_(n_inter_ranks), n_intra_ranks_(n_intra_ranks) {}

  virtual ~base() = default;

  virtual std::size_t block_size() const = 0;

  virtual std::size_t local_size(int inter_rank) const = 0;

  virtual std::size_t effective_size() const = 0;

  // Returns the segment info that specifies the owner and the range [offset_b, offset_e)
  // of the block that contains the given offset.
  // pm_offset is the offset from the beginning of the owner's local physical memory for the block.
  virtual segment get_segment(std::size_t offset) const = 0;

  virtual numa_segment get_numa_segment(int inter_rank, std::size_t pm_offset) const = 0;

  virtual bool should_map_all_home() const = 0;

protected:
  std::size_t size_;
  int         n_inter_ranks_;
  int         n_intra_ranks_;
};

template <block_size_t BlockSize>
class block : public base {
public:
  block(std::size_t size, int n_inter_ranks, int n_intra_ranks)
    : base(size, n_inter_ranks, n_intra_ranks),
      n_blk_((size + BlockSize - 1) / BlockSize) {}

  std::size_t block_size() const override { return BlockSize; }

  std::size_t local_size(int inter_rank) const override {
    int seg_id = inter_rank;
    auto [blk_id_b, blk_id_e] = get_seg_range(seg_id);
    return std::max(std::size_t(1), blk_id_e - blk_id_b) * BlockSize;
  }

  std::size_t effective_size() const override {
    return n_blk_ * BlockSize;
  }

  segment get_segment(std::size_t offset) const override {
    ITYR_CHECK(offset < effective_size());

    std::size_t blk_id = offset / BlockSize;
    int         seg_id = blk_id * n_inter_ranks_ / n_blk_;

    auto [blk_id_b, blk_id_e] = get_seg_range(seg_id);
    ITYR_CHECK(blk_id_b <= blk_id);
    ITYR_CHECK(blk_id < blk_id_e);

    return segment{seg_id,
                   blk_id_b * BlockSize,
                   blk_id_e * BlockSize,
                   0};
  }

  numa_segment get_numa_segment(int inter_rank, std::size_t pm_offset) const override {
    ITYR_CHECK(pm_offset < local_size(inter_rank));

    auto n_numa_blk = (local_size(inter_rank) + BlockSize - 1) / BlockSize;

    std::size_t blk_id = pm_offset / BlockSize;
    int         seg_id = blk_id * n_intra_ranks_ / n_numa_blk;

    std::size_t blk_id_b = (seg_id * n_numa_blk + n_intra_ranks_ - 1) / n_intra_ranks_;
    std::size_t blk_id_e = ((seg_id + 1) * n_numa_blk + n_intra_ranks_ - 1) / n_intra_ranks_;

    ITYR_CHECK(blk_id_b <= blk_id);
    ITYR_CHECK(blk_id < blk_id_e);

    return numa_segment{seg_id,
                        blk_id_b * BlockSize,
                        blk_id_e * BlockSize};
  }

  bool should_map_all_home() const override {
    return true;
  }

private:
  std::tuple<std::size_t, std::size_t> get_seg_range(int seg_id) const {
    std::size_t blk_id_b = (seg_id * n_blk_ + n_inter_ranks_ - 1) / n_inter_ranks_;
    std::size_t blk_id_e = ((seg_id + 1) * n_blk_ + n_inter_ranks_ - 1) / n_inter_ranks_;
    return {blk_id_b, blk_id_e};
  }

  std::size_t n_blk_;
};

ITYR_TEST_CASE("[ityr::ori::mem_mapper::block] calculate local block size") {
  constexpr block_size_t bs = 65536;
  auto local_block_size = [](std::size_t size, int n_inter_ranks, int inter_rank) -> std::size_t {
    return block<bs>(size, n_inter_ranks, 1).local_size(inter_rank);
  };
  ITYR_CHECK(local_block_size(bs * 4     , 4, 0) == bs    );
  ITYR_CHECK(local_block_size(bs * 12    , 4, 0) == bs * 3);
  ITYR_CHECK(local_block_size(bs * 14    , 4, 0) == bs * 4);
  ITYR_CHECK(local_block_size(bs * 14    , 4, 1) == bs * 3);
  ITYR_CHECK(local_block_size(bs * 14    , 4, 2) == bs * 4);
  ITYR_CHECK(local_block_size(bs * 14    , 4, 3) == bs * 3);
  ITYR_CHECK(local_block_size(1          , 4, 0) == bs    );
  ITYR_CHECK(local_block_size(1          , 4, 1) == bs    ); // cannot be zero
  ITYR_CHECK(local_block_size(1          , 1, 0) == bs    );
  ITYR_CHECK(local_block_size(bs * 3     , 1, 0) == bs * 3);
}

ITYR_TEST_CASE("[ityr::ori::mem_mapper::block] get block information at specified offset") {
  constexpr block_size_t bs = 65536;
  auto get_segment = [](std::size_t size, int n_inter_ranks, std::size_t offset) -> segment {
    return block<bs>(size, n_inter_ranks, 1).get_segment(offset);
  };
  ITYR_CHECK(get_segment(bs * 4     , 4, 0          ) == (segment{0, 0      , bs     , 0}));
  ITYR_CHECK(get_segment(bs * 4     , 4, bs         ) == (segment{1, bs     , bs * 2 , 0}));
  ITYR_CHECK(get_segment(bs * 4     , 4, bs * 2     ) == (segment{2, bs * 2 , bs * 3 , 0}));
  ITYR_CHECK(get_segment(bs * 4     , 4, bs * 3     ) == (segment{3, bs * 3 , bs * 4 , 0}));
  ITYR_CHECK(get_segment(bs * 4     , 4, bs * 4 - 1 ) == (segment{3, bs * 3 , bs * 4 , 0}));
  ITYR_CHECK(get_segment(bs * 14    , 4, 0          ) == (segment{0, 0      , bs * 4 , 0}));
  ITYR_CHECK(get_segment(bs * 14    , 4, bs         ) == (segment{0, 0      , bs * 4 , 0}));
  ITYR_CHECK(get_segment(bs * 14    , 4, bs * 5     ) == (segment{1, bs * 4 , bs * 7 , 0}));
  ITYR_CHECK(get_segment(bs * 14 - 1, 4, bs * 14 - 1) == (segment{3, bs * 11, bs * 14, 0}));
}

template <block_size_t BlockSize>
class cyclic : public base {
public:
  cyclic(std::size_t size, int n_inter_ranks, int n_intra_ranks, std::size_t seg_size = BlockSize)
    : base(size, n_inter_ranks, n_intra_ranks),
      seg_size_(seg_size) {
    ITYR_CHECK(seg_size >= BlockSize);
    ITYR_CHECK(seg_size % BlockSize == 0);
  }

  std::size_t block_size() const override { return BlockSize; }

  std::size_t local_size(int) const override {
    return local_size_impl();
  }

  std::size_t effective_size() const override {
    return local_size_impl() * n_inter_ranks_;
  }

  segment get_segment(std::size_t offset) const override {
    ITYR_CHECK(offset < effective_size());
    std::size_t blk_id_g = offset / seg_size_;
    std::size_t blk_id_l = blk_id_g / n_inter_ranks_;
    return segment{static_cast<int>(blk_id_g % n_inter_ranks_),
                   blk_id_g * seg_size_,
                   (blk_id_g + 1) * seg_size_,
                   blk_id_l * seg_size_};
  }

  numa_segment get_numa_segment(int inter_rank, std::size_t) const override {
    // interleave all
    return numa_segment{-1, 0, local_size(inter_rank)};
  }

  bool should_map_all_home() const override {
    return false;
  }

private:
  // non-virtual common part
  std::size_t local_size_impl() const {
    std::size_t n_blk_g = (size_ + seg_size_ - 1) / seg_size_;
    std::size_t n_blk_l = (n_blk_g + n_inter_ranks_ - 1) / n_inter_ranks_;
    return n_blk_l * seg_size_;
  }

  std::size_t seg_size_;
};

ITYR_TEST_CASE("[ityr::ori::mem_mapper::cyclic] calculate local block size") {
  constexpr block_size_t bs = 65536;
  std::size_t ss = bs * 2;
  auto local_block_size = [=](std::size_t size, int n_inter_ranks, int inter_rank) -> std::size_t {
    return cyclic<bs>(size, n_inter_ranks, 1, ss).local_size(inter_rank);
  };
  ITYR_CHECK(local_block_size(ss * 4     , 4, 0) == ss    );
  ITYR_CHECK(local_block_size(ss * 12    , 4, 0) == ss * 3);
  ITYR_CHECK(local_block_size(ss * 13    , 4, 0) == ss * 4);
  ITYR_CHECK(local_block_size(ss * 12 + 1, 4, 0) == ss * 4);
  ITYR_CHECK(local_block_size(ss * 12 - 1, 4, 0) == ss * 3);
  ITYR_CHECK(local_block_size(1          , 4, 0) == ss    );
  ITYR_CHECK(local_block_size(1          , 1, 0) == ss    );
  ITYR_CHECK(local_block_size(ss * 3     , 1, 0) == ss * 3);
}

ITYR_TEST_CASE("[ityr::ori::mem_mapper::cyclic] get block information at specified offset") {
  constexpr block_size_t bs = 65536;
  std::size_t ss = bs * 2;
  auto get_segment = [=](std::size_t size, int n_inter_ranks, std::size_t offset) -> segment {
    return cyclic<bs>(size, n_inter_ranks, 1, ss).get_segment(offset);
  };
  ITYR_CHECK(get_segment(ss * 4     , 4, 0         ) == (segment{0, 0      , ss     , 0     }));
  ITYR_CHECK(get_segment(ss * 4     , 4, ss        ) == (segment{1, ss     , ss * 2 , 0     }));
  ITYR_CHECK(get_segment(ss * 4     , 4, ss * 2    ) == (segment{2, ss * 2 , ss * 3 , 0     }));
  ITYR_CHECK(get_segment(ss * 4     , 4, ss * 3    ) == (segment{3, ss * 3 , ss * 4 , 0     }));
  ITYR_CHECK(get_segment(ss * 4     , 4, ss * 4 - 1) == (segment{3, ss * 3 , ss * 4 , 0     }));
  ITYR_CHECK(get_segment(ss * 12    , 4, 0         ) == (segment{0, 0      , ss     , 0     }));
  ITYR_CHECK(get_segment(ss * 12    , 4, ss        ) == (segment{1, ss     , ss * 2 , 0     }));
  ITYR_CHECK(get_segment(ss * 12    , 4, ss * 3    ) == (segment{3, ss * 3 , ss * 4 , 0     }));
  ITYR_CHECK(get_segment(ss * 12    , 4, ss * 5 + 2) == (segment{1, ss * 5 , ss * 6 , ss    }));
  ITYR_CHECK(get_segment(ss * 12 - 1, 4, ss * 11   ) == (segment{3, ss * 11, ss * 12, ss * 2}));
}

template <block_size_t BlockSize>
class block_adws : public base {
public:
  block_adws(std::size_t size, int n_inter_ranks, int n_intra_ranks)
    : base(size, n_inter_ranks, n_intra_ranks),
      n_blk_((size + BlockSize - 1) / BlockSize) {}

  std::size_t block_size() const override { return BlockSize; }

  std::size_t local_size(int inter_rank) const override {
    int seg_id = n_inter_ranks_ - inter_rank - 1;
    auto [blk_id_b, blk_id_e] = get_seg_range(seg_id);
    return std::max(std::size_t(1), blk_id_e - blk_id_b) * BlockSize;
  }

  std::size_t effective_size() const override {
    return n_blk_ * BlockSize;
  }

  segment get_segment(std::size_t offset) const override {
    ITYR_CHECK(offset < effective_size());

    std::size_t blk_id = offset / BlockSize;
    int         seg_id = ((blk_id + 1) * n_inter_ranks_ + n_blk_ - 1) / n_blk_ - 1;

    auto [blk_id_b, blk_id_e] = get_seg_range(seg_id);
    ITYR_CHECK(blk_id_b <= blk_id);
    ITYR_CHECK(blk_id < blk_id_e);

    return segment{n_inter_ranks_ - seg_id - 1,
                   blk_id_b * BlockSize,
                   blk_id_e * BlockSize,
                   0};
  }

  numa_segment get_numa_segment(int inter_rank, std::size_t pm_offset) const override {
    ITYR_CHECK(pm_offset < local_size(inter_rank));

    auto n_numa_blk = (local_size(inter_rank) + BlockSize - 1) / BlockSize;

    std::size_t blk_id = pm_offset / BlockSize;
    int         seg_id = ((blk_id + 1) * n_intra_ranks_ + n_numa_blk - 1) / n_numa_blk - 1;

    std::size_t blk_id_b = (seg_id * n_numa_blk) / n_intra_ranks_;
    std::size_t blk_id_e = ((seg_id + 1) * n_numa_blk) / n_intra_ranks_;

    ITYR_CHECK(blk_id_b <= blk_id);
    ITYR_CHECK(blk_id < blk_id_e);

    return numa_segment{n_intra_ranks_ - seg_id - 1,
                        blk_id_b * BlockSize,
                        blk_id_e * BlockSize};
  }

  bool should_map_all_home() const override {
    return true;
  }

private:
  std::tuple<std::size_t, std::size_t> get_seg_range(int seg_id) const {
    std::size_t blk_id_b = (seg_id * n_blk_) / n_inter_ranks_;
    std::size_t blk_id_e = ((seg_id + 1) * n_blk_) / n_inter_ranks_;
    return {blk_id_b, blk_id_e};
  }

  std::size_t n_blk_;
};

}
