module;
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <ffnvcodec/dynlink_loader.h>
#include <stdexcept>
#include <jni.h>
#include "jnipp.h"
#include "gl.h"
#include "wgl.h"
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/hwcontext_d3d12va.h>
#include <libavutil/hwcontext_cuda.h>
#include <libswscale/swscale.h>
}
module Media;

import Resource;

using namespace MediaPlayer;

void VideoFrame::UpdateSW(const AVFrame* frame)
{
	// sws 上下文与 RGBA 输出帧跨帧复用，仅当输入尺寸或像素格式变化时重建，
	// 避免每帧 sws_getContext + av_frame_alloc 的开销（软解兜底路径的卡顿源）。
	const auto format = (AVPixelFormat)frame->format;
	if (!sws || swsWidth != frame->width || swsHeight != frame->height || swsFormat != format)
	{
		if (sws) sws_freeContext(sws);
		if (swsOutput) av_frame_free(&swsOutput);
		sws = sws_getContext(frame->width, frame->height, format, frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
		swsOutput = av_frame_alloc();
		swsOutput->format = AV_PIX_FMT_RGBA;
		swsOutput->width = frame->width;
		swsOutput->height = frame->height;
		av_frame_get_buffer(swsOutput, 1);
		swsWidth = frame->width;
		swsHeight = frame->height;
		swsFormat = format;
	}
	if (!sws || !swsOutput) return;
	sws_scale(sws, frame->data, frame->linesize, 0, frame->height, swsOutput->data, swsOutput->linesize);
	glBindTexture(GL_TEXTURE_2D, hw_texture);
	if (!init) {
		init = true;
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, swsOutput->width, swsOutput->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	}
	// ROW_LENGTH 单位为像素，RGBA 每像素 4 字节。
	glPixelStorei(GL_UNPACK_ROW_LENGTH, swsOutput->linesize[0] / 4);
	glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
	glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, swsOutput->width, swsOutput->height, GL_RGBA, GL_UNSIGNED_BYTE, swsOutput->data[0]);
}

void VideoFrame::UpdateD3D11(const AVFrame* frame)
{
	auto tex = (ID3D11Texture2D*)frame->data[0];
	auto index = (int64_t)frame->data[1];
	D3D11_TEXTURE2D_DESC desc;
	tex->GetDesc(&desc);
	// 解码器帧池的纹理是共享 array texture，不同帧通过 slice（frame->data[1]）区分。
	// SRV 绑定在（纹理, slice）上，两者都变化时才重建，避免每帧 CreateShaderResourceView 的开销。
	if (cachedD3D11Tex != tex || cachedD3D11Slice != index)
	{
		cachedD3D11Tex = tex;
		cachedD3D11Slice = index;
		srv1 = nullptr;
		srv2 = nullptr;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R8_UNORM;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Texture2DArray.MipLevels = 1;
		srvDesc.Texture2DArray.ArraySize = 1;
		srvDesc.Texture2DArray.FirstArraySlice = (UINT)index;
		D3D11Device->CreateShaderResourceView(tex, &srvDesc, &srv1);
		srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		D3D11Device->CreateShaderResourceView(tex, &srvDesc, &srv2);
	}
	ID3D11ShaderResourceView* srvs[] = { srv1.Get(), srv2.Get() };
	if (ComputeResult == nullptr)
	{
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.ArraySize = 1;
		desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		desc.Usage = D3D11_USAGE_DEFAULT;
		D3D11Device->CreateTexture2D(&desc, nullptr, &ComputeResult);
		D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = desc.Format;
		ud.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		D3D11Device->CreateUnorderedAccessView(ComputeResult.Get(), &ud, &UAV);
		TextureObject = wglDXRegisterObjectNV(DXNVDevice, ComputeResult.Get(), hw_texture, GL_TEXTURE_2D, WGL_ACCESS_READ_ONLY_NV);
		wglDXLockObjectsNV(DXNVDevice, 1, &TextureObject);
	}
	wglDXUnlockObjectsNV(DXNVDevice, 1, &TextureObject);
	D3D11DeviceContext->CSSetShader(ComputeShader.Get(), nullptr, 0);
	D3D11DeviceContext->CSSetShaderResources(0, 2, srvs);
	D3D11DeviceContext->CSSetUnorderedAccessViews(0, 1, UAV.GetAddressOf(), nullptr);
	D3D11DeviceContext->Dispatch(ceil(desc.Width / 32.f), ceil(desc.Height / 32.f), 1);
	wglDXLockObjectsNV(DXNVDevice, 1, &TextureObject);
}

