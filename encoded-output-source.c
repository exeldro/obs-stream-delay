#include <obs-module.h>
#include <libavcodec/avcodec.h>
#include <pthread.h>
#include <util/darray.h>
#include <media-io/video-scaler.h>
#include <media-io/video-io.h>
#include <util/platform.h>
#include <obs-frontend-api.h>

void add_encoded_output_source(obs_source_t *source);
void remove_encoded_output_source(obs_source_t *source);

struct output_view_source_info {
	obs_source_t *source;
};

const char *output_view_source_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("EncodedOutputView");
}

void *output_view_source_create(obs_data_t *settings, obs_source_t *source)
{
	UNUSED_PARAMETER(settings);
	struct output_view_source_info *context = bzalloc(sizeof(struct output_view_source_info));
	context->source = source;

	add_encoded_output_source(source);
	return context;
}

void output_view_source_destroy(void *data)
{
	struct output_view_source_info *context = data;
	remove_encoded_output_source(context->source);
	bfree(data);
}

struct obs_source_info encoded_output_view_source = {
	.id = "encoded_output_view_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = output_view_source_name,
	.create = output_view_source_create,
	.destroy = output_view_source_destroy,
};
