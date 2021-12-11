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

#include "fft_denoiser.h"
#include "gain_estimator.h"
#include "noise_estimator.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI (3.14159265358979323846)
#endif

#define WHITENING_DECAY_RATE 1000.f
#define WHITENING_FLOOR 0.02f

static void get_info_from_bins(float *fft_power, float *fft_magnitude,
                               float *fft_phase, int half_fft_size,
                               int fft_size, const float *fft_buffer);
static bool is_empty(const float *spectrum, int N);
static void fft_denoiser_update_wetdry_target(FFTDenoiser *self, bool enable);
static void fft_denoiser_soft_bypass(FFTDenoiser *self);
static void get_denoised_spectrum(FFTDenoiser *self);
static void get_residual_spectrum(FFTDenoiser *self, float whitening_factor);

static void get_final_spectrum(FFTDenoiser *self, bool residual_listen,
                               float reduction_amount);
static inline float from_db_to_cv(float gain_db);

struct FFTDenoiser {
  int fft_size;
  int half_fft_size;
  int samp_rate;
  int hop;

  float *fft_spectrum;
  float *processed_fft_spectrum;

  float tau;
  float wet_dry_target;
  float wet_dry;

  float *gain_spectrum;
  float *residual_spectrum;
  float *denoised_spectrum;
  float *whitened_residual_spectrum;

  float *power_spectrum;
  float *phase_spectrum;
  float *magnitude_spectrum;

  GainEstimator *gain_estimation;
  NoiseEstimator *noise_estimation;
  NoiseProfile *noise_profile;
  DenoiseParameters denoise_parameters;

  float *residual_max_spectrum;
  float max_decay_rate;
  int whitening_window_count;
};

FFTDenoiser *fft_denoiser_initialize(int samp_rate, int fft_size,
                                     int overlap_factor) {
  FFTDenoiser *self = (FFTDenoiser *)calloc(1, sizeof(FFTDenoiser));

  self->fft_size = fft_size;
  self->half_fft_size = self->fft_size / 2;
  self->hop = self->fft_size / overlap_factor;
  self->samp_rate = samp_rate;

  self->fft_spectrum = (float *)calloc((self->fft_size), sizeof(float));
  self->processed_fft_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));

  self->tau = (1.f - expf(-2.f * M_PI * 25.f * 64.f / self->samp_rate));
  self->wet_dry = 0.f;

  self->residual_max_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->max_decay_rate =
      expf(-1000.f / (((WHITENING_DECAY_RATE)*self->samp_rate) / self->hop));

  self->residual_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->denoised_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->gain_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->whitened_residual_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));

  self->whitening_window_count = 0.f;

  self->gain_estimation =
      gain_estimation_initialize(self->fft_size, self->samp_rate, self->hop);
  self->noise_estimation = noise_estimation_initialize(self->fft_size);

  self->power_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->magnitude_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));
  self->phase_spectrum =
      (float *)calloc((self->half_fft_size + 1), sizeof(float));

  return self;
}

void fft_denoiser_free(FFTDenoiser *self) {
  gain_estimation_free(self->gain_estimation);
  noise_estimation_free(self->noise_estimation);

  free(self->fft_spectrum);
  free(self->processed_fft_spectrum);
  free(self->gain_spectrum);
  free(self->residual_spectrum);
  free(self->whitened_residual_spectrum);
  free(self->denoised_spectrum);
  free(self->residual_max_spectrum);
  free(self->power_spectrum);
  free(self->magnitude_spectrum);
  free(self->phase_spectrum);
  free(self);
}

void load_denoise_parameters(FFTDenoiser *self,
                             const DenoiseParameters new_parameters) {
  self->denoise_parameters = new_parameters;
}

void load_noise_profile(FFTDenoiser *self, NoiseProfile *noise_profile) {
  self->noise_profile = noise_profile;
}

void fft_denoiser_run(FFTDenoiser *self, float *fft_spectrum) {
  bool enable = (bool)*self->denoise_parameters.enable;

  bool learn_noise = (bool)*self->denoise_parameters.learn_noise;
  bool residual_listen = (bool)*self->denoise_parameters.residual_listen;
  float transient_protection = *self->denoise_parameters.transient_threshold;
  float masking = *self->denoise_parameters.masking_ceiling_limit / 100.f;
  float release = *self->denoise_parameters.release_time;
  float noise_rescale = *self->denoise_parameters.noise_rescale;
  float reduction_amount =
      from_db_to_cv(*self->denoise_parameters.reduction_amount * -1.f);
  float whitening_factor = *self->denoise_parameters.whitening_factor;
  float *noise_spectrum = self->noise_profile->noise_profile;

  fft_denoiser_update_wetdry_target(self, enable);

  memcpy(self->fft_spectrum, fft_spectrum, sizeof(float) * self->fft_size);

  get_info_from_bins(self->power_spectrum, self->magnitude_spectrum,
                     self->phase_spectrum, self->half_fft_size, self->fft_size,
                     self->fft_spectrum);

  if (is_empty(self->power_spectrum, self->half_fft_size) == false) {
    if (learn_noise) {
      noise_estimation_run(self->noise_estimation, noise_spectrum,
                           self->power_spectrum);
    } else {
      if (is_noise_estimation_available(self->noise_estimation)) {
        gain_estimation_run(self->gain_estimation, self->power_spectrum,
                            noise_spectrum, self->gain_spectrum,
                            transient_protection, masking, release,
                            noise_rescale);

        get_denoised_spectrum(self);

        get_residual_spectrum(self, whitening_factor);

        get_final_spectrum(self, residual_listen, reduction_amount);
      }
    }
  }

  fft_denoiser_soft_bypass(self);

  memcpy(fft_spectrum, self->processed_fft_spectrum,
         sizeof(float) * self->half_fft_size + 1);
}

