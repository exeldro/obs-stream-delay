
#include "stream-delay.hpp"
#include "output-delay.h"

#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QVBoxLayout>
#include <QStyle>

#include <util/platform.h>

#include "obs-module.h"
#include "version.h"
#include "obs-frontend-api.h"

#include "pthread.h"
#include <util/config-file.h>
#include <util/threading.h>
#include <util/darray.h>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("Exeldro");
OBS_MODULE_USE_DEFAULT_LOCALE("output-delay", "en-US")

#define MICROSECOND_DEN 1000000

extern "C" {
extern struct obs_source_info encoded_output_view_source;

void add_encoded_output_source(obs_source_t *source);
void remove_encoded_output_source(obs_source_t *source);
};

StreamDelayDock *stream_delay_dock = nullptr;

bool obs_module_load()
{
	blog(LOG_INFO, "[Stream Delay] loaded version %s", PROJECT_VERSION);
	const auto main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	obs_frontend_push_ui_translation(obs_module_get_string);
	stream_delay_dock = new StreamDelayDock(main_window);

	//setObjectName("StreamDelayDock");
	//setWindowTitle(QString::fromUtf8("StreamDelay"));
#if LIBOBS_API_VER >= MAKE_SEMANTIC_VERSION(30, 0, 0)
	obs_frontend_add_dock_by_id("StreamDelayDock", obs_module_text("StreamDelay"), stream_delay_dock);
#else
	const auto d = new QDockWidget(main_window);
	d->setObjectName(QString::fromUtf8("StreamDelayDock"));
	d->setWindowTitle(QString::fromUtf8(obs_module_text("StreamDelay")));
	d->setWidget(stream_delay_dock);
	d->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
	d->setFloating(true);
	obs_frontend_add_dock(stream_delay_dock);
#endif
	obs_frontend_pop_ui_translation();
	obs_register_source(&encoded_output_view_source);
	//obs_register_output(&encoded_output);
	return true;
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Description");
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("StreamDelay");
}

void add_encoded_output_source(obs_source_t *source)
{
	if (!stream_delay_dock)
		return;
	stream_delay_dock->add_encoded_output_source(source);
}
void remove_encoded_output_source(obs_source_t *source)
{
	if (!stream_delay_dock)
		return;
	stream_delay_dock->remove_encoded_output_source(source);
}

static void log_av_error(const char *method, int ret)
{
	char err[512];
	av_strerror(ret, err, sizeof(err));
	blog(LOG_ERROR, "%s failed with: %s", method, err);
}

void StreamDelayDock::add_encoded_output_source(obs_source_t *source)
{
	da_push_back(sources, &source);
}

void StreamDelayDock::remove_encoded_output_source(obs_source_t *source)
{
	da_erase_item(sources, &source);
}

StreamDelayDock::StreamDelayDock(QWidget *parent) : QWidget(parent)
{
	pthread_mutex_init(&interleaved_mutex, nullptr);
	da_init(interleaved_packets);
	pthread_mutex_init(&replacement_mutex, nullptr);
	da_init(replacement_packets);

	pthread_mutex_init(&output_video_mutex, nullptr);
	da_init(output_video_packets);

	pthread_mutex_init(&codec_context_mutex, nullptr);

	da_init(sources);
	//obs_output_create()
	//obs_enum_outputs()

	//OBS_OUTPUT_ENCODED

	//get encoders from output
	//pthread_mutex_lock(encoder->callbacks_mutex
	//foreach encoder->callbacks
	// store and replace callback
	//pthread_mutex_unlock(encoder->callbacks_mutex

	auto l = new QGridLayout;

	int row = 0;

	enabled = new QCheckBox(QString::fromUtf8(obs_module_text("Enabled")));
	l->addWidget(enabled, row, 0);

	row++;

	auto label = new QLabel(QString::fromUtf8(obs_module_text("Replacement")));
	l->addWidget(label, row, 0);

	replacementDuration = new QLabel(QString::fromUtf8("0 ms"));
	l->addWidget(replacementDuration, row, 1);

	auto test = new QPushButton(QString::fromUtf8(obs_module_text("Record")));
	connect(test, &QPushButton::clicked, [this] {
		if (start_record || stop_record)
			return;
		if (recording) {
			//recording = false;
			//updateReplacementDuration();
			stop_record = true;
			if (!desiredButton->isEnabled())
				desiredButton->setEnabled(true);
			if (!replaceButton->isEnabled())
				replaceButton->setEnabled(true);
		} else {
			start_record = true;
		}
	});
	l->addWidget(test, row, 2);

	row++;

	label = new QLabel(QString::fromUtf8(obs_module_text("CurrentDelay")));
	l->addWidget(label, row, 0);

	currentDelay = new QLabel("0 ms");
	l->addWidget(currentDelay, row, 1);

	auto live = new QPushButton(QString::fromUtf8(obs_module_text("Live")));
	l->addWidget(live, row, 2);
	connect(live, &QPushButton::clicked, [this] {
		skip_to = 0;
		delay_to = -1;
		if (replace)
			stop_replace = true;
	});

	row++;

	label = new QLabel(QString::fromUtf8(obs_module_text("DesiredDelay")));
	l->addWidget(label, row, 0);

	desiredDelay = new QSpinBox();
	desiredDelay->setSuffix(" ms");
	desiredDelay->setMinimum(0);
	desiredDelay->setMaximum(1000000);
	l->addWidget(desiredDelay, row, 1);

	desiredButton = new QPushButton(QString::fromUtf8(obs_module_text("Go")));
	desiredButton->setEnabled(false);
	l->addWidget(desiredButton, row, 2);
	connect(desiredButton, &QPushButton::clicked, [this] {
		int64_t new_delay = desiredDelay->value() * 1000;
		if (new_delay > delay_usec) {
			delay_to = new_delay;
			skip_to = -1;
		} else if (new_delay < delay_usec) {
			skip_to = new_delay;
			delay_to = -1;
		}
	});

	row++;

	label = new QLabel(QString::fromUtf8(obs_module_text("Replace")));
	l->addWidget(label, row, 0);

	replaceDuration = new QLabel("");
	replaceDuration->setProperty("themeID", "good");
	l->addWidget(replaceDuration, row, 1);

	replaceButton = new QPushButton(QString::fromUtf8(obs_module_text("Go")));
	replaceButton->setEnabled(false);
	connect(replaceButton, &QPushButton::clicked, [this] {
		if (replace) {
			if (!start_replace)
				stop_replace = true;
		} else {
			if (!stop_replace)
				start_replace = true;
		}
	});
	l->addWidget(replaceButton, row, 2);

	setLayout(l);

	updateTimer.start(200);
	connect(&updateTimer, &QTimer::timeout, [this] {
		updateReplacementDuration();
		updateReplaceDuration();
		updateCurrentDelay();
	});

	obs_frontend_add_event_callback(frontend_event, this);
}

void StreamDelayDock::updateReplacementDuration()
{
	pthread_mutex_lock(&replacement_mutex);
	if (replacement_packets.num) {

		auto duration =
			replacement_packets.array[replacement_packets.num - 1].dts_usec - replacement_packets.array[0].dts_usec;
		duration /= 1000;
		pthread_mutex_unlock(&replacement_mutex);
		replacementDuration->setText(QString::fromUtf8("%1 ms").arg(duration));
	} else {
		pthread_mutex_unlock(&replacement_mutex);
		replacementDuration->setText("0 ms");
	}
}

void StreamDelayDock::updateReplaceDuration()
{
	if (!replace && !delaying) {
		if (!replaceDuration->text().isEmpty()) {
			replaceDuration->setText("");
		}
		if (currentDelay->property("themeID").toString() != QString("good")) {
			currentDelay->setProperty("themeID", "good");
			currentDelay->style()->unpolish(currentDelay);
			currentDelay->style()->polish(currentDelay);
		}
		return;
	}
	if (!currentDelay->property("themeID").toString().isEmpty()) {
		currentDelay->setProperty("themeID", "");
		currentDelay->style()->unpolish(currentDelay);
		currentDelay->style()->polish(currentDelay);
	}
	pthread_mutex_lock(&replacement_mutex);
	if (replace_index < replacement_packets.num) {

		auto duration = replacement_packets.array[replace_index].dts_usec - replacement_packets.array[0].dts_usec;
		duration /= 1000;
		pthread_mutex_unlock(&replacement_mutex);
		replaceDuration->setText(QString::fromUtf8("%1 ms").arg(duration));
	} else {
		pthread_mutex_unlock(&replacement_mutex);
		replaceDuration->setText("0 ms");
	}
}

void StreamDelayDock::updateCurrentDelay()
{
	pthread_mutex_lock(&interleaved_mutex);
	if (interleaved_packets.num) {

		auto duration =
			interleaved_packets.array[interleaved_packets.num - 1].dts_usec - interleaved_packets.array[0].dts_usec;
		duration /= 1000;
		pthread_mutex_unlock(&interleaved_mutex);
		currentDelay->setText(QString::fromUtf8("%1 ms").arg(duration));
	} else {
		pthread_mutex_unlock(&interleaved_mutex);
		currentDelay->setText("0 ms");
	}
}

void StreamDelayDock::disconnect_encoders()
{
	pthread_mutex_lock(&codec_context_mutex);
	if (codec_context) {
		stop_decode = true;
		pthread_join(decode_thread, NULL);
		avcodec_free_context(&codec_context);
		codec_context = nullptr;
		stop_decode = false;
	}
	pthread_mutex_unlock(&codec_context_mutex);

	pthread_mutex_lock(&interleaved_mutex);
	if (interleaved_packets.num)
		discard_to_idx(interleaved_packets.num);
	pthread_mutex_unlock(&interleaved_mutex);

	if (!output)
		return;
	if (video_encode_callback) {
		auto venc = obs_output_get_video_encoder(output);
		if (venc) {
			encoder_replace_callback(venc, nullptr, output, this);
		}
		video_encode_callback = nullptr;
	}
	if (audio_encode_callback) {
		for (size_t idx = 0; idx < MAX_AUDIO_MIXES; idx++) {
			auto aenc = obs_output_get_audio_encoder(output, idx);
			if (!aenc)
				continue;
			encoder_replace_callback(aenc, nullptr, output, this);
		}
		audio_encode_callback = nullptr;
	}
	output = nullptr;
}

StreamDelayDock::~StreamDelayDock()
{
	disconnect();

	da_free(interleaved_packets);
	da_free(output_video_packets);
	da_free(replacement_packets);
	da_free(sources);

	//obs_output_release(output);
}

size_t StreamDelayDock::get_audio_encoder_idx(obs_encoder_t *encoder)
{
	if (!output)
		return 0;
	for (size_t idx = 0; idx < MAX_AUDIO_MIXES; idx++) {
		if (obs_output_get_audio_encoder(output, idx) == encoder)
			return idx;
	}
	return 0;
}

void StreamDelayDock::insert_interleaved_packet(encoder_packet *packet)
{
	size_t idx;
	for (idx = 0; idx < interleaved_packets.num; idx++) {
		const struct encoder_packet *cur_packet = interleaved_packets.array + idx;

		if (packet->dts_usec == cur_packet->dts_usec && packet->type == OBS_ENCODER_VIDEO) {
			break;
		}
		if (packet->dts_usec < cur_packet->dts_usec) {
			break;
		}
	}

	da_insert(interleaved_packets, idx, packet);
}

