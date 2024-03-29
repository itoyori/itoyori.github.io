#pragma once

#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

#include "ityr/common/util.hpp"
#include "ityr/common/mpi_util.hpp"
#include "ityr/common/topology.hpp"

namespace ityr::common {

class mmap_noreplace_exception : public std::exception {};

inline void munmap(void* addr, std::size_t size);

inline void* mmap_no_physical_mem(void*       addr,
                                  std::size_t size,
                                  bool        replace   = false,
                                  std::size_t alignment = alignof(max_align_t));

class virtual_mem {
public:
  virtual_mem() {}
  virtual_mem(std::size_t size, std::size_t alignment = alignof(max_align_t))
    : addr_(mmap_no_physical_mem(nullptr, size, false, alignment)), size_(size) {}
  virtual_mem(void* addr, std::size_t size, std::size_t alignment = alignof(max_align_t))
    : addr_(mmap_no_physical_mem(addr, size, false, alignment)), size_(size) {}

  ~virtual_mem() { destroy(); }

  virtual_mem(const virtual_mem&) = delete;
  virtual_mem& operator=(const virtual_mem&) = delete;

  virtual_mem(virtual_mem&& vm) : addr_(vm.addr_), size_(vm.size_) { vm.addr_ = nullptr; }
  virtual_mem& operator=(virtual_mem&& vm) {
    destroy();
    addr_ = vm.addr();
    size_ = vm.size();
    vm.addr_ = nullptr;
    return *this;
  }

  void* addr() const { return addr_; }
  std::size_t size() const { return size_; }

  void shrink(std::size_t to_size) {
    ITYR_CHECK(addr_);
    ITYR_CHECK(to_size <= size_);

    std::size_t pagesize = get_page_size();
    std::size_t curr_page_end = round_up_pow2(size_, pagesize);
    std::size_t next_page_end = round_up_pow2(to_size, pagesize);
    if (curr_page_end > next_page_end) {
      munmap(reinterpret_cast<std::byte*>(addr_) + next_page_end,
             curr_page_end - next_page_end);
    }

    size_ = to_size;
  }

private:
  void destroy() {
    if (addr_) {
      munmap(addr_, size_);
    }
  }

