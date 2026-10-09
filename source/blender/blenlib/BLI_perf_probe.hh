/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup blenlib
 *
 * Windowed runtime timing probes for on-device diagnostics. The whole family is off unless
 * enabled, and costs a couple of cycles when off.
 *
 * Enable with the `BLENDER_PERF` environment variable, or on Android the `debug.blender.perf`
 * system property (`setprop debug.blender.perf 1`, changeable at runtime). Reports print to
 * stdout, which this port's NativeActivity redirects to logcat under the tag "blender":
 *
 *   adb logcat -s blender | grep '\[perf\]'
 *
 * Usage:
 *
 *   void foo() {
 *     PERF_ZONE(foo);
 *     ...
 *   }
 *
 * prints `[perf] foo: 0.44 ms avg (last 30)` every 30 invocations.
 */

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

#ifdef __ANDROID__
#  include <sys/system_properties.h>
#endif

namespace blender::perf {

inline bool enabled()
{
  if (const char *env = getenv("BLENDER_PERF")) {
    if (atoi(env) != 0) {
      return true;
    }
  }
#ifdef __ANDROID__
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.blender.perf", value) > 0 && value[0] != '\0') {
    return atoi(value) != 0;
  }
#endif
  return false;
}

using TimePoint = std::chrono::steady_clock::time_point;

/** Rolling-average accumulator; one static instance per `PERF_ZONE` call site. */
class ZAccum {
 public:
  ZAccum(const char *name) : name_(name) {}

  void begin(TimePoint *t, bool *on) const
  {
    *on = enabled();
    if (*on) {
      *t = std::chrono::steady_clock::now();
    }
  }

  void end(const TimePoint t, const bool on)
  {
    if (!on) {
      return;
    }
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t)
                          .count();
    sum_ += ms;
    count_++;
    if (count_ >= window_) {
      printf("[perf] %s: %.2f ms avg (last %d)\n", name_, sum_ / double(count_), count_);
      sum_ = 0.0;
      count_ = 0;
    }
  }

 private:
  const char *name_;
  static constexpr int window_ = 30;
  int count_ = 0;
  double sum_ = 0.0;
};

/** RAII guard pairing a call site with its accumulator. */
class ZScope {
 public:
  ZScope(ZAccum &accum) : accum_(accum)
  {
    accum_.begin(&t_, &on_);
  }

  ~ZScope()
  {
    accum_.end(t_, on_);
  }

 private:
  ZAccum &accum_;
  TimePoint t_;
  bool on_ = false;
};

}  // namespace blender::perf

/** Profile the enclosing scope; prints a rolling windowed average to stdout when enabled. */
#define PERF_ZONE(name_) \
  static ::blender::perf::ZAccum _perf_accum_##name_(#name_); \
  ::blender::perf::ZScope _perf_scope_##name_(_perf_accum_##name_)