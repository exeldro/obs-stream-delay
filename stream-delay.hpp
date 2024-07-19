
#include <qcheckbox.h>
#include <QDockWidget>
#include <qlabel.h>
#include <QPushButton>
#include <qspinbox.h>
#include <QTimer>
extern "C" {
#include <libavcodec/avcodec.h>
}

#include "obs-frontend-api.h"
#include "output-delay.h"
#include "pthread.h"

enum delay_mode {
	DELAY_MODE_LIVE,
	DELAY_MODE_DELAYING,
	DELAY_MODE_DELAYED,
	DELAY_MODE_BACK_TO_LIVE,
};

class StreamDelayDock : public QWidget {
	Q_OBJECT

private:
	QCheckBox *enabled;
	QLabel *replacementDuration;
	QLabel *currentDelay;
	QSpinBox *desiredDelay;
	QLabel *replaceDuration;

	QPushButton *desiredButton;
	QPushButton *replaceButton;

	QTimer updateTimer;
	obs_output_t *output = nullptr;
	new_packet_func video_encode_callback = nullptr;
	new_packet_func audio_encode_callback = nullptr;

	pthread_mutex_t interleaved_mutex;
	DARRAY(struct encoder_packet) interleaved_packets;

	pthread_mutex_t replacement_mutex;
	DARRAY(struct encoder_packet) replacement_packets;

	pthread_mutex_t output_video_mutex;
	DARRAY(struct encoder_packet) output_video_packets;
	pthread_t decode_thread;
	static void *decode_output(void *data);
	bool stop_decode = false;
	AVCodecContext *codec_context = nullptr;
	pthread_mutex_t codec_context_mutex;

	DARRAY(obs_source_t *) sources;

	static void AVFrameToSourceFrame(struct obs_source_frame *dst, AVFrame *src, AVRational time_base);
	static enum video_range_type AVRangeToOBSRange(enum AVColorRange r);
	static enum video_format AVPixelFormatToOBSFormat(int f);
	static enum video_colorspace AVColorSpaceToOBSSpace(enum AVColorSpace s, enum AVColorTransferCharacteristic trc,
							    enum AVColorPrimaries color_primaries);
	static enum AVCodecID NameToAVCodecID(const char *str);

	bool received_audio = false;
	bool received_video = false;

	bool start_record = false;
	bool stop_record = false;
	bool recording = false;
	bool replace = false;
	bool delaying = false;
	bool start_replace = false;
	bool stop_replace = false;
	bool stop_delay = false;
	int64_t delay_to = -1;
	int64_t skip_to = -1;

	size_t replace_index = 0;
	int64_t replace_diff_usec = 0;

	int64_t delay_usec = 0;

	int64_t video_offset = 0;
	int64_t audio_offsets[MAX_AUDIO_MIXES];
	int64_t highest_audio_ts = 0;
	int64_t highest_video_ts = 0;

	int64_t last_audio_dts = 0;
	int64_t last_video_dts = 0;

	size_t get_audio_encoder_idx(obs_encoder_t *encoder);
	void insert_interleaved_packet(encoder_packet *packet);
	void discard_to_idx(size_t idx);
	void discard_unused_audio_packets(int64_t dts_usec);
	void encoder_packet_create_instance(encoder_packet *dst, const encoder_packet *src);
	void apply_interleaved_packet_offset(encoder_packet *out);
	int64_t packet_dts_usec(encoder_packet *packet);
	void check_received(encoder_packet *out);
	void set_higher_ts(encoder_packet *packet);
	bool prune_interleaved_packets();
	int prune_premature_packets();
	size_t num_audio_mixes();
	int find_first_packet_type_idx(obs_encoder_type type, size_t audio_idx);
	int find_last_packet_type_idx(obs_encoder_type type, size_t audio_idx);
	size_t get_interleaved_start_idx();
	encoder_packet *find_first_packet_type(obs_encoder_type type, size_t audio_idx);
	encoder_packet *find_last_packet_type(obs_encoder_type type, size_t audio_idx);
	bool initialize_interleaved_packets();
	bool get_audio_and_video_packets(encoder_packet **video, encoder_packet **audio, size_t audio_mixes);
	void resort_interleaved_packets();
	void send_interleaved();
	bool has_higher_opposing_ts(encoder_packet *packet);
	static void encoderCallback(void *p, encoder_packet *packet);
	static void frontend_event(obs_frontend_event event, void *private_data);
	static void output_start(void *p, calldata_t *calldata);
	static void output_starting(void *p, calldata_t *calldata);
	static void output_activate(void *p, calldata_t *calldata);
	static void output_stop(void *p, calldata_t *calldata);
	static void output_deactivate(void *p, calldata_t *calldata);

	void disconnect_encoders();
	void connect_encoders();

private slots:
	void updateReplacementDuration();
	void updateReplaceDuration();
	void updateCurrentDelay();

public:
	StreamDelayDock(QWidget *parent = nullptr);
	~StreamDelayDock();

	void add_encoded_output_source(obs_source_t *source);
	void remove_encoded_output_source(obs_source_t *source);
};