void StreamDelayDock::discard_to_idx(size_t idx)
{
	for (size_t i = 0; i < idx; i++) {
		struct encoder_packet *packet = &interleaved_packets.array[i];
		obs_encoder_packet_release(packet);
	}

	da_erase_range(interleaved_packets, 0, idx);
}

void StreamDelayDock::discard_unused_audio_packets(int64_t dts_usec)
{
	size_t idx = 0;

	for (; idx < interleaved_packets.num; idx++) {
		struct encoder_packet *p = &interleaved_packets.array[idx];

		if (p->dts_usec >= dts_usec)
			break;
	}

	if (idx)
		discard_to_idx(idx);
}

void StreamDelayDock::encoder_packet_create_instance(struct encoder_packet *dst, const struct encoder_packet *src)
{
	*dst = *src;
	long *p_refs = (long *)bmalloc(src->size + sizeof(long));
	dst->data = (uint8_t *)(p_refs + 1);
	*p_refs = 1;
	memcpy(dst->data, src->data, src->size);
}

void StreamDelayDock::encoderCallback(void *p, encoder_packet *packet)
{
	auto *d = static_cast<StreamDelayDock *>(p);

	if (!obs_output_active(d->output)) {
		return;
	}

	if (!output_data_active(d->output)) {
		d->disconnect_encoders();
	}

	if (packet->type == OBS_ENCODER_AUDIO)
		packet->track_idx = d->get_audio_encoder_idx(packet->encoder);

	pthread_mutex_lock(&d->interleaved_mutex);
	/* if first video frame is not a keyframe, discard until received */
	if (!d->received_video && packet->type == OBS_ENCODER_VIDEO && !packet->keyframe) {
		d->discard_unused_audio_packets(packet->dts_usec);
		pthread_mutex_unlock(&d->interleaved_mutex);
		return;
	}

	struct encoder_packet out = {0};
	d->encoder_packet_create_instance(&out, packet);

	const bool was_started = d->received_audio && d->received_video;
	if (was_started)
		d->apply_interleaved_packet_offset(&out);
	else
		d->check_received(packet);

	d->insert_interleaved_packet(&out);
	d->set_higher_ts(&out);

	/* when both video and audio have been received, we're ready
	 * to start sending out packets (one at a time) */
	if (d->received_audio && d->received_video) {
		if (!was_started) {
			if (d->prune_interleaved_packets()) {
				if (d->initialize_interleaved_packets()) {
					d->resort_interleaved_packets();
					d->send_interleaved();
				}
			}
		} else {
			d->send_interleaved();
		}
	}

	pthread_mutex_unlock(&d->interleaved_mutex);
}

enum video_range_type StreamDelayDock::AVRangeToOBSRange(enum AVColorRange r)
{
	return r == AVCOL_RANGE_JPEG ? VIDEO_RANGE_FULL : VIDEO_RANGE_DEFAULT;
}

enum video_format StreamDelayDock::AVPixelFormatToOBSFormat(int f)
{
	switch (f) {
	case AV_PIX_FMT_NONE:
		return VIDEO_FORMAT_NONE;
	case AV_PIX_FMT_YUV420P:
		return VIDEO_FORMAT_I420;
	case AV_PIX_FMT_YUYV422:
		return VIDEO_FORMAT_YUY2;
	case AV_PIX_FMT_YUV422P:
		return VIDEO_FORMAT_I422;
	case AV_PIX_FMT_YUV422P10LE:
		return VIDEO_FORMAT_I210;
	case AV_PIX_FMT_YUV444P:
		return VIDEO_FORMAT_I444;
	case AV_PIX_FMT_YUV444P12LE:
		return VIDEO_FORMAT_I412;
	case AV_PIX_FMT_UYVY422:
		return VIDEO_FORMAT_UYVY;
	case AV_PIX_FMT_YVYU422:
		return VIDEO_FORMAT_YVYU;
	case AV_PIX_FMT_NV12:
		return VIDEO_FORMAT_NV12;
	case AV_PIX_FMT_RGBA:
		return VIDEO_FORMAT_RGBA;
	case AV_PIX_FMT_BGRA:
		return VIDEO_FORMAT_BGRA;
	case AV_PIX_FMT_YUVA420P:
		return VIDEO_FORMAT_I40A;
	case AV_PIX_FMT_YUV420P10LE:
		return VIDEO_FORMAT_I010;
	case AV_PIX_FMT_YUVA422P:
		return VIDEO_FORMAT_I42A;
	case AV_PIX_FMT_YUVA444P:
		return VIDEO_FORMAT_YUVA;
#if LIBAVUTIL_BUILD >= AV_VERSION_INT(56, 31, 100)
	case AV_PIX_FMT_YUVA444P12LE:
		return VIDEO_FORMAT_YA2L;
#endif
	case AV_PIX_FMT_BGR0:
		return VIDEO_FORMAT_BGRX;
	case AV_PIX_FMT_P010LE:
		return VIDEO_FORMAT_P010;
	default:;
	}

	return VIDEO_FORMAT_NONE;
}