void VideoFrame::UpdateD3D12(const AVFrame* frame)
{
	auto df = (AVD3D12VAFrame*)frame->data[0];
	auto tex = df->texture;
	auto td = tex->GetDesc();

	// 首次创建输出纹理、UAV、共享句柄与 GL 内存对象。
	if (OutputBuffer == nullptr)
	{
		D3D12_HEAP_PROPERTIES prop{};
		D3D12_RESOURCE_DESC rd{};
		prop.Type = D3D12_HEAP_TYPE_DEFAULT;
		rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		rd.Width = td.Width;
		rd.Height = td.Height;
		rd.DepthOrArraySize = 1;
		rd.MipLevels = 1;
		rd.SampleDesc.Count = 1;
		rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		D3D12Device->CreateCommittedResource(&prop, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(OutputBuffer.GetAddressOf()));
		D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
		ud.Format = rd.Format;
		ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		D3D12Device->CreateUnorderedAccessView(OutputBuffer.Get(), nullptr, &ud, UAV0.CPUHandle);
		D3D12Device->CreateSharedHandle(OutputBuffer.Get(), nullptr, GENERIC_ALL, nullptr, &SharedHandle);
		glCreateMemoryObjectsEXT(1, &memory);
		glImportMemoryWin32HandleEXT(memory, 0, GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, SharedHandle);
		glTextureStorageMem2DEXT(hw_texture, 1, GL_RGBA8, td.Width, td.Height, memory, 0);
	}

	// 对上一帧 GPU 工作做有限等待（4ms），超时则丢弃本帧保留旧画面，
	// 避免渲染线程无限期阻塞等待 GPU compute，这是 D3D12 路径掉帧的主要来源。
	// 等待成功后，当前纹理必然是完整写好的画面，因此不会出现撕裂。
	if (FenceValue > fence->GetCompletedValue())
	{
		if (FAILED(fence->SetEventOnCompletion(FenceValue, FenceEvent)) || WaitForSingleObject(FenceEvent, 4) != WAIT_OBJECT_0)
			return;
	}

	// 每帧重建 SRV 描述符，指向解码器最新的纹理（解码纹理可能逐帧轮换）。
	D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
	sd.Format = DXGI_FORMAT_R8_UNORM;
	sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 4, 4, 4);
	sd.Texture2D.MipLevels = 1;
	D3D12Device->CreateShaderResourceView(tex, &sd, SRV0.CPUHandle);
	sd.Format = DXGI_FORMAT_R8G8_UNORM;
	sd.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(0, 1, 4, 4);
	sd.Texture2D.PlaneSlice = 1;
	D3D12Device->CreateShaderResourceView(tex, &sd, SRV1.CPUHandle);

	// 每帧重新录制命令列表。
	CommandAllocator->Reset();
	CommandList->Reset(CommandAllocator.Get(), PSO.Get());
	CommandList->SetComputeRootSignature(RootSignature.Get());
	CommandList->SetDescriptorHeaps(1, DescriptorHeap.GetAddressOf());
	CommandList->SetComputeRootDescriptorTable(0, DescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	CommandList->Dispatch(ceil(td.Width / 32.f), ceil(td.Height / 32.f), 1);
	CommandList->Close();

	CommandQueue->Wait(df->sync_ctx.fence, df->sync_ctx.fence_value);
	CommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)CommandList.GetAddressOf());
	CommandQueue->Signal(fence.Get(), ++FenceValue);
}

