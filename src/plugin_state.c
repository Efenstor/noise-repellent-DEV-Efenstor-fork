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

#include "plugin_state.h"
#include <stdlib.h>
#include <string.h>

struct PluginState
{
	LV2_URID_Map *map;
	LV2_URID atom_Int;
	LV2_URID atom_Vector;
	LV2_URID property_fft_size;
	LV2_URID property_saved_noise_profile;
};

bool plugin_state_configure(PluginState *self, const LV2_Feature *const *features)
{
	for (int i = 0; features[i]; ++i)
	{
		if (!strcmp(features[i]->URI, LV2_URID__map))
		{
			self->map = (LV2_URID_Map *)features[i]->data;
		}
	}

	if (!self->map)
	{
		return false;
	}

	self->atom_Int = self->map->map(self->map->handle, LV2_ATOM__Int);
	self->atom_Vector = self->map->map(self->map->handle, LV2_ATOM__Vector);
	self->property_fft_size = self->map->map(self->map->handle, NOISEREPELLENT_URI "#fftsize");
	self->property_saved_noise_profile = self->map->map(self->map->handle, NOISEREPELLENT_URI "#savednoiseprofile");

	return true;
}

void plugin_state_savestate(PluginState *self, LV2_State_Store_Function store, LV2_State_Handle handle,
							int fft_size, NoiseProfile *noise_profile)
{
	store(handle, self->property_fft_size, &fft_size, sizeof(int), self->atom_Int,
		  LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

	store(handle, self->property_saved_noise_profile, (void *)noise_profile, sizeof(noise_profile),
		  self->atom_Vector, LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

bool plugin_state_restorestate(PluginState *self, LV2_State_Retrieve_Function retrieve, LV2_State_Handle handle,
							   NoiseProfile *noise_profile, int *fft_size)
{
	size_t size;
	uint32_t type;
	uint32_t valflags;

	const int *fftsize = (const int *)retrieve(handle, self->property_fft_size, &size, &type, &valflags);
	if (!fftsize || type != self->atom_Int)
	{
		return false;
	}

	const void *saved_noise_profile = retrieve(handle, self->property_saved_noise_profile, &size, &type, &valflags);
	if (!saved_noise_profile || size != sizeof(noise_profile) || type != self->atom_Vector)
	{
		return false;
	}

	memcpy(fft_size, fftsize, sizeof(int));
	set_noise_profile(noise_profile, (float *)LV2_ATOM_BODY(saved_noise_profile));

	return true;
}

void plugin_state_free(PluginState *self)
{
	self->atom_Int = 0;
	self->atom_Vector = 0;
	free(self);
}

PluginState *plugin_state_initialize()
{
	PluginState *self = (PluginState *)malloc(1 * sizeof(PluginState));

	return self;
}
