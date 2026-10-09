/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "BKE_appdir.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"
#ifdef _WIN32
#  include "BLI_winstuff.hh"
#endif

#include "vk_backend.hh"
#include "vk_device.hh"
#include "vk_pipeline_diag.hh"
#include "vk_shader.hh"
#include "vk_shader_compiler.hh"

#include <cstring>
#include <iostream>
#include <string>

#ifdef __ANDROID__
#  include <sys/system_properties.h>
#endif

#include "CLG_log.h"

namespace blender::gpu {

static CLG_LogRef LOG = {"gpu.vulkan"};

/**
 * Which Vulkan version the shaders are compiled for, and with it the SPIR-V version they are
 * emitted as. Vulkan 1.2 emits SPIR-V 1.5; a Vulkan 1.1 device (e.g. Adreno 642L) only accepts
 * SPIR-V 1.3.
 *
 * On Android all mobile GPU drivers (Adreno, Mali and PowerVR) are asked for the older version
 * whatever it reports supporting. Their shader compilers refuse or mishandle a number of modules
 * emitted as SPIR-V 1.5 -- modules `spirv-val` accepts and every desktop driver builds -- failing
 * with VK_ERROR_UNKNOWN from vkCreateComputePipelines, or emitting `OpCopyLogical` for struct
 * copies (SPIR-V >= 1.4) that several of these drivers reject at pipeline creation. Those modules
 * are the ones EEVEE uses for shadows and light culling, so there is no turning the effect off
 * instead. Blender asks for no SPIR-V 1.4 or 1.5 feature, so the older target costs nothing; the
 * workaround right below, which turns the optimizer off for the same drivers, has the same shape.
 */
#ifndef __ANDROID__
static bool compile_for_vulkan_11()
{
  const uint32_t api_version = VKBackend::get().device.physical_device_properties_get().apiVersion;
  return api_version < VK_API_VERSION_1_2 ||
         GPU_type_matches(GPU_DEVICE_QUALCOMM, GPU_OS_ANY, GPU_DRIVER_ANY);
}
#endif

static shaderc_env_version spirv_target_env_get()
{
#ifdef __ANDROID__
  char value[PROP_VALUE_MAX] = {};
  if (__system_property_get("debug.blender.spirv", value) > 0 && value[0] != '\0') {
    if (strcmp(value, "vk10") == 0) {
      return shaderc_env_version_vulkan_1_0;
    }
    if (strcmp(value, "vk12") == 0) {
      return shaderc_env_version_vulkan_1_2;
    }
    if (strcmp(value, "vk13") == 0) {
      return shaderc_env_version_vulkan_1_3;
    }
  }
  return shaderc_env_version_vulkan_1_1;
#else
  return compile_for_vulkan_11() ? shaderc_env_version_vulkan_1_1 :
                                   shaderc_env_version_vulkan_1_2;
#endif
}

static const char *spirv_cache_version_str()
{
  switch (spirv_target_env_get()) {
    case shaderc_env_version_vulkan_1_0:
      return "vk10";
    case shaderc_env_version_vulkan_1_2:
      return "vk12";
    case shaderc_env_version_vulkan_1_3:
      return "vk13";
    default:
      return "vk11";
  }
}

static std::optional<std::string> cache_dir_get()
{
  static std::optional<std::string> result = []() -> std::optional<std::string> {
    static char tmp_dir_buffer[FILE_MAX];
    /* Shader builder doesn't return the correct appdir. */
    BKE_appdir_folder_caches(tmp_dir_buffer, sizeof(tmp_dir_buffer));

    /* Version the cache by the target, not by the device: the source hash alone would otherwise
     * reuse SPIR-V compiled for a different SPIR-V version. */
    const char *ver = spirv_cache_version_str();
    std::string cache_dir = std::string(tmp_dir_buffer) + "vk-spirv-cache-" + ver + SEP_STR;
    BLI_dir_create_recursive(cache_dir.c_str());
    return cache_dir;
  }();

  return result;
}

/* -------------------------------------------------------------------- */
/** \name SPIR-V disk cache
 * \{ */

struct SPIRVSidecar {
  /** Size of the SPIRV binary. */
  uint64_t spirv_size;
};

static bool read_spirv_from_disk(VKShaderModule &shader_module)
{
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    /* RenderDoc uses spirv shaders including debug information. */
    return false;
  }
  if (!cache_dir_get().has_value()) {
    return false;
  }
  shader_module.build_sources_hash();
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";

  if (!BLI_exists(spirv_path.c_str()) || !BLI_exists(sidecar_path.c_str())) {
    return false;
  }