static void get_info_from_bins(float *fft_power, float *fft_magnitude,
                               float *fft_phase, int half_fft_size,
                               int fft_size, const float *fft_buffer) {
  float real_bin = fft_buffer[0];

  fft_power[0] = real_bin * real_bin;
  fft_magnitude[0] = real_bin;
  fft_phase[0] = atan2f(real_bin, 0.f);

  for (int k = 1; k <= half_fft_size; k++) {
    float imag_bin = 0.f;
    float magnitude = 0.f;
    float power = 0.f;
    float phase = 0.f;

    real_bin = fft_buffer[k];
    imag_bin = fft_buffer[fft_size - k];

    if (k < half_fft_size) {
      power = (real_bin * real_bin + imag_bin * imag_bin);
      magnitude = sqrtf(power);
      phase = atan2f(real_bin, imag_bin);
    } else {
      power = real_bin * real_bin;
      magnitude = real_bin;
      phase = atan2f(real_bin, 0.f);
    }

    fft_power[k] = power;
    fft_magnitude[k] = magnitude;
    fft_phase[k] = phase;
  }
}

static bool is_empty(const float *spectrum, int N) {
  for (int k = 1; k <= N; k++) {
    if (spectrum[k] > FLT_MIN) {
      return false;
    }
  }
  return true;
}

static void fft_denoiser_update_wetdry_target(FFTDenoiser *self, bool enable) {
  if (enable) {
    self->wet_dry_target = 1.f;
  } else {
    self->wet_dry_target = 0.f;
  }

  self->wet_dry += self->tau * (self->wet_dry_target - self->wet_dry) + FLT_MIN;
}

static void fft_denoiser_soft_bypass(FFTDenoiser *self) {
  for (int k = 1; k <= self->half_fft_size; k++) {
    self->processed_fft_spectrum[k] =
        (1.f - self->wet_dry) * self->fft_spectrum[k] +
        self->processed_fft_spectrum[k] * self->wet_dry;
  }
}

static void residual_spectrum_whitening(FFTDenoiser *self,
                                        float whitening_factor) {
  self->whitening_window_count++;

  for (int k = 1; k <= self->half_fft_size; k++) {
    if (self->whitening_window_count > 1.f) {
      self->residual_max_spectrum[k] =
          fmaxf(fmaxf(self->residual_spectrum[k], WHITENING_FLOOR),
                self->residual_max_spectrum[k] * self->max_decay_rate);
    } else {
      self->residual_max_spectrum[k] =
          fmaxf(self->residual_spectrum[k], WHITENING_FLOOR);
    }
  }

  for (int k = 1; k <= self->half_fft_size; k++) {
    if (self->residual_spectrum[k] > FLT_MIN) {
      self->whitened_residual_spectrum[k] =
          self->residual_spectrum[k] / self->residual_max_spectrum[k];

      self->residual_spectrum[k] =
          (1.f - whitening_factor) * self->residual_spectrum[k] +
          whitening_factor * self->whitened_residual_spectrum[k];
    }
  }
}

static void get_denoised_spectrum(FFTDenoiser *self) {
  for (int k = 1; k <= self->half_fft_size; k++) {
    self->denoised_spectrum[k] = self->fft_spectrum[k] * self->gain_spectrum[k];
  }
}

static void get_residual_spectrum(FFTDenoiser *self, float whitening_factor) {
  for (int k = 1; k <= self->half_fft_size; k++) {
    self->residual_spectrum[k] =
        self->fft_spectrum[k] - self->denoised_spectrum[k];
  }

  if (whitening_factor > 0.f) {
    residual_spectrum_whitening(self, whitening_factor);
  }
}

static void get_final_spectrum(FFTDenoiser *self, bool residual_listen,
                               float reduction_amount) {
  if (residual_listen) {
    for (int k = 1; k <= self->half_fft_size; k++) {
      self->processed_fft_spectrum[k] = self->residual_spectrum[k];
    }
  } else {
    for (int k = 1; k <= self->half_fft_size; k++) {
      self->processed_fft_spectrum[k] =
          self->denoised_spectrum[k] +
          self->residual_spectrum[k] * reduction_amount;
    }
  }
}

static inline float from_db_to_cv(float gain_db) {
  return expf(gain_db / 10.f * logf(10.f));
}
