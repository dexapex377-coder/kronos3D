/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * Diagnostic logging for Vulkan pipeline/module compilation on Android.
 *
 * Every module/pipeline compile result is appended to a file in the caches dir
 * (`vk-pipeline-diag.txt`) and echoed to logcat under the tag `blender-pipe-diag`.
 * This lets a build run unattended and dump every shader/pipeline the driver
 * accepts or rejects (the Android driver refuses several with VK_ERROR_UNKNOWN
 * and no diagnostics of its own).
 */

#pragma once

#include "BKE_appdir.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"

#include "vk_common.hh"

#include <cstdarg>
#include <cstdio>
#include <string>

#ifdef __ANDROID__
#  include <android/log.h>
#endif

namespace blender::gpu {

inline void vk_pipeline_diag_log_line(const std::string &line)
{
#ifdef __ANDROID__
  __android_log_print(ANDROID_LOG_INFO, "blender-pipe-diag", "%s", line.c_str());
#else
  fprintf(stderr, "%s\n", line.c_str());
#endif

  static char diag_dir[FILE_MAX];
  if (diag_dir[0] == '\0') {
    BKE_appdir_folder_caches(diag_dir, sizeof(diag_dir));
    BLI_dir_create_recursive(diag_dir);
  }
  const std::string diag_file = std::string(diag_dir) + SEP_STR + "vk-pipeline-diag.txt";
  FILE *fp = BLI_fopen(diag_file.c_str(), "a");
  if (fp) {
    fprintf(fp, "%s\n", line.c_str());
    fclose(fp);
  }
}

inline void vk_pipeline_diag_logf(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  vk_pipeline_diag_log_line(buf);
}

/**
 * Parse an optimized SPIR-V binary for the OpExecutionMode LocalSize (17) of the main entry
 * point. Used to log the workgroup size that the device will actually need to launch, since a
 * compute pipeline can fail with VK_ERROR_UNKNOWN when the requested local size exceeds the
 * device limits (a classic PowerVR failure mode).
 */
inline bool vk_pipeline_diag_spirv_local_size(const uint32_t *words,
                                              size_t word_count,
                                              uint32_t r_local_size[3])
{
  /* SPIR-V header is 5 words: magic, version, generator, bound, schema. */
  if (word_count < 6) {
    return false;
  }
  size_t offset = 5;
  while (offset < word_count) {
    const uint32_t instruction = words[offset];
    const uint32_t opcode = instruction & 0xFFFFu;
    const uint32_t word_count_inst = instruction >> 16;
    const size_t next = offset + word_count_inst;
    if (next > word_count || word_count_inst == 0) {
      return false;
    }
    /* OpExecutionMode = 0x10. Layout: [opcode, entrypoint id, mode, ...args]. */
    if (opcode == 0x10 && word_count_inst >= 4 && words[offset + 2] == 17) {
      /* LocalSize mode: [x, y, z] follow the mode literal. */
      r_local_size[0] = words[offset + 3];
      r_local_size[1] = words[offset + 4];
      r_local_size[2] = (word_count_inst >= 6) ? words[offset + 5] : 1;
      return true;
    }
    offset = next;
  }
  return false;
}

}  // namespace blender::gpu