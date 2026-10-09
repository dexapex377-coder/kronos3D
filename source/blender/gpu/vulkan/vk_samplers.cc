/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 */

#include "vk_samplers.hh"

namespace blender::gpu {

void VKSamplers::init()
{
  if (custom_sampler_cache_[0][0].is_initialized()) {
    return;
  }
  for (int no_linear = 0; no_linear < 2; no_linear++) {
    custom_sampler_cache_[GPU_SAMPLER_CUSTOM_COMPARE][no_linear].create(
        GPUSamplerState::compare_sampler(), bool(no_linear));
    custom_sampler_cache_[GPU_SAMPLER_CUSTOM_ICON][no_linear].create(
        GPUSamplerState::icon_sampler(), bool(no_linear));
  }

  GPUSamplerState state = {};
  for (int extend_yz_i = 0; extend_yz_i < GPU_SAMPLER_EXTEND_MODES_COUNT; extend_yz_i++) {
    state.extend_yz = static_cast<GPUSamplerExtendMode>(extend_yz_i);
    for (int extend_x_i = 0; extend_x_i < GPU_SAMPLER_EXTEND_MODES_COUNT; extend_x_i++) {
      state.extend_x = static_cast<GPUSamplerExtendMode>(extend_x_i);
      for (int filtering_i = 0; filtering_i < GPU_SAMPLER_FILTERING_TYPES_COUNT; filtering_i++) {
        state.filtering = GPUSamplerFiltering(filtering_i);
        for (int no_linear = 0; no_linear < 2; no_linear++) {
          sampler_cache_[extend_yz_i][extend_x_i][filtering_i][no_linear].create(state,
                                                                                bool(no_linear));
        }
      }
    }
  }
}

void VKSamplers::free()
{
  for (int no_linear = 0; no_linear < 2; no_linear++) {
    custom_sampler_cache_[GPU_SAMPLER_CUSTOM_COMPARE][no_linear].free();
    custom_sampler_cache_[GPU_SAMPLER_CUSTOM_ICON][no_linear].free();
  }

  for (int extend_yz_i = 0; extend_yz_i < GPU_SAMPLER_EXTEND_MODES_COUNT; extend_yz_i++) {
    for (int extend_x_i = 0; extend_x_i < GPU_SAMPLER_EXTEND_MODES_COUNT; extend_x_i++) {
      for (int filtering_i = 0; filtering_i < GPU_SAMPLER_FILTERING_TYPES_COUNT; filtering_i++) {
        for (int no_linear = 0; no_linear < 2; no_linear++) {
          sampler_cache_[extend_yz_i][extend_x_i][filtering_i][no_linear].free();
        }
      }
    }
  }
}

const VKSampler &VKSamplers::get(const GPUSamplerState &sampler_state,
                                 bool no_linear_filter) const
{
  BLI_assert(sampler_state.type != GPU_SAMPLER_STATE_TYPE_INTERNAL);

  const int no_linear = int(no_linear_filter);
  if (sampler_state.type == GPU_SAMPLER_STATE_TYPE_CUSTOM) {
    return custom_sampler_cache_[sampler_state.custom_type][no_linear];
  }
  return sampler_cache_[sampler_state.extend_yz][sampler_state.extend_x][sampler_state.filtering]
                       [no_linear];
}

}  // namespace blender::gpu