  BLI_file_touch(spirv_path.c_str());
  BLI_file_touch(sidecar_path.c_str());

  /* Read sidecar. */
  fstream sidecar_file(sidecar_path, std::ios::binary | std::ios::in | std::ios::ate);
  std::streamsize sidecar_size_on_disk = sidecar_file.tellg();
  SPIRVSidecar sidecar = {};
  if (sidecar_size_on_disk != sizeof(sidecar)) {
    return false;
  }
  sidecar_file.seekg(0, std::ios::beg);
  sidecar_file.read(reinterpret_cast<char *>(&sidecar), sizeof(sidecar));

  /* Read spirv binary. */
  fstream spirv_file(spirv_path, std::ios::binary | std::ios::in | std::ios::ate);
  std::streamsize size = spirv_file.tellg();
  if (size != sidecar.spirv_size) {
    return false;
  }
  spirv_file.seekg(0, std::ios::beg);
  shader_module.spirv_binary.resize(size / 4);
  spirv_file.read(reinterpret_cast<char *>(shader_module.spirv_binary.data()), size);

  CLOG_TRACE(&LOG, "reading SpirV from disk %s", spirv_path.c_str());
  return true;
}

static void write_spirv_to_disk(VKShaderModule &shader_module)
{
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    return;
  }
  if (!cache_dir_get().has_value()) {
    return;
  }

  /* Write the spirv binary */
  std::string spirv_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash + ".spv";
  CLOG_TRACE(&LOG, "write SpirV to disk %s", spirv_path.c_str());
  size_t size = (shader_module.compilation_result.end() -
                 shader_module.compilation_result.begin()) *
                sizeof(uint32_t);
  fstream spirv_file(spirv_path, std::ios::binary | std::ios::out);
  spirv_file.write(reinterpret_cast<const char *>(shader_module.compilation_result.begin()), size);

  /* Write the sidecar */
  SPIRVSidecar sidecar = {size};
  std::string sidecar_path = (*cache_dir_get()) + SEP_STR + shader_module.sources_hash +
                             ".sidecar.bin";
  fstream sidecar_file(sidecar_path, std::ios::binary | std::ios::out);
  sidecar_file.write(reinterpret_cast<const char *>(&sidecar), sizeof(SPIRVSidecar));
}

