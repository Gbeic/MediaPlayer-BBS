module;
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <jni.h>
#include "jnipp.h"
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}
module Media;

#define ThrowOnFailed(X) if((X)<0) throw (X)

using namespace jni;
using namespace MediaPlayer;

static bool Support(const AVCodec* codec, const AVHWDeviceType type)
{
	int index = 0;
	bool support = false;
	while (true) {
		const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
		if (!config) break;
		if (config->device_type == type) {
			support = true;
			break;
		}
		index++;
	}
	return support;
}

int VideoDecoder::RegisterMethods(JNIEnv* env)
{
	std::vector<JNINativeMethod> methods;
	methods.emplace_back(JNIMethod("open", "(Ljava/lang/String;II)J", Open));
	methods.emplace_back(JNIMethod("decode", "()V", (void(*)(JNIEnv*, jobject))Decode));
	methods.emplace_back(JNIMethod("renderTimeNative", "(D)V", (void(*)(JNIEnv*, jobject, jdouble))RenderTime));
	methods.emplace_back(JNIMethod("release", "(J)V", Release));
	return env->RegisterNatives(env->FindClass("net/hacker/mediaplayer/VideoDecoder"), methods.data(), methods.size());
}

VideoDecoder* VideoDecoder::Open(JNIEnv* env, jobject obj, const jstring path, const GLuint texture, const AVHWDeviceType type)
{
	try {
		auto ptr = std::make_unique<VideoDecoder>(toString(path), type, texture);
		auto [num, den] = ptr->format->streams[ptr->index]->avg_frame_rate;
		Object object(obj);
		if (num > 0 && den > 0) {
			object.set("frameRate", den / (double)num);
			object.set("framesPerSecond", num / (double)den);
		}
		object.set("width", ptr->context->width);
		object.set("height", ptr->context->height);
		object.set("duration", ptr->format->duration == AV_NOPTS_VALUE ? 0.0 : ptr->format->duration / (double)AV_TIME_BASE);
		object.set("hasAudio", av_find_best_stream(ptr->format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0);
		for (int attempts = 0; attempts < 240; attempts++) {
			if (ptr->Decode() >= 0) return ptr.release();
		}
		throw std::runtime_error("No video frame decoded while opening");
	}
	catch (...)
	{
		Throw(env, "Open failed");
		return nullptr;
	}
}

void VideoDecoder::Decode(JNIEnv*, const jobject obj)
{
	GetPtr<VideoDecoder>(obj)->Decode();
}

void VideoDecoder::RenderTime(JNIEnv*, const jobject obj, const jdouble seconds)
{
	GetPtr<VideoDecoder>(obj)->RenderTime(seconds);
}

void VideoDecoder::Release(JNIEnv*, jclass, const jlong ptr)
{
	delete (VideoDecoder*)ptr;
}

VideoDecoder::VideoDecoder(const std::string& url, const AVHWDeviceType hw_type, GLuint tex)
{
	format = avformat_alloc_context();
	if (!format) throw std::runtime_error("Failed to allocate AVFormatContext");

	ThrowOnFailed(avformat_open_input(&format, url.data(), nullptr, nullptr));
	ThrowOnFailed(avformat_find_stream_info(format, nullptr));

	index = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	if (index < 0) throw std::runtime_error("Can't find video stream in input file");

	const AVCodecParameters* origin_par = format->streams[index]->codecpar;
	codec = avcodec_find_decoder(origin_par->codec_id);
	if (!codec) throw std::runtime_error("No suitable codec found");

	context = avcodec_alloc_context3(codec);
	if (!context) throw std::runtime_error("Failed to allocate codec context");

	type = hw_type;

	if (type != AV_HWDEVICE_TYPE_NONE) {
		if (!Support(codec, type)) throw std::runtime_error("HW not supported");
		AVBufferRef* hw_device_ctx;
		ThrowOnFailed(av_hwdevice_ctx_create(&hw_device_ctx, type, nullptr, nullptr, 0));
		context->hw_device_ctx = hw_device_ctx;
	}

	if (avcodec_parameters_to_context(context, origin_par) < 0)
		throw std::runtime_error("Error initializing the decoder context");

	ThrowOnFailed(avcodec_open2(context, codec, nullptr));

	packet = av_packet_alloc();
	if (!packet) throw std::runtime_error("Failed to allocate packet");

	frame = av_frame_alloc();
	if (!frame) throw std::runtime_error("Failed to allocate frame");

	vframe = std::make_unique<VideoFrame>(type, (AVHWDeviceContext*)(context->hw_device_ctx ? context->hw_device_ctx->data : nullptr), tex);
}


VideoDecoder::~VideoDecoder()
{
	// VideoFrame 可能仍持有硬件帧、CUDA 上下文或图形互操作资源，必须先释放。
	vframe.reset();
	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&context);
	avformat_free_context(format);
}

