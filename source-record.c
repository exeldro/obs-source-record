#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/config-file.h>
#include <util/platform.h>
#include <util/threading.h>
#include <util/dstr.h>
#include "version.h"
#include "obs-websocket-api.h"

#include <errno.h>
#include <stdio.h>

#ifndef _WIN32
#include <dlfcn.h>
#endif

#define OUTPUT_MODE_NONE 0
#define OUTPUT_MODE_ALWAYS 1
#define OUTPUT_MODE_STREAMING 2
#define OUTPUT_MODE_RECORDING 3
#define OUTPUT_MODE_STREAMING_OR_RECORDING 4
#define OUTPUT_MODE_VIRTUAL_CAMERA 5

#define AUDIO_TRACK_ALL -1
#define AUDIO_TRACK_CUSTOM -2

#define BACKGROUND_CHANNEL 0
#define SOURCE_CHANNEL 1

struct source_record_filter_context {
	obs_source_t *source;
	video_t *video_output;
	audio_t *audio_output;
	bool output_active;
	uint32_t width;
	uint32_t height;
	enum video_format view_format;
	enum video_format wanted_view_format;
	uint64_t last_frame_time_ns;
	obs_view_t *view;
	bool starting_file_output;
	bool starting_stream_output;
	bool starting_replay_output;
	bool stopping_file_output;
	bool stopping_stream_output;
	bool stopping_replay_output;
	bool restart;
	int start_failures;
	volatile long active_stop_threads;
	obs_output_t *fileOutput;
	obs_output_t *streamOutput;
	obs_output_t *replayOutput;
	obs_encoder_t *encoder;
	obs_encoder_t *audioEncoder[MAX_AUDIO_MIXES];
	obs_service_t *service;
	bool record;
	bool stream;
	bool replayBuffer;
	obs_hotkey_pair_id enableHotkey;
	obs_hotkey_pair_id pauseHotkeys;
	obs_hotkey_id splitHotkey;
	obs_hotkey_id chapterHotkey;
	obs_hotkey_id replayHotkey;
	int audio_track;
	uint32_t audio_track_mask;
	obs_weak_source_t *audio_source;
	bool closing;
	bool exiting;
	long long replay_buffer_duration;
	bool remove_after_record;
	long long record_max_seconds;
	int last_frontend_event;
	/* Path, container and filename pattern the running file output was
	 * started with, to restart it when they change while recording. */
	struct dstr rec_path;
	struct dstr rec_format;
	struct dstr rec_filename;
};

DARRAY(obs_source_t *) source_record_filters;

static void *vendor;

static void run_queued(obs_task_t task, void *param)
{
	if (obs_in_task_thread(OBS_TASK_UI) && obs_get_video()) {
		obs_queue_task(OBS_TASK_GRAPHICS, task, param, false);
	} else {
		obs_queue_task(OBS_TASK_UI, task, param, false);
	}
}

static const char *source_record_filter_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Source Record";
}

struct video_frame {
	uint8_t *data[MAX_AV_PLANES];
	uint32_t linesize[MAX_AV_PLANES];
};

static bool EncoderAvailable(const char *encoder)
{
	const char *val;
	int i = 0;

	while (obs_enum_encoder_types(i++, &val))
		if (strcmp(val, encoder) == 0)
			return true;

	return false;
}

static void calc_min_ts(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	uint64_t *min_ts = param;
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;
	if (!*min_ts || ts < *min_ts)
		*min_ts = ts;
}

static void mix_audio(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	if (!child || obs_source_audio_pending(child))
		return;
	const uint64_t ts = obs_source_get_audio_timestamp(child);
	if (!ts)
		return;
	struct obs_source_audio *mixed_audio = param;
	const size_t pos = (size_t)ns_to_audio_frames(mixed_audio->samples_per_sec, ts - mixed_audio->timestamp);

	if (pos > AUDIO_OUTPUT_FRAMES)
		return;

	const size_t count = AUDIO_OUTPUT_FRAMES - pos;

	struct obs_source_audio_mix child_audio;
	obs_source_get_audio_mix(child, &child_audio);
	for (size_t ch = 0; ch < (size_t)mixed_audio->speakers; ch++) {
		float *out = ((float *)mixed_audio->data[ch]) + pos;
		float *in = child_audio.output[0].data[ch];
		if (!in)
			continue;
		for (size_t i = 0; i < count; i++) {
			out[i] += in[i];
		}
	}
}

static bool audio_input_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts, uint32_t mixers,
				 struct audio_output_data *mixes)
{
	UNUSED_PARAMETER(end_ts_in);
	struct source_record_filter_context *filter = param;
	if (!mixers || filter->closing || obs_source_removed(filter->source)) {
		*out_ts = start_ts_in;
		return true;
	}

	obs_source_t *audio_source = NULL;
	bool release_audio = false;
	if (filter->audio_source) {
		audio_source = obs_weak_source_get_source(filter->audio_source);
		if (audio_source)
			release_audio = true;
	} else {
		audio_source = obs_filter_get_parent(filter->source);
	}
	if (!audio_source || obs_source_removed(audio_source)) {
		*out_ts = start_ts_in;
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}

	const uint32_t flags = obs_source_get_output_flags(audio_source);
	if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
		uint64_t min_ts = 0;
		obs_source_enum_active_tree(audio_source, calc_min_ts, &min_ts);
		if (min_ts) {
			struct obs_source_audio mixed_audio = {0};
			for (size_t i = 0; i < MAX_AUDIO_CHANNELS; i++) {
				mixed_audio.data[i] = (uint8_t *)mixes->data[i];
			}
			mixed_audio.timestamp = min_ts;
			mixed_audio.speakers = audio_output_get_channels(filter->audio_output);
			mixed_audio.samples_per_sec = audio_output_get_sample_rate(filter->audio_output);
			mixed_audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
			obs_source_enum_active_tree(audio_source, mix_audio, &mixed_audio);

			for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
				if ((mixers & (1 << mix_idx)) == 0)
					continue;
				// clamp audio
				for (size_t ch = 0; ch < (size_t)mixed_audio.speakers; ch++) {
					float *mix_data = mixes[mix_idx].data[ch];
					float *mix_end = &mix_data[AUDIO_OUTPUT_FRAMES];

					while (mix_data < mix_end) {
						float val = *mix_data;
						val = (val > 1.0f) ? 1.0f : val;
						val = (val < -1.0f) ? -1.0f : val;
						*(mix_data++) = val;
					}
				}
			}
			*out_ts = min_ts;
		} else {
			*out_ts = start_ts_in;
		}
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}
	if ((flags & OBS_SOURCE_AUDIO) == 0) {
		*out_ts = start_ts_in;
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}

	const uint64_t source_ts = obs_source_get_audio_timestamp(audio_source);
	if (!source_ts) {
		*out_ts = start_ts_in;
		if (release_audio)
			obs_source_release(audio_source);
		return true;
	}

	if (obs_source_audio_pending(audio_source)) {
		if (release_audio)
			obs_source_release(audio_source);
		return false;
	}

	struct obs_source_audio_mix audio;
	obs_source_get_audio_mix(audio_source, &audio);

	const size_t channels = audio_output_get_channels(filter->audio_output);
	for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
		if ((mixers & (1 << mix_idx)) == 0)
			continue;
		for (size_t ch = 0; ch < channels; ch++) {
			float *out = mixes[mix_idx].data[ch];
			float *in = audio.output[0].data[ch];
			if (!in)
				continue;
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
				out[i] += in[i];
				if (out[i] > 1.0f)
					out[i] = 1.0f;
				if (out[i] < -1.0f)
					out[i] = -1.0f;
			}
		}
	}

	*out_ts = source_ts;
	if (release_audio)
		obs_source_release(audio_source);

	return true;
}

static const char *GetFormatExt(const char *format)
{
	if (strcmp(format, "fragmented_mp4") == 0)
		return "mp4";
	if (strcmp(format, "hybrid_mp4") == 0)
		return "mp4";
	if (strcmp(format, "hybrid_mov") == 0)
		return "mov";
	if (strcmp(format, "fragmented_mov") == 0)
		return "mov";
	if (strcmp(format, "hls") == 0)
		return "m3u8";
	if (strcmp(format, "mpegts") == 0)
		return "ts";
	return format;
}

static void start_file_output_task(void *data)
{
	struct source_record_filter_context *context = data;
	if (obs_output_start(context->fileOutput)) {
		context->start_failures = 0;
		if (!context->output_active) {
			context->output_active = true;
			obs_source_inc_showing(obs_filter_get_parent(context->source));
		}
	} else {
		context->start_failures++;
	}
	context->starting_file_output = false;
}

static void start_stream_output_task(void *data)
{
	struct source_record_filter_context *context = data;
	if (obs_output_start(context->streamOutput)) {
		if (!context->output_active) {
			context->output_active = true;
			obs_source_inc_showing(obs_filter_get_parent(context->source));
		}
	}
	context->starting_stream_output = false;
}

static bool outputs_wanted(struct source_record_filter_context *context)
{
	if (!context->source || context->closing)
		return false;
	obs_source_t *parent = obs_filter_get_parent(context->source);
	return parent && !obs_source_removed(parent) && obs_source_enabled(context->source) &&
	       (context->replayBuffer || context->record || context->stream);
}

static void release_encoders(void *param)
{
	struct source_record_filter_context *context = param;
	if (outputs_wanted(context))
		return;
	if (context->encoder && !obs_encoder_active(context->encoder)) {
		obs_encoder_release(context->encoder);
		context->encoder = NULL;
	}
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!context->audioEncoder[i] || obs_encoder_active(context->audioEncoder[i]))
			continue;
		obs_encoder_release(context->audioEncoder[i]);
		context->audioEncoder[i] = NULL;
	}
}

enum output_type {
	OUTPUT_TYPE_FILE,
	OUTPUT_TYPE_STREAM,
	OUTPUT_TYPE_REPLAY,
};

struct stop_output {
	struct source_record_filter_context *context;
	obs_weak_source_t *weak_source;
	obs_output_t *output;
	enum output_type type;
};

static void source_record_replay_saved(void *data, calldata_t *cd)
{
	struct source_record_filter_context *context = data;
	if (!context || !context->source)
		return;

	const char *path = calldata_string(cd, "path");
	char *path_fallback = NULL;
	if ((!path || !strlen(path)) && context->replayOutput) {
		proc_handler_t *ph = obs_output_get_proc_handler(context->replayOutput);
		if (ph) {
			calldata_t proc_cd;
			calldata_init(&proc_cd);
			if (proc_handler_call(ph, "get_last_replay", &proc_cd)) {
				const char *last_path = calldata_string(&proc_cd, "path");
				if (last_path && strlen(last_path))
					path_fallback = bstrdup(last_path);
			}
			calldata_free(&proc_cd);
		}
	}

	const char *emit_path = "";
	if (path && strlen(path)) {
		emit_path = path;
	} else if (path_fallback) {
		emit_path = path_fallback;
	}

	obs_data_t *event_data = obs_data_create();
	obs_data_set_string(event_data, "path", emit_path);
	obs_data_set_string(event_data, "filter", obs_source_get_name(context->source));

	obs_source_t *parent = obs_filter_get_parent(context->source);
	if (parent) {
		obs_data_set_string(event_data, "source", obs_source_get_name(parent));
	}

	obs_websocket_vendor_emit_event(vendor, "replay_buffer_saved", event_data);
	obs_data_release(event_data);
	bfree(path_fallback);
}

static void stop_output_complete_task(void *data)
{
	struct stop_output *so = data;
	if (!so)
		return;

	obs_source_t *source = obs_weak_source_get_source(so->weak_source);
	if (source) {
		struct source_record_filter_context *context = obs_obj_get_data(source);
		if (context) {
			if (so->type == OUTPUT_TYPE_FILE)
				context->stopping_file_output = false;
			else if (so->type == OUTPUT_TYPE_STREAM)
				context->stopping_stream_output = false;
			else if (so->type == OUTPUT_TYPE_REPLAY)
				context->stopping_replay_output = false;

			if (!context->record && !context->stream && !context->replayBuffer &&
			    !context->stopping_file_output && !context->stopping_stream_output && !context->stopping_replay_output) {
				if (context->output_active) {
					context->output_active = false;
					obs_source_t *parent = obs_filter_get_parent(context->source);
					if (parent)
						obs_source_dec_showing(parent);
				}
			}

			if (context->encoder || context->audioEncoder[0]) {
				release_encoders(context);
			}
		}
		obs_source_release(source);
	}
	obs_weak_source_release(so->weak_source);
	bfree(so);
}

static void *stop_output_thread_func(void *data)
{
	struct stop_output *so = data;
	if (!so)
		return NULL;

	if (so->output) {
		if (obs_output_active(so->output)) {
			obs_output_stop(so->output);
			uint64_t start_time = os_gettime_ns();
			bool forced = false;
			while (obs_output_active(so->output)) {
				os_sleep_ms(10);
				if (!forced && (os_gettime_ns() - start_time > 5000000000ULL)) {
					obs_output_force_stop(so->output);
					forced = true;
				}
				if (forced && (os_gettime_ns() - start_time > 10000000000ULL)) {
					blog(LOG_WARNING, "Source Record: output timed out waiting to stop");
					break;
				}
			}
		}
		obs_output_release(so->output);
		so->output = NULL;
	}

	if (so->context) {
		os_atomic_dec_long(&so->context->active_stop_threads);
	}

	run_queued(stop_output_complete_task, so);
	return NULL;
}

static void stop_output_async(struct source_record_filter_context *context, obs_output_t **p_output, enum output_type type)
{
	if (!p_output || !*p_output)
		return;

	obs_output_t *output = *p_output;
	*p_output = NULL;

	if (type == OUTPUT_TYPE_FILE)
		context->stopping_file_output = true;
	else if (type == OUTPUT_TYPE_STREAM)
		context->stopping_stream_output = true;
	else if (type == OUTPUT_TYPE_REPLAY)
		context->stopping_replay_output = true;

	struct stop_output *so = bzalloc(sizeof(struct stop_output));
	so->context = context;
	so->weak_source = obs_source_get_weak_source(context->source);
	so->output = output;
	so->type = type;

	if (context) {
		os_atomic_inc_long(&context->active_stop_threads);
	}

	pthread_t th;
	if (pthread_create(&th, NULL, stop_output_thread_func, so) == 0) {
		pthread_detach(th);
	} else {
		stop_output_thread_func(so);
	}
}

static void start_replay_task(void *data)
{
	struct source_record_filter_context *context = data;
	if (obs_output_start(context->replayOutput)) {
		if (!context->output_active) {
			context->output_active = true;
			obs_source_inc_showing(obs_filter_get_parent(context->source));
		}
	}
	context->starting_replay_output = false;
}