enum video_colorspace StreamDelayDock::AVColorSpaceToOBSSpace(enum AVColorSpace s, enum AVColorTransferCharacteristic trc,
							      enum AVColorPrimaries color_primaries)
{
	switch (s) {
	case AVCOL_SPC_BT709:
		return (trc == AVCOL_TRC_IEC61966_2_1) ? VIDEO_CS_SRGB : VIDEO_CS_709;
	case AVCOL_SPC_FCC:
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
	case AVCOL_SPC_SMPTE240M:
		return VIDEO_CS_601;
	case AVCOL_SPC_BT2020_NCL:
		return (trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG : VIDEO_CS_2100_PQ;
	default:
		return (color_primaries == AVCOL_PRI_BT2020)
			       ? ((trc == AVCOL_TRC_ARIB_STD_B67) ? VIDEO_CS_2100_HLG : VIDEO_CS_2100_PQ)
			       : VIDEO_CS_DEFAULT;
	}
}

void StreamDelayDock::AVFrameToSourceFrame(struct obs_source_frame *dst, AVFrame *src, AVRational time_base)
{
	memset(dst, 0, sizeof(struct obs_source_frame));

	dst->width = src->width;
	dst->height = src->height;
	AVRational r = {1, 1000000000};
	dst->timestamp = av_rescale_q(src->best_effort_timestamp, time_base, r);

	enum video_range_type range = AVRangeToOBSRange(src->color_range);
	enum video_format format = AVPixelFormatToOBSFormat(src->format);
	enum video_colorspace space = AVColorSpaceToOBSSpace(src->colorspace, src->color_trc, src->color_primaries);

	// ToDo: HDR support (max_luminance)
	dst->format = format;
	dst->full_range = range == VIDEO_RANGE_FULL;
	video_format_get_parameters_for_format(space, range, format, dst->color_matrix, dst->color_range_min, dst->color_range_max);

	switch (src->color_trc) {
	case AVCOL_TRC_BT709:
	case AVCOL_TRC_GAMMA22:
	case AVCOL_TRC_GAMMA28:
	case AVCOL_TRC_SMPTE170M:
	case AVCOL_TRC_SMPTE240M:
	case AVCOL_TRC_IEC61966_2_1:
		dst->trc = VIDEO_TRC_SRGB;
		break;
	case AVCOL_TRC_SMPTE2084:
		dst->trc = VIDEO_TRC_PQ;
		break;
	case AVCOL_TRC_ARIB_STD_B67:
		dst->trc = VIDEO_TRC_HLG;
		break;
	default:
		dst->trc = VIDEO_TRC_DEFAULT;
	}

	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		dst->data[i] = src->data[i];
		dst->linesize[i] = abs(src->linesize[i]);
	}
}

void *StreamDelayDock::decode_output(void *data)
{
	os_set_thread_name("StreamDelayDock decode thread");

	auto d = (StreamDelayDock *)data;
	struct obs_source_frame frame = {0};
	AVFrame *av_frame = av_frame_alloc();
	auto venc = obs_output_get_video_encoder(d->output);

	struct encoder_packet packet = {0};
	if (obs_encoder_get_extra_data(venc, &packet.data, &packet.size)) {
		AVPacket av_packet = {};
		av_packet.size = (int)packet.size;
		av_packet.data = packet.data;
		if (avcodec_send_packet(d->codec_context, &av_packet) != 0) {
			blog(LOG_DEBUG, "Sending extra data failed.");
		}
	}

	bool got_first_keyframe = false;

	while (!d->stop_decode) {
		os_sleep_ms(1);
		pthread_mutex_lock(&d->output_video_mutex);
		if (!d->output_video_packets.num) {
			pthread_mutex_unlock(&d->output_video_mutex);
			continue;
		}
		struct encoder_packet pkt = d->output_video_packets.array[0];
		if (!got_first_keyframe && pkt.keyframe)
			got_first_keyframe = true;

		if (!got_first_keyframe) {
			da_erase(d->output_video_packets, 0);
			obs_encoder_packet_release(&pkt);
			continue;
		}

		AVPacket av_packet = {};
		av_packet.size = (int)pkt.size;
		av_packet.pts = pkt.pts;
		av_packet.dts = pkt.dts;
		av_packet.data = pkt.data;

		int ret = avcodec_send_packet(d->codec_context, &av_packet);
		if (ret == AVERROR(EAGAIN)) {
			pthread_mutex_unlock(&d->output_video_mutex);
			continue;
		}
		if (ret < 0 || ret == AVERROR_EOF) {
			obs_encoder_packet_release(&pkt);
			da_erase(d->output_video_packets, 0);
			pthread_mutex_unlock(&d->output_video_mutex);
			continue;
		}
		da_erase(d->output_video_packets, 0);
		pthread_mutex_unlock(&d->output_video_mutex);
		while (ret >= 0) {
			ret = avcodec_receive_frame(d->codec_context, av_frame);
			if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
				obs_encoder_packet_release(&pkt);
				break;
			} else if (ret < 0) {
				log_av_error("avcodec_receive_frame", ret);
				continue;
			}
			AVRational r = {pkt.timebase_num, pkt.timebase_den};
			AVFrameToSourceFrame(&frame, av_frame, r);

			for (size_t i = 0; i < d->sources.num; i++) {
				obs_source_output_video(d->sources.array[i], &frame);
			}
		}

		obs_encoder_packet_release(&pkt);
	}

	av_frame_free(&av_frame);

	return NULL;
}

enum AVCodecID StreamDelayDock::NameToAVCodecID(const char *str)
{
	if (strcmp(str, "hevc") == 0)
		return AV_CODEC_ID_HEVC;
	if (strcmp(str, "av1") == 0)
		return AV_CODEC_ID_AV1;
	if (strcmp(str, "h264") == 0)
		return AV_CODEC_ID_H264;

	return AV_CODEC_ID_FIRST_UNKNOWN;
}