int VideoDecoder::Decode()
{
	for (;;) {
		int ret = av_read_frame(format, packet);
		if (ret < 0) {
			av_packet_unref(packet);
			return ret;
		}

		if (packet->stream_index != index) {
			av_packet_unref(packet);
			continue;
		}

		int result = avcodec_send_packet(context, packet);
		av_packet_unref(packet);
		if (result < 0 && result != AVERROR(EAGAIN)) {
			av_log(nullptr, AV_LOG_ERROR, "Error submitting a packet for decoding\n");
			continue;
		}

		bool success = false;
		for (;;) {
			result = avcodec_receive_frame(context, frame);
			if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) break;
			if (result < 0) {
				av_log(nullptr, AV_LOG_ERROR, "Error decoding frame\n");
				av_frame_unref(frame);
				break;
			}

			const auto timestamp = frame->best_effort_timestamp != AV_NOPTS_VALUE
				? frame->best_effort_timestamp
				: frame->pts;
			if (timestamp != AV_NOPTS_VALUE) {
				const auto stream = format->streams[index];
				const auto startTimestamp = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
				const auto decodedSeconds = (timestamp - startTimestamp) * av_q2d(stream->time_base);
				if (std::isfinite(decodedSeconds)) {
					currentFrameSeconds = decodedSeconds;
					hasCurrentFrame = true;
				}
			}
			vframe->Update(frame, context->hwaccel != nullptr);
			av_frame_unref(frame);
			success = true;
		}
		if (success) return 0;
	}
}

void VideoDecoder::RenderTime(double seconds)
{
	if (!std::isfinite(seconds) || seconds < 0) seconds = 0;

	AVStream* stream = format->streams[index];
	const auto frameDuration = stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0
		? av_q2d(av_inv_q(stream->avg_frame_rate))
		: 1.0 / 60.0;
	const auto seekThreshold = std::max(0.25, frameDuration * 8.0);
	const auto frameTolerance = frameDuration * 0.5;
	const bool needsSeek = !hasCurrentFrame
		|| seconds < currentFrameSeconds - frameTolerance
		|| seconds > currentFrameSeconds + seekThreshold;

	if (needsSeek) {
		const auto startTimestamp = stream->start_time == AV_NOPTS_VALUE ? 0 : stream->start_time;
		const int64_t timestamp = startTimestamp
			+ av_rescale_q((int64_t)(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, stream->time_base);
		if (av_seek_frame(format, index, timestamp, AVSEEK_FLAG_BACKWARD) < 0) return;
		// 时间轴跳转后必须清空解码器缓存，否则会混入 seek 前的旧帧。
		avcodec_flush_buffers(context);
		hasCurrentFrame = false;

		// seek 到关键帧后连续解码，直到纹理对应的帧真正到达目标时间。
		for (int attempts = 0; attempts < 240; attempts++) {
			const int result = Decode();
			if (result == AVERROR_EOF) return;
			if (hasCurrentFrame && currentFrameSeconds >= seconds - frameTolerance) return;
		}
		return;
	}

	if (seconds <= currentFrameSeconds + frameTolerance) return;

	// 正向播放时一个游戏 tick 可能跨越多帧，不能只消费一个 packet。
	for (int attempts = 0; attempts < 240; attempts++) {
		const int result = Decode();
		if (result == AVERROR_EOF) return;
		if (hasCurrentFrame && currentFrameSeconds >= seconds - frameTolerance) return;
	}
}