static void ensure_directory(char *path)
{
#ifdef _WIN32
	char *backslash = strrchr(path, '\\');
	if (backslash)
		*backslash = '/';
#endif

	char *slash = strrchr(path, '/');
	if (slash) {
		*slash = 0;
		os_mkdirs(path);
		*slash = '/';
	}

#ifdef _WIN32
	if (backslash)
		*backslash = '\\';
#endif
}

static void remove_filter(void *data, calldata_t *calldata)
{
	UNUSED_PARAMETER(calldata);
	struct source_record_filter_context *filter = data;
	signal_handler_t *sh = obs_output_get_signal_handler(filter->fileOutput);
	signal_handler_disconnect(sh, "stop", remove_filter, filter);
	obs_source_t *source = obs_filter_get_parent(filter->source);
	if (!source && filter->view) {
		source = obs_view_get_source(filter->view, SOURCE_CHANNEL);
		obs_source_release(source);
	}
	obs_source_filter_remove(source, filter->source);
}

static void stop_output_sync(struct source_record_filter_context *context, obs_output_t *output)
{
	if (!output)
		return;
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	if (sh)
		signal_handler_disconnect(sh, "stop", remove_filter, context);
	if (obs_output_active(output)) {
		obs_output_force_stop(output);
		uint64_t start_time = os_gettime_ns();
		while (obs_output_active(output)) {
			os_sleep_ms(10);
			if (os_gettime_ns() - start_time > 5000000000ULL) {
				blog(LOG_WARNING, "Source Record: synchronous stop timed out");
				break;
			}
		}
	}
}

static const char *get_encoder_id(obs_data_t *settings)
{
	const char *enc_id = obs_data_get_string(settings, "encoder");
	if (strlen(enc_id) == 0 || strcmp(enc_id, "x264") == 0 || strcmp(enc_id, "x264_lowcpu") == 0) {
		enc_id = "obs_x264";
	} else if (strcmp(enc_id, "qsv") == 0) {
		enc_id = "obs_qsv11_v2";
	} else if (strcmp(enc_id, "qsv_av1") == 0) {
		enc_id = "obs_qsv11_av1";
	} else if (strcmp(enc_id, "amd") == 0) {
		enc_id = "h264_texture_amf";
	} else if (strcmp(enc_id, "amd_hevc") == 0) {
		enc_id = "h265_texture_amf";
	} else if (strcmp(enc_id, "amd_av1") == 0) {
		enc_id = "av1_texture_amf";
	} else if (strcmp(enc_id, "nvenc") == 0) {
		enc_id = EncoderAvailable("obs_nvenc_h264_tex") ? "obs_nvenc_h264_tex"
								: (EncoderAvailable("jim_nvenc") ? "jim_nvenc" : "ffmpeg_nvenc");
	} else if (strcmp(enc_id, "nvenc_hevc") == 0) {
		enc_id = EncoderAvailable("obs_nvenc_hevc_tex")
				 ? "obs_nvenc_hevc_tex"
				 : (EncoderAvailable("jim_hevc_nvenc") ? "jim_hevc_nvenc" : "ffmpeg_hevc_nvenc");
	} else if (strcmp(enc_id, "nvenc_av1") == 0) {
		enc_id = EncoderAvailable("obs_nvenc_av1_tex") ? "obs_nvenc_av1_tex" : "jim_av1_nvenc";
	} else if (strcmp(enc_id, "apple_h264") == 0) {
		enc_id = "com.apple.videotoolbox.videoencoder.ave.avc";
	} else if (strcmp(enc_id, "apple_hevc") == 0) {
		enc_id = "com.apple.videotoolbox.videoencoder.ave.hevc";
	}
	return enc_id;
}

/* --- Custom Output (FFmpeg) ----------------------------------------------
 *
 * OBS ships a generic libavcodec based output ("ffmpeg_output") which is what
 * the "Custom Output (FFmpeg)" output mode in the OBS settings uses. Unlike
 * the OBS encoders it can encode pixel formats that carry an alpha channel
 * (e.g. ProRes 4444 / yuva444p10le), so it is used for the custom output mode
 * of this filter.
 */

bool output_exists(const char *id);

/* Container name as understood by libavformat */
static const char *get_ffmpeg_format_name(const char *format)
{
	if (strcmp(format, "mkv") == 0)
		return "matroska";
	if (strcmp(format, "ts") == 0)
		return "mpegts";
	if (strcmp(format, "m3u8") == 0)
		return "hls";
	if (strcmp(format, "webm") == 0)
		return "webm";
	if (strcmp(format, "hybrid_mp4") == 0 || strcmp(format, "fragmented_mp4") == 0)
		return "mp4";
	if (strcmp(format, "hybrid_mov") == 0 || strcmp(format, "fragmented_mov") == 0)
		return "mov";
	/* flv, mp4, mov and webm are already valid libavformat names */
	return format;
}

/* True when the recording should go through the OBS "ffmpeg_output" */
static bool use_custom_ffmpeg_output(obs_data_t *settings)
{
	if (!obs_data_get_bool(settings, "custom_ffmpeg_output"))
		return false;
	if (!output_exists("ffmpeg_output")) {
		blog(LOG_WARNING, "Source Record: 'ffmpeg_output' is not available, using the default output");
		return false;
	}
	return true;
}

static size_t get_ffmpeg_audio_mixers(obs_data_t *settings)
{
	const int audio_track = obs_data_get_bool(settings, "different_audio")
					? (int)obs_data_get_int(settings, "audio_track")
					: 0;
	if (audio_track > 0 && audio_track <= MAX_AUDIO_MIXES)
		return (size_t)1 << (audio_track - 1);
	return 1;
}

/*
 * Pixel format of the video output the source is rendered into. Recording
 * with an alpha channel requires a format that has one, otherwise the
 * transparency is dropped before the frames reach the encoder.
 */
static enum video_format get_view_format_settings(obs_data_t *settings)
{
	if (obs_data_get_bool(settings, "custom_ffmpeg_output") && obs_data_get_bool(settings, "ff_alpha"))
		return VIDEO_FORMAT_BGRA;

	struct obs_video_info ovi = {0};
	if (obs_get_video_info(&ovi))
		return ovi.output_format;
	return VIDEO_FORMAT_NONE;
}

static enum video_format get_view_format(obs_source_t *source)
{
	if (!source)
		return VIDEO_FORMAT_NONE;
	obs_data_t *settings = obs_source_get_settings(source);
	const enum video_format format = get_view_format_settings(settings);
	obs_data_release(settings);
	return format;
}

static void (*obs_encoder_set_gpu_scale_type_func)(obs_encoder_t *encoder, enum obs_scale_type gpu_scale_type) = NULL;

static bool (*obs_encoder_set_frame_rate_divisor_func)(obs_encoder_t *, uint32_t) = NULL;

