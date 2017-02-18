#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/dstr.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>



#include "obs-ffmpeg-compat.h"
#include "obs-ffmpeg-formats.h"

#include <libff/ff-demuxer.h>

//#include <windows.h>
//
//
//void my_log_callback(void *ptr, int level, const char *fmt, va_list vargs)
//{
//	char szBuff[2048];
//	int i = _vsnprintf(szBuff, sizeof(szBuff), fmt, vargs);
//
//	OutputDebugStringA(szBuff);
//}

//av_log_set_callback(my_log_callback);
//av_log_set_level(AV_LOG_VERBOSE);



struct stinger_info {
	obs_source_t *source;

	gs_effect_t *effect;
	gs_eparam_t *ep_a_tex;
	gs_eparam_t *ep_b_tex;

	float lastTime;

	gs_texture_t * stinger_texture;
	gs_image_file_t stinger_error_image;

	float cutTime;
	size_t cutFrame;
	bool validInput;

	char *path;
	size_t curFrame;
	size_t numberOfFrames;

	struct ff_demuxer *demuxer;
	struct SwsContext *sws_ctx;
	int sws_width;
	int sws_height;
	enum AVPixelFormat sws_format;
	uint8_t *sws_data;
	int sws_linesize;

	enum AVDiscard frame_drop;
	enum video_range_type range;
	int audio_buffer_size;
	int video_buffer_size;
	bool is_advanced;
	bool is_looping;
	bool is_forcing_scale;
	bool is_hw_decoding;
	bool is_clear_on_media_end;
	bool restart_on_activate;
	
};

static const char *stinger_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("StingerTransition");
}

bool update_sws_context(struct stinger_info *s, AVFrame *frame)
{
	if (frame->width != s->sws_width
		|| frame->height != s->sws_height
		|| frame->format != s->sws_format) {
		if (s->sws_ctx != NULL)
			sws_freeContext(s->sws_ctx);

		if (frame->width <= 0 || frame->height <= 0) {
			blog(LOG_ERROR, "unable to create a sws "
				"context that has a width(%d) or "
				"height(%d) of zero.", frame->width,
				frame->height);
			goto fail;
		}

		s->sws_ctx = sws_getContext(
			frame->width,
			frame->height,
			frame->format,
			frame->width,
			frame->height,
			AV_PIX_FMT_BGRA,
			SWS_BILINEAR,
			NULL, NULL, NULL);

		if (s->sws_ctx == NULL) {
			blog(LOG_ERROR, "unable to create sws "
				"context with src{w:%d,h:%d,f:%d}->"
				"dst{w:%d,h:%d,f:%d}",
				frame->width, frame->height,
				frame->format, frame->width,
				frame->height, AV_PIX_FMT_BGRA);
			goto fail;

		}

		if (s->sws_data != NULL)
			bfree(s->sws_data);
		s->sws_data = bzalloc(frame->width * frame->height * 4);
		if (s->sws_data == NULL) {
			blog(LOG_ERROR, "unable to allocate sws "
				"pixel data with size %d",
				frame->width * frame->height * 4);
			goto fail;
		}

		s->sws_linesize = frame->width * 4;
		s->sws_width = frame->width;
		s->sws_height = frame->height;
		s->sws_format = frame->format;
	}

	return true;

fail:
	if (s->sws_ctx != NULL)
		sws_freeContext(s->sws_ctx);
	s->sws_ctx = NULL;

	if (s->sws_data)
		bfree(s->sws_data);
	s->sws_data = NULL;

	s->sws_linesize = 0;
	s->sws_width = 0;
	s->sws_height = 0;
	s->sws_format = 0;

	return false;
}

static void setNextFrameTexture(struct stinger_info *stinger, AVFrame *frame)
{
	if (!stinger || !stinger->validInput)
		return;

	obs_enter_graphics();
	gs_texture_destroy(stinger->stinger_texture);
	stinger->stinger_texture = gs_texture_create(
		frame->width, frame->height, 5, 1, //5 is AV_PIX_FMT_BGRA
		(const uint8_t**)&frame->data, 0);
	obs_leave_graphics();

	stinger->curFrame++;
}


static bool video_frame_scale(struct ff_frame *frame,
struct stinger_info *s, AVFrame *pFrame)
{
	if (!update_sws_context(s, frame->frame))
		return false;

	sws_scale(
		s->sws_ctx,
		(uint8_t const *const *)frame->frame->data,
		frame->frame->linesize,
		0,
		frame->frame->height,
		&s->sws_data,
		&s->sws_linesize
		);

	pFrame->data[0] = s->sws_data;
	pFrame->linesize[0] = s->sws_linesize;
	pFrame->format = AV_PIX_FMT_BGRA;

