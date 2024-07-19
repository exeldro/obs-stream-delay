#include "output-delay.h"
#include "obs-internal.h"
#include "obs-module.h"
#include "obs.h"


new_packet_func encoder_replace_callback(obs_encoder_t *enc,
					 new_packet_func new_fuc,
					 void *new_param, obs_output_t *output)
{
	if (!enc)
		return NULL;
	new_packet_func old_func = NULL;
	pthread_mutex_lock(&enc->callbacks_mutex);
	bool exists = false;
	for (size_t i = 0; i < enc->callbacks.num; i++) {
		struct encoder_callback *cb = enc->callbacks.array + i;
		if (cb->param == output) {
			if (cb->new_packet == new_fuc) {
				exists = true;
				continue;
			}
			old_func = cb->new_packet;
			if (exists || !new_fuc) {
				da_erase(enc->callbacks, i);
				break;
			}
			cb->new_packet = new_fuc;
			cb->param = new_param;
			break;
		}
	}
	if (!enc->callbacks.num && !new_fuc) {
		//pthread_mutex_unlock(&enc->callbacks_mutex);
		//obs_encoder_release(enc);
		//return old_func;
	}
	output->active_delay_ns = 1;
	pthread_mutex_unlock(&enc->callbacks_mutex);
	return old_func;
}

bool output_data_active(struct obs_output *output)
{
	return os_atomic_load_bool(&output->data_active);
}