static void update_video_encoder(struct source_record_filter_context *filter, obs_data_t *settings)
{
	if (obs_encoder_video(filter->encoder) != filter->video_output) {
		if (obs_encoder_active(filter->encoder)) {
			obs_encoder_release(filter->encoder);
			const char *enc_id = get_encoder_id(settings);
			filter->encoder = obs_video_encoder_create(enc_id, obs_source_get_name(filter->source), settings, NULL);
		}
		obs_encoder_set_video(filter->encoder, filter->video_output);
	}
	uint32_t divisor = (uint32_t)obs_data_get_int(settings, "frame_rate_divisor");
	if (divisor > 1 && obs_encoder_set_frame_rate_divisor_func)
		obs_encoder_set_frame_rate_divisor_func(filter->encoder, divisor);
	bool scale = obs_data_get_bool(settings, "scale");
	if (scale) {
		uint32_t width = (uint32_t)obs_data_get_int(settings, "width");
		uint32_t height = (uint32_t)obs_data_get_int(settings, "height");
		if (width > 0 && height > 0) {
			obs_encoder_set_scaled_size(filter->encoder, width, height);
		} else {
			obs_encoder_set_scaled_size(filter->encoder, 0, 0);
		}
		if (obs_encoder_set_gpu_scale_type_func)
			obs_encoder_set_gpu_scale_type_func(filter->encoder,
							    (enum obs_scale_type)obs_data_get_int(settings, "scale_type"));
	} else {
		obs_encoder_set_scaled_size(filter->encoder, 0, 0);
	}
	if (filter->fileOutput && !use_custom_ffmpeg_output(settings) && obs_output_get_video_encoder(filter->fileOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
	if (filter->streamOutput && obs_output_get_video_encoder(filter->streamOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
	if (filter->replayOutput && obs_output_get_video_encoder(filter->replayOutput) != filter->encoder)
		obs_output_set_video_encoder(filter->replayOutput, filter->encoder);
}

static const char *get_ffmpeg_video_encoder(const char *name)
{
	if (!name || !*name)
		return "prores";
	if (astrcmpi(name, "qtrle") == 0 || strstr(name, "QuickTime") != NULL || strstr(name, "RLE") != NULL)
		return "qtrle";
	if (astrcmpi(name, "prores_ks") == 0 || strstr(name, "prores_ks") != NULL)
		return "prores_ks";
	if (astrcmpi(name, "prores") == 0 || strstr(name, "ProRes") != NULL)
		return "prores";
	if (astrcmpi(name, "ffv1") == 0 || strstr(name, "FFV1") != NULL)
		return "ffv1";
	if (astrcmpi(name, "libvpx-vp9") == 0 || strstr(name, "VP9") != NULL || strstr(name, "vp9") != NULL)
		return "libvpx-vp9";
	if (astrcmpi(name, "x264") == 0 || astrcmpi(name, "libx264") == 0)
		return "libx264";
	return name;
}

static void strip_ffmpeg_option(struct dstr *str, const char *opt_name)
{
	if (!str || !str->array || !opt_name || !*opt_name)
		return;

	const size_t opt_len = strlen(opt_name);
	char *pos = strstr(str->array, opt_name);
	while (pos) {
		if (pos == str->array || *(pos - 1) == ' ' || *(pos - 1) == '\t') {
			char *end = pos + opt_len;
			while (*end == ' ' || *end == '\t')
				end++;
			if (*end == '=') {
				end++;
				while (*end == ' ' || *end == '\t')
					end++;
				while (*end != '\0' && *end != ' ' && *end != '\t')
					end++;
				while (*end == ' ' || *end == '\t')
					end++;
				memmove(pos, end, strlen(end) + 1);
				str->len = strlen(str->array);
				pos = strstr(str->array, opt_name);
				continue;
			}
		}
		pos = strstr(pos + 1, opt_name);
	}
	while (str->len > 0 && (str->array[str->len - 1] == ' ' || str->array[str->len - 1] == '\t')) {
		str->array[--str->len] = '\0';
	}
}

/* Two filters with the same folder and filename format generate the same path
 * and would write over each other (#181, #105). Claim the name with an
 * exclusive create and add the same " (2)" suffix OBS uses when it is taken. */
static void claim_unique_path(struct dstr *path)
{
	const char *slash = strrchr(path->array, '/');
	const char *backslash = strrchr(path->array, '\\');
	if (backslash > slash)
		slash = backslash;
	const char *dot = strrchr(path->array, '.');
	if (dot && slash && dot < slash)
		dot = NULL;
	const size_t ext_start = dot ? (size_t)(dot - path->array) : path->len;

	struct dstr ext;
	dstr_init_copy(&ext, path->array + ext_start);
	struct dstr candidate;
	dstr_init_copy_dstr(&candidate, path);

	for (int n = 2; n < 1000; n++) {
		FILE *f = os_fopen(candidate.array, "wbx");
		if (f) {
			fclose(f);
			dstr_copy_dstr(path, &candidate);
			break;
		}
		if (errno != EEXIST)
			break;
		dstr_resize(&candidate, ext_start);
		dstr_catf(&candidate, " (%d)", n);
		dstr_cat_dstr(&candidate, &ext);
	}
	dstr_free(&ext);
	dstr_free(&candidate);
}

static void remember_record_settings(struct source_record_filter_context *filter, obs_data_t *settings)
{
	dstr_copy(&filter->rec_path, obs_data_get_string(settings, "path"));
	dstr_copy(&filter->rec_format, obs_data_get_string(settings, "rec_format"));
	dstr_copy(&filter->rec_filename, obs_data_get_string(settings, "filename_formatting"));
}

/* True when the path, container or filename pattern differs from what the
 * running file output was started with. */
static bool record_settings_changed(struct source_record_filter_context *filter, obs_data_t *settings)
{
	return dstr_cmp(&filter->rec_path, obs_data_get_string(settings, "path")) != 0 ||
	       dstr_cmp(&filter->rec_format, obs_data_get_string(settings, "rec_format")) != 0 ||
	       dstr_cmp(&filter->rec_filename, obs_data_get_string(settings, "filename_formatting")) != 0;
}

static void start_file_output(struct source_record_filter_context *filter, obs_data_t *settings)
{
	if (filter->stopping_file_output)
		return;

	obs_data_t *s = obs_data_create();
	const char *format = obs_data_get_string(settings, "rec_format");
	const char *directory = obs_data_get_string(settings, "path");
	remember_record_settings(filter, settings);
	char *filename =
		os_generate_formatted_filename(GetFormatExt(format), true, obs_data_get_string(settings, "filename_formatting"));
	struct dstr path;
	dstr_init(&path);
	dstr_printf(&path, "%s/%s", directory, filename);
	bfree(filename);
	ensure_directory(path.array);
	claim_unique_path(&path);
	obs_data_set_string(s, "path", path.array);
	obs_data_set_string(s, "directory", directory);
	obs_data_set_string(s, "format", obs_data_get_string(settings, "filename_formatting"));
	obs_data_set_string(s, "extension", GetFormatExt(format));
	/* Files after a split are named by the output itself, which replaces
	 * spaces by underscores unless allowed (#164). */
	obs_data_set_bool(s, "allow_spaces", true);
	/* The outputs ignore a manual split (hotkey, button, websocket) unless
	 * split_file is on, so it always is; the checkbox only decides whether
	 * the automatic size and time limits apply. */
	const bool auto_split = obs_data_get_bool(settings, "split_file");
	obs_data_set_bool(s, "split_file", true);
	obs_data_set_int(s, "max_size_mb", auto_split ? obs_data_get_int(settings, "max_size_mb") : 0);
	obs_data_set_int(s, "max_time_sec", auto_split ? obs_data_get_int(settings, "max_time_sec") : 0);
	/* What OBS itself passes for its fragmented formats. Without it the muxer
	 * writes a plain MP4/MOV that is lost if OBS dies while recording. */
	if (strcmp(format, "fragmented_mp4") == 0 || strcmp(format, "fragmented_mov") == 0)
		obs_data_set_string(s, "muxer_settings", "movflags=frag_keyframe+empty_moov+delay_moov");

	const char *output_id = "ffmpeg_muxer";
	if (strcmp(format, "hybrid_mp4") == 0) {
		output_id = "mp4_output";
	} else if (strcmp(format, "hybrid_mov") == 0) {
		output_id = "mov_output";
	}
	const bool custom = use_custom_ffmpeg_output(settings);
	if (custom) {
		output_id = "ffmpeg_output";
		/* The ffmpeg output encodes and muxes on its own, so it takes the
		 * encoder and container settings directly instead of OBS encoders. */
		obs_data_set_string(s, "url", path.array);
		obs_data_set_string(s, "format_name", get_ffmpeg_format_name(format));
		const char *video_encoder = get_ffmpeg_video_encoder(obs_data_get_string(settings, "ff_video_encoder"));
		obs_data_set_string(s, "video_encoder", video_encoder);

		const char *video_settings = obs_data_get_string(settings, "ff_video_settings");
		struct dstr vsettings;
		dstr_init(&vsettings);
		if (video_settings && *video_settings)
			dstr_copy(&vsettings, video_settings);

		/* Strip pix_fmt if present because ffmpeg_output sets context->pix_fmt directly
		 * and av_opt_set will fail with a warning if passed pix_fmt */
		strip_ffmpeg_option(&vsettings, "pix_fmt");

		if (strcmp(video_encoder, "prores") == 0 || strcmp(video_encoder, "prores_ks") == 0) {
			if (!strstr(vsettings.array ? vsettings.array : "", "profile=")) {
				if (vsettings.len)
					dstr_cat(&vsettings, " ");
				dstr_cat(&vsettings, "profile=4");
			}
		}
		if (!strstr(vsettings.array ? vsettings.array : "", "threads")) {
			uint32_t cores = os_get_logical_cores();
			uint32_t default_threads = cores >= 8 ? 4 : (cores >= 4 ? 2 : 1);
			if (vsettings.len)
				dstr_cat(&vsettings, " ");
			dstr_catf(&vsettings, "threads=%u", default_threads);
		}
		obs_data_set_string(s, "video_settings", vsettings.array);
		dstr_free(&vsettings);

		obs_data_set_int(s, "video_bitrate", obs_data_get_int(settings, "ff_video_bitrate"));
		obs_data_set_int(s, "gop_size", 120);
		obs_data_set_string(s, "audio_encoder", obs_data_get_string(settings, "ff_audio_encoder"));
		obs_data_set_string(s, "audio_settings", obs_data_get_string(settings, "ff_audio_settings"));
		obs_data_set_int(s, "audio_bitrate", obs_data_get_int(settings, "ff_audio_bitrate"));
		const char *muxer_settings = obs_data_get_string(settings, "ff_muxer_settings");
		if (strcmp(format, "fragmented_mp4") == 0 || strcmp(format, "fragmented_mov") == 0) {
			/* crash resilient fragmented files */
			struct dstr mux;
			dstr_init(&mux);
			dstr_copy(&mux, "movflags=+frag_keyframe+empty_moov+default_base_moof");
			if (muxer_settings && strlen(muxer_settings)) {
				dstr_cat(&mux, " ");
				dstr_cat(&mux, muxer_settings);
			}
			obs_data_set_string(s, "muxer_settings", mux.array);
			dstr_free(&mux);
		} else if (muxer_settings && strlen(muxer_settings)) {
			obs_data_set_string(s, "muxer_settings", muxer_settings);
		}
		if (obs_data_get_bool(settings, "scale")) {
			uint32_t scale_width = (uint32_t)obs_data_get_int(settings, "width");
			uint32_t scale_height = (uint32_t)obs_data_get_int(settings, "height");
			if (scale_width > 0 && scale_height > 0) {
				obs_data_set_int(s, "scale_width", (int)scale_width);
				obs_data_set_int(s, "scale_height", (int)scale_height);
			}
		}
	}
	dstr_free(&path);
	if (filter->fileOutput && strcmp(obs_output_get_id(filter->fileOutput), output_id) != 0) {
		if (obs_output_active(filter->fileOutput)) {
			stop_output_async(filter, &filter->fileOutput, OUTPUT_TYPE_FILE);
			obs_data_release(s);
			return;
		} else {
			obs_output_release(filter->fileOutput);
			filter->fileOutput = NULL;
		}
	}
	if (!filter->fileOutput) {
		filter->fileOutput = obs_output_create(output_id, obs_source_get_name(filter->source), s, NULL);
		if (filter->fileOutput && filter->remove_after_record) {
			signal_handler_t *sh = obs_output_get_signal_handler(filter->fileOutput);
			signal_handler_connect(sh, "stop", remove_filter, filter);
		}
	} else {
		obs_output_update(filter->fileOutput, s);
	}
	obs_data_release(s);
	if (custom && filter->fileOutput) {
		/* The ffmpeg output pulls the raw frames and audio itself. */
		obs_output_set_media(filter->fileOutput, filter->video_output, filter->audio_output);
		obs_output_set_mixers(filter->fileOutput,
				      filter->audio_output ? get_ffmpeg_audio_mixers(settings) : (size_t)0);
	} else {
		if (filter->encoder) {
			update_video_encoder(filter, settings);
			obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
		}
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if (!filter->audioEncoder[i])
				continue;

			obs_encoder_set_audio(filter->audioEncoder[i], filter->audio_output);
			obs_output_set_audio_encoder(filter->fileOutput, filter->audioEncoder[i], i);
		}
	}

	filter->starting_file_output = true;

	run_queued(start_file_output_task, filter);
}

#define FTL_PROTOCOL "ftl"
#define RTMP_PROTOCOL "rtmp"

static void start_stream_output(struct source_record_filter_context *filter, obs_data_t *settings)
{
	if (filter->stopping_stream_output)
		return;

	if (!filter->service) {
		const char *server = obs_data_get_string(settings, "server");
		bool whip = strstr(server, "whip") != NULL;
		if (whip)
			obs_data_set_string(settings, "bearer_token", obs_data_get_string(settings, "key"));

		filter->service = obs_service_create(whip ? "whip_custom" : "rtmp_custom", obs_source_get_name(filter->source),
						     settings, NULL);
	} else {
		obs_service_update(filter->service, settings);
	}
	obs_service_apply_encoder_settings(filter->service, settings, NULL);

	const char *type = NULL;
#ifdef _WIN32
	void *handle = os_dlopen("obs");
#else
	void *handle = dlopen(NULL, RTLD_LAZY);
#endif
	if (handle) {
		const char *(*type_func)(obs_service_t *) =
			(const char *(*)(obs_service_t *))os_dlsym(handle, "obs_service_get_output_type");
		if (!type_func)
			type_func = (const char *(*)(obs_service_t *))os_dlsym(handle, "obs_service_get_preferred_output_type");
		if (type_func) {
			type = type_func(filter->service);
		}
		if (!type) {
			const char *url = NULL;
			const char *(*url_func)(obs_service_t *) =
				(const char *(*)(obs_service_t *))os_dlsym(handle, "obs_service_get_url");
			if (url_func) {
				url = url_func(filter->service);
			} else {
				const char *(*info_func)(obs_service_t *, uint32_t) =
					(const char *(*)(obs_service_t *, uint32_t))os_dlsym(handle,
											     "obs_service_get_connect_info");
				if (info_func)
					url = info_func(filter->service, 0); // OBS_SERVICE_CONNECT_INFO_SERVER_URL
			}
			type = "rtmp_output";
			if (url != NULL && strncmp(url, "ftl", 3) == 0) {
				type = "ftl_output";
			} else if (url != NULL && strncmp(url, "rtmp", 4) != 0) {
				type = "ffmpeg_mpegts_muxer";
			}
		}
		os_dlclose(handle);
	} else {
		type = "rtmp_output";
	}

	if (!filter->streamOutput) {
		filter->streamOutput = obs_output_create(type, obs_source_get_name(filter->source), settings, NULL);
	} else {
		obs_output_update(filter->streamOutput, settings);
	}
	obs_output_set_service(filter->streamOutput, filter->service);

	if (filter->encoder) {
		update_video_encoder(filter, settings);
		obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
	}

	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!filter->audioEncoder[i])
			continue;
		obs_encoder_set_audio(filter->audioEncoder[i], filter->audio_output);
		obs_output_set_audio_encoder(filter->streamOutput, filter->audioEncoder[i], i);
	}

	filter->starting_stream_output = true;

	run_queued(start_stream_output_task, filter);
}

static void start_replay_output(struct source_record_filter_context *filter, obs_data_t *settings)
{
	if (filter->stopping_replay_output)
		return;

	obs_data_t *s = obs_data_create();

	obs_data_set_string(s, "directory", obs_data_get_string(settings, "path"));
	obs_data_set_string(s, "format", obs_data_get_string(settings, "replay_filename_formatting"));
	obs_data_set_string(s, "extension", GetFormatExt(obs_data_get_string(settings, "rec_format")));
	obs_data_set_bool(s, "allow_spaces", true);
	filter->replay_buffer_duration = obs_data_get_int(settings, "replay_duration");
	obs_data_set_int(s, "max_time_sec", filter->replay_buffer_duration);
	obs_data_set_int(s, "max_size_mb", obs_data_get_int(settings, "replay_max_size_mb"));
	if (!filter->replayOutput) {
		obs_data_t *hotkeys = obs_data_get_obj(settings, "replay_hotkeys");
		struct dstr name;
		obs_source_t *parent = obs_filter_get_parent(filter->source);
		if (parent) {
			dstr_init_copy(&name, obs_source_get_name(parent));
			dstr_cat(&name, " - ");
			dstr_cat(&name, obs_source_get_name(filter->source));
		} else {
			dstr_init_copy(&name, obs_source_get_name(filter->source));
		}

		filter->replayOutput = obs_output_create("replay_buffer", name.array, s, hotkeys);
		signal_handler_t *sh = obs_output_get_signal_handler(filter->replayOutput);
		if (sh)
			signal_handler_connect(sh, "saved", source_record_replay_saved, filter);
		if (filter->remove_after_record) {
			sh = obs_output_get_signal_handler(filter->replayOutput);
			signal_handler_connect(sh, "stop", remove_filter, filter);
		}
		dstr_free(&name);
		obs_data_release(hotkeys);
	} else {
		obs_output_update(filter->replayOutput, s);
	}
	obs_data_release(s);
	if (filter->encoder) {
		update_video_encoder(filter, settings);
	}
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		if (!filter->audioEncoder[i])
			continue;
		obs_encoder_set_audio(filter->audioEncoder[i], filter->audio_output);

		if (obs_output_get_audio_encoder(filter->replayOutput, i) != filter->audioEncoder[i])
			obs_output_set_audio_encoder(filter->replayOutput, filter->audioEncoder[i], i);
	}

	filter->starting_replay_output = true;

	run_queued(start_replay_task, filter);
}

static void copy_defaults(obs_data_t *from, obs_data_t *to)
{
	for (obs_data_item_t *default_item = obs_data_first(from); default_item != NULL; obs_data_item_next(&default_item)) {
		if (!obs_data_item_has_default_value(default_item))
			continue;
		enum obs_data_type item_type = obs_data_item_gettype(default_item);
		const char *name = obs_data_item_get_name(default_item);
		if (item_type == OBS_DATA_STRING) {
			obs_data_set_default_string(to, name, obs_data_item_get_default_string(default_item));
		} else if (item_type == OBS_DATA_NUMBER) {
			enum obs_data_number_type num_type = obs_data_item_numtype(default_item);
			if (num_type == OBS_DATA_NUM_INT) {
				obs_data_set_default_int(to, name, obs_data_item_get_default_int(default_item));
			} else if (num_type == OBS_DATA_NUM_DOUBLE) {
				obs_data_set_default_double(to, name, obs_data_item_get_default_double(default_item));
			}
		} else if (item_type == OBS_DATA_BOOLEAN) {
			obs_data_set_default_bool(to, name, obs_data_item_get_default_bool(default_item));
		}
	}
}

static void set_encoder_defaults(obs_data_t *settings)
{
	obs_data_t *enc_defaults = obs_encoder_defaults(get_encoder_id(settings));
	if (enc_defaults) {
		copy_defaults(enc_defaults, settings);
		obs_data_release(enc_defaults);
	}
	const char *enc_id = obs_data_get_string(settings, "audio_encoder");
	if (!enc_id || !strlen(enc_id))
		enc_id = "ffmpeg_aac";
	enc_defaults = obs_encoder_defaults(enc_id);
	if (enc_defaults) {
		if (obs_data_has_default_value(enc_defaults, "bitrate")) {
			obs_data_set_default_int(settings, "audio_bitrate", obs_data_get_default_int(enc_defaults, "bitrate"));
			obs_data_unset_default_value(enc_defaults, "bitrate");
		}
		copy_defaults(enc_defaults, settings);
		obs_data_release(enc_defaults);
	}
}

static uint32_t get_audio_track_mask(obs_data_t *settings)
{
	uint32_t mask = 0;
	char name[16];
	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		snprintf(name, sizeof(name), "audio_track_%d", i + 1);
		if (obs_data_get_bool(settings, name))
			mask |= (1u << i);
	}
	return mask;
}