void StreamDelayDock::connect_encoders()
{
	if (!output)
		return;
	if (!enabled->isChecked())
		return;

	auto venc = obs_output_get_video_encoder(output);
	if (venc) {
		auto old = encoder_replace_callback(venc, encoderCallback, this, output);
		if (old) {
			if (video_encode_callback && old != video_encode_callback) {
				blog(LOG_INFO, "[Stream Delay] video callback changed");
			}
			video_encode_callback = old;
		}
		enum AVCodecID codec_id = NameToAVCodecID(obs_encoder_get_codec(venc));
		const AVCodec *codec = avcodec_find_decoder(codec_id);
		if (codec) {
			pthread_mutex_lock(&codec_context_mutex);
			if (!codec_context || codec_context->codec != codec) {
				if (codec_context) {
					stop_decode = true;
					pthread_join(decode_thread, NULL);
					avcodec_free_context(&codec_context);
					codec_context = nullptr;
					stop_decode = false;
				}
				codec_context = avcodec_alloc_context3(codec);
				codec_context->width = obs_encoder_get_width(venc);
				codec_context->height = obs_encoder_get_height(venc);

				int ret = avcodec_open2(codec_context, codec, NULL);
				if (ret) {
					//log_av_error("avcodec_open2", ret);
					avcodec_free_context(&codec_context);
					codec_context = nullptr;
				} else {
					pthread_create(&decode_thread, NULL, decode_output, this);
				}
			} else if (codec_context) {
				codec_context->width = obs_encoder_get_width(venc);
				codec_context->height = obs_encoder_get_height(venc);
			}
			pthread_mutex_unlock(&codec_context_mutex);
		}
	}
	for (size_t idx = 0; idx < MAX_AUDIO_MIXES; idx++) {
		auto aenc = obs_output_get_audio_encoder(output, idx);
		if (!aenc)
			continue;
		auto old = encoder_replace_callback(aenc, encoderCallback, this, output);
		if (old) {
			if (audio_encode_callback && old != audio_encode_callback) {
				blog(LOG_INFO, "[Stream Delay] audio callback changed");
			}
			audio_encode_callback = old;
		}
	}
}

void StreamDelayDock::apply_interleaved_packet_offset(struct encoder_packet *out)
{
	int64_t offset;

	/* audio and video need to start at timestamp 0, and the encoders
	 * may not currently be at 0 when we get data.  so, we store the
	 * current dts as offset and subtract that value from the dts/pts
	 * of the output packet. */
	offset = (out->type == OBS_ENCODER_VIDEO) ? video_offset : audio_offsets[out->track_idx];

	out->dts -= offset;
	out->pts -= offset;

	/* convert the newly adjusted dts to relative dts time to ensure proper
	 * interleaving.  if we're using an audio encoder that's already been
	 * started on another output, then the first audio packet may not be
	 * quite perfectly synced up in terms of system time (and there's
	 * nothing we can really do about that), but it will always at least be
	 * within a 23ish millisecond threshold (at least for AAC) */
	out->dts_usec = packet_dts_usec(out);
}

int64_t StreamDelayDock::packet_dts_usec(struct encoder_packet *packet)
{
	return packet->dts * MICROSECOND_DEN / packet->timebase_den;
}

void StreamDelayDock::check_received(struct encoder_packet *out)
{
	if (out->type == OBS_ENCODER_VIDEO) {
		if (!received_video)
			received_video = true;
	} else {
		if (!received_audio)
			received_audio = true;
	}
}

void StreamDelayDock::set_higher_ts(struct encoder_packet *packet)
{
	if (packet->type == OBS_ENCODER_VIDEO) {
		if (highest_video_ts < packet->dts_usec)
			highest_video_ts = packet->dts_usec;
	} else {
		if (highest_audio_ts < packet->dts_usec)
			highest_audio_ts = packet->dts_usec;
	}
}

bool StreamDelayDock::prune_interleaved_packets()
{
	size_t start_idx = 0;
	int prune_start = prune_premature_packets();

#if DEBUG_STARTING_PACKETS == 1
	blog(LOG_DEBUG, "--------- Pruning! %d ---------", prune_start);
	for (size_t i = 0; i < output->interleaved_packets.num; i++) {
		struct encoder_packet *packet = &output->interleaved_packets.array[i];
		blog(LOG_DEBUG, "packet: %s %d, ts: %lld, pruned = %s", packet->type == OBS_ENCODER_AUDIO ? "audio" : "video",
		     (int)packet->track_idx, packet->dts_usec, (int)i < prune_start ? "true" : "false");
	}
#endif

	/* prunes the first video packet if it's too far away from audio */
	if (prune_start == -1)
		return false;
	else if (prune_start != 0)
		start_idx = (size_t)prune_start;
	else
		start_idx = get_interleaved_start_idx();

	if (start_idx)
		discard_to_idx(start_idx);

	return true;
}

int StreamDelayDock::prune_premature_packets()
{
	size_t audio_mixes = num_audio_mixes();
	struct encoder_packet *video;
	int video_idx;
	int max_idx;
	int64_t duration_usec;
	int64_t max_diff = 0;
	int64_t diff = 0;

	video_idx = find_first_packet_type_idx(OBS_ENCODER_VIDEO, 0);
	if (video_idx == -1) {
		received_video = false;
		return -1;
	}

	max_idx = video_idx;
	video = &interleaved_packets.array[video_idx];
	duration_usec = video->timebase_num * 1000000LL / video->timebase_den;

	for (size_t i = 0; i < audio_mixes; i++) {
		struct encoder_packet *audio;
		int audio_idx;

		audio_idx = find_first_packet_type_idx(OBS_ENCODER_AUDIO, i);
		if (audio_idx == -1) {
			received_audio = false;
			return -1;
		}

		audio = &interleaved_packets.array[audio_idx];
		if (audio_idx > max_idx)
			max_idx = audio_idx;

		diff = audio->dts_usec - video->dts_usec;
		if (diff > max_diff)
			max_diff = diff;
	}

	return max_diff > duration_usec ? max_idx + 1 : 0;
}