	setNextFrameTexture(s, pFrame);

	return true;
}
static bool video_frame_hwaccel(struct ff_frame *frame,
struct stinger_info *s, AVFrame *pFrame)
{
	// 4th plane is pixelbuf reference for mac
	for (int i = 0; i < 3; i++) {
		pFrame->data[i] = frame->frame->data[i];
		pFrame->linesize[i] = frame->frame->linesize[i];
	}

	//if (!set_obs_frame_colorprops(frame, s, obs_frame))
	//	return false;

	setNextFrameTexture(s, pFrame);
	return true;
}

static bool video_frame_direct(struct ff_frame *frame,
struct stinger_info *s, AVFrame *pFrame)
{
	int i;

	for (i = 0; i < MAX_AV_PLANES; i++) {
		pFrame->data[i] = frame->frame->data[i];
		pFrame->linesize[i] = frame->frame->linesize[i];
	}

	//if (!set_obs_frame_colorprops(frame, s, obs_frame))
	//	return false;

	setNextFrameTexture(s, pFrame);
	return true;
}

static bool video_frame(struct ff_frame *frame, void *opaque)
{
	struct stinger_info *s = opaque;

	// Media ended
	if (frame == NULL) {
		return true;
	}
	AVFrame *pFrame = NULL;
	pFrame = av_frame_alloc();

	pFrame->format = AV_PIX_FMT_BGRA;
	pFrame->width = frame->frame->width;
	pFrame->height = frame->frame->height;

	av_image_alloc(pFrame->data,
		pFrame->linesize,
		pFrame->width,
		pFrame->height,
		AV_PIX_FMT_BGRA,
		16
		);

	enum video_format format =
		ffmpeg_to_obs_video_format(frame->frame->format);

	if (s->is_forcing_scale || format == VIDEO_FORMAT_NONE)
		return video_frame_scale(frame, s, pFrame);
	else if (s->is_hw_decoding)
		return video_frame_hwaccel(frame, s, pFrame);
	else
		return video_frame_direct(frame, s, pFrame);
}

//static bool audio_frame(struct ff_frame *frame, void *opaque)
//{
//	struct ffmpeg_source *s = opaque;
//
//	struct obs_source_audio audio_data = { 0 };
//
//	uint64_t pts;
//
//	// Media ended
//	if (frame == NULL || frame->frame == NULL)
//		return true;
//
//	pts = (uint64_t)(frame->pts * 1000000000.0L);
//
//	int channels = av_frame_get_channels(frame->frame);
//
//	for (int i = 0; i < channels; i++)
//		audio_data.data[i] = frame->frame->data[i];
//
//	audio_data.samples_per_sec = frame->frame->sample_rate;
//	audio_data.frames = frame->frame->nb_samples;
//	audio_data.timestamp = pts;
//	audio_data.format =
//		convert_ffmpeg_sample_format(frame->frame->format);
//	audio_data.speakers = channels;
//
//	obs_source_output_audio(s->source, &audio_data);
//
//	return true;
//}



static void ffmpeg_source_start(struct stinger_info *s)
{
	if (s->demuxer != NULL)
		ff_demuxer_free(s->demuxer);

	s->demuxer = ff_demuxer_init();
	s->demuxer->options.is_hw_decoding = s->is_hw_decoding;
	s->demuxer->options.is_looping = false;

	ff_demuxer_set_callbacks(&s->demuxer->video_callbacks,
		video_frame, NULL,
		NULL, NULL, NULL, s);

	//ff_demuxer_set_callbacks(&s->demuxer->audio_callbacks,
	//	audio_frame, NULL,
	//	NULL, NULL, NULL, s);

	ff_demuxer_open(s->demuxer, s->path, NULL);
}