static void update_encoder(struct source_record_filter_context *filter, obs_data_t *settings, bool need_obs_encoder)
{
	const bool custom = use_custom_ffmpeg_output(settings);
	const char *enc_id = get_encoder_id(settings);
	const bool need_new_encoder = !filter->encoder || strcmp(obs_encoder_get_id(filter->encoder), enc_id) != 0;
	if (need_obs_encoder && custom && obs_data_get_bool(settings, "ff_alpha")) {
		blog(LOG_WARNING,
		     "Source Record: Alpha recording (BGRA) is not supported with standard OBS streaming or replay buffer encoders. Streaming or replay buffer may fail.");
	}
	if (!need_obs_encoder) {
		/* No active output needs a standard OBS video encoder. Drop one
		 * that is left over. */
		if (filter->encoder && !obs_encoder_active(filter->encoder)) {
			obs_encoder_release(filter->encoder);
			filter->encoder = NULL;
		}
	} else if (need_new_encoder && filter->encoder && obs_encoder_active(filter->encoder)) {
		/* Fix: an active encoder is never released here. Releasing an
		 * in-use encoder caused crashes; it is swapped once idle. */
	} else if (need_new_encoder) {
		obs_encoder_release(filter->encoder);
		filter->encoder = NULL;
		set_encoder_defaults(settings);
		filter->encoder = obs_video_encoder_create(enc_id, obs_source_get_name(filter->source), settings, NULL);

		obs_encoder_set_video(filter->encoder, filter->video_output);
		uint32_t divisor = (uint32_t)obs_data_get_int(settings, "frame_rate_divisor");
		if (divisor > 1 && obs_encoder_set_frame_rate_divisor_func)
			obs_encoder_set_frame_rate_divisor_func(filter->encoder, divisor);
		bool scale = obs_data_get_bool(settings, "scale");
		if (scale) {
			uint32_t width = (uint32_t)obs_data_get_int(settings, "width");
			uint32_t height = (uint32_t)obs_data_get_int(settings, "height");
			if (width > 0 && height > 0) {
				obs_encoder_set_scaled_size(filter->encoder, width, height);
			} else {
				obs_encoder_set_scaled_size(filter->encoder, 0, 0);
			}
			if (obs_encoder_set_gpu_scale_type_func)
				obs_encoder_set_gpu_scale_type_func(filter->encoder,
								    (enum obs_scale_type)obs_data_get_int(settings, "scale_type"));
		} else {
			obs_encoder_set_scaled_size(filter->encoder, 0, 0);
		}
		if (filter->fileOutput && !custom && obs_output_get_video_encoder(filter->fileOutput) != filter->encoder)
			obs_output_set_video_encoder(filter->fileOutput, filter->encoder);
		if (filter->streamOutput && obs_output_get_video_encoder(filter->streamOutput) != filter->encoder)
			obs_output_set_video_encoder(filter->streamOutput, filter->encoder);
		if (filter->replayOutput && obs_output_get_video_encoder(filter->replayOutput) != filter->encoder)
			obs_output_set_video_encoder(filter->replayOutput, filter->encoder);
	} else if (!obs_encoder_active(filter->encoder)) {
		/* Fix: re-point the encoder at the current video_output in case
		 * the view was recreated after a parent size change. */
		obs_encoder_set_video(filter->encoder, filter->video_output);
		obs_encoder_update(filter->encoder, settings);
	}
	const int audio_track = obs_data_get_bool(settings, "different_audio") ? (int)obs_data_get_int(settings, "audio_track") : 0;
	const uint32_t audio_track_mask = audio_track == AUDIO_TRACK_CUSTOM ? get_audio_track_mask(settings) : 0;
	const bool uses_master_audio = audio_track > 0 || audio_track == AUDIO_TRACK_ALL || audio_track == AUDIO_TRACK_CUSTOM;
	const bool used_master_audio =
		filter->audio_track > 0 || filter->audio_track == AUDIO_TRACK_ALL || filter->audio_track == AUDIO_TRACK_CUSTOM;
	if (filter->closing) {
		if (!used_master_audio && filter->audio_output) {
			audio_output_close(filter->audio_output);
			filter->audio_output = NULL;
		}
	} else if (!filter->audio_output) {
		if (uses_master_audio) {
			filter->audio_output = obs_get_audio();
		} else {
			struct audio_output_info oi = {0};
			oi.name = obs_source_get_name(filter->source);
			oi.speakers = SPEAKERS_STEREO;
			oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
			oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
			oi.input_param = filter;
			oi.input_callback = audio_input_callback;
			audio_output_open(&filter->audio_output, &oi);
		}
	} else if (uses_master_audio && !used_master_audio) {
		audio_output_close(filter->audio_output);
		filter->audio_output = obs_get_audio();
	} else if (!uses_master_audio && used_master_audio) {
		filter->audio_output = NULL;
		struct audio_output_info oi = {0};
		oi.name = obs_source_get_name(filter->source);
		oi.speakers = SPEAKERS_STEREO;
		oi.samples_per_sec = audio_output_get_sample_rate(obs_get_audio());
		oi.format = AUDIO_FORMAT_FLOAT_PLANAR;
		oi.input_param = filter;
		oi.input_callback = audio_input_callback;
		audio_output_open(&filter->audio_output, &oi);
	}

	if (!need_obs_encoder) {
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if (!filter->audioEncoder[i] || obs_encoder_active(filter->audioEncoder[i]))
				continue;
			obs_encoder_release(filter->audioEncoder[i]);
			filter->audioEncoder[i] = NULL;
		}
	} else if (!filter->audioEncoder[0] || filter->audio_track != audio_track ||
	    (audio_track == AUDIO_TRACK_CUSTOM && filter->audio_track_mask != audio_track_mask)) {
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if (!filter->audioEncoder[i])
				continue;
			obs_encoder_release(filter->audioEncoder[i]);
			filter->audioEncoder[i] = NULL;
		}
		enc_id = obs_data_get_string(settings, "audio_encoder");
		if (!enc_id || !strlen(enc_id))
			enc_id = "ffmpeg_aac";

		obs_data_t *audio_settings = obs_data_create();
		if (obs_data_has_user_value(settings, "audio_bitrate") || obs_data_has_default_value(settings, "audio_bitrate")) {
			obs_data_set_int(audio_settings, "bitrate", obs_data_get_int(settings, "audio_bitrate"));
		}
		if (audio_track > 0) {
			filter->audioEncoder[0] = obs_audio_encoder_create(enc_id, obs_source_get_name(filter->source),
									   audio_settings, audio_track - 1, NULL);
		} else if (audio_track == AUDIO_TRACK_ALL) {
			struct dstr name;
			dstr_init(&name);
			config_t *config = obs_frontend_get_profile_config();
			for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
				char trackNameSetting[16];
				snprintf(trackNameSetting, sizeof(trackNameSetting), "Track%dName", i);
				const char *trackName = config_get_string(config, "AdvOut", trackNameSetting);
				if (trackName && strlen(trackName) > 0) {
					dstr_printf(&name, "%s", trackName);
				} else {
					dstr_printf(&name, "%s %d", obs_module_text("Track"), i + 1);
				}
				filter->audioEncoder[i] = obs_audio_encoder_create(enc_id, name.array, audio_settings, i, NULL);
			}
			dstr_free(&name);
		} else if (audio_track == AUDIO_TRACK_CUSTOM) {
			config_t *config = obs_frontend_get_profile_config();
			struct dstr name;
			dstr_init(&name);
			int out_idx = 0;
			for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
				if ((audio_track_mask & (1u << i)) == 0)
					continue;
				char trackNameSetting[16];
				snprintf(trackNameSetting, sizeof(trackNameSetting), "Track%dName", i);
				const char *trackName = config_get_string(config, "AdvOut", trackNameSetting);
				if (trackName && strlen(trackName) > 0) {
					dstr_printf(&name, "%s", trackName);
				} else {
					dstr_printf(&name, "%s %d", obs_module_text("Track"), i + 1);
				}
				filter->audioEncoder[out_idx] = obs_audio_encoder_create(enc_id, name.array, audio_settings, i, NULL);
				out_idx++;
			}
			dstr_free(&name);
		} else {
			filter->audioEncoder[0] =
				obs_audio_encoder_create(enc_id, obs_source_get_name(filter->source), audio_settings, 0, NULL);
		}
		obs_data_release(audio_settings);
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if (!filter->audioEncoder[i])
				continue;
			if (filter->audio_output)
				obs_encoder_set_audio(filter->audioEncoder[i], filter->audio_output);

			if (filter->fileOutput && !custom)
				obs_output_set_audio_encoder(filter->fileOutput, filter->audioEncoder[i], i);
			if (filter->replayOutput)
				obs_output_set_audio_encoder(filter->replayOutput, filter->audioEncoder[i], i);
		}
	}
	filter->audio_track = audio_track;
	filter->audio_track_mask = audio_track_mask;
}

static void source_record_filter_update(void *data, obs_data_t *settings)
{
	struct source_record_filter_context *filter = data;
	obs_source_t *parent = obs_filter_get_parent(filter->source);
	if (obs_obj_is_private(parent)) {
		filter->closing = true;
		return;
	}
	/* A settings change may change the pixel format the source has to be
	 * rendered in, and allows another attempt at starting the output. */
	filter->wanted_view_format = get_view_format_settings(settings);
	filter->start_failures = 0;
	if (obs_data_get_bool(settings, "scale")) {
		const char *res = obs_data_get_string(settings, "resolution");
		uint32_t width, height;
		if (sscanf(res, "%dx%d", &width, &height) == 2 && width > 0 && height > 0) {
			obs_data_set_int(settings, "width", width);
			obs_data_set_int(settings, "height", height);
		} else {
			struct dstr str;
			dstr_init(&str);
			dstr_printf(&str, "%dx%d", (int)obs_data_get_int(settings, "width"),
				    (int)obs_data_get_int(settings, "height"));
			obs_data_set_string(settings, "resolution", str.array);
			dstr_free(&str);
		}
	}
	filter->remove_after_record = obs_data_get_bool(settings, "remove_after_record");
	filter->record_max_seconds = obs_data_get_int(settings, "record_max_seconds");
	const long long record_mode = obs_data_get_int(settings, "record_mode");
	const long long stream_mode = obs_data_get_int(settings, "stream_mode");
	const bool replay_buffer = obs_data_get_bool(settings, "replay_buffer") && !filter->closing;

	bool record = false;
	if (filter->closing) {
	} else if (record_mode == OUTPUT_MODE_ALWAYS) {
		record = true;
	} else if (record_mode == OUTPUT_MODE_RECORDING) {
		record = obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED;
	} else if (record_mode == OUTPUT_MODE_STREAMING) {
		record = obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED;
	} else if (record_mode == OUTPUT_MODE_STREAMING_OR_RECORDING) {
		record = (obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED) ||
			 (obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED);
	} else if (record_mode == OUTPUT_MODE_VIRTUAL_CAMERA) {
		record = obs_frontend_virtualcam_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED;
	}

	bool stream = false;
	if (filter->closing) {
	} else if (stream_mode == OUTPUT_MODE_ALWAYS) {
		stream = true;
	} else if (stream_mode == OUTPUT_MODE_RECORDING) {
		stream = obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED;
	} else if (stream_mode == OUTPUT_MODE_STREAMING) {
		stream = obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			 filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED;
	} else if (stream_mode == OUTPUT_MODE_STREAMING_OR_RECORDING) {
		stream = (obs_frontend_streaming_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_STREAMING_STOPPED) ||
			 (obs_frontend_recording_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPING &&
			  filter->last_frontend_event != OBS_FRONTEND_EVENT_RECORDING_STOPPED);
	} else if (stream_mode == OUTPUT_MODE_VIRTUAL_CAMERA) {
		stream = obs_frontend_virtualcam_active() && filter->last_frontend_event != OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED;
	}

	const bool custom = use_custom_ffmpeg_output(settings);
	const bool need_obs_encoder = stream || replay_buffer || (!custom && record);
	if (!filter->closing && (record_mode != OUTPUT_MODE_NONE || stream_mode != OUTPUT_MODE_NONE || replay_buffer)) {
		update_encoder(filter, settings, need_obs_encoder);
	}

	if (parent && filter->view && (record || replay_buffer)) {
		obs_source_t *view_source = obs_view_get_source(filter->view, SOURCE_CHANNEL);
		if (view_source != parent)
			obs_view_set_source(filter->view, SOURCE_CHANNEL, parent);
		obs_source_release(view_source);
	}

	if (parent && filter->view && !filter->closing) {
		obs_source_t *background_source = obs_view_get_source(filter->view, BACKGROUND_CHANNEL);
		if (obs_data_get_bool(settings, "custom_ffmpeg_output") && obs_data_get_bool(settings, "ff_alpha")) {
			if (background_source) {
				obs_view_set_source(filter->view, BACKGROUND_CHANNEL, NULL);
				obs_source_release(background_source);
			}
		} else {
			if (!background_source) {
				background_source = obs_source_create_private("color_source", "Source Record Background", NULL);
				obs_view_set_source(filter->view, BACKGROUND_CHANNEL, background_source);
			}
			obs_data_t *css = obs_source_get_settings(background_source);
			if (obs_data_get_int(css, "color") != obs_data_get_int(settings, "backgroundColor") ||
			    obs_data_get_int(css, "width") != obs_source_get_width(parent) ||
			    obs_data_get_int(css, "height") != obs_source_get_height(parent)) {
				obs_data_set_int(css, "color", obs_data_get_int(settings, "backgroundColor"));
				obs_data_set_int(css, "width", obs_source_get_width(parent));
				obs_data_set_int(css, "height", obs_source_get_height(parent));
				obs_source_update(background_source, css);
			}
			obs_data_release(css);
			obs_source_release(background_source);
		}
	}

	if (record != filter->record) {
		if (record) {
			if (obs_source_enabled(filter->source) && filter->video_output)
				start_file_output(filter, settings);
		} else if (filter->fileOutput) {
			if (filter->closing) {
				stop_output_sync(filter, filter->fileOutput);
				obs_output_release(filter->fileOutput);
				filter->fileOutput = NULL;
			} else {
				stop_output_async(filter, &filter->fileOutput, OUTPUT_TYPE_FILE);
			}
		}
		filter->record = record;
		if (!record && !filter->stream && !filter->replayBuffer &&
		    !filter->stopping_file_output && !filter->stopping_stream_output && !filter->stopping_replay_output &&
		    filter->output_active) {
			filter->output_active = false;
			obs_source_t *parent = obs_filter_get_parent(filter->source);
			if (parent)
				obs_source_dec_showing(parent);
		}
	} else if (record && filter->fileOutput && !filter->closing && !filter->stopping_file_output &&
		   record_settings_changed(filter, settings)) {
		/* A running muxer cannot move to another file, so the recording is
		 * split: the current file is finished and the tick starts a new one
		 * with the new path, container or filename pattern. */
		blog(LOG_INFO, "[Source Record] recording settings of '%s' changed while recording, restarting the output",
		     obs_source_get_name(filter->source));
		signal_handler_t *sh = obs_output_get_signal_handler(filter->fileOutput);
		if (sh)
			signal_handler_disconnect(sh, "stop", remove_filter, filter);
		stop_output_async(filter, &filter->fileOutput, OUTPUT_TYPE_FILE);
	}

	if (record && filter->fileOutput && filter->last_frontend_event == OBS_FRONTEND_EVENT_RECORDING_PAUSED &&
	    !obs_output_paused(filter->fileOutput)) {
		obs_output_pause(filter->fileOutput, true);
		filter->last_frontend_event = -1;
	} else if (record && filter->fileOutput && filter->last_frontend_event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED &&
		   obs_output_paused(filter->fileOutput)) {
		obs_output_pause(filter->fileOutput, false);
		filter->last_frontend_event = -1;
	}

	if (replay_buffer != filter->replayBuffer) {
		if (replay_buffer) {
			if (obs_source_enabled(filter->source) && filter->video_output)
				start_replay_output(filter, settings);
		} else if (filter->replayOutput) {
			obs_data_t *hotkeys = obs_hotkeys_save_output(filter->replayOutput);
			obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
			obs_data_release(hotkeys);
			if (filter->closing) {
				stop_output_sync(filter, filter->replayOutput);
				obs_output_release(filter->replayOutput);
				filter->replayOutput = NULL;
			} else {
				stop_output_async(filter, &filter->replayOutput, OUTPUT_TYPE_REPLAY);
			}
		}

		filter->replayBuffer = replay_buffer;
	} else if (replay_buffer && filter->replayOutput && obs_source_enabled(filter->source)) {
		obs_data_t *replay_settings = obs_output_get_settings(filter->replayOutput);
		if (filter->replay_buffer_duration != obs_data_get_int(settings, "replay_duration") ||
		    obs_data_get_int(replay_settings, "max_size_mb") != obs_data_get_int(settings, "replay_max_size_mb")) {
			obs_data_t *hotkeys = obs_hotkeys_save_output(filter->replayOutput);
			obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
			obs_data_release(hotkeys);
			stop_output_async(filter, &filter->replayOutput, OUTPUT_TYPE_REPLAY);
		} else {
			obs_data_set_string(replay_settings, "directory", obs_data_get_string(settings, "path"));
			obs_data_set_string(replay_settings, "format",
					    obs_data_get_string(settings, "replay_filename_formatting"));
			obs_data_set_string(replay_settings, "extension",
					    GetFormatExt(obs_data_get_string(settings, "rec_format")));
		}
		obs_data_release(replay_settings);
	}

	if (parent && filter->view && stream) {
		obs_source_t *view_source = obs_view_get_source(filter->view, SOURCE_CHANNEL);
		if (view_source != parent)
			obs_view_set_source(filter->view, SOURCE_CHANNEL, parent);
		obs_source_release(view_source);
	}

	if (stream != filter->stream) {
		if (stream) {
			if (obs_source_enabled(filter->source) && filter->video_output)
				start_stream_output(filter, settings);
		} else if (filter->streamOutput) {
			if (filter->closing) {
				stop_output_sync(filter, filter->streamOutput);
				obs_output_release(filter->streamOutput);
				filter->streamOutput = NULL;
			} else {
				stop_output_async(filter, &filter->streamOutput, OUTPUT_TYPE_STREAM);
			}
		}
		filter->stream = stream;
	}

	if (!replay_buffer && !record && !stream) {
		if (filter->encoder && !obs_encoder_active(filter->encoder)) {
			obs_encoder_release(filter->encoder);
			filter->encoder = NULL;
		}
		for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
			if (!filter->audioEncoder[i] || obs_encoder_active(filter->audioEncoder[i]))
				continue;
			obs_encoder_release(filter->audioEncoder[i]);
			filter->audioEncoder[i] = NULL;
		}
	}

	if (obs_data_get_bool(settings, "different_audio")) {
		const char *source_name = obs_data_get_string(settings, "audio_source");
		if (!strlen(source_name)) {
			if (filter->audio_source) {
				obs_weak_source_release(filter->audio_source);
				filter->audio_source = NULL;
			}
		} else {
			obs_source_t *source = obs_weak_source_get_source(filter->audio_source);
			if (source)
				obs_source_release(source);
			if (!source || strcmp(source_name, obs_source_get_name(source)) != 0) {
				if (filter->audio_source) {
					obs_weak_source_release(filter->audio_source);
					filter->audio_source = NULL;
				}
				source = obs_get_source_by_name(source_name);
				if (source) {
					filter->audio_source = obs_source_get_weak_source(source);
					obs_source_release(source);
				}
			}
		}

	} else if (filter->audio_source) {
		obs_weak_source_release(filter->audio_source);
		filter->audio_source = NULL;
	}
}