void VideoFrame::UpdateCUDA(const AVFrame* frame)
{
	void* CreateCUDAArraySurface(const void* array);
	int DestroyCUDAArraySurface(void* surface);
	int RunCUDACompute(void* y, void* uv, void* output, void* stream, uint32_t width, uint32_t height, uint32_t stepY, uint32_t stepUV);
	if (!cuda || !cu_ctx) return;
	CUcontext previous{};
	if (cuda->cuCtxPushCurrent(cu_ctx) != CUDA_SUCCESS) return;
	bool mapped = false;
	CUarray array{};
	if (!cu_res) {
		glBindTexture(GL_TEXTURE_2D, hw_texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frame->width, frame->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		if (cuda->cuGraphicsGLRegisterImage(&cu_res, hw_texture, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_SURFACE_LDST) != CUDA_SUCCESS) goto cleanup;
	}
	// 必须先 map 资源才能获取 mapped array（unmap 后 array 视为失效），
	// 因此每帧都在 map 状态下取 array 并核对指针，只有指针或分辨率变化时才重建 surface，
	// 其余帧复用 surface，避免每帧创建/销毁的驱动开销。
	if (cuda->cuGraphicsMapResources(1, &cu_res, stream) != CUDA_SUCCESS) goto cleanup;
	mapped = true;
	if (cuda->cuGraphicsSubResourceGetMappedArray(&array, cu_res, 0, 0) != CUDA_SUCCESS) goto cleanup;
	if (!cu_surface || cu_surface_width != frame->width || cu_surface_height != frame->height || cu_array != (void*)array)
	{
		if (cu_surface) DestroyCUDAArraySurface(cu_surface);
		cu_surface = CreateCUDAArraySurface(array);
		if (!cu_surface) goto cleanup;
		cu_surface_width = frame->width;
		cu_surface_height = frame->height;
		cu_array = (void*)array;
	}
	if (RunCUDACompute(frame->data[0], frame->data[1], cu_surface, stream, frame->width, frame->height, frame->linesize[0], frame->linesize[1]) != 0) goto cleanup;

cleanup:
	if (mapped) cuda->cuGraphicsUnmapResources(1, &cu_res, stream);
	cuda->cuCtxPopCurrent(&previous);
}

VideoFrame::VideoFrame(const AVHWDeviceType type, const AVHWDeviceContext* hwctx, GLuint tex) : FenceEvent(nullptr), DXNVDevice(nullptr), TextureObject(nullptr), cuda(nullptr), cu_ctx(nullptr), stream(nullptr), cu_res(nullptr), cu_surface(nullptr), hw_texture(tex), hwtype(type), hwaccel(false), init(false)
{
	switch (type)
	{
	case AV_HWDEVICE_TYPE_D3D11VA:
	{
		auto va = (AVD3D11VADeviceContext*)hwctx->hwctx;
		D3D11Device = va->device;
		D3D11DeviceContext = va->device_context;
		DXNVDevice = wglDXOpenDeviceNV(D3D11Device.Get());
		D3D11Device->CreateComputeShader(&Resources::cs, Resources::cs_size, nullptr, &ComputeShader);
		break;
	}
	case AV_HWDEVICE_TYPE_D3D12VA:
	{
		auto va = (AVD3D12VADeviceContext*)hwctx->hwctx;
		D3D12Device = va->device;
		{
			ComPtr<ID3DBlob> rs;
			D3DGetBlobPart(&Resources::cs_dxc, Resources::cs_dxc_size, D3D_BLOB_ROOT_SIGNATURE, 0, &rs);
			D3D12Device->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(RootSignature.GetAddressOf()));
			D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
			desc.CS.pShaderBytecode = &Resources::cs_dxc;
			desc.CS.BytecodeLength = Resources::cs_dxc_size;
			desc.pRootSignature = RootSignature.Get();
			D3D12Device->CreateComputePipelineState(&desc, IID_PPV_ARGS(PSO.GetAddressOf()));
		}
		{
			D3D12_COMMAND_QUEUE_DESC desc{};
			desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
			D3D12Device->CreateCommandQueue(&desc, IID_PPV_ARGS(CommandQueue.GetAddressOf()));
			D3D12Device->CreateCommandAllocator(desc.Type, IID_PPV_ARGS(CommandAllocator.GetAddressOf()));
			D3D12Device->CreateCommandList(0, desc.Type, CommandAllocator.Get(), PSO.Get(), IID_PPV_ARGS(CommandList.GetAddressOf()));
			D3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
			FenceEvent = CreateEventA(nullptr, false, false, nullptr);
		}
		{
			D3D12_DESCRIPTOR_HEAP_DESC desc{};
			desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			desc.NumDescriptors = 3;
			D3D12Device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(DescriptorHeap.GetAddressOf()));
			auto size = D3D12Device->GetDescriptorHandleIncrementSize(desc.Type);
			SRV0.CPUHandle = DescriptorHeap->GetCPUDescriptorHandleForHeapStart();
			SRV0.GPUHandle = DescriptorHeap->GetGPUDescriptorHandleForHeapStart();
			SRV1.CPUHandle = { SRV0.CPUHandle.ptr + size };
			SRV1.GPUHandle = { SRV0.GPUHandle.ptr + size };
			UAV0.CPUHandle = { SRV1.CPUHandle.ptr + size };
			UAV0.GPUHandle = { SRV1.GPUHandle.ptr + size };
		}
		break;
	}
	case AV_HWDEVICE_TYPE_CUDA:
	{
		if (cuda_load_functions(&cuda, nullptr) != CUDA_SUCCESS || !cuda) {
			throw std::runtime_error("CUDA driver functions unavailable");
		}
		auto va = (AVCUDADeviceContext*)hwctx->hwctx;
		cu_ctx = va->cuda_ctx;
		stream = va->stream;
		break;
	}
	default:
		break;
	}
}

