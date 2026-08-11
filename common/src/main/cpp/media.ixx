module;
#include <jni.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <memory>
#include <thread>
#include <vector>
#include <tuple>
#include <d3d12.h>
#include <d3d11_4.h>
#include <vector>
#include <wrl.h>
#include <ffnvcodec/dynlink_loader.h>
#include "jnipp.h"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
export module Media;

using namespace Microsoft::WRL;

namespace MediaPlayer
{
	inline void Throw(JNIEnv* env, const char* msg)
	{
		env->ThrowNew(env->FindClass("com/sun/jdi/NativeMethodException"), msg);
	}

	template <typename F> requires std::is_function_v<std::remove_pointer_t<F>>
	consteval JNINativeMethod JNIMethod(const char* name, const char* sig, F ptr)
	{
		return { const_cast<char*>(name),const_cast<char*>(sig), (void*)ptr };
	}

	template<typename T>
	T* GetPtr(const jni::Object& obj)
	{
		return (T*)obj.get<jlong>("ptr");
	}

	inline std::string toString(const jstring str)
	{
		std::string result;
		if (str != nullptr)
		{
			JNIEnv* env = jni::env();
			const char* chars = env->GetStringUTFChars(str, nullptr);
			result.assign(chars, env->GetStringUTFLength(str));
			env->ReleaseStringUTFChars(str, chars);
			env->DeleteLocalRef(str);
		}
		return result;
	}

	struct Descriptor
	{
		D3D12_CPU_DESCRIPTOR_HANDLE CPUHandle;
		D3D12_GPU_DESCRIPTOR_HANDLE GPUHandle;
	};
}

export namespace MediaPlayer
{
	class VideoFrame final
	{
		ComPtr<ID3D11Device> D3D11Device;
		ComPtr<ID3D11DeviceContext> D3D11DeviceContext;
		ComPtr<ID3D11Texture2D> ComputeResult;
		ComPtr<ID3D11ComputeShader> ComputeShader;
		ComPtr<ID3D11UnorderedAccessView> UAV;
		ComPtr<ID3D11ShaderResourceView> srv1, srv2;   // D3D11 缓存的解码纹理 SRV，纹理或 slice 变化时才重建
		ID3D11Texture2D* cachedD3D11Tex{};             // 上次 SRV 对应的解码纹理
		int64_t cachedD3D11Slice{ -1 };                // 上次 SRV 对应的纹理 array slice
		ComPtr<ID3D12Device> D3D12Device;
		ComPtr<ID3D12CommandQueue> CommandQueue;
		ComPtr<ID3D12CommandAllocator> CommandAllocator;
		ComPtr<ID3D12GraphicsCommandList> CommandList;
		ComPtr<ID3D12RootSignature> RootSignature;
		ComPtr<ID3D12PipelineState> PSO;
		ComPtr<ID3D12DescriptorHeap> DescriptorHeap;
		ComPtr<ID3D12Resource> OutputBuffer;
		ComPtr<ID3D12Fence> fence;
		HANDLE FenceEvent;
		HANDLE SharedHandle{};
		uint32_t memory{};
		uint64_t FenceValue{};
		Descriptor SRV0, SRV1, UAV0;
		SwsContext* sws{};                        // 软解复用的 sws 上下文
		AVFrame* swsOutput{};                     // 软解复用的 RGBA 输出帧
		int swsWidth{}, swsHeight{};              // sws 上下文对应的输入尺寸
		AVPixelFormat swsFormat{};                // sws 上下文对应的输入像素格式
		void* cu_surface{};                       // CUDA surface 对象，跨帧复用
		int cu_surface_width{}, cu_surface_height{}; // surface 对应的尺寸，变化时重建
		HANDLE DXNVDevice;
		void* TextureObject;
		CudaFunctions* cuda;
		CUcontext cu_ctx;
		CUstream stream;
		CUgraphicsResource cu_res;
		GLuint hw_texture;
		AVHWDeviceType hwtype;
		bool hwaccel, init;

		void UpdateSW(const AVFrame* frame);
		void UpdateD3D11(const AVFrame* frame);
		void UpdateD3D12(const AVFrame* frame);
		void UpdateCUDA(const AVFrame* frame);
	public:
		VideoFrame(AVHWDeviceType type, const AVHWDeviceContext* hwctx, GLuint tex);
		~VideoFrame();
		void Update(const AVFrame* frame, bool hwaccel = false);
	};

	class VideoDecoder final
	{
		std::unique_ptr<VideoFrame> vframe;
		AVFormatContext* format;
		const AVCodec* codec;
		AVCodecContext* context;
		AVHWDeviceType type;
		int index;
		AVPacket* packet;
		AVFrame* frame;
		AVFrame* pendingFrame;
		AVFrame* readyFrame;
		AVFrame* presentationFrame;
		double currentFrameSeconds{};
		double requestedSeconds{};
		double readyFrameSeconds{};
		uint64_t requestGeneration{};
		uint64_t readyFrameGeneration{};
		bool hasCurrentFrame{};
		bool hasPendingFrame{};
		bool hasRequestedTime{};
		bool hasReadyFrame{};
		bool presentationInProgress{};
		bool timelineWorkerStarted{};
		bool stoppingTimelineWorker{};
		// 无锁快速路径标志：画面已就绪且目标时间未变（暂停/静止画面）时为 false，
		// 渲染线程可直接返回，避免暂停时每帧加锁唤醒后台线程。
		std::atomic_bool quickBusy{ false };
		// 最近一次完整路径处理的目标时间，配合 quickBusy 判断是否可走快速路径。
		std::atomic<double> lastQuickSeconds{ -1.0 };
		std::thread timelineWorker;
		std::mutex timelineMutex;
		std::condition_variable timelineCondition;

		int Decode(bool updateTexture = true);
		bool BeginSeek(double seconds);
		bool DecodeTimelineTarget(double seconds, uint64_t& generation);
		void PublishPendingFrame(uint64_t generation);
		void RunTimelineWorker();
		void EnsureTimelineWorker();
		void RenderTime(double seconds);
	public:
		explicit VideoDecoder(const std::string& url, AVHWDeviceType type, GLuint tex);
		~VideoDecoder();
		static int RegisterMethods(JNIEnv* env);
		static VideoDecoder* Open(JNIEnv* env, jobject obj, jstring path, GLuint texture, AVHWDeviceType type);
		static void Decode(JNIEnv* env, jobject obj);
		static void RenderTime(JNIEnv* env, jobject obj, jdouble seconds);
		static void Release(JNIEnv*, jclass, jlong ptr);
	};

	class AudioDecoder final
	{
		AVFormatContext* format;
		const AVCodec* codec;
		AVCodecContext* context;
		int index;
		AVPacket* packet;
		AVFrame* frame;
		SwrContext* swr{};
		AVChannelLayout layout{};

		int Decode(std::vector<std::tuple<size_t, AVFrame*>>& vec, size_t& size);
		jobject Decode(bool mono);
	public:
		explicit AudioDecoder(const std::string& url);
		~AudioDecoder();
		static int RegisterMethods(JNIEnv* env);
		static AudioDecoder* Open(JNIEnv* env, jobject obj, jstring path);
		static jobject Decode(JNIEnv* env, jobject obj, bool mono);
		static void Release(JNIEnv*, jclass, jlong ptr);
	};

	void Init(JNIEnv* env, jclass, jlong proc);

	int RegisterMethods(JNIEnv* env)
	{
		av_log_set_level(AV_LOG_QUIET);
		std::vector<JNINativeMethod> methods;
		methods.emplace_back(JNIMethod("init", "(J)V", Init));
		return env->RegisterNatives(env->FindClass("net/hacker/mediaplayer/MediaPlayer"), methods.data(), methods.size()) + VideoDecoder::RegisterMethods(env) + AudioDecoder::RegisterMethods(env);
	}
}