static void source_record_filter_save(void *data, obs_data_t *settings)
{
	struct source_record_filter_context *filter = data;
	if (filter->replayOutput) {
		obs_data_t *hotkeys = obs_hotkeys_save_output(filter->replayOutput);
		obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
		obs_data_release(hotkeys);
	}
}

static const char *config_string_or(config_t *config, const char *section, const char *name, const char *fallback)
{
	const char *val = config ? config_get_string(config, section, name) : NULL;
	return (val && *val) ? val : fallback;
}

static void source_record_filter_defaults(obs_data_t *settings)
{
	config_t *config = obs_frontend_get_profile_config();

	const char *mode = config_string_or(config, "Output", "Mode", "Simple");
	const bool adv_out = astrcmpi(mode, "Advanced") == 0;
	/* Advanced output records either with the standard recorder or with
	 * the custom FFmpeg output, each with its own path (#42). */
	const char *type = config_string_or(config, "AdvOut", "RecType", "Standard");
	const bool adv_ffmpeg = astrcmpi(type, "FFmpeg") == 0;
	const char *adv_path = adv_ffmpeg ? config_string_or(config, "AdvOut", "FFFilePath", NULL)
					  : config_string_or(config, "AdvOut", "RecFilePath", NULL);
	const char *rec_path = adv_out ? adv_path : config_string_or(config, "SimpleOutput", "FilePath", NULL);
	if (!rec_path)
		rec_path = config_string_or(config, "SimpleOutput", "FilePath", "");

	obs_data_set_default_string(settings, "path", rec_path);
	const char *format = config_string_or(config, "Output", "FilenameFormatting", "%CCYY-%MM-%DD %hh-%mm-%ss");
	obs_data_set_default_string(settings, "filename_formatting", format);
	obs_data_set_default_string(settings, "replay_filename_formatting", format);
	const char *section = adv_out ? "AdvOut" : "SimpleOutput";
	const char *rec_format = config_string_or(config, section, "RecFormat2", NULL);
	if (!rec_format)
		rec_format = config_string_or(config, section, "RecFormat", "mkv");
	obs_data_set_default_string(settings, "rec_format", rec_format);

	/* Custom Output (FFmpeg) */
	obs_data_set_default_bool(settings, "custom_ffmpeg_output", false);
	obs_data_set_default_string(settings, "ff_video_encoder", "prores");
	obs_data_set_default_string(settings, "ff_video_settings", "profile=4");
	obs_data_set_default_int(settings, "ff_video_bitrate", 10000);
	obs_data_set_default_bool(settings, "ff_alpha", true);
	obs_data_set_default_string(settings, "ff_audio_encoder", "aac");
	obs_data_set_default_int(settings, "ff_audio_bitrate", 160);
	obs_data_set_default_string(settings, "ff_audio_settings", "");
	obs_data_set_default_string(settings, "ff_muxer_settings", "");

	obs_data_set_default_int(settings, "backgroundColor", 0);

	const char *enc_id;
	if (adv_out) {
		enc_id = config_string_or(config, "AdvOut", "RecEncoder", "none");
		if (astrcmpi(enc_id, "none") == 0)
			enc_id = config_string_or(config, "AdvOut", "Encoder", "x264");
	} else {
		const char *quality = config_string_or(config, "SimpleOutput", "RecQuality", "Stream");
		if (astrcmpi(quality, "Stream") == 0) {
			enc_id = config_string_or(config, "SimpleOutput", "StreamEncoder", "x264");
		} else if (astrcmpi(quality, "Lossless") == 0) {
			/* lossless uses the ffmpeg output, not an encoder */
			enc_id = "x264";
		} else {
			enc_id = config_string_or(config, "SimpleOutput", "RecEncoder", "x264");
		}
	}
	if (strcmp(enc_id, "jim_nvenc") == 0 || strcmp(enc_id, "obs_nvenc_h264_tex") == 0)
		enc_id = "nvenc";
	obs_data_set_default_string(settings, "encoder", enc_id);

	obs_data_set_default_string(settings, "audio_encoder", "ffmpeg_aac");

	set_encoder_defaults(settings);
	obs_data_set_default_int(settings, "replay_duration", 5);
	obs_data_set_default_int(settings, "replay_max_size_mb", 10000);
	obs_data_set_default_int(settings, "max_size_mb", 2048);
	obs_data_set_default_int(settings, "max_time_sec", 15 * 60);
}

static void source_record_filter_filter_remove(void *data, obs_source_t *parent);

static void update_task(void *param)
{
	struct source_record_filter_context *context = param;
	obs_source_update(context->source, NULL);
}

static void frontend_event(enum obs_frontend_event event, void *data)
{
	struct source_record_filter_context *context = data;
	if (event == OBS_FRONTEND_EVENT_STREAMING_STARTING || event == OBS_FRONTEND_EVENT_STREAMING_STARTED ||
	    event == OBS_FRONTEND_EVENT_STREAMING_STOPPING || event == OBS_FRONTEND_EVENT_STREAMING_STOPPED ||
	    event == OBS_FRONTEND_EVENT_RECORDING_STARTING || event == OBS_FRONTEND_EVENT_RECORDING_STARTED ||
	    event == OBS_FRONTEND_EVENT_RECORDING_STOPPING || event == OBS_FRONTEND_EVENT_RECORDING_STOPPED ||
	    event == OBS_FRONTEND_EVENT_VIRTUALCAM_STARTED || event == OBS_FRONTEND_EVENT_VIRTUALCAM_STOPPED ||
	    event == OBS_FRONTEND_EVENT_RECORDING_PAUSED || event == OBS_FRONTEND_EVENT_RECORDING_UNPAUSED) {
		context->last_frontend_event = (int)event;

		obs_queue_task(OBS_TASK_GRAPHICS, update_task, data, false);
	} else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		context->closing = true;
		context->exiting = true;
		obs_source_update(context->source, NULL);
	} else if (event == OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP) {
		context->closing = true;
		obs_source_update(context->source, NULL);
	}
}

static void *source_record_filter_create(obs_data_t *settings, obs_source_t *source)
{
	struct source_record_filter_context *context = bzalloc(sizeof(struct source_record_filter_context));
	context->source = source;

	da_push_back(source_record_filters, &source);
	context->last_frontend_event = -1;
	context->enableHotkey = OBS_INVALID_HOTKEY_PAIR_ID;
	context->pauseHotkeys = OBS_INVALID_HOTKEY_PAIR_ID;
	context->splitHotkey = OBS_INVALID_HOTKEY_ID;
	context->chapterHotkey = OBS_INVALID_HOTKEY_ID;
	context->replayHotkey = OBS_INVALID_HOTKEY_ID;
	source_record_filter_update(context, settings);
	obs_frontend_add_event_callback(frontend_event, context);
	return context;
}

static void source_record_filter_destroy(void *data)
{
	struct source_record_filter_context *context = data;
	da_erase_item(source_record_filters, &context->source);
	context->closing = true;
	if (context->output_active) {
		obs_source_t *parent = obs_filter_get_parent(context->source);
		if (parent)
			obs_source_dec_showing(parent);
		context->output_active = false;
	}
	obs_frontend_remove_event_callback(frontend_event, context);

	stop_output_sync(context, context->fileOutput);
	stop_output_sync(context, context->streamOutput);
	stop_output_sync(context, context->replayOutput);

	while (os_atomic_load_long(&context->active_stop_threads) > 0) {
		os_sleep_ms(10);
	}

	if (context->enableHotkey != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(context->enableHotkey);

	if (context->pauseHotkeys != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(context->pauseHotkeys);

	if (context->splitHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(context->splitHotkey);

	if (context->chapterHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(context->chapterHotkey);

	if (context->replayHotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(context->replayHotkey);

	obs_output_release(context->fileOutput);
	obs_output_release(context->streamOutput);
	obs_output_release(context->replayOutput);
	context->fileOutput = NULL;
	context->streamOutput = NULL;
	context->replayOutput = NULL;

	for (int i = 0; i < MAX_AUDIO_MIXES; i++) {
		obs_encoder_release(context->audioEncoder[i]);
		context->audioEncoder[i] = NULL;
	}
	obs_encoder_release(context->encoder);
	context->encoder = NULL;

	obs_weak_source_release(context->audio_source);
	context->audio_source = NULL;

	if (context->audio_track == 0 && context->audio_output)
		audio_output_close(context->audio_output);
	context->audio_output = NULL;

	obs_service_release(context->service);
	context->service = NULL;

	if (context->view) {
		obs_view_set_source(context->view, BACKGROUND_CHANNEL, NULL);
		obs_view_set_source(context->view, SOURCE_CHANNEL, NULL);
		if (context->video_output) {
			obs_view_remove(context->view);
			context->video_output = NULL;
		}
		obs_view_destroy(context->view);
		context->view = NULL;
	}

	dstr_free(&context->rec_path);
	dstr_free(&context->rec_format);
	dstr_free(&context->rec_filename);

	context->source = NULL;
	bfree(context);
}

static bool source_record_enable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;

	if (obs_source_enabled(context->source))
		return false;

	obs_source_set_enabled(context->source, true);
	return true;
}

static bool source_record_disable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;
	if (!obs_source_enabled(context->source))
		return false;
	obs_source_set_enabled(context->source, false);
	return true;
}

static bool source_record_pause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;

	if (obs_output_paused(context->fileOutput))
		return false;

	obs_output_pause(context->fileOutput, true);
	return true;
}

static bool source_record_unpause_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct source_record_filter_context *context = data;
	if (!pressed)
		return false;
	if (!obs_output_paused(context->fileOutput))
		return false;

	obs_output_pause(context->fileOutput, false);
	return true;
}

static void source_record_split_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct source_record_filter_context *context = data;
	if (!context->fileOutput)
		return;
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	proc_handler_call(ph, "split_file", &cd);
	calldata_free(&cd);
}

static void source_record_chapter_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct source_record_filter_context *context = data;
	if (!context->fileOutput)
		return;
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	proc_handler_call(ph, "add_chapter", &cd);
	calldata_free(&cd);
}

static void source_record_replay_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (!pressed)
		return;
	struct source_record_filter_context *context = data;
	if (!context->replayOutput)
		return;
	proc_handler_t *ph = obs_output_get_proc_handler(context->replayOutput);
	struct calldata cd;
	calldata_init(&cd);
	proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
}

/* Keep the bindings of the replay buffer output's own hotkeys when the
 * output is recreated (#24). */
static void save_replay_hotkeys(struct source_record_filter_context *context)
{
	if (!context->replayOutput)
		return;
	obs_data_t *hotkeys = obs_hotkeys_save_output(context->replayOutput);
	obs_data_t *settings = obs_source_get_settings(context->source);
	obs_data_set_obj(settings, "replay_hotkeys", hotkeys);
	obs_data_release(settings);
	obs_data_release(hotkeys);
}

/* The view holds a reference to the parent and OBS renders every view each
 * frame, whether or not anything encodes it (#38, #135). Once nothing records
 * the source is taken off the view and the video output is dropped. */