size_t StreamDelayDock::num_audio_mixes()
{
	size_t mix_count = 1;

	if ((obs_output_get_flags(output) & OBS_OUTPUT_MULTI_TRACK) != 0) {
		mix_count = 0;

		for (size_t i = 0; i < MAX_AUDIO_MIXES; i++) {
			if (!obs_output_get_audio_encoder(output, i))
				break;

			mix_count++;
		}
	}

	return mix_count;
}

int StreamDelayDock::find_first_packet_type_idx(enum obs_encoder_type type, size_t audio_idx)
{
	for (size_t i = 0; i < interleaved_packets.num; i++) {
		const struct encoder_packet *packet = &interleaved_packets.array[i];

		if (packet->type == type) {
			if (type == OBS_ENCODER_AUDIO && packet->track_idx != audio_idx) {
				continue;
			}

			return (int)i;
		}
	}

	return -1;
}

int StreamDelayDock::find_last_packet_type_idx(enum obs_encoder_type type, size_t audio_idx)
{
	for (size_t i = interleaved_packets.num; i > 0; i--) {
		const struct encoder_packet *packet = &interleaved_packets.array[i - 1];

		if (packet->type == type) {
			if (type == OBS_ENCODER_AUDIO && packet->track_idx != audio_idx) {
				continue;
			}

			return (int)(i - 1);
		}
	}

	return -1;
}

size_t StreamDelayDock::get_interleaved_start_idx()
{
	int64_t closest_diff = 0x7FFFFFFFFFFFFFFFLL;
	struct encoder_packet *first_video = find_first_packet_type(OBS_ENCODER_VIDEO, 0);
	size_t video_idx = DARRAY_INVALID;
	size_t idx = 0;

	for (size_t i = 0; i < interleaved_packets.num; i++) {
		struct encoder_packet *packet = &interleaved_packets.array[i];
		int64_t diff;

		if (packet->type != OBS_ENCODER_AUDIO) {
			if (packet == first_video)
				video_idx = i;
			continue;
		}

		diff = llabs(packet->dts_usec - first_video->dts_usec);
		if (diff < closest_diff) {
			closest_diff = diff;
			idx = i;
		}
	}

	return video_idx < idx ? video_idx : idx;
}

struct encoder_packet *StreamDelayDock::find_first_packet_type(enum obs_encoder_type type, size_t audio_idx)
{
	const int idx = find_first_packet_type_idx(type, audio_idx);
	return (idx != -1) ? &interleaved_packets.array[idx] : nullptr;
}

struct encoder_packet *StreamDelayDock::find_last_packet_type(enum obs_encoder_type type, size_t audio_idx)
{
	int idx = find_last_packet_type_idx(type, audio_idx);
	return (idx != -1) ? &interleaved_packets.array[idx] : NULL;
}

bool StreamDelayDock::initialize_interleaved_packets()
{
	struct encoder_packet *video;
	struct encoder_packet *audio[MAX_AUDIO_MIXES];
	struct encoder_packet *last_audio[MAX_AUDIO_MIXES];
	size_t audio_mixes = num_audio_mixes();
	size_t start_idx;

	if (!get_audio_and_video_packets(&video, audio, audio_mixes))
		return false;

	for (size_t i = 0; i < audio_mixes; i++)
		last_audio[i] = find_last_packet_type(OBS_ENCODER_AUDIO, i);

	/* ensure that there is audio past the first video packet */
	for (size_t i = 0; i < audio_mixes; i++) {
		if (last_audio[i]->dts_usec < video->dts_usec) {
			received_audio = false;
			return false;
		}
	}

	/* clear out excess starting audio if it hasn't been already */
	start_idx = get_interleaved_start_idx();
	if (start_idx) {
		discard_to_idx(start_idx);
		if (!get_audio_and_video_packets(&video, audio, audio_mixes))
			return false;
	}

	/* get new offsets */
	video_offset = video->pts;
	for (size_t i = 0; i < audio_mixes; i++)
		audio_offsets[i] = audio[i]->dts;

	/* subtract offsets from highest TS offset variables */
	highest_audio_ts -= audio[0]->dts_usec;
	highest_video_ts -= video->dts_usec;

	/* apply new offsets to all existing packet DTS/PTS values */
	for (size_t i = 0; i < interleaved_packets.num; i++) {
		struct encoder_packet *packet = &interleaved_packets.array[i];
		apply_interleaved_packet_offset(packet);
	}

	return true;
}

bool StreamDelayDock::get_audio_and_video_packets(struct encoder_packet **video, struct encoder_packet **audio, size_t audio_mixes)
{
	*video = find_first_packet_type(OBS_ENCODER_VIDEO, 0);
	if (!*video)
		received_video = false;

	for (size_t i = 0; i < audio_mixes; i++) {
		audio[i] = find_first_packet_type(OBS_ENCODER_AUDIO, i);
		if (!audio[i]) {
			received_audio = false;
			return false;
		}
	}

	if (!*video) {
		return false;
	}

	return true;
}

void StreamDelayDock::resort_interleaved_packets()
{
	DARRAY(struct encoder_packet) old_array;

	old_array.da = interleaved_packets.da;
	memset(&interleaved_packets, 0, sizeof(interleaved_packets));

	for (size_t i = 0; i < old_array.num; i++)
		insert_interleaved_packet(&old_array.array[i]);

	da_free(old_array);
}