VideoFrame::~VideoFrame()
{
	if (TextureObject)
	{
		wglDXUnlockObjectsNV(DXNVDevice, 1, &TextureObject);
		wglDXUnregisterObjectNV(DXNVDevice, TextureObject);
		wglDXCloseDeviceNV(DXNVDevice);
	}
	if (memory)
	{
		glDeleteMemoryObjectsEXT(1, &memory);
		CloseHandle(SharedHandle);
	}
	if (FenceEvent) CloseHandle(FenceEvent);
	if (sws) sws_freeContext(sws);
	if (swsOutput) av_frame_free(&swsOutput);
	// CUDA surface 对象引用 GL 纹理的 mapped array，必须在注销 GL 互操作资源前销毁。
	if (cu_surface)
	{
		extern int DestroyCUDAArraySurface(void*);
		if (cuda && cu_ctx) {
			CUcontext previous{};
			if (cuda->cuCtxPushCurrent(cu_ctx) == CUDA_SUCCESS) {
				DestroyCUDAArraySurface(cu_surface);
				cuda->cuCtxPopCurrent(&previous);
			}
		}
		cu_surface = nullptr;
	}
	if (cu_res)
	{
		if (cuda && cu_ctx) {
			CUcontext previous{};
			if (cuda->cuCtxPushCurrent(cu_ctx) == CUDA_SUCCESS) {
				cuda->cuGraphicsUnregisterResource(cu_res);
				cuda->cuCtxPopCurrent(&previous);
			}
		}
	}
	if (cuda) cuda_free_functions(&cuda);
}

void VideoFrame::Update(const AVFrame* frame, bool hwaccel)
{
	this->hwaccel = hwaccel;
	if (hwaccel)
	{
		switch (hwtype)
		{
		case AV_HWDEVICE_TYPE_D3D11VA:
			UpdateD3D11(frame);
			break;
		case AV_HWDEVICE_TYPE_D3D12VA:
			UpdateD3D12(frame);
			break;
		case AV_HWDEVICE_TYPE_CUDA:
			UpdateCUDA(frame);
		default:
			break;
		}
	}
	else UpdateSW(frame);
}

void MediaPlayer::Init(JNIEnv* env, jclass, jlong proc)
{
	auto static init = false;
	if (init) return;
	init = true;
	if (!gladLoadGL((GLADloadfunc)proc)) Throw(env, "init failed");
	if (!gladLoadWGL(wglGetCurrentDC(), (GLADloadfunc)wglGetProcAddress)) Throw(env, "init failed");
}