static void release_idle_view(struct source_record_filter_context *context)
{
	if (!context->view || context->starting_file_output || context->starting_stream_output ||
	    context->starting_replay_output || context->stopping_file_output || context->stopping_stream_output ||
	    context->stopping_replay_output || os_atomic_load_long(&context->active_stop_threads) > 0)
		return;

	obs_source_t *view_source = obs_view_get_source(context->view, SOURCE_CHANNEL);
	if (view_source) {
		obs_view_set_source(context->view, SOURCE_CHANNEL, NULL);
		obs_source_release(view_source);
	}

	/* an encoder that has been used cannot be moved to another video_t, so
	 * the video_t only goes once the encoder has been released */
	if (context->video_output && !context->encoder) {
		obs_view_remove(context->view);
		context->video_output = NULL;
		context->width = 0;
		context->height = 0;
		context->view_format = VIDEO_FORMAT_NONE;
	}
}

static void source_record_filter_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct source_record_filter_context *context = data;
	if (context->closing)
		return;

	obs_source_t *parent = obs_filter_get_parent(context->source);
	if (!parent)
		return;

	if (obs_obj_is_private(parent)) {
		context->closing = true;
		return;
	}

	if (context->enableHotkey == OBS_INVALID_HOTKEY_PAIR_ID)
		context->enableHotkey = obs_hotkey_pair_register_source(
			parent, "source_record.enable", obs_module_text("SourceRecordEnable"), "source_record.disable",
			obs_module_text("SourceRecordDisable"), source_record_enable_hotkey, source_record_disable_hotkey, context,
			context);

	if (context->pauseHotkeys == OBS_INVALID_HOTKEY_PAIR_ID)
		context->pauseHotkeys = obs_hotkey_pair_register_source(
			parent, "source_record.PauseRecording", obs_frontend_get_locale_string("Basic.Main.PauseRecording"),
			"source_record.UnpauseRecording", obs_frontend_get_locale_string("Basic.Main.UnpauseRecording"),
			source_record_pause_hotkey, source_record_unpause_hotkey, context, context);

	if (context->splitHotkey == OBS_INVALID_HOTKEY_ID)
		context->splitHotkey = obs_hotkey_register_source(parent, "source_record.SplitRecording",
								  obs_frontend_get_locale_string("Basic.Main.SplitFile"),
								  source_record_split_hotkey, context);

	if (context->chapterHotkey == OBS_INVALID_HOTKEY_ID)
		context->chapterHotkey = obs_hotkey_register_source(parent, "source_record.AddChapterMarker",
								    obs_frontend_get_locale_string("Basic.Main.AddChapterMarker"),
								    source_record_chapter_hotkey, context);

	if (context->replayHotkey == OBS_INVALID_HOTKEY_ID)
		context->replayHotkey = obs_hotkey_register_source(parent, "source_record.SaveReplay",
								   obs_module_text("SaveReplay"),
								   source_record_replay_hotkey, context);


	uint32_t width = obs_source_get_width(parent);
	width += (width & 1);
	uint32_t height = obs_source_get_height(parent);
	height += (height & 1);
	/* Fix: never destroy the video_output while an encoder is still feeding
	 * from it. A size change on the parent (window capture / PipeWire on
	 * focus change) used to call obs_view_remove() immediately, freeing the
	 * video_output under a running encoder + replay buffer -> crash. */
	const bool size_changed = width && height && (context->width != width || context->height != height);
	/* The pixel format can change as well, e.g. when the custom ffmpeg
	 * output with alpha is enabled or disabled. */
	if (context->wanted_view_format == VIDEO_FORMAT_NONE)
		context->wanted_view_format = get_view_format(context->source);
	const bool format_changed =
		context->video_output != NULL && context->view_format != context->wanted_view_format;

	const bool wanted = outputs_wanted(context);

	if (width && height && !context->video_output && wanted) {
		struct obs_video_info ovi = {0};
		obs_get_video_info(&ovi);

		ovi.base_width = width;
		ovi.base_height = height;
		ovi.output_width = width;
		ovi.output_height = height;
		ovi.output_format = context->wanted_view_format;

		if (!context->view)
			context->view = obs_view_create();

		context->video_output = obs_view_add2(context->view, &ovi);
		if (context->video_output) {
			context->width = width;
			context->height = height;
			context->view_format = context->wanted_view_format;
		}
	} else if ((size_changed || format_changed) && context->video_output) {
		if (context->output_active) {
			/* defer: let the restart branch stop the outputs first */
			context->restart = true;
		} else if (!context->stopping_file_output && !context->stopping_stream_output &&
			   !context->stopping_replay_output &&
			   os_atomic_load_long(&context->active_stop_threads) == 0 &&
			   (!context->encoder || !obs_encoder_active(context->encoder))) {
			/* outputs are down and the encoder is idle: safe to swap */
			struct obs_video_info ovi = {0};
			obs_get_video_info(&ovi);

			ovi.base_width = width;
			ovi.base_height = height;
			ovi.output_width = width;
			ovi.output_height = height;
			ovi.output_format = context->wanted_view_format;

			obs_view_remove(context->view);
			context->video_output = obs_view_add2(context->view, &ovi);
			if (context->video_output) {
				context->width = width;
				context->height = height;
				context->view_format = context->wanted_view_format;
				if (context->encoder)
					obs_encoder_set_video(context->encoder, context->video_output);
			}
		}
		/* else: encoder or outputs still winding down, retry on the next tick */
	}

	if (context->restart && context->output_active) {
		save_replay_hotkeys(context);
		stop_output_async(context, &context->fileOutput, OUTPUT_TYPE_FILE);
		stop_output_async(context, &context->streamOutput, OUTPUT_TYPE_STREAM);
		stop_output_async(context, &context->replayOutput, OUTPUT_TYPE_REPLAY);
		context->output_active = false;
		context->restart = false;
		obs_source_dec_showing(obs_filter_get_parent(context->source));
	} else if (wanted) {
		const bool need_file = context->record && !context->fileOutput && !context->starting_file_output &&
				       !context->stopping_file_output;
		const bool need_stream = context->stream && !context->streamOutput && !context->starting_stream_output &&
					 !context->stopping_stream_output;
		const bool need_replay = context->replayBuffer && !context->replayOutput &&
					 !context->starting_replay_output && !context->stopping_replay_output;

		if (need_file || need_stream || need_replay) {
			if (!context->video_output || !width || !height)
				return;
			/* Fix: don't start new outputs (and reuse / re-point the encoder)
			 * while a previous output is still draining it -> use-after-free. */
			if (context->stopping_file_output || context->stopping_stream_output || context->stopping_replay_output ||
			    os_atomic_load_long(&context->active_stop_threads) > 0)
				return;
			/* An encoder still in use by another running output is shared, as
			 * OBS does for its own outputs. An idle but still active encoder
			 * (nothing running) is draining: wait for it. */
			if (context->encoder && obs_encoder_active(context->encoder) && !context->fileOutput &&
			    !context->streamOutput && !context->replayOutput)
				return;
			obs_data_t *s = obs_source_get_settings(context->source);
			const bool custom = use_custom_ffmpeg_output(s);
			const bool need_obs_encoder =
				context->stream || context->replayBuffer || (!custom && context->record);
			update_encoder(context, s, need_obs_encoder);
			if (context->record || context->stream || context->replayBuffer) {
				obs_source_t *view_source = obs_view_get_source(context->view, SOURCE_CHANNEL);
				if (view_source != parent)
					obs_view_set_source(context->view, SOURCE_CHANNEL, parent);
				obs_source_release(view_source);
			}

			obs_source_t *background_source = obs_view_get_source(context->view, BACKGROUND_CHANNEL);
			if (!background_source) {
				background_source =
					obs_source_create_private("color_source", "Source Record Background", NULL);
				obs_view_set_source(context->view, BACKGROUND_CHANNEL, background_source);
			}
			obs_data_t *css = obs_source_get_settings(background_source);
			if (obs_data_get_int(css, "color") != obs_data_get_int(s, "backgroundColor") ||
			    obs_data_get_int(css, "width") != obs_source_get_width(parent) ||
			    obs_data_get_int(css, "height") != obs_source_get_height(parent)) {
				obs_data_set_int(css, "color", obs_data_get_int(s, "backgroundColor"));
				obs_data_set_int(css, "width", obs_source_get_width(parent));
				obs_data_set_int(css, "height", obs_source_get_height(parent));
				obs_source_update(background_source, css);
			}
			obs_data_release(css);
			obs_source_release(background_source);

			if (need_file) {
				/* Every attempt creates a new timestamped folder on disk, so give
				 * up after a few tries instead of hammering the start. Changing a
				 * setting resets the counter. */
				if (context->start_failures >= 10) {
					if (context->start_failures == 10) {
						blog(LOG_ERROR,
						     "Source Record: could not start the recording output for '%s'. "
						     "Check the recording settings of the source.",
						     obs_source_get_name(context->source));
						context->start_failures++;
					}
				} else {
					start_file_output(context, s);
				}
			}
			if (need_stream)
				start_stream_output(context, s);
			if (need_replay)
				start_replay_output(context, s);
			obs_data_release(s);
		}
	} else if (context->output_active) {
		/* filter disabled or parent removed (#109) */
		save_replay_hotkeys(context);
		stop_output_async(context, &context->fileOutput, OUTPUT_TYPE_FILE);
		stop_output_async(context, &context->streamOutput, OUTPUT_TYPE_STREAM);
		stop_output_async(context, &context->replayOutput, OUTPUT_TYPE_REPLAY);
		context->output_active = false;
		obs_source_dec_showing(parent);
	} else if (!context->starting_file_output && !context->starting_stream_output && !context->starting_replay_output) {
		if (context->fileOutput || context->streamOutput || context->replayOutput) {
			/* outputs that never started, e.g. after a failed start */
			save_replay_hotkeys(context);
			stop_output_async(context, &context->fileOutput, OUTPUT_TYPE_FILE);
			stop_output_async(context, &context->streamOutput, OUTPUT_TYPE_STREAM);
			stop_output_async(context, &context->replayOutput, OUTPUT_TYPE_REPLAY);
		} else {
			release_idle_view(context);
		}
	}

	if (context->output_active && context->fileOutput && context->record_max_seconds) {
		int totalFrames = obs_output_get_total_frames(context->fileOutput);
		video_t *video = obs_output_video(context->fileOutput);
		uint64_t frameTimeNs = video_output_get_frame_time(video);
		long long msecs = util_mul_div64(totalFrames, frameTimeNs, 1000000ULL);
		if (msecs >= context->record_max_seconds * 1000) {
			obs_data_t *settings = obs_data_create();
			obs_data_set_int(settings, "record_mode", OUTPUT_MODE_NONE);
			obs_source_update(context->source, settings);
			obs_data_release(settings);
		}
	}
}

static void all_properties_changed(obs_properties_t *props, obs_data_t *settings)
{
	obs_property_t *property = obs_properties_first(props);
	while (property) {
		if (obs_property_get_type(property) == OBS_PROPERTY_GROUP) {
			all_properties_changed(obs_property_group_content(property), settings);
		} else {
			obs_property_modified(property, settings);
		}
		obs_property_next(&property);
	}
}

static bool encoder_changed(void *data, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(property);
	obs_properties_remove_by_name(props, "encoder_group");
	obs_properties_remove_by_name(props, "audio_encoder_group");
	bool visible = obs_property_visible(obs_properties_get(props, "others"));
	obs_properties_remove_by_name(props, "others");
	obs_properties_remove_by_name(props, "plugin_info");
	set_encoder_defaults(settings);
	const char *enc_id = get_encoder_id(settings);
	obs_properties_t *enc_props = obs_get_encoder_properties(enc_id);
	if (enc_props) {
		all_properties_changed(enc_props, settings);
		obs_properties_add_group(props, "encoder_group", obs_encoder_get_display_name(enc_id), OBS_GROUP_NORMAL, enc_props);
	}

	enc_id = obs_data_get_string(settings, "audio_encoder");
	if (!enc_id || !strlen(enc_id))
		enc_id = "ffmpeg_aac";
	enc_props = obs_get_encoder_properties(enc_id);
	if (enc_props) {
		all_properties_changed(enc_props, settings);
		const char *name = obs_encoder_get_display_name(enc_id);
		if (!name || !strlen(name))
			name = obs_module_text("AudioEncoder");
		obs_property_t *b = obs_properties_get(enc_props, "bitrate");
		if (b) {
			obs_property_int_set_suffix(obs_properties_add_int(enc_props, "audio_bitrate", obs_property_description(b),
									   obs_property_int_min(b), obs_property_int_max(b),
									   obs_property_int_step(b)),
						    obs_property_int_suffix(b));
			obs_properties_remove_by_name(enc_props, "bitrate");
		}
		obs_properties_add_group(props, "audio_encoder_group", name, OBS_GROUP_NORMAL, enc_props);
	}

	obs_property_t *p = obs_properties_add_text(props, "others", obs_module_text("OtherSourceRecords"), OBS_TEXT_INFO);
	obs_property_set_visible(p, visible);
	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/source-record.1285/\">Source Record</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return true;
}

static bool audio_track_changed(void *data, obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(property);
	const bool custom = obs_data_get_int(settings, "audio_track") == AUDIO_TRACK_CUSTOM;
	char prop_name[16];
	for (int i = 1; i <= MAX_AUDIO_MIXES; i++) {
		snprintf(prop_name, sizeof(prop_name), "audio_track_%d", i);
		obs_property_t *track_check = obs_properties_get(props, prop_name);
		if (track_check)
			obs_property_set_visible(track_check, custom);
	}
	return true;
}

static bool list_add_audio_sources(void *data, obs_source_t *source)
{
	obs_property_t *p = data;
	const uint32_t flags = obs_source_get_output_flags(source);
	if ((flags & OBS_SOURCE_COMPOSITE) != 0) {
		obs_property_list_add_string(p, obs_source_get_name(source), obs_source_get_name(source));
	} else if ((flags & OBS_SOURCE_AUDIO) != 0) {
		obs_property_list_add_string(p, obs_source_get_name(source), obs_source_get_name(source));
	}
	return true;
}

static bool source_record_split_button(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	struct source_record_filter_context *context = data;
	if (!context || !context->fileOutput)
		return false;
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	proc_handler_call(ph, "split_file", &cd);
	calldata_free(&cd);
	return false;
}

bool output_exists(const char *id)
{
	const char *output_id;
	size_t i = 0;
	while (obs_enum_output_types(i++, &output_id)) {
		if (strcmp(id, output_id) == 0)
			return true;
	}
	return false;
}

