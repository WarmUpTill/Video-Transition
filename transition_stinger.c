#include <obs-module.h>
#include <graphics/image-file.h>
#include <util/dstr.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/pixfmt.h>

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

	AVFormatContext *pFormatCtx;
	AVCodecContext *pCodecCtxOrig;
	AVCodecContext *pCodecCtx;
	AVCodec *pCodec;
	AVFrame *pFrame;
	AVFrame *pFrameRGB;
	struct SwsContext *sws_ctx;
	int videoStream;
	
};

static const char *stinger_get_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return obs_module_text("StingerTransition");
}

static void stinger_update(void *data, obs_data_t *settings)
{
	struct stinger_info *stinger = data;

	stinger->path = obs_data_get_string(settings, "stingerPath");
	stinger->cutFrame = obs_data_get_int(settings, "cutFrame");

	int numberOfFrames = checkInputFile(stinger->path);

	if (numberOfFrames > 1){
		stinger->validInput = true;
		stinger->numberOfFrames = numberOfFrames;
		obs_data_set_int(settings, "numberOfFrames", numberOfFrames);

		sws_freeContext(stinger->sws_ctx);

		av_free(stinger->pFrame);
		av_free(stinger->pFrameRGB);

		avcodec_close(stinger->pCodecCtx);
		avcodec_close(stinger->pCodecCtxOrig);

		avformat_close_input(&stinger->pFormatCtx);

		stinger->pFormatCtx = NULL;

		if (avformat_open_input(&stinger->pFormatCtx, stinger->path, NULL, NULL) != 0)
		{
			stinger->validInput = false;
			return; // Couldn't open file
		}

		if (avformat_find_stream_info(stinger->pFormatCtx, NULL)<0)
		{
			stinger->validInput = false;
			return; // Couldn't find stream information
		}

		int i;
		stinger->pCodecCtxOrig = NULL;
		stinger->pCodecCtx = NULL;
		stinger->pCodec = NULL;
		stinger->videoStream = 0;

		stinger->videoStream = av_find_best_stream(stinger->pFormatCtx, AVMEDIA_TYPE_VIDEO, -1, -1, stinger->pCodec, 0);

		stinger->pCodecCtxOrig = stinger->pFormatCtx->streams[stinger->videoStream]->codec;

		stinger->pCodec = avcodec_find_decoder(stinger->pCodecCtxOrig->codec_id);
		if (stinger->pCodec == NULL)
		{
			stinger->validInput = false;
			return; // Codec not found
		}
		stinger->pCodecCtx = avcodec_alloc_context3(stinger->pCodec);
		if (avcodec_copy_context(stinger->pCodecCtx, stinger->pCodecCtxOrig) != 0)
		{
			stinger->validInput = false;
			return; // Error copying codec context
		}
		// Open codec
		if (avcodec_open2(stinger->pCodecCtx, stinger->pCodec, NULL)<0)
		{
			stinger->validInput = false;
			return; // Could not open codec
		}

		stinger->pFrame = NULL;
		stinger->pFrameRGB = NULL;

		stinger->pFrame = av_frame_alloc();
		stinger->pFrameRGB = av_frame_alloc();

		stinger->sws_ctx = NULL;

		// initialize SWS context for software scaling
		stinger->sws_ctx = sws_getContext(stinger->pCodecCtx->width,
			stinger->pCodecCtx->height,
			stinger->pCodecCtx->pix_fmt,
			stinger->pCodecCtx->width,
			stinger->pCodecCtx->height,
			AV_PIX_FMT_BGRA,
			SWS_BILINEAR,
			NULL,
			NULL,
			NULL
			);

		stinger->pFrameRGB->format = AV_PIX_FMT_BGRA;
		stinger->pFrameRGB->width = stinger->pCodecCtx->width;
		stinger->pFrameRGB->height = stinger->pCodecCtx->height;

		av_image_alloc(stinger->pFrameRGB->data,
			stinger->pFrameRGB->linesize,
			stinger->pCodecCtx->width,
			stinger->pCodecCtx->height,
			AV_PIX_FMT_BGRA,
			16
			);

		stinger->curFrame = 0;

		double duration = (double)stinger->numberOfFrames / (double)stinger->pCodecCtxOrig->framerate.num * (double)stinger->pCodecCtxOrig->framerate.den * 1000.0;

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

	sws_freeContext(stinger->sws_ctx);

	av_free(stinger->pFrame);
	av_free(stinger->pFrameRGB);

	avcodec_close(stinger->pCodecCtx);
	avcodec_close(stinger->pCodecCtxOrig);

	avformat_close_input(&stinger->pFormatCtx);

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

static void setNextFrameTexture(struct stinger_info *stinger)
{
	if (!stinger || !stinger->validInput)
		return;

	//av_log_set_callback(my_log_callback);
	//av_log_set_level(AV_LOG_VERBOSE);

	int frameFinished;
	AVPacket packet;
	int i = 0;

	if (av_read_frame(stinger->pFormatCtx, &packet) >= 0)
	{
		if (packet.stream_index == stinger->videoStream)
		{
			avcodec_decode_video2(stinger->pCodecCtx, stinger->pFrame, &frameFinished, &packet);

			if (frameFinished) {

				// Convert the image from its native format to BGRA
				sws_scale(stinger->sws_ctx, (uint8_t const * const *)stinger->pFrame->data,
					stinger->pFrame->linesize, 0, stinger->pCodecCtx->height,
					stinger->pFrameRGB->data, stinger->pFrameRGB->linesize);

				obs_enter_graphics();
				gs_texture_destroy(stinger->stinger_texture);
				stinger->stinger_texture = gs_texture_create(
					stinger->pFrameRGB->width, stinger->pFrameRGB->height, 5, 1, //5 is AV_PIX_FMT_BGRA
					(const uint8_t**)&stinger->pFrameRGB->data, 0);
				obs_leave_graphics();
			}
		}
		av_free_packet(&packet);
	}
	stinger->curFrame++;
}

static void seekBeginning(struct stinger_info *stinger){
	if (!stinger->validInput)
		return;
	av_seek_frame(stinger->pFormatCtx,
		stinger->videoStream,
		0, AVSEEK_FLAG_BACKWARD);
}

static void stinger_callback(void *data, gs_texture_t *a, gs_texture_t *b,
		float t, uint32_t cx, uint32_t cy)
{
	struct stinger_info *stinger = data;

	if (t - stinger->lastTime < 0.0f)
	{
		seekBeginning(stinger);
		stinger->curFrame = 0;
		stinger->lastTime = t;
	}

	setNextFrameTexture(stinger); //increases curFrame
	
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

	//av_log_set_callback(my_log_callback);
	//av_log_set_level(AV_LOG_VERBOSE);

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

	return ppts;
}


static void stinger_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "cutFrame", 1);
	obs_data_set_default_int(settings, "numberOfFrames", 1);
	obs_data_set_default_bool(settings, "validInput", false);
	obs_data_set_default_string(settings, "stingerPath", "");
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

//TODO add float time cut slider