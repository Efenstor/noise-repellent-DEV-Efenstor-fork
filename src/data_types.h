/*
noise-repellent -- Noise Reduction LV2

Copyright 2016 Luciano Dato <lucianodato@gmail.com>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/
*/

#ifndef DATA_TYPES_H
#define DATA_TYPES_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t noise_profile_size;
  float *noise_profile;
} NoiseProfile;

typedef struct {
  bool enable;
  bool learn_noise;
  bool residual_listen;
  float reduction_amount;
  float release_time;
  float masking_ceiling_limit;
  float whitening_factor;
  float transient_threshold;
  float noise_rescale;
} DenoiseParameters;

#endif