static obs_properties_t *source_record_filter_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *record = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(record, "record_mode", obs_module_text("RecordMode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("None"), OUTPUT_MODE_NONE);
	obs_property_list_add_int(p, obs_module_text("Always"), OUTPUT_MODE_ALWAYS);
	obs_property_list_add_int(p, obs_module_text("Streaming"), OUTPUT_MODE_STREAMING);
	obs_property_list_add_int(p, obs_module_text("Recording"), OUTPUT_MODE_RECORDING);
	obs_property_list_add_int(p, obs_module_text("StreamingOrRecording"), OUTPUT_MODE_STREAMING_OR_RECORDING);
	obs_property_list_add_int(p, obs_module_text("VirtualCamera"), OUTPUT_MODE_VIRTUAL_CAMERA);

	obs_properties_add_path(record, "path", obs_module_text("Path"), OBS_PATH_DIRECTORY, NULL, NULL);
	obs_properties_add_text(record, "filename_formatting", obs_module_text("FilenameFormatting"), OBS_TEXT_DEFAULT);
	p = obs_properties_add_list(record, "rec_format", obs_module_text("RecFormat"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "flv", "flv");
	obs_property_list_add_string(p, "mp4", "mp4");
	if (output_exists("mp4_output"))
		obs_property_list_add_string(p, "hybrid_mp4", "hybrid_mp4");
	if (output_exists("mov_output"))
		obs_property_list_add_string(p, "hybrid_mov", "hybrid_mov");
	obs_property_list_add_string(p, "fragmented_mp4", "fragmented_mp4");
	obs_property_list_add_string(p, "fragmented_mov", "fragmented_mov");
	obs_property_list_add_string(p, "mov", "mov");
	obs_property_list_add_string(p, "mkv", "mkv");
	obs_property_list_add_string(p, "webm", "webm");
	obs_property_list_add_string(p, "ts", "ts");
	obs_property_list_add_string(p, "m3u8", "m3u8");

	obs_properties_t *split_file = obs_properties_create();
	p = obs_properties_add_int(split_file, "max_time_sec",
				   obs_frontend_get_locale_string("Basic.Settings.Output.SplitFile.Time"), 0, 31536000, 1);
	obs_property_int_set_suffix(p, " s");
	p = obs_properties_add_int(split_file, "max_size_mb",
				   obs_frontend_get_locale_string("Basic.Settings.Output.SplitFile.Size"), 0, 1073741824, 1);
	obs_property_int_set_suffix(p, " MB");
	obs_properties_add_group(record, "split_file", obs_frontend_get_locale_string("Basic.Settings.Output.EnableSplitFile"),
				 OBS_GROUP_CHECKABLE, split_file);
	/* Manual splits work without automatic splitting, so the button is not
	 * inside the checkable group that greys it out. */
	obs_properties_add_button2(record, "split_file_now", obs_frontend_get_locale_string("Basic.Main.SplitFile"),
				   source_record_split_button, data);

	obs_properties_add_int(record, "record_max_seconds", obs_module_text("MaxSeconds"), 0, 31536000, 1);

	/* Custom Output (FFmpeg): encoders and containers the OBS encoders do
	 * not cover, e.g. ProRes 4444 with an alpha channel. */
	if (output_exists("ffmpeg_output")) {
		obs_properties_t *ffmpeg = obs_properties_create();
		p = obs_properties_add_list(ffmpeg, "ff_video_encoder", obs_module_text("FFmpegVideoEncoder"),
					    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p, obs_module_text("FFmpegProRes4444"), "prores");
		obs_property_list_add_string(p, obs_module_text("FFmpegQTRLE"), "qtrle");
		obs_property_list_add_string(p, obs_module_text("FFmpegFFV1"), "ffv1");
		obs_property_list_add_string(p, obs_module_text("FFmpegVP9"), "libvpx-vp9");
		obs_property_list_add_string(p, "x264", "libx264");
		obs_property_list_add_string(p, obs_module_text("FFmpegProRes4444KS"), "prores_ks");
		obs_property_set_long_description(p, obs_module_text("FFmpegVideoEncoderDescription"));
		p = obs_properties_add_text(ffmpeg, "ff_video_settings", obs_module_text("FFmpegVideoSettings"),
					    OBS_TEXT_DEFAULT);
		obs_property_set_long_description(p, obs_module_text("FFmpegVideoSettingsDescription"));
		p = obs_properties_add_int(ffmpeg, "ff_video_bitrate", obs_module_text("FFmpegVideoBitrate"), 0,
					   100000000, 1);
		obs_property_int_set_suffix(p, " kbps");
		p = obs_properties_add_bool(ffmpeg, "ff_alpha", obs_module_text("FFmpegAlpha"));
		obs_property_set_long_description(p, obs_module_text("FFmpegAlphaDescription"));
		p = obs_properties_add_list(ffmpeg, "ff_audio_encoder", obs_module_text("FFmpegAudioEncoder"),
					    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p, "AAC", "aac");
		obs_property_list_add_string(p, "Opus", "libopus");
		obs_property_list_add_string(p, "FLAC", "flac");
		obs_property_list_add_string(p, "ALAC", "alac");
		obs_property_list_add_string(p, obs_module_text("FFmpegPCM"), "pcm_s16le");
		obs_property_list_add_string(p, "MP3", "libmp3lame");
		p = obs_properties_add_int(ffmpeg, "ff_audio_bitrate", obs_module_text("FFmpegAudioBitrate"), 0, 1000000,
					   1);
		obs_property_int_set_suffix(p, " kbps");
		obs_properties_add_text(ffmpeg, "ff_audio_settings", obs_module_text("FFmpegAudioSettings"),
					OBS_TEXT_DEFAULT);
		p = obs_properties_add_text(ffmpeg, "ff_muxer_settings", obs_module_text("FFmpegMuxerSettings"),
					    OBS_TEXT_DEFAULT);
		obs_property_set_long_description(p, obs_module_text("FFmpegMuxerSettingsDescription"));
		obs_properties_add_group(record, "custom_ffmpeg_output", obs_module_text("CustomOutputFFmpeg"),
					 OBS_GROUP_CHECKABLE, ffmpeg);
	}

	obs_properties_add_group(props, "record", obs_module_text("Record"), OBS_GROUP_NORMAL, record);

	obs_properties_t *replay = obs_properties_create();

	p = obs_properties_add_int(replay, "replay_duration", obs_module_text("Duration"), 1, 10000, 1);
	obs_property_int_set_suffix(p, "s");

	p = obs_properties_add_int(replay, "replay_max_size_mb", obs_module_text("MaxMemory"), 1, 1000000, 1);
	obs_property_int_set_suffix(p, " MB");

	obs_properties_add_text(replay, "replay_filename_formatting", obs_module_text("FilenameFormatting"), OBS_TEXT_DEFAULT);

	obs_properties_add_group(props, "replay_buffer", obs_module_text("ReplayBuffer"), OBS_GROUP_CHECKABLE, replay);

	obs_properties_t *stream = obs_properties_create();

	p = obs_properties_add_list(stream, "stream_mode", obs_module_text("StreamMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(p, obs_module_text("None"), OUTPUT_MODE_NONE);
	obs_property_list_add_int(p, obs_module_text("Always"), OUTPUT_MODE_ALWAYS);
	obs_property_list_add_int(p, obs_module_text("Streaming"), OUTPUT_MODE_STREAMING);
	obs_property_list_add_int(p, obs_module_text("Recording"), OUTPUT_MODE_RECORDING);
	obs_property_list_add_int(p, obs_module_text("StreamingOrRecording"), OUTPUT_MODE_STREAMING_OR_RECORDING);
	obs_property_list_add_int(p, obs_module_text("VirtualCamera"), OUTPUT_MODE_VIRTUAL_CAMERA);

	obs_properties_add_text(stream, "server", obs_module_text("Server"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(stream, "key", obs_module_text("Key"), OBS_TEXT_PASSWORD);

	obs_properties_add_group(props, "stream", obs_module_text("Stream"), OBS_GROUP_NORMAL, stream);

	obs_properties_t *background = obs_properties_create();

	obs_properties_add_color(background, "backgroundColor", obs_module_text("BackgroundColor"));

	obs_properties_add_group(props, "background", obs_module_text("Background"), OBS_GROUP_NORMAL, background);

	obs_properties_t *audio = obs_properties_create();

	p = obs_properties_add_list(audio, "audio_track", obs_module_text("AudioTrack"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("None"), 0);
	obs_property_list_add_int(p, obs_module_text("All"), AUDIO_TRACK_ALL);
	config_t* config = obs_frontend_get_profile_config();
	const char *track = obs_module_text("Track");
	for (int i = 1; i <= MAX_AUDIO_MIXES; i++) {
		char trackNameSetting[16];
		snprintf(trackNameSetting, sizeof(trackNameSetting), "Track%dName", i);
		const char *trackName = config_get_string(config, "AdvOut", trackNameSetting);
		if (trackName && strlen(trackName) > 0) {
			obs_property_list_add_int(p, trackName, i);
		} else {
			char trackNameDefault[64];
			snprintf(trackNameDefault, sizeof(trackNameDefault), "%s %i", track, i);
			obs_property_list_add_int(p, trackNameDefault, i);
		}
	}
	obs_property_list_add_int(p, obs_module_text("Custom"), AUDIO_TRACK_CUSTOM);
	obs_property_set_modified_callback2(p, audio_track_changed, data);

	for (int i = 1; i <= MAX_AUDIO_MIXES; i++) {
		char prop_name[16];
		snprintf(prop_name, sizeof(prop_name), "audio_track_%d", i);
		char buffer[64];
		snprintf(buffer, 64, "%s %i", track, i);
		obs_property_t *track_check = obs_properties_add_bool(audio, prop_name, buffer);
		obs_property_set_visible(track_check, false);
	}

	p = obs_properties_add_list(audio, "audio_source", obs_module_text("Source"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(list_add_audio_sources, p);
	obs_enum_scenes(list_add_audio_sources, p);

	obs_properties_add_group(props, "different_audio", obs_module_text("DifferentAudio"), OBS_GROUP_CHECKABLE, audio);

	obs_properties_t *scale = obs_properties_create();
	p = obs_properties_add_list(scale, "resolution", obs_module_text("Resolution"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, "640x480", "640x480");
	obs_property_list_add_string(p, "800x600", "800x600");
	obs_property_list_add_string(p, "1280x720", "1280x720");
	obs_property_list_add_string(p, "1920x1080", "1920x1080");
	obs_property_list_add_string(p, "2560x1440", "2560x1440");

	if (obs_encoder_set_gpu_scale_type_func) {
		p = obs_properties_add_list(scale, "scale_type", obs_module_text("ScaleType"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Bilinear"),
					  OBS_SCALE_BILINEAR);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Area"),
					  OBS_SCALE_AREA);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Bicubic"),
					  OBS_SCALE_BICUBIC);
		obs_property_list_add_int(p, obs_frontend_get_locale_string("Basic.Settings.Video.DownscaleFilter.Lanczos"),
					  OBS_SCALE_LANCZOS);
	}

	obs_properties_add_group(props, "scale", obs_module_text("Scale"), OBS_GROUP_CHECKABLE, scale);

	if (obs_encoder_set_frame_rate_divisor_func) {
		p = obs_properties_add_list(props, "frame_rate_divisor", obs_module_text("Framerate"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		struct obs_video_info ovi;
		obs_get_video_info(&ovi);
		float fps = ovi.fps_den > 0 ? (float)ovi.fps_num / (float)ovi.fps_den : 0.0f;
		struct dstr str;
		dstr_init(&str);
		dstr_printf(&str, "%.2f fps", fps);
		obs_property_list_add_int(p, str.array, 0);
		for (int i = 2; i <= 10; i++) {
			dstr_printf(&str, "%.2f fps (/%d)", fps / (float)i, i);
			obs_property_list_add_int(p, str.array, i);
		}
		dstr_free(&str);
	}

	p = obs_properties_add_list(props, "encoder", obs_module_text("VideoEncoder"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_t *audio_encoder = obs_properties_add_list(props, "audio_encoder", obs_module_text("AudioEncoder"),
								OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	obs_property_list_add_string(p, obs_module_text("Software"), "x264");
	if (EncoderAvailable("obs_qsv11"))
		obs_property_list_add_string(p, obs_module_text("QSV.H264"), "qsv");
	if (EncoderAvailable("obs_qsv11_av1"))
		obs_property_list_add_string(p, obs_module_text("QSV.AV1"), "qsv_av1");
	if (EncoderAvailable("ffmpeg_nvenc") || EncoderAvailable("jim_nvenc") || EncoderAvailable("obs_nvenc_h264_tex"))
		obs_property_list_add_string(p, obs_module_text("NVENC.H264"), "nvenc");
	if (EncoderAvailable("jim_av1_nvenc") || EncoderAvailable("obs_nvenc_av1_tex"))
		obs_property_list_add_string(p, obs_module_text("NVENC.AV1"), "nvenc_av1");
	if (EncoderAvailable("h265_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.HEVC"), "amd_hevc");
	if (EncoderAvailable("ffmpeg_hevc_nvenc") || EncoderAvailable("obs_nvenc_hevc_tex"))
		obs_property_list_add_string(p, obs_module_text("NVENC.HEVC"), "nvenc_hevc");
	if (EncoderAvailable("h264_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.H264"), "amd");
	if (EncoderAvailable("av1_texture_amf"))
		obs_property_list_add_string(p, obs_module_text("AMD.AV1"), "amd_av1");
	if (EncoderAvailable("com.apple.videotoolbox.videoencoder.ave.avc"))
		obs_property_list_add_string(p, obs_module_text("Apple.H264"), "apple_h264");
	if (EncoderAvailable("com.apple.videotoolbox.videoencoder.ave.hevc"))
		obs_property_list_add_string(p, obs_module_text("Apple.HEVC"), "apple_hevc");

	const char *enc_id = NULL;
	size_t i = 0;
	while (obs_enum_encoder_types(i++, &enc_id)) {
		const uint32_t caps = obs_get_encoder_caps(enc_id);
		if ((caps & (OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL)) != 0)
			continue;
		const char *name = obs_encoder_get_display_name(enc_id);
		if (obs_get_encoder_type(enc_id) == OBS_ENCODER_VIDEO) {
			obs_property_list_add_string(p, name, enc_id);
		} else {
			obs_property_list_add_string(audio_encoder, name, enc_id);
		}
	}
	obs_property_set_modified_callback2(p, encoder_changed, data);
	obs_property_set_modified_callback2(audio_encoder, encoder_changed, data);

	obs_properties_add_group(props, "encoder_group", obs_module_text("VideoEncoder"), OBS_GROUP_NORMAL,
				 obs_properties_create());
	obs_properties_add_group(props, "audio_encoder_group", obs_module_text("AudioEncoder"), OBS_GROUP_NORMAL,
				 obs_properties_create());

	p = obs_properties_add_text(props, "others", obs_module_text("OtherSourceRecords"), OBS_TEXT_INFO);
	if (data) {
		struct source_record_filter_context *context = data;
		struct dstr sources_text;
		dstr_init(&sources_text);
		for (size_t i = 0; i < source_record_filters.num; i++) {
			if (source_record_filters.array[i] == context->source)
				continue;
			if (sources_text.len)
				dstr_cat(&sources_text, "\n");
			obs_source_t *parent = obs_filter_get_parent(source_record_filters.array[i]);
			if (parent) {
				dstr_cat(&sources_text, obs_source_get_name(parent));
				dstr_cat(&sources_text, " - ");
			}
			dstr_cat(&sources_text, obs_source_get_name(source_record_filters.array[i]));
		}
		if (sources_text.len > 0) {
			obs_data_t *settings = obs_source_get_settings(context->source);
			obs_data_set_string(settings, "others", sources_text.array);
			obs_data_release(settings);
			obs_property_set_visible(p, true);
		} else {
			obs_property_set_visible(p, false);
		}
		dstr_free(&sources_text);
	} else {
		obs_property_set_visible(p, false);
	}

	obs_properties_add_text(
		props, "plugin_info",
		"<a href=\"https://obsproject.com/forum/resources/source-record.1285/\">Source Record</a> (" PROJECT_VERSION
		") by <a href=\"https://www.exeldro.com\">Exeldro</a>",
		OBS_TEXT_INFO);
	return props;
}

static void source_record_filter_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct source_record_filter_context *context = data;
	obs_source_skip_video_filter(context->source);
}

static void source_record_filter_filter_remove(void *data, obs_source_t *parent)
{
	UNUSED_PARAMETER(parent);
	struct source_record_filter_context *context = data;
	context->closing = true;
	stop_output_sync(context, context->fileOutput);
	stop_output_sync(context, context->streamOutput);
	stop_output_sync(context, context->replayOutput);
	while (os_atomic_load_long(&context->active_stop_threads) > 0) {
		os_sleep_ms(10);
	}
	obs_frontend_remove_event_callback(frontend_event, context);
}

struct obs_source_info source_record_filter_info = {
	.id = "source_record_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = source_record_filter_get_name,
	.create = source_record_filter_create,
	.destroy = source_record_filter_destroy,
	.update = source_record_filter_update,
	.load = source_record_filter_update,
	.save = source_record_filter_save,
	.get_defaults = source_record_filter_defaults,
	.video_tick = source_record_filter_tick,
	.get_properties = source_record_filter_properties,
	.filter_remove = source_record_filter_filter_remove,
	.video_render = source_record_filter_render,
};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("source-record", "en-US")
MODULE_EXPORT const char *obs_module_description(void)
{
	return "Source Record Filter";
}

static void find_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	const char *id = obs_source_get_unversioned_id(child);
	if (strcmp(id, "source_record_filter") != 0)
		return;
	obs_source_t **filter = param;
	*filter = child;
}

static void find_source_by_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	if (strcmp(obs_source_get_unversioned_id(child), "source_record_filter") != 0)
		return;

	DARRAY(obs_source_t *) *sources = param;
	darray_push_back(sizeof(obs_source_t *), &sources->da, &parent);
}

static bool find_source(void *data, obs_source_t *source)
{
	obs_source_enum_filters(source, find_source_by_filter, data);
	return true;
}

obs_source_t *get_source_record_filter(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data, bool create)
{
	const char *filter_name = obs_data_get_string(request_data, "filter");
	obs_source_t *filter = NULL;
	config_t *config = obs_frontend_get_profile_config();
	if (strlen(filter_name)) {
		filter = obs_source_get_filter_by_name(source, filter_name);
		if (!filter) {
			if (response_data)
				obs_data_set_string(response_data, "error", "filter not found");
			return NULL;
		}
		if (strcmp(obs_source_get_unversioned_id(filter), "source_record_filter") != 0) {
			if (response_data)
				obs_data_set_string(response_data, "error", "filter is not source record filter");
			obs_source_release(filter);
			return NULL;
		}
		struct source_record_filter_context *context = obs_obj_get_data(filter);
		if (context && context->output_active && create) {
			context->restart = true;
		}
	} else {
		obs_source_enum_filters(source, find_filter, &filter);
		filter = obs_source_get_ref(filter);
		if (!filter) {
			if (!create) {
				if (response_data)
					obs_data_set_string(response_data, "error", "failed to find filter");
				return NULL;
			}

			const char *filename = obs_data_get_string(request_data, "filename");
			if (!strlen(filename)) {
				filename = config_get_string(config, "Output", "FilenameFormatting");
			}
			obs_data_t *settings = obs_data_create();
			obs_data_set_bool(settings, "remove_after_record", true);
			char *filter_name = os_generate_formatted_filename(NULL, true, filename);
			filter = obs_source_get_filter_by_name(source, filter_name);
			if (!filter) {
				filter = obs_source_create("source_record_filter", filter_name, settings, NULL);
			} else if (strcmp(obs_source_get_unversioned_id(filter), "source_record_filter") != 0) {
				if (response_data)
					obs_data_set_string(response_data, "error", "filter is not source record filter");
				obs_source_release(filter);
				bfree(filter_name);
				obs_data_release(settings);
				return NULL;
			} else {
				struct source_record_filter_context *context = obs_obj_get_data(filter);
				if (context && context->output_active && create) {
					context->restart = true;
				}
			}
			bfree(filter_name);
			obs_data_release(settings);
			if (!filter) {
				if (response_data)
					obs_data_set_string(response_data, "error", "failed to create filter");
				return NULL;
			}
			obs_source_filter_add(source, filter);
		}
	}
	if (create && !obs_source_enabled(filter))
		obs_source_set_enabled(filter, true);
	return filter;
}

static bool start_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter) {
		if (response_data)
			obs_data_set_string(response_data, "error", "no source record filter");
		return false;
	}
	obs_data_t *settings = obs_source_get_settings(filter);
	const char *filename = obs_data_get_string(request_data, "filename");
	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (context && context->output_active) {
		if (strlen(filename)) {
			if (strstr(filename, "%") || strcmp(filename, obs_data_get_string(settings, "filename_formatting")) != 0) {
				context->restart = true;
			}
		} else if (strstr(obs_data_get_string(settings, "filename_formatting"), "%")) {
			context->restart = true;
		}
	}

	if (strlen(filename))
		obs_data_set_string(settings, "filename_formatting", filename);
	if (obs_data_has_user_value(request_data, "max_seconds"))
		obs_data_set_int(settings, "record_max_seconds", obs_data_get_int(request_data, "max_seconds"));
	obs_data_set_int(settings, "record_mode", OUTPUT_MODE_ALWAYS);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool pause_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter) {
		if (response_data)
			obs_data_set_string(response_data, "error", "no source record filter");
		return false;
	}

	struct source_record_filter_context *context = obs_obj_get_data(filter);
	obs_source_release(filter);
	if (!context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		return false;
	}
	obs_output_pause(context->fileOutput, true);
	return true;
}

static bool unpause_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter) {
		if (response_data)
			obs_data_set_string(response_data, "error", "no source record filter");
		return false;
	}

	struct source_record_filter_context *context = obs_obj_get_data(filter);
	obs_source_release(filter);
	if (!context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		return false;
	}
	obs_output_pause(context->fileOutput, false);
	return true;
}

static bool split_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter) {
		if (response_data)
			obs_data_set_string(response_data, "error", "no source record filter");
		return false;
	}

	struct source_record_filter_context *context = obs_obj_get_data(filter);
	obs_source_release(filter);
	if (!context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		return false;
	}
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	if (!proc_handler_call(ph, "split_file", &cd)) {
		calldata_free(&cd);
		if (response_data)
			obs_data_set_string(response_data, "error", "split_file call failed");
		return false;
	}
	calldata_free(&cd);
	return true;
}

static bool add_chapter_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter) {
		if (response_data)
			obs_data_set_string(response_data, "error", "no source record filter");
		return false;
	}
	struct source_record_filter_context *context = obs_obj_get_data(filter);
	obs_source_release(filter);
	if (!context->fileOutput) {
		if (response_data)
			obs_data_set_string(response_data, "error", "record output is not active");
		return false;
	}
	proc_handler_t *ph = obs_output_get_proc_handler(context->fileOutput);
	struct calldata cd;
	calldata_init(&cd);
	calldata_set_string(&cd, "chapter_name", obs_data_get_string(request_data, "chapter_name"));
	if (!proc_handler_call(ph, "add_chapter", &cd)) {
		calldata_free(&cd);
		if (response_data)
			obs_data_set_string(response_data, "error", "add_chapter call failed");
		return false;
	}
	calldata_free(&cd);
	return true;
}

static bool stop_record_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "record_mode", OUTPUT_MODE_NONE);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static void websocket_start_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_record_source(source, request_data, NULL);
		success = start_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = start_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_pause_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = pause_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = pause_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_unpause_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = unpause_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = unpause_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_split_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = split_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = split_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_add_chapter_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = add_chapter_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = add_chapter_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_record(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_record_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = stop_record_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static bool start_replay_buffer_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t *settings = obs_source_get_settings(filter);
	const char *filename = obs_data_get_string(request_data, "filename");
	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (context && context->output_active) {
		if (strlen(filename)) {
			if (strstr(filename, "%") || strcmp(filename, obs_data_get_string(settings, "replay_filename_formatting")) != 0) {
				context->restart = true;
			}
		} else if (strstr(obs_data_get_string(settings, "replay_filename_formatting"), "%")) {
			context->restart = true;
		}
	}

	if (strlen(filename))
		obs_data_set_string(settings, "replay_filename_formatting", filename);

	obs_data_set_bool(settings, "replay_buffer", true);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool stop_replay_buffer_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t *settings = obs_data_create();
	obs_data_set_bool(settings, "replay_buffer", false);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static bool save_replay_buffer_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;
	struct source_record_filter_context *context = obs_obj_get_data(filter);
	if (!context->replayOutput) {
		blog(LOG_WARNING, "[Source Record] replay_buffer_save requested but replay output is not active");
		obs_source_release(filter);
		return false;
	}

	proc_handler_t *ph = obs_output_get_proc_handler(context->replayOutput);
	calldata_t cd = {0};
	bool success = proc_handler_call(ph, "save", &cd);
	calldata_free(&cd);
	if (!success)
		blog(LOG_WARNING, "[Source Record] replay_buffer_save proc call failed");
	else
		blog(LOG_INFO, "[Source Record] replay_buffer_save requested successfully");
	obs_source_release(filter);
	return success;
}

static void websocket_start_replay_buffer(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_replay_buffer_source(source, request_data, NULL);
		success = start_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = start_replay_buffer_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_replay_buffer(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = stop_replay_buffer_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_save_replay_buffer(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = save_replay_buffer_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = save_replay_buffer_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	if (!success)
		blog(LOG_WARNING, "[Source Record] websocket replay_buffer_save finished with failures");
	else
		blog(LOG_INFO, "[Source Record] websocket replay_buffer_save finished successfully");
	obs_data_set_bool(response_data, "success", success);
}

static bool start_stream_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, true);
	if (!filter)
		return false;
	obs_data_t *settings = obs_source_get_settings(filter);

	const char *server = obs_data_get_string(request_data, "server");
	if (server && strlen(server))
		obs_data_set_string(settings, "server", server);

	const char *key = obs_data_get_string(request_data, "key");
	if (key && strlen(key))
		obs_data_set_string(settings, "key", key);

	obs_data_set_int(settings, "stream_mode", OUTPUT_MODE_ALWAYS);

	obs_source_update(filter, settings);
	obs_data_release(settings);

	obs_source_release(filter);
	return true;
}

static bool stop_stream_source(obs_source_t *source, obs_data_t *request_data, obs_data_t *response_data)
{
	obs_source_t *filter = get_source_record_filter(source, request_data, response_data, false);
	if (!filter)
		return false;

	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "stream_mode", OUTPUT_MODE_NONE);
	obs_source_update(filter, settings);
	obs_data_release(settings);
	obs_source_release(filter);
	return true;
}

