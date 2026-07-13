#include <windows.h>
#include <gl/GL.h>
#include <jni.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace
{
    std::string ToString(JNIEnv* env, jstring value)
    {
        if (value == nullptr) return {};

        const char* chars = env->GetStringUTFChars(value, nullptr);
        std::string result(chars == nullptr ? "" : chars);
        env->ReleaseStringUTFChars(value, chars);
        return result;
    }

    void Throw(JNIEnv* env, const char* message)
    {
        env->ThrowNew(env->FindClass("java/lang/RuntimeException"), message);
    }

    template<typename T>
    T GetField(JNIEnv* env, jobject object, const char* name, const char* signature)
    {
        jfieldID field = env->GetFieldID(env->GetObjectClass(object), name, signature);
        return reinterpret_cast<T>(env->GetLongField(object, field));
    }

    void SetDouble(JNIEnv* env, jobject object, const char* name, double value)
    {
        jfieldID field = env->GetFieldID(env->GetObjectClass(object), name, "D");
        env->SetDoubleField(object, field, value);
    }

    void SetInt(JNIEnv* env, jobject object, const char* name, int value)
    {
        jfieldID field = env->GetFieldID(env->GetObjectClass(object), name, "I");
        env->SetIntField(object, field, value);
    }

    void SetBoolean(JNIEnv* env, jobject object, const char* name, bool value)
    {
        jfieldID field = env->GetFieldID(env->GetObjectClass(object), name, "Z");
        env->SetBooleanField(object, field, value ? JNI_TRUE : JNI_FALSE);
    }

    struct SoftwareVideoDecoder
    {
        AVFormatContext* format = nullptr;
        const AVCodec* codec = nullptr;
        AVCodecContext* context = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        AVFrame* rgba = nullptr;
        SwsContext* sws = nullptr;
        int videoIndex = -1;
        int texture = 0;
        bool textureInitialized = false;

        SoftwareVideoDecoder(const std::string& path, int textureId) : texture(textureId)
        {
            if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0) {
                throw std::runtime_error("无法打开视频文件");
            }

            if (avformat_find_stream_info(format, nullptr) < 0) {
                throw std::runtime_error("无法读取视频流信息");
            }

            videoIndex = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (videoIndex < 0) {
                throw std::runtime_error("视频文件中没有可解码的视频流");
            }

            const AVCodecParameters* parameters = format->streams[videoIndex]->codecpar;
            codec = avcodec_find_decoder(parameters->codec_id);
            if (codec == nullptr) {
                throw std::runtime_error("找不到合适的视频解码器");
            }

            context = avcodec_alloc_context3(codec);
            if (context == nullptr) {
                throw std::runtime_error("无法创建视频解码上下文");
            }

            if (avcodec_parameters_to_context(context, parameters) < 0) {
                throw std::runtime_error("无法初始化视频解码参数");
            }

            if (avcodec_open2(context, codec, nullptr) < 0) {
                throw std::runtime_error("无法打开视频解码器");
            }

            packet = av_packet_alloc();
            frame = av_frame_alloc();
            rgba = av_frame_alloc();

            if (packet == nullptr || frame == nullptr || rgba == nullptr) {
                throw std::runtime_error("无法分配视频帧缓存");
            }

            rgba->format = AV_PIX_FMT_RGBA;
            rgba->width = context->width;
            rgba->height = context->height;

            if (av_image_alloc(rgba->data, rgba->linesize, rgba->width, rgba->height, AV_PIX_FMT_RGBA, 1) < 0) {
                throw std::runtime_error("无法分配 RGBA 转换缓存");
            }
        }

        ~SoftwareVideoDecoder()
        {
            if (rgba != nullptr) {
                av_freep(&rgba->data[0]);
            }

            sws_freeContext(sws);
            av_frame_free(&rgba);
            av_frame_free(&frame);
            av_packet_free(&packet);
            avcodec_free_context(&context);

            if (format != nullptr) {
                avformat_close_input(&format);
            }
        }

        double FrameRate() const
        {
            AVRational rate = format->streams[videoIndex]->avg_frame_rate;
            if (rate.num <= 0 || rate.den <= 0) {
                rate = format->streams[videoIndex]->r_frame_rate;
            }

            return rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0.0;
        }

        double Duration() const
        {
            if (format->duration == AV_NOPTS_VALUE) return 0.0;
            return format->duration / static_cast<double>(AV_TIME_BASE);
        }

        bool HasAudio() const
        {
            return av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0) >= 0;
        }

        bool Decode()
        {
            while (av_read_frame(format, packet) >= 0) {
                if (packet->stream_index != videoIndex) {
                    av_packet_unref(packet);
                    continue;
                }

                int sendResult = avcodec_send_packet(context, packet);
                av_packet_unref(packet);
                if (sendResult < 0) continue;

                while (true) {
                    int receiveResult = avcodec_receive_frame(context, frame);
                    if (receiveResult == AVERROR(EAGAIN) || receiveResult == AVERROR_EOF) break;
                    if (receiveResult < 0) return false;

                    Upload(frame);
                    av_frame_unref(frame);
                    return true;
                }
            }

            return false;
        }

        void RenderTime(double seconds)
        {
            seconds = std::max(0.0, seconds);
            AVStream* stream = format->streams[videoIndex];
            int64_t timestamp = av_rescale_q(static_cast<int64_t>(seconds * AV_TIME_BASE), AV_TIME_BASE_Q, stream->time_base);

            // 时间轴跳转后清空解码器缓存，避免 seek 前的旧帧混入当前画面。
            if (av_seek_frame(format, videoIndex, timestamp, AVSEEK_FLAG_BACKWARD) >= 0) {
                avcodec_flush_buffers(context);
            }

            // 第一阶段先解到目标时间附近。后续可以继续比较 best_effort_timestamp，挑选更接近目标秒数的帧。
            for (int attempts = 0; attempts < 120; attempts++) {
                if (Decode()) return;
            }
        }

        void Upload(const AVFrame* source)
        {
            sws = sws_getCachedContext(
                    sws,
                    source->width,
                    source->height,
                    static_cast<AVPixelFormat>(source->format),
                    rgba->width,
                    rgba->height,
                    AV_PIX_FMT_RGBA,
                    SWS_BILINEAR,
                    nullptr,
                    nullptr,
                    nullptr
            );

            if (sws == nullptr) return;

            sws_scale(sws, source->data, source->linesize, 0, source->height, rgba->data, rgba->linesize);

            glBindTexture(GL_TEXTURE_2D, texture);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

            if (!textureInitialized) {
                textureInitialized = true;
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->width, rgba->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data[0]);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rgba->width, rgba->height, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data[0]);
            }
        }
    };

    SoftwareVideoDecoder* GetDecoder(JNIEnv* env, jobject object)
    {
        return GetField<SoftwareVideoDecoder*>(env, object, "ptr", "J");
    }

    jlong VideoOpen(JNIEnv* env, jobject object, jstring path, jint texture, jint)
    {
        try {
            auto* decoder = new SoftwareVideoDecoder(ToString(env, path), texture);
            double fps = decoder->FrameRate();

            SetDouble(env, object, "framesPerSecond", fps);
            SetDouble(env, object, "frameRate", fps > 0.0 ? 1.0 / fps : 0.0);
            SetInt(env, object, "width", decoder->context->width);
            SetInt(env, object, "height", decoder->context->height);
            SetDouble(env, object, "duration", decoder->Duration());
            SetBoolean(env, object, "hasAudio", decoder->HasAudio());

            decoder->Decode();
            return reinterpret_cast<jlong>(decoder);
        } catch (const std::exception& e) {
            Throw(env, e.what());
            return 0;
        }
    }

    void VideoDecode(JNIEnv* env, jobject object)
    {
        SoftwareVideoDecoder* decoder = GetDecoder(env, object);
        if (decoder != nullptr) decoder->Decode();
    }

    void VideoRenderTime(JNIEnv* env, jobject object, jdouble seconds)
    {
        SoftwareVideoDecoder* decoder = GetDecoder(env, object);
        if (decoder != nullptr) decoder->RenderTime(seconds);
    }

    void VideoRelease(JNIEnv*, jclass, jlong ptr)
    {
        delete reinterpret_cast<SoftwareVideoDecoder*>(ptr);
    }

    jlong AudioOpen(JNIEnv*, jobject, jstring)
    {
        return 1;
    }

    void AudioRelease(JNIEnv*, jclass, jlong)
    {
    }

    jobject AudioDecode(JNIEnv* env, jobject, jboolean mono)
    {
        jclass bufferClass = env->FindClass("java/nio/ByteBuffer");
        jmethodID allocateDirect = env->GetStaticMethodID(bufferClass, "allocateDirect", "(I)Ljava/nio/ByteBuffer;");
        jobject buffer = env->CallStaticObjectMethod(bufferClass, allocateDirect, 0);
        jclass audioClass = env->FindClass("net/hacker/mediaplayer/Audio");
        jmethodID constructor = env->GetMethodID(audioClass, "<init>", "(Ljava/nio/ByteBuffer;I)V");
        return env->NewObject(audioClass, constructor, buffer, mono ? 1 : 2);
    }

    void MediaInit(JNIEnv*, jclass, jlong)
    {
        // 软件解码版只使用 OpenGL 1.1 的基础纹理上传函数，不需要额外加载扩展函数。
    }

    int Register(JNIEnv* env, const char* className, JNINativeMethod* methods, jint count)
    {
        jclass clazz = env->FindClass(className);
        return env->RegisterNatives(clazz, methods, count);
    }

    JNINativeMethod Method(const char* name, const char* signature, void* function)
    {
        return {const_cast<char*>(name), const_cast<char*>(signature), function};
    }
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*)
{
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_21) != JNI_OK) return JNI_EVERSION;

    JNINativeMethod mediaMethods[] = {
            Method("init", "(J)V", reinterpret_cast<void*>(MediaInit))
    };
    JNINativeMethod videoMethods[] = {
            Method("open", "(Ljava/lang/String;II)J", reinterpret_cast<void*>(VideoOpen)),
            Method("decode", "()V", reinterpret_cast<void*>(VideoDecode)),
            Method("renderTimeNative", "(D)V", reinterpret_cast<void*>(VideoRenderTime)),
            Method("release", "(J)V", reinterpret_cast<void*>(VideoRelease))
    };
    JNINativeMethod audioMethods[] = {
            Method("open", "(Ljava/lang/String;)J", reinterpret_cast<void*>(AudioOpen)),
            Method("decode", "(Z)Lnet/hacker/mediaplayer/Audio;", reinterpret_cast<void*>(AudioDecode)),
            Method("release", "(J)V", reinterpret_cast<void*>(AudioRelease))
    };

    if (Register(env, "net/hacker/mediaplayer/MediaPlayer", mediaMethods, 1) != JNI_OK) return JNI_ERR;
    if (Register(env, "net/hacker/mediaplayer/VideoDecoder", videoMethods, 4) != JNI_OK) return JNI_ERR;
    if (Register(env, "net/hacker/mediaplayer/AudioDecoder", audioMethods, 3) != JNI_OK) return JNI_ERR;

    return JNI_VERSION_21;
}