void VKShaderCompiler::cache_dir_clear_old()
{
  if (!cache_dir_get().has_value()) {
    return;
  }

  direntry *entries = nullptr;
  uint32_t dir_len = BLI_filelist_dir_contents(cache_dir_get()->c_str(), &entries);
  for (int i : IndexRange(dir_len)) {
    direntry entry = entries[i];
    if (S_ISDIR(entry.s.st_mode)) {
      continue;
    }
    const time_t ts_now = time(nullptr);
    const time_t delete_threshold = 60 /*seconds*/ * 60 /*minutes*/ * 24 /*hours*/ * 30 /*days*/;
    if (entry.s.st_mtime + delete_threshold < ts_now) {
      BLI_delete(entry.path, false, false);
    }
  }
  BLI_filelist_free(entries, dir_len);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Compilation
 * \{ */

static StringRef to_stage_name(shaderc_shader_kind stage)
{
  switch (stage) {
    case shaderc_vertex_shader:
      return "vertex";
    case shaderc_geometry_shader:
      return "geometry";
    case shaderc_fragment_shader:
      return "fragment";
    case shaderc_compute_shader:
      return "compute";

    default:
      BLI_assert_msg(false, "Do not know how to convert shaderc_shader_kind to stage name.");
      break;
  }
  return "unknown stage";
}

static std::string patch_line_directives(std::string source)
{
  /* Patch line directives so that we can make error reporting consistent. */
  size_t start_pos = 0;
  while ((start_pos = source.find("#line ", start_pos)) != std::string::npos) {
    source[start_pos] = '/';
    source[start_pos + 1] = '/';
  }
  return source;
}

static bool compile_ex(shaderc::Compiler &compiler,
                       VKShader &shader,
                       shaderc_shader_kind stage,
                       VKShaderModule &shader_module)
{
  std::string full_name = shader.name_get() + "_" + to_stage_name(stage);

  shader_module.original_sources = std::move(shader_module.combined_sources);

  Shader::dump_source_to_disk(
      shader.name_get(), full_name, ".glsl", shader_module.original_sources);

  if (!shader.skip_preprocessor) {
    shader_module.combined_sources = Shader::run_preprocessor(shader_module.original_sources,
                                                              G.debug & G_DEBUG_GPU_SHADER_NO_DCE);

    Shader::dump_source_to_disk(
        shader.name_get(), full_name + ".expanded", ".glsl", shader_module.combined_sources);
  }
  else {
    shader_module.combined_sources = shader_module.original_sources;
  }

  if (read_spirv_from_disk(shader_module)) {
    return true;
  }

  shaderc::CompileOptions options;
  bool do_optimize = true;
  const shaderc_env_version env_version = spirv_target_env_get();
  options.SetTargetEnvironment(shaderc_target_env_vulkan, env_version);
  if (G.debug & G_DEBUG_GPU_RENDERDOC) {
    do_optimize = false;
  }
  /* WORKAROUND: Qualcomm driver can crash when handling optimized SPIR-V. */
  if (GPU_type_matches(GPU_DEVICE_QUALCOMM, GPU_OS_ANY, GPU_DRIVER_ANY)) {
    do_optimize = false;
  }
  options.SetOptimizationLevel(do_optimize ? shaderc_optimization_level_performance :
                                             shaderc_optimization_level_zero);

  /* Increase the max id bound.
   *
   * SPIR-V has a default max id bound set to 0x3fffff which is the minimum amount of ids that
   * needs to be supported by any platform. However during optimization the max id bound can
   * increase very fast and lowered at the end. As glslang uses max id bound in their internal
   * structures to allocate arrays out of bound errors can occur.
   *
   * Increasing the max id bound to a larger number to increase the internal arrays of the
   * compiler to work around the compiler crash.
   *
   * NOTE: Test-files in #144614 and #143516 would surpass the default limit during compilation.
   * The final optimized SPIR-V is far less than the default so be fine to be used on platforms
   * with minimum spec.
   *
   * https://registry.khronos.org/SPIR-V/specs/1.0/SPIRV.html#_a_id_limits_a_universal_limits
   */
  options.SetMaxIdBound(0xffffff);

  /* Should always be called after setting the optimization level. Setting optimization level
   * resets all previous passes. */
  if (G.debug & G_DEBUG_GPU_SHADER_DEBUG_INFO) {
    options.SetGenerateDebugInfo();
  }

  /* Removes line directive. */
  std::string sources = patch_line_directives(shader_module.combined_sources);

  shader_module.compilation_result = compiler.CompileGlslToSpv(
      sources, stage, full_name.c_str(), options);
  bool compilation_succeeded = shader_module.compilation_result.GetCompilationStatus() ==
                               shaderc_compilation_status_success;
  if (compilation_succeeded) {
    write_spirv_to_disk(shader_module);
  }

  vk_pipeline_diag_logf(
      "MODULE %s | stage=%s | spirv-target=%s | hash=%s | status=%s | size=%zu",
      shader.name_get().c_str(),
      to_stage_name(stage).data(),
      spirv_cache_version_str(),
      shader_module.sources_hash.c_str(),
      compilation_succeeded ? "OK" : shader_module.compilation_result.GetErrorMessage().c_str(),
      compilation_succeeded ? size_t(shader_module.compilation_result.end() -
                                     shader_module.compilation_result.begin()) :
                              0);

  if (compilation_succeeded && stage == shaderc_compute_shader) {
    uint32_t local_size[3] = {0, 0, 0};
    if (vk_pipeline_diag_spirv_local_size(shader_module.compilation_result.begin(),
                                          size_t(shader_module.compilation_result.end() -
                                                 shader_module.compilation_result.begin()),
                                          local_size))
    {
      const VkPhysicalDeviceLimits &limits = VKBackend::get().device.physical_device_properties_get()
                                                 .limits;
      vk_pipeline_diag_logf(
          "MODULE-LOCALSIZE %s | local=(%u,%u,%u) | maxWGI=%u | maxWGS=(%u,%u,%u)",
          shader.name_get().c_str(),
          local_size[0],
          local_size[1],
          local_size[2],
          limits.maxComputeWorkGroupInvocations,
          limits.maxComputeWorkGroupSize[0],
          limits.maxComputeWorkGroupSize[1],
          limits.maxComputeWorkGroupSize[2]);
    }
  }
  return compilation_succeeded;
}

bool VKShaderCompiler::compile_module(VKShader &shader,
                                      shaderc_shader_kind stage,
                                      VKShaderModule &shader_module)
{
  static bool target_logged = []() {
    vk_pipeline_diag_logf("SPIRV-TARGET=%s (debug.blender.spirv override)",
                          spirv_cache_version_str());
    return true;
  }();
  (void)target_logged;
  shaderc::Compiler compiler;
  return compile_ex(compiler, shader, stage, shader_module);
}

/** \} */

}  // namespace blender::gpu
