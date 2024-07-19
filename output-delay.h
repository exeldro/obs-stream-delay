#pragma once
#include "obs.h"

#ifdef __cplusplus
extern "C" {
#endif

struct encoder_callback {
	bool sent_first_packet;
	void (*new_packet)(void *param, struct encoder_packet *packet);
	void *param;
};

typedef void (*new_packet_func)(void *param, struct encoder_packet *packet);
new_packet_func encoder_replace_callback(obs_encoder_t *enc, new_packet_func new_fuc, void *new_param, void *old_param);
bool output_data_active(struct obs_output *output);

#ifdef __cplusplus
}
#endif