void StreamDelayDock::send_interleaved()
{
	while (interleaved_packets.num) {
		struct encoder_packet out = interleaved_packets.array[0];

		/* do not send an interleaved packet if there's no packet of the
		* opposing type of a higher timestamp in the interleave buffer.
		* this ensures that the timestamps are monotonic */
		if (!has_higher_opposing_ts(&out))
			return;

		if (skip_to >= 0 && out.type == OBS_ENCODER_VIDEO && out.keyframe) {
			size_t next_keyframe = 0;
			struct encoder_packet nk;
			for (size_t i = 0; i < interleaved_packets.num; i++) {
				if (interleaved_packets.array[i].type != OBS_ENCODER_VIDEO || !out.keyframe)
					continue;
				//todo check if it can find audio frames after
				bool audio_found = false;
				for (size_t j = i; j < interleaved_packets.num; j++) {
					if (interleaved_packets.array[j].type != OBS_ENCODER_VIDEO) {
						audio_found = true;
						break;
					}
				}
				if (!audio_found)
					break;

				auto duration = interleaved_packets.array[interleaved_packets.num - 1].dts_usec -
						interleaved_packets.array[i].dts_usec;
				if (duration < skip_to) {
					break;
				}
				next_keyframe = i;
				nk = interleaved_packets.array[i];
			}
			if (next_keyframe) {
				for (size_t i = 0; i < next_keyframe; i++) {
					obs_encoder_packet_release(&interleaved_packets.array[i]);
				}
				da_erase_range(interleaved_packets, 0, next_keyframe);
				out = interleaved_packets.array[0];
				delay_usec = skip_to;
				skip_to = -1;
			}
		}

		auto last_index = interleaved_packets.num - 1;
		auto last_type = interleaved_packets.array[last_index].type;
		while (last_index > 0 && interleaved_packets.array[last_index].type == last_type)
			last_index--;

		auto duration = interleaved_packets.array[last_index].dts_usec - interleaved_packets.array[0].dts_usec;
		if (duration < delay_usec && !delaying && delay_to < 0) {
			return;
		}
		if (delaying && duration > delay_usec) {
			stop_delay = true;
		}

		bool internal_start_replace = false;
		bool internal_start_delay = false;
		if ((start_replace || delay_to >= 0) && out.type == OBS_ENCODER_VIDEO && out.keyframe) {
			replace_index = 0;
			replace_diff_usec =
				replacement_packets.num ? out.dts_usec - replacement_packets.array[replace_index].dts_usec : 0;

			internal_start_replace = start_replace;
			if (delay_to >= 0) {
				delay_usec = delay_to;
				delay_to = -1;
				internal_start_delay = true;
			}
			start_replace = false;
		}

		if (start_record && out.type == OBS_ENCODER_VIDEO && out.keyframe) {
			pthread_mutex_lock(&replacement_mutex);
			for (size_t i = 0; i < replacement_packets.num; i++) {
				encoder_packet p = replacement_packets.array[i];
				obs_encoder_packet_release(&p);
			}
			replacement_packets.num = 0;
			pthread_mutex_unlock(&replacement_mutex);
			start_record = false;
			recording = true;
		}

		if (stop_record && out.type == OBS_ENCODER_VIDEO && out.keyframe) {
			recording = false;
			stop_record = false;
			updateReplacementDuration();
		}

		if (recording) {
			obs_encoder_packet_ref(&out, &out);
			pthread_mutex_lock(&replacement_mutex);
			da_push_back(replacement_packets, &out);
			pthread_mutex_unlock(&replacement_mutex);
		}

		if (stop_delay && out.type == OBS_ENCODER_VIDEO && out.keyframe) {
			delay_usec = (last_video_dts + 1 - out.dts) * out.timebase_num * MICROSECOND_DEN / out.timebase_den;
			delaying = false;
			stop_delay = false;
		}

		struct encoder_packet *packet_to_release = &out;
		if (delaying || internal_start_delay) {
			packet_to_release = nullptr;
		} else {
			da_erase(interleaved_packets, 0);
		}

		if (stop_replace && out.type == OBS_ENCODER_VIDEO && out.keyframe) {
			int64_t current_delay =
				(last_video_dts + 1 - out.dts) * out.timebase_num * MICROSECOND_DEN / out.timebase_den;
			if (current_delay > delay_usec) {
				skip_to = delay_usec;
				delay_usec = current_delay;
			}
			replace = false;
			stop_replace = false;
		}

		//this should never happen, can be removed
		if ((internal_start_replace || internal_start_delay) && out.type != OBS_ENCODER_VIDEO) {
			obs_encoder_packet_release(packet_to_release);
			continue;
		}

		if (internal_start_replace || internal_start_delay || replace || delaying) {
			pthread_mutex_lock(&replacement_mutex);
			if (replace_index >= replacement_packets.num) {
				/*if (out.type != OBS_ENCODER_VIDEO) {
					pthread_mutex_unlock(
						&replacement_mutex);
					obs_encoder_packet_release(
						packet_to_release);
					continue;
				}*/
				replace_index = 0;
				if (internal_start_replace || internal_start_delay) {
					replace_diff_usec =
						out.dts_usec + delay_usec - replacement_packets.array[replace_index].dts_usec;
				} else {
					auto last_index = replacement_packets.num - 1;
					while (last_index > 0 && replacement_packets.array[last_index].type != OBS_ENCODER_VIDEO)
						last_index--;

					auto diff =
						replacement_packets.array[last_index].dts - replacement_packets.array[0].dts + 1;

					auto diff_usec = diff * replacement_packets.array[0].timebase_num * MICROSECOND_DEN /
							 replacement_packets.array[0].timebase_den;
					replace_diff_usec += diff_usec;
				}
			}
			out = replacement_packets.array[replace_index];
			if (*(((long *)out.data) - 1) < 1) {
				*(((long *)out.data) - 1) = 1;
			}
			out.sys_dts_usec += replace_diff_usec;
			out.dts_usec += replace_diff_usec;

			int64_t dp = replace_diff_usec * out.timebase_den / out.timebase_num;
			auto dts_diff = dp / MICROSECOND_DEN;
			if (dp - (dts_diff * MICROSECOND_DEN) > MICROSECOND_DEN / 2) {
				dts_diff++;
			}
			out.dts += dts_diff;
			out.pts += dts_diff;
			obs_encoder_packet_ref(&out, &out);

			if (internal_start_replace && !replace)
				replace = true;
			if (internal_start_delay && !delaying)
				delaying = true;
			replace_index++;
			pthread_mutex_unlock(&replacement_mutex);
		} else if (delay_usec) {
			out.sys_dts_usec += delay_usec;
			out.dts_usec += delay_usec;
			int64_t dp = delay_usec * out.timebase_den / out.timebase_num;
			int64_t diff = dp / MICROSECOND_DEN;
			if (dp - (diff * MICROSECOND_DEN) > MICROSECOND_DEN / 2) {
				diff++;
			}
			out.dts += diff;
			out.pts += diff;
		}

		if (out.type == OBS_ENCODER_VIDEO) {

			if (last_video_dts && /* last_video_dts != out.dts &&*/
			    /*last_video_dts + 1 != out.dts &&*/
			    last_video_dts >= out.dts) {
				blog(LOG_INFO, "[Stream Delay] video dts wrong");
			}
			last_video_dts = out.dts;

			obs_encoder_packet_ref(&out, &out);
			pthread_mutex_lock(&output_video_mutex);
			da_push_back(output_video_packets, &out);
			pthread_mutex_unlock(&output_video_mutex);

			if (video_encode_callback) {
				obs_encoder_packet_ref(&out, &out);
				video_encode_callback(output, &out);
			}
		} else {

			if (last_audio_dts && last_audio_dts >= out.dts) {
				blog(LOG_INFO, "[Stream Delay] audio dts wrong");
			}
			last_audio_dts = out.dts;

			//size_t idx = d->get_audio_encoder_idx(packet->encoder);
			if (audio_encode_callback) {
				obs_encoder_packet_ref(&out, &out);
				audio_encode_callback(output, &out);
			}
		}
		obs_encoder_packet_release(packet_to_release);
		if (delaying || skip_to >= 0)
			return;
	}
}