static void websocket_start_stream(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		if (obs_data_get_bool(request_data, "stop_existing"))
			stop_stream_source(source, request_data, NULL);
		success = start_stream_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = start_stream_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

static void websocket_stop_stream(obs_data_t *request_data, obs_data_t *response_data, void *param)
{
	UNUSED_PARAMETER(param);
	const char *source_name = obs_data_get_string(request_data, "source");
	bool success = true;
	if (strlen(source_name)) {
		obs_source_t *source = obs_get_source_by_name(source_name);
		if (!source) {
			obs_data_set_string(response_data, "error", "source not found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		success = stop_stream_source(source, request_data, response_data);
		obs_source_release(source);
	} else {
		DARRAY(obs_source_t *) sources = {0};
		obs_enum_sources(find_source, &sources);
		obs_enum_scenes(find_source, &sources);
		if (!sources.num) {
			obs_data_set_string(response_data, "error", "no source found");
			obs_data_set_bool(response_data, "success", false);
			return;
		}
		for (size_t i = 0; i < sources.num; i++) {
			success = stop_stream_source(sources.array[i], request_data, response_data) && success;
		}
		da_free(sources);
	}
	obs_data_set_bool(response_data, "success", success);
}

bool obs_module_load(void)
{
	blog(LOG_INFO, "[Source Record] loaded version %s", PROJECT_VERSION);
	obs_register_source(&source_record_filter_info);

	da_init(source_record_filters);

	vendor = obs_websocket_register_vendor("source-record");
	obs_websocket_vendor_register_request(vendor, "record_start", websocket_start_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_pause", websocket_pause_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_unpause", websocket_unpause_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_split", websocket_split_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_add_chapter", websocket_add_chapter_record, NULL);
	obs_websocket_vendor_register_request(vendor, "record_stop", websocket_stop_record, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_start", websocket_start_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_stop", websocket_stop_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "replay_buffer_save", websocket_save_replay_buffer, NULL);
	obs_websocket_vendor_register_request(vendor, "stream_start", websocket_start_stream, NULL);
	obs_websocket_vendor_register_request(vendor, "stream_stop", websocket_stop_stream, NULL);

	return true;
}

void obs_module_post_load(void)
{
#ifdef _WIN32
	void *handle = os_dlopen("obs");
#else
	void *handle = dlopen(NULL, RTLD_LAZY);
#endif
	if (handle) {
		obs_encoder_set_frame_rate_divisor_func =
			(bool (*)(obs_encoder_t *, uint32_t))os_dlsym(handle, "obs_encoder_set_frame_rate_divisor");
		obs_encoder_set_gpu_scale_type_func =
			(void (*)(obs_encoder_t *, enum obs_scale_type))os_dlsym(handle, "obs_encoder_set_gpu_scale_type");
		os_dlclose(handle);
	}
}

void obs_module_unload(void)
{
	da_free(source_record_filters);
}

const char *obs_module_name(void)
{
	return obs_module_text("SourceRecord");
}