static void stinger_update(void *data, obs_data_t *settings)
{
	struct stinger_info *stinger = data;


	bool is_local_file = obs_data_get_bool(settings, "is_local_file");
	bool is_advanced = obs_data_get_bool(settings, "advanced");

	stinger->is_hw_decoding = obs_data_get_bool(settings, "hw_decode");
	stinger->is_forcing_scale = true;
	stinger->lastTime = 1.0f; //to make sure it plays on first scene change

	stinger->path = obs_data_get_string(settings, "stingerPath");
	stinger->cutFrame = obs_data_get_int(settings, "cutFrame");

	int numberOfFrames = checkInputFile(stinger->path);

	if (numberOfFrames > 1){
		stinger->validInput = true;
		stinger->numberOfFrames = numberOfFrames;
		obs_data_set_int(settings, "numberOfFrames", numberOfFrames);

		AVCodecContext *pCodecCtx = NULL;
		AVCodec *pCodec = NULL;
		AVFormatContext *pFormatCtx = NULL;

		if (avformat_open_input(&pFormatCtx, stinger->path, NULL, NULL) != 0)
		{
			stinger->validInput = false;
			return; // Couldn't open file
		}

		if (avformat_find_stream_info(pFormatCtx, NULL)<0)
		{
			stinger->validInput = false;
			return; // Couldn't find stream information
		}

		int videoStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, pCodec, 0);

		pCodecCtx = pFormatCtx->streams[videoStream]->codec;

		pCodec = avcodec_find_decoder(pCodecCtx->codec_id);

		if (pCodec == NULL)
		{
			stinger->validInput = false;
			return; // Codec not found
		}

		double duration = 
			(double)stinger->numberOfFrames / 
			(double)pCodecCtx->framerate.num * 
			(double)pCodecCtx->framerate.den * 
			1000.0;


		avcodec_close(pCodecCtx);
		avformat_close_input(&pFormatCtx);

		obs_transition_enable_fixed(stinger->source, true,
			(uint32_t)duration);
	}
	else
	{
		stinger->validInput = false;
		stinger->cutFrame = 1;
		stinger->numberOfFrames = 1;
		obs_data_set_int(settings, "numberOfFrames", 1);
		obs_data_set_int(settings, "cutFrame", 1);

		struct dstr path = { 0 };

		const char* file = obs_module_file("");
		dstr_copy(&path, file);
		bfree(file);
		dstr_cat(&path, "/NoStingerVideoLoaded.png");

		obs_enter_graphics();
		gs_image_file_free(&stinger->stinger_error_image);
		obs_leave_graphics();

		gs_image_file_init(&stinger->stinger_error_image, path.array);

		obs_enter_graphics();
		gs_image_file_init_texture(&stinger->stinger_error_image);
		obs_leave_graphics();
		stinger->stinger_texture = stinger->stinger_error_image.texture;
		dstr_free(&path);

		obs_transition_enable_fixed(stinger->source, true,
			3000);
	}
}

static void *stinger_create(obs_data_t *settings, obs_source_t *source)
{
	struct stinger_info *stinger;
	char *file = obs_module_file("stinger_transition.effect");
	gs_effect_t *effect;

	obs_enter_graphics();
	effect = gs_effect_create_from_file(file, NULL);
	obs_leave_graphics();
	bfree(file);

	if (!effect) {
		blog(LOG_ERROR, "Could not find stinger_transition.effect");
		return NULL;
	}

	stinger = bzalloc(sizeof(*stinger));

	stinger->effect = effect;
	stinger->ep_a_tex = gs_effect_get_param_by_name(effect, "a_tex");
	stinger->ep_b_tex = gs_effect_get_param_by_name(effect, "b_tex");

	stinger->source = source;

	stinger_update(stinger, settings);

	return stinger;
}

static void stinger_destroy(void *data)
{
	struct stinger_info *stinger = data;


	if (stinger->demuxer)
		ff_demuxer_free(stinger->demuxer);

	if (stinger->sws_ctx != NULL)
		sws_freeContext(stinger->sws_ctx);
	bfree(stinger->sws_data);

	obs_enter_graphics();
	if (!stinger->validInput)
	{
		gs_image_file_free(&stinger->stinger_error_image);
		stinger->stinger_texture = NULL;
	}
	gs_texture_destroy(stinger->stinger_texture);
	obs_leave_graphics();

	bfree(stinger);
}

static void stinger_callback(void *data, gs_texture_t *a, gs_texture_t *b,
		float t, uint32_t cx, uint32_t cy)
{
	struct stinger_info *stinger = data;

	if (t - stinger->lastTime < 0.0f) //new scene change
	{
		ffmpeg_source_start(stinger);
		stinger->curFrame = 0;
		stinger->lastTime = t;
	}
	
	if (stinger->curFrame < stinger->cutFrame - 1)
		gs_effect_set_texture(stinger->ep_a_tex, a);
	else
		gs_effect_set_texture(stinger->ep_a_tex, b);

	gs_effect_set_texture(stinger->ep_b_tex, stinger->stinger_texture);


	while (gs_effect_loop(stinger->effect, "Stinger"))
		gs_draw_sprite(NULL, 0, cx, cy);

	stinger->lastTime = t;
}

static void stinger_video_render(void *data, gs_effect_t *effect)
{
	struct stinger_info *stinger = data;
	obs_transition_video_render(stinger->source, stinger_callback);
	UNUSED_PARAMETER(effect);
}
//TODO AUDIO RENDER OF STINGER VIDEO
static float mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;;
}