  void* addr_ = nullptr;
  std::size_t size_;
};

ITYR_TEST_CASE("[ityr::common::virtual_mem] allocate virtual memory") {
  std::size_t pagesize = get_page_size();
  void* addr = nullptr;
  {
    virtual_mem vm(32 * pagesize);
    ITYR_CHECK(vm.addr() != nullptr);
    addr = vm.addr();
  }
  {
    virtual_mem vm_longlived;
    {
      // check if the same virtual address can be mapped after the previous mapping has been freed
      virtual_mem vm(addr, 16 * pagesize);
      ITYR_CHECK(vm.addr() == addr);
      // mappings for the same virtual address cannot be replaced
      ITYR_CHECK_THROWS_AS(virtual_mem vm2(addr, pagesize), mmap_noreplace_exception);
      vm_longlived = std::move(vm);
    }
    // The VM mapping remains vaild even after it is moved to objects with longer lifetimes
    ITYR_CHECK_THROWS_AS(virtual_mem vm3(addr, pagesize), mmap_noreplace_exception);
  }
  // The VM mapping is correctly freed after exiting the block
  virtual_mem vm4(addr, pagesize);
}

inline void munmap(void* addr, std::size_t size) {
  ITYR_CHECK(size > 0);
  ITYR_CHECK_MESSAGE(reinterpret_cast<uintptr_t>(addr) % get_page_size() == 0,
                     "The address passed to munmap() must be page-aligned");
  if (::munmap(addr, size) == -1) {
    perror("munmap");
    die("[ityr::common::virtual_mem] munmap(%p, %lu) failed", addr, size);
  }
}

inline void* mmap_no_physical_mem(void*       addr,
                                  std::size_t size,
                                  bool        replace,
                                  std::size_t alignment) {
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;

  std::size_t alloc_size;
  if (addr == nullptr) {
    alloc_size = size + alignment;
  } else {
    ITYR_CHECK(reinterpret_cast<uintptr_t>(addr) % alignment == 0);
    alloc_size = size;
    if (!replace) {
      flags |= MAP_FIXED_NOREPLACE;
    } else {
      flags |= MAP_FIXED;
    }
  }

  void* allocated_p = mmap(addr, alloc_size, PROT_NONE, flags, -1, 0);
  if (allocated_p == MAP_FAILED) {
    if (errno == EEXIST) {
      // MAP_FIXED_NOREPLACE error
      throw mmap_noreplace_exception{};
    } else {
      perror("mmap");
      die("[ityr::common::virtual_mem] mmap(%p, %lu, ...) failed", addr, alloc_size);
    }
  }

  if (addr == nullptr) {
    std::size_t pagesize = get_page_size();

    uintptr_t allocated_addr = reinterpret_cast<uintptr_t>(allocated_p);
    ITYR_CHECK(allocated_addr % pagesize == 0);

    uintptr_t ret_addr = round_up_pow2(allocated_addr, alignment);
    ITYR_CHECK(ret_addr % pagesize == 0);

    // truncate the head end
    ITYR_CHECK(ret_addr >= allocated_addr);
    if (ret_addr - allocated_addr > 0) {
      munmap(allocated_p, ret_addr - allocated_addr);
    }

    // truncate the tail end
    uintptr_t allocated_addr_end = allocated_addr + alloc_size;
    uintptr_t ret_page_end = round_up_pow2(ret_addr + size, pagesize);
    ITYR_CHECK(allocated_addr_end >= ret_page_end);
    if (allocated_addr_end - ret_page_end > 0) {
      munmap(reinterpret_cast<std::byte*>(ret_page_end), allocated_addr_end - ret_page_end);
    }

    return reinterpret_cast<std::byte*>(ret_addr);
  } else {
    ITYR_CHECK(addr == allocated_p);
    return allocated_p;
  }
}

inline virtual_mem reserve_same_vm_coll(std::size_t size,
                                        std::size_t alignment = alignof(max_align_t)) {
  ITYR_CHECK(size > 0);

  uintptr_t vm_addr = 0;
  virtual_mem vm;

  std::vector<virtual_mem> prev_vms;
  int max_trial = 100;
  std::size_t alloc_size = round_up_pow2(size, get_page_size());
  topology::rank_t leader_rank = 0;

  std::size_t alloc_size_max = std::max(alloc_size, std::size_t(1) << 40);

  // Repeat until the same virtual memory address is allocated
  // TODO: smarter allocation using `pmap` result?
  for (int n_trial = 0; n_trial <= max_trial; n_trial++) {
    if (topology::my_rank() == leader_rank) {
      vm = virtual_mem(alloc_size, alignment);
      vm_addr = reinterpret_cast<uintptr_t>(vm.addr());
    }

    vm_addr = mpi_bcast_value(vm_addr, leader_rank, topology::mpicomm());

    topology::rank_t failed_rank = -1;
    if (topology::my_rank() != leader_rank) {
      // unmap overlapping virtual addresses that were previously allocated
      for (auto&& prev_vm : prev_vms) {
        if (reinterpret_cast<uint64_t>(prev_vm.addr()) < vm_addr + alloc_size &&
            vm_addr < reinterpret_cast<uint64_t>(prev_vm.addr()) + prev_vm.size()) {
          // call destructor
          prev_vm = virtual_mem();
        }
      }

      try {
        vm = virtual_mem(reinterpret_cast<void*>(vm_addr), alloc_size, alignment);
      } catch (mmap_noreplace_exception& e) {
        failed_rank = topology::my_rank();
      }
    }

    // Among the failed processes, the process with the maximum rank will become the next leader
    auto failed_rank_max = mpi_allreduce_value(failed_rank, topology::mpicomm(), MPI_MAX);

    if (failed_rank_max == -1) {
      // success; prev_vms are automatically freed
      vm.shrink(size);
      return vm;
    }

    if (failed_rank == -1) {
      // Defer the deallocation of previous virtual addresses to prevent
      // the same address from being allocated in the next turn
      prev_vms.push_back(std::move(vm));
    }

    leader_rank = failed_rank_max;
    alloc_size = std::min(alloc_size_max, 2 * alloc_size);;
  }

  die("Reservation of virtual memory address failed (size=%ld, max_trial=%d)", size, max_trial);
}

ITYR_TEST_CASE("[ityr::common::virtual_mem] allocate the same virtual memory across processes") {
  runtime_options opts;
  singleton_initializer<topology::instance> topo;

  std::size_t pagesize = get_page_size();
  virtual_mem vm = reserve_same_vm_coll(pagesize * 32);
  ITYR_CHECK(vm.addr() != nullptr);

  uintptr_t vm_addr = reinterpret_cast<uintptr_t>(vm.addr());
  std::size_t vm_size = vm.size();

  uintptr_t vm_addr_root = mpi_bcast_value(vm_addr, 0, topology::mpicomm());
  std::size_t vm_size_root = mpi_bcast_value(vm_size, 0, topology::mpicomm());

  ITYR_CHECK(vm_addr == vm_addr_root);
  ITYR_CHECK(vm_size == vm_size_root);
  ITYR_CHECK(vm_size == pagesize * 32);
}

}
