/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * std::atomic_ref polyfill for the NDK's libc++, which does not yet implement
 * this C++20 type (feature macro __cpp_lib_atomic_ref is disabled there).
 * Backed by the compiler's __atomic builtins — a real implementation, not a
 * stub. Force-included on Android via platform_android.cmake.
 */

#pragma once

#include <atomic>

#if !defined(__cpp_lib_atomic_ref)

namespace std {

template<class T> class atomic_ref {
  T *ptr_;
  static int order_(memory_order o) noexcept
  {
    return static_cast<int>(o);
  }

 public:
  explicit atomic_ref(T &obj) noexcept : ptr_(&obj) {}
  atomic_ref(const atomic_ref &) noexcept = default;

  T load(memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_load_n(ptr_, order_(o));
  }
  void store(T v, memory_order o = memory_order_seq_cst) const noexcept
  {
    __atomic_store_n(ptr_, v, order_(o));
  }
  T exchange(T v, memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_exchange_n(ptr_, v, order_(o));
  }
  T fetch_add(T v, memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_fetch_add(ptr_, v, order_(o));
  }
  T fetch_sub(T v, memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_fetch_sub(ptr_, v, order_(o));
  }
  T fetch_or(T v, memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_fetch_or(ptr_, v, order_(o));
  }
  T fetch_and(T v, memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_fetch_and(ptr_, v, order_(o));
  }
  bool compare_exchange_strong(T &expected,
                               T desired,
                               memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_compare_exchange_n(ptr_, &expected, desired, false, order_(o), order_(o));
  }
  bool compare_exchange_weak(T &expected,
                             T desired,
                             memory_order o = memory_order_seq_cst) const noexcept
  {
    return __atomic_compare_exchange_n(ptr_, &expected, desired, true, order_(o), order_(o));
  }
  operator T() const noexcept
  {
    return load();
  }
};

}  // namespace std

#endif /* !__cpp_lib_atomic_ref */
