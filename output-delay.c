#include "output-delay.h"
#include "obs-module.h"
#include "obs.h"
#include <util/darray.h>
#include <util/threading.h>

struct encoder_callback {
	bool sent_first_packet;
	void (*new_packet)(void *param, struct encoder_packet *packet);
	void *param;
};

new_packet_func encoder_replace_callback(obs_encoder_t *enc,
					 new_packet_func new_fuc,
					 void *new_param, obs_output_t *output)
{
	if (!enc)
		return NULL;
	new_packet_func old_func = NULL;
	pthread_mutex_t *callbacks_mutex = (pthread_mutex_t *)((char *)enc + 1304);
	pthread_mutex_lock(callbacks_mutex);
	bool exists = false;
	DARRAY(struct encoder_callback) *callbacks = (void *)((char *)enc + 1312); 
	for (size_t i = 0; i < callbacks->num; i++) {
		struct encoder_callback *cb = callbacks->array + i;
		if (cb->param == output) {
			if (cb->new_packet == new_fuc) {
				exists = true;
				continue;
			}
			old_func = cb->new_packet;
			if (exists || !new_fuc) {
				da_erase(*callbacks, i);
				break;
			}
			cb->new_packet = new_fuc;
			cb->param = new_param;
			break;
		}
	}
	if (!callbacks->num && !new_fuc) {
		//pthread_mutex_unlock(&enc->callbacks_mutex);
		//obs_encoder_release(enc);
		//return old_func;
	}
	
	uint64_t *active_delay_ns = (uint64_t *)(((char *)output) + 3056);
	*active_delay_ns = 1;
	pthread_mutex_unlock(callbacks_mutex);
	return old_func;
}

bool output_data_active(struct obs_output *output)
{
	return *((volatile bool *)(((char *)output) + 529));
}