bool StreamDelayDock::has_higher_opposing_ts(struct encoder_packet *packet)
{
	if (packet->type == OBS_ENCODER_VIDEO)
		return highest_audio_ts > packet->dts_usec;
	else
		return highest_video_ts > packet->dts_usec;
}

void StreamDelayDock::output_start(void *p, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto *d = static_cast<StreamDelayDock *>(p);
	d->connect_encoders();
}

void StreamDelayDock::output_starting(void *p, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto *d = static_cast<StreamDelayDock *>(p);
	d->connect_encoders();
}

void StreamDelayDock::output_activate(void *p, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto *d = static_cast<StreamDelayDock *>(p);
	d->connect_encoders();
}

void StreamDelayDock::output_stop(void *p, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto *d = static_cast<StreamDelayDock *>(p);
	d->disconnect_encoders();
}

void StreamDelayDock::output_deactivate(void *p, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	auto *d = static_cast<StreamDelayDock *>(p);
	d->disconnect_encoders();
}

void StreamDelayDock::frontend_event(enum obs_frontend_event event, void *p)
{
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING) {

		auto *d = static_cast<StreamDelayDock *>(p);
		d->enabled->setEnabled(false);
		if (!d->enabled->isChecked())
			return;
		auto o = obs_frontend_get_streaming_output();
		if (o != d->output) {
			if (d->output) {
				d->disconnect_encoders();
				auto sh = obs_output_get_signal_handler(d->output);
				signal_handler_disconnect(sh, "start", output_start, d);
				signal_handler_disconnect(sh, "starting", output_starting, d);
				signal_handler_disconnect(sh, "activate", output_activate, d);
				signal_handler_disconnect(sh, "stop", output_stop, d);
				signal_handler_disconnect(sh, "deactivate", output_deactivate, d);
			}
			d->output = o;
			if (d->output) {
				auto sh = obs_output_get_signal_handler(d->output);
				signal_handler_connect(sh, "start", output_start, d);
				signal_handler_connect(sh, "starting", output_starting, d);
				signal_handler_connect(sh, "activate", output_activate, d);
				signal_handler_connect(sh, "stop", output_stop, d);
				signal_handler_connect(sh, "deactivate", output_deactivate, d);
			}
		}
		obs_output_release(o);
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STARTED) {
		auto *d = static_cast<StreamDelayDock *>(p);
		d->enabled->setEnabled(false);
		if (!d->enabled->isChecked())
			return;
		auto o = obs_frontend_get_streaming_output();
		if (o != d->output) {
			d->disconnect_encoders();
			d->output = o;
			d->connect_encoders();
		}
		obs_output_release(o);
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPING) {
		//auto *d = static_cast<StreamDelayDock *>(p);
		//d->disconnect_encoders();
	} else if (event == OBS_FRONTEND_EVENT_STREAMING_STOPPED) {
		auto *d = static_cast<StreamDelayDock *>(p);
		d->disconnect_encoders();
		d->enabled->setEnabled(true);
	} else if (event == OBS_FRONTEND_EVENT_EXIT) {
		auto *d = static_cast<StreamDelayDock *>(p);
		if (d->output) {
			d->disconnect_encoders();
			auto sh = obs_output_get_signal_handler(d->output);
			signal_handler_disconnect(sh, "start", output_start, d);
			signal_handler_disconnect(sh, "starting", output_starting, d);
			signal_handler_disconnect(sh, "activate", output_activate, d);
			signal_handler_disconnect(sh, "stop", output_stop, d);
		}
		//d->stop_replace()
	}
}