static float mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

static bool stinger_audio_render(void *data, uint64_t *ts_out,
		struct obs_source_audio_mix *audio, uint32_t mixers,
		size_t channels, size_t sample_rate)
{
	struct stinger_info *stinger = data;
	return obs_transition_audio_render(stinger->source, ts_out,
		audio, mixers, channels, sample_rate, mix_a, mix_b);
}


static int checkInputFile(const char* file)
{
	int numberOfFrames = 0;

	if (!file)
		return 0;

	AVFormatContext *pFormatCtx = NULL;

	if (avformat_open_input(&pFormatCtx, file, NULL, NULL) != 0)
	{
		blog(LOG_WARNING, "Couldn't open stinger video file");
		return -1;
	}

	if (avformat_find_stream_info(pFormatCtx, NULL)<0)
	{
		blog(LOG_WARNING, "Couldn't find stinger video stream information");
		return -1;
	}

	int i;
	AVCodecContext *pCodecCtxOrig = NULL;
	AVCodecContext *pCodecCtx = NULL;
	AVCodec *pCodec = NULL;

	int videoStream = av_find_best_stream(pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, pCodec, 0);

	pCodecCtxOrig = pFormatCtx->streams[videoStream]->codec;

	pCodec = avcodec_find_decoder(pCodecCtxOrig->codec_id);
	if (pCodec == NULL) {
		blog(LOG_WARNING, "Unsupported codec of stinger video");
		return -1;
	}

	pCodecCtx = avcodec_alloc_context3(pCodec);

	if (avcodec_copy_context(pCodecCtx, pCodecCtxOrig) != 0)
	{
		blog(LOG_ERROR, "Couldn't copy codec context of stinger video");
		return -1;
	}

	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0){
		blog(LOG_ERROR, "Couldn't open codec of stinger video");
		return -1;
	}

	AVFrame *pFrame = NULL;
	pFrame = av_frame_alloc();

	int frameFinished;
	AVPacket packet;

	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == videoStream) {
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
			if (frameFinished) {
				numberOfFrames++;
			}
		}
		av_free_packet(&packet);
	}
	av_free(pFrame);

	avcodec_close(pCodecCtx);
	avcodec_close(pCodecCtxOrig);

	avformat_close_input(&pFormatCtx);

	return numberOfFrames;

}


static bool stingerPathModified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	char* prevPath = obs_data_get_string(settings, "prevPath");
	char* file = obs_data_get_string(settings, "stingerPath");
	int numberOfFrames = 0;

	obs_property_t *slider = obs_properties_get(props, "cutFrame");
	if (!slider)
		slider = obs_properties_add_int_slider(props, "cutFrame", "Transition at frame", 1, 1, 1);

	if (strcmp(file, prevPath) == 0)
	{
		numberOfFrames = obs_data_get_int(settings, "numberOfFrames");
		obs_property_int_set_limits(slider, 1, numberOfFrames, 1);
		return true;
	}

	struct dstr path = { 0 };
	dstr_copy(&path, file);
	
	numberOfFrames = checkInputFile(path.array);

	if (numberOfFrames > 1)
		obs_property_int_set_limits(slider, 1, numberOfFrames, 1);
	else
		obs_property_int_set_limits(slider, 1, 1, 1);

	obs_data_set_string(settings, "prevPath", path.array);

	dstr_free(&path);

	UNUSED_PARAMETER(property);
}


static obs_properties_t *stinger_properties(void *data)
{
	struct stinger_info *stinger = data;

	obs_properties_t *ppts = obs_properties_create();

	obs_property_t *pathProp = obs_properties_add_path(ppts, "stingerPath", "Path to stinger video", OBS_PATH_FILE, "", "");
	obs_property_set_modified_callback(pathProp, stingerPathModified);
	obs_properties_add_bool(ppts, "hw_decode",
		obs_module_text("HardwareDecode"));

	return ppts;
}


static void stinger_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "cutFrame", 1);
	obs_data_set_default_int(settings, "numberOfFrames", 1);
	obs_data_set_default_bool(settings, "validInput", false);
	obs_data_set_default_string(settings, "stingerPath", "");
#if defined(_WIN32)
	obs_data_set_default_bool(settings, "hw_decode", true);
#endif
}


struct obs_source_info stinger_transition = {
	.id = "stinger_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = stinger_get_name,
	.create = stinger_create,
	.destroy = stinger_destroy,
	.update = stinger_update,
	.video_render = stinger_video_render,
	.audio_render = stinger_audio_render,
	.get_properties = stinger_properties,
	.get_defaults = stinger_defaults
};
