#include "stdafx.h"
#include "GDxRenderer.h"
#include "GDxRendererFactory.h"
#include "GDxTexture.h"
#include "GDxFloat4.h"
#include "GDxFloat4x4.h"
#include "GDxFilmboxManager.h"
#include "GDxMesh.h"
#include "GDxSceneObject.h"
#include "GDxInputLayout.h"
#include "GDxShaderManager.h"
#include "GDxStaticVIBuffer.h"

#include <WindowsX.h>

using Microsoft::WRL::ComPtr;
using namespace std;
using namespace DirectX;


#pragma region Class

GDxRenderer& GDxRenderer::GetRenderer()
{
	static GDxRenderer *instance = new GDxRenderer();
	return *instance;
}

GDxRenderer::GDxRenderer()
{
	// Estimate the scene bounding sphere manually since we know how the scene was constructed.
	// The grid is the "widest object" with a width of 20 and depth of 30.0f, and centered at
	// the world space origin.  In general, you need to loop over every world space vertex
	// position and compute the bounding sphere.
	mSceneBounds.Center = XMFLOAT3(0.0f, 0.0f, 0.0f);
	mSceneBounds.Radius = sqrtf(10.0f*10.0f + 15.0f*15.0f);
}

GDxRenderer::~GDxRenderer()
{
	if (md3dDevice != nullptr)
		FlushCommandQueue();
}

#pragma endregion

#pragma region Main

bool GDxRenderer::InitDirect3D()
{
	ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&mdxgiFactory)));

	// Try to create hardware device.
	HRESULT hardwareResult = D3D12CreateDevice(
		nullptr,             // default adapter
		D3D_FEATURE_LEVEL_11_0,
		IID_PPV_ARGS(&md3dDevice));

	// Fallback to WARP device.
	if (FAILED(hardwareResult))
	{
		ComPtr<IDXGIAdapter> pWarpAdapter;
		ThrowIfFailed(mdxgiFactory->EnumWarpAdapter(IID_PPV_ARGS(&pWarpAdapter)));

		ThrowIfFailed(D3D12CreateDevice(
			pWarpAdapter.Get(),
			D3D_FEATURE_LEVEL_11_0,
			IID_PPV_ARGS(&md3dDevice)));
	}

	ThrowIfFailed(md3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE,
		IID_PPV_ARGS(&mFence)));

	mRtvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	mDsvDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
	mCbvSrvUavDescriptorSize = md3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

	// Check 4X MSAA quality support for our back buffer format.
	// All Direct3D 11 capable devices support 4X MSAA for all render 
	// target formats, so we only need to check quality support.

	D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS msQualityLevels;
	msQualityLevels.Format = mBackBufferFormat;
	msQualityLevels.SampleCount = 4;
	msQualityLevels.Flags = D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE;
	msQualityLevels.NumQualityLevels = 0;
	ThrowIfFailed(md3dDevice->CheckFeatureSupport(
		D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
		&msQualityLevels,
		sizeof(msQualityLevels)));

	//m4xMsaaQuality = msQualityLevels.NumQualityLevels;
	//assert(m4xMsaaQuality > 0 && "Unexpected MSAA quality level.");

#ifdef _DEBUG
	LogAdapters(); // ��ӡ�Կ��豸��Ϣ
#endif

	CreateCommandObjects();           // �����������������С�����������������б�
	CreateSwapChain();		          // ����������
	CreateRtvAndDsvDescriptorHeaps(); // ����RTV��DSV��������

	return true;
}

// Ԥ��ʼ��
void GDxRenderer::PreInitialize(HWND OutputWindow, double width, double height)
{
	mhMainWnd = OutputWindow;
	mClientWidth = (int)width;
	mClientHeight = (int)height;

	if (!InitDirect3D())     // ��ʼ��D3D����
		return;

	CreateRendererFactory(); // ������Ⱦ���������󣨺�������ͼ��������ͼ���������������ʡ��������񡢴��������������������������塢����Imgui����ȳ�Ա������
	CreateFilmboxManager();  // ����FBX������

	// Do the initial resize code.
	OnResize();

	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr)); // ����������У�Ŀ�����ظ�������Դ
}

void GDxRenderer::Initialize()
{
	auto numThreads = thread::hardware_concurrency(); // ����Ӳ���߳������ĵ�����
	mRendererThreadPool = std::make_unique<GGiThreadPool>(numThreads);

	InitializeGpuProfiler(); // ��ʼ��GPU Profiler��GPU�������ߣ�
	BuildDescriptorHeaps();  // ������������
	BuildRootSignature();	 // ������ǩ��
	BuildFrameResources();	 // 
	BuildPSOs();			 // ��������״̬����

	/*
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists2[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists2), cmdsLists2);
	*/

	// Wait until initialization is complete.
	FlushCommandQueue(); // ˢ���������

#ifdef USE_IMGUI
	pImgui->Initialize(MainWnd(), md3dDevice.Get(), NUM_FRAME_RESOURCES, mSrvDescriptorHeap.Get());
#endif

	/*
	mCurrFrameResource = mFrameResources[0].get();

	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), nullptr));
	*/

	CubemapPreIntegration(); // ׼����������ͼ

	// ִ�г�ʼ������
	// Execute the initialization commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// �ȴ���ʼ���������
	// Wait until initialization is complete.
	FlushCommandQueue();

	// �ڵ��޳���դ����
	GRiOcclusionCullingRasterizer::GetInstance().Init(
		DEPTH_READBACK_BUFFER_SIZE_X,
		DEPTH_READBACK_BUFFER_SIZE_Y,
		Z_LOWER_BOUND,
		Z_UPPER_BOUND,
#if USE_REVERSE_Z
		true
#else
		false
#endif
	);

	// ����������з��ž��볡���������������ģAO������Ӱ�ȣ�
	BuildMeshSDF();
}

void GDxRenderer::Draw(const GGiGameTimer* gt)
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

	// Reuse the memory associated with command recording.
	// We can only reset when the associated command lists have finished execution on the GPU.
	ThrowIfFailed(cmdListAlloc->Reset());

	// A command list can be reset after it has been added to the command queue via ExecuteCommandList.
	// Reusing the command list reuses memory.
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["GBuffer"].Get()));

	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	GDxGpuProfiler::GetGpuProfiler().BeginFrame();

	auto matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

	// Clear the back buffer.
	mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::LightSteelBlue, 0, nullptr);

	// Clear depth buffer.
#if USE_REVERSE_Z
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
#else
	mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
#endif

	GGiCpuProfiler::GetInstance().StartCpuProfile("Cpu Draw Call");

	// G-Buffer Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("G-Buffer Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["GBuffer"]->mRtv[0]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["GBuffer"]->mRtv[0]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GBuffer"].Get());

		mCommandList->SetPipelineState(mPSOs["GBuffer"].Get());


		UINT objCBByteSize = GDxUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

		//mCommandList->SetGraphicsRootDescriptorTable(2, mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		mCommandList->SetGraphicsRootDescriptorTable(3, GetGpuSrv(mTextrueHeapIndex));

		matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(4, matBuffer->GetGPUVirtualAddress());

		mCommandList->OMSetStencilRef(1);

		// Indicate a state transition on the resource usage.
		for (size_t i = 0; i < mRtvHeaps["GBuffer"]->mRtv.size(); i++)
		{
			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[i]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			// Clear the back buffer.
			DirectX::XMVECTORF32 clearColor = { mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[0],
			mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[1],
			mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[2],
			mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mClearColor[3]
			};

			// WE ALREADY WROTE THE DEPTH INFO TO THE DEPTH BUFFER IN DrawNormalsAndDepth,
			// SO DO NOT CLEAR DEPTH.
			mCommandList->ClearRenderTargetView(mRtvHeaps["GBuffer"]->mRtvHeap.handleCPU((UINT)i), clearColor, 0, nullptr);
		}

		// Specify the buffers we are going to render to.
		//mCommandList->OMSetRenderTargets(mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, &(mRtvHeaps["GBuffer"]->mRtvHeap.hCPUHeapStart), true, &DepthStencilView());
		mCommandList->OMSetRenderTargets(mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, &(mRtvHeaps["GBuffer"]->mRtvHeap.hCPUHeapStart), true, &DepthStencilView());

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Deferred, true, true, true);

		for (size_t i = 0; i < mRtvHeaps["GBuffer"]->mRtv.size(); i++)
		{
			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[i]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
		}

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("G-Buffer Pass");
	}

	// Modified by Ssi:
	// Transparent Pass
	{
		// Modified by Ssi:
		mCommandList->SetPipelineState(mPSOs["Transparent"].Get());
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, objectCB->GetGPUVirtualAddress()); // �󶨸�������

		// Modified by Ssi: ����Tranparent��������
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Transparent, true, true, false);
	}

	// Depth Downsample Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Depth Downsample Pass");

		mCommandList->SetComputeRootSignature(mRootSignatures["DepthDownsamplePass"].Get());

		mCommandList->SetPipelineState(mPSOs["DepthDownsamplePass"].Get());

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["DepthDownsamplePass"]->GetResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetComputeRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetComputeRootDescriptorTable(1, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetComputeRootDescriptorTable(2, mUavs["DepthDownsamplePass"]->GetGpuUav());

		UINT numGroupsX = (UINT)(DEPTH_READBACK_BUFFER_SIZE_X / DEPTH_DOWNSAMPLE_THREAD_NUM_X);
		UINT numGroupsY = (UINT)(DEPTH_READBACK_BUFFER_SIZE_Y / DEPTH_DOWNSAMPLE_THREAD_NUM_Y);

		mCommandList->Dispatch(numGroupsX, numGroupsY, 1);

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["DepthDownsamplePass"]->GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));

		mCommandList->CopyResource(mDepthReadbackBuffer.Get(), mUavs["DepthDownsamplePass"]->GetResource());

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["DepthDownsamplePass"]->GetResource(),
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Depth Downsample Pass");
	}

	// Tile/Cluster Pass
	{
#if USE_TBDR
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Tile Pass");
#elif USE_CBDR
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Cluster Pass");
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif

		mCommandList->RSSetViewports(1, &(mUavs["TileClusterPass"]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mUavs["TileClusterPass"]->mScissorRect));

		mCommandList->SetComputeRootSignature(mRootSignatures["TileClusterPass"].Get());

		mCommandList->SetPipelineState(mPSOs["TileClusterPass"].Get());

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["TileClusterPass"]->GetResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		auto lightCB = mCurrFrameResource->LightCB->Resource();
		mCommandList->SetComputeRootConstantBufferView(0, lightCB->GetGPUVirtualAddress());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetComputeRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetComputeRootDescriptorTable(2, mUavs["TileClusterPass"]->GetGpuUav());

		mCommandList->SetComputeRootDescriptorTable(3, GetGpuSrv(mDepthBufferSrvIndex));

#if USE_TBDR
		UINT numGroupsX = (UINT)ceilf((float)mClientWidth / TILE_SIZE_X);
		UINT numGroupsY = (UINT)ceilf((float)mClientHeight / TILE_SIZE_Y);
		UINT numGroupsZ = 1;
#elif USE_CBDR
		UINT numGroupsX = (UINT)ceilf((float)mClientWidth / CLUSTER_SIZE_X);
		UINT numGroupsY = (UINT)ceilf((float)mClientHeight / CLUSTER_SIZE_Y);
		UINT numGroupsZ = 1;
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif
		mCommandList->Dispatch(numGroupsX, numGroupsY, numGroupsZ);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["TileClusterPass"]->GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

#if USE_TBDR
		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Tile Pass");
#elif USE_CBDR
		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Cluster Pass");
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif
	}

	// SDF Tile Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SDF Tile Pass");

		mCommandList->RSSetViewports(1, &(mUavs["SdfTilePass"]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mUavs["SdfTilePass"]->mScissorRect));

		mCommandList->SetComputeRootSignature(mRootSignatures["SdfTilePass"].Get());

		mCommandList->SetPipelineState(mPSOs["SdfTilePass"].Get());

		// Indicate a state transition on the resource usage.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["SdfTilePass"]->GetResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		mCommandList->SetComputeRoot32BitConstant(0, mSceneObjectSdfNum, 0);

		auto meshSdfDesBuffer = mMeshSdfDescriptorBuffer->Resource();
		mCommandList->SetComputeRootShaderResourceView(1, meshSdfDesBuffer->GetGPUVirtualAddress());

		auto soSdfDesBuffer = mCurrFrameResource->SceneObjectSdfDescriptorBuffer->Resource();
		mCommandList->SetComputeRootShaderResourceView(2, soSdfDesBuffer->GetGPUVirtualAddress());

		mCommandList->SetComputeRootDescriptorTable(3, mUavs["SdfTilePass"]->GetGpuUav());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetComputeRootConstantBufferView(4, passCB->GetGPUVirtualAddress());

		UINT numGroupsX = (UINT)ceilf((float)SDF_GRID_NUM / SDF_TILE_THREAD_NUM_X - 0.001f);
		UINT numGroupsY = (UINT)ceilf((float)SDF_GRID_NUM / SDF_TILE_THREAD_NUM_Y - 0.001f);
		UINT numGroupsZ = 1;
		mCommandList->Dispatch(numGroupsX, numGroupsY, numGroupsZ);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mUavs["SdfTilePass"]->GetResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SDF Tile Pass");
	}

	// GTAO Raw Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GTAO");

		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GTAO Raw Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GtaoRaw"].Get());

		mCommandList->SetPipelineState(mPSOs["GtaoRaw"].Get());

		float fovRad = FOV_Y * 2;
		float invHalfTanFov = 1 / tanf(fovRad * 0.5f);
		float focalLenX = invHalfTanFov * (((float)mClientHeight * GTAO_RESOLUTION_SCALE) / ((float)mClientWidth * GTAO_RESOLUTION_SCALE));
		float focalLenY = invHalfTanFov;
		float invFocalLenX = 1.0f / focalLenX;
		float invFocalLenY = 1.0f / focalLenY;

		float projScale = ((float)mClientHeight * GTAO_RESOLUTION_SCALE) / (tanf(fovRad * 0.5f) * 2) * 0.5f;

		float temporalRotation = mGtaoTemporalRotations[mGtaoSampleStep % 6];
		float temporalOffset = mGtaoSpatialOffsets[(mGtaoSampleStep / 6) % 4];
		mGtaoSampleStep++;

		float inputFloat[7] =
		{
			2.0f * invFocalLenX,
			2.0f * invFocalLenY,
			-1.0f * invFocalLenX,
			-1.0f * invFocalLenY,
			projScale,
			temporalOffset,
			temporalRotation / 360.0f
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 7, inputFloat, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferNormalSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(4, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferOrmSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(5, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferAlbedoSrvIndexOffert));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoRawSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoRawSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GTAO Raw Pass");
	}

	// GTAO Temporal Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GTAO Temporal Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GtaoFilter"].Get());

		mCommandList->SetPipelineState(mPSOs["GtaoTemporal"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoRawSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferVelocitySrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(4, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex));
		mGtaoTemporalHistoryIndex = (mGtaoTemporalHistoryIndex + 1) % 2;

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoTemporalSrvIndexOffset), clearColor, 0, nullptr);

		DirectX::XMVECTORF32 hisClearColor = { mRtvHeaps["GTAO"]->mRtv[mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex]->mProperties.mClearColor[0],
		mRtvHeaps["GTAO"]->mRtv[mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex]->mProperties.mClearColor[1],
		mRtvHeaps["GTAO"]->mRtv[mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex]->mProperties.mClearColor[2],
		mRtvHeaps["GTAO"]->mRtv[mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex), hisClearColor, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE temporalRtvs[2] =
		{
			mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoTemporalSrvIndexOffset),
			mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex)
		};
		mCommandList->OMSetRenderTargets(2, temporalRtvs, false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoHistory1SrvIndexOffset + mGtaoTemporalHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GTAO Temporal Pass");
	}

	// GTAO Bilateral X Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GTAO Bilateral X Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GtaoFilter"].Get());

		mCommandList->SetPipelineState(mPSOs["GtaoBilateralX"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoTemporalSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferVelocitySrvIndexOffert));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoBlurXSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoBlurXSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GTAO Bilateral X Pass");
	}

	// GTAO Bilateral Y Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GTAO Bilateral Y Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GtaoFilter"].Get());

		mCommandList->SetPipelineState(mPSOs["GtaoBilateralY"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoBlurXSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferVelocitySrvIndexOffert));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoBlurYSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoBlurYSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GTAO Bilateral Y Pass");

#if !GTAO_USE_UPSAMPLE
		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GTAO");
#endif
	}

#if GTAO_USE_UPSAMPLE
	// GTAO Upsample Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GTAO Upsample Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GtaoFilter"].Get());

		mCommandList->SetPipelineState(mPSOs["GtaoUpsample"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoBlurYSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferVelocitySrvIndexOffert));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoUpsampleSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["GTAO"]->mRtvHeap.handleCPU(mGtaoUpsampleSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GTAO Upsample Pass");

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GTAO");
	}
#endif

	// Shadow Map Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Shadow Map Pass");

		int shadowMapIndex = mFrameCount % SHADOW_CASCADE_NUM;

		ShadowViewport.TopLeftX = 0.0f;
		ShadowViewport.TopLeftY = 0.0f;
		ShadowViewport.Width = (FLOAT)ShadowMapResolution;
		ShadowViewport.Height = (FLOAT)ShadowMapResolution;
		ShadowViewport.MinDepth = 0.0f;
		ShadowViewport.MaxDepth = 1.0f;

		ShadowScissorRect = { 0, 0, (int)(ShadowViewport.Width), (int)(ShadowViewport.Height) };

		mCommandList->RSSetViewports(1, &ShadowViewport);
		mCommandList->RSSetScissorRects(1, &ShadowScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["ShadowMap"].Get());

		mCommandList->SetPipelineState(mPSOs["ShadowMap"].Get());

		UINT objCBByteSize = GDxUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCascadedShadowMap[shadowMapIndex]->GetShadowmapResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

		// Clear depth buffer.
#if USE_REVERSE_Z
		mCommandList->ClearDepthStencilView(GetDsv(mShadowMapDsvIndex + shadowMapIndex), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 0.0f, 0, 0, nullptr);
#else
		mCommandList->ClearDepthStencilView(GetDsv(mShadowMapDsvIndex + shadowMapIndex), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
#endif

		// Specify the buffers we are going to render to.
		mCommandList->OMSetRenderTargets(0, nullptr, false, &GetDsv(mShadowMapDsvIndex + shadowMapIndex));

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Deferred, true, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCascadedShadowMap[shadowMapIndex]->GetShadowmapResource(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Shadow Map Pass");
	}

#if USE_SHADOW_MAP_PREFILTER
	// Shadow Map Prefilter Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Shadow Map Prefilter Pass");

		mCommandList->RSSetViewports(1, &ShadowViewport);
		mCommandList->RSSetScissorRects(1, &ShadowScissorRect);

		mCommandList->SetComputeRootSignature(mRootSignatures["ShadowMapPrefilter"].Get());

		mCommandList->SetPipelineState(mPSOs["ShadowMapPrefilter"].Get());

		int shadowMapIndex = mFrameCount % SHADOW_CASCADE_NUM;

		// Blur in X direction.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCascadedShadowMap[shadowMapIndex]->GetXPrefilteredResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		mCommandList->SetComputeRoot32BitConstant(0, 0xFFFFFFFF, 0);

		mCommandList->SetComputeRootDescriptorTable(1, GetGpuSrv(mCascadedShadowMapSrvIndex + shadowMapIndex));

		mCommandList->SetComputeRootDescriptorTable(2, GetGpuSrv(mXPrefilteredShadowMapUavIndex + shadowMapIndex));

		UINT numGroupsX = (UINT)ceilf((float)ShadowMapResolution / SHADOW_MAP_PREFILTER_GROUP_SIZE - 0.001f);
		UINT numGroupsY = ShadowMapResolution;
		UINT numGroupsZ = 1;
		mCommandList->Dispatch(numGroupsX, numGroupsY, numGroupsZ);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCascadedShadowMap[shadowMapIndex]->GetXPrefilteredResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		// Blur in Y direction.
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCascadedShadowMap[shadowMapIndex]->GetYPrefilteredResource(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));

		mCommandList->SetComputeRoot32BitConstant(0, 0, 0);

		mCommandList->SetComputeRootDescriptorTable(1, GetGpuSrv(mXPrefilteredShadowMapSrvIndex + shadowMapIndex));

		mCommandList->SetComputeRootDescriptorTable(2, GetGpuSrv(mYPrefilteredShadowMapUavIndex + shadowMapIndex));

		mCommandList->Dispatch(numGroupsX, numGroupsY, numGroupsZ);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCascadedShadowMap[shadowMapIndex]->GetYPrefilteredResource(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Shadow Map Prefilter Pass");
	}
#endif

	// Screen Space Shadow Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Screen Space Shadow Pass");

		//ID3D12DescriptorHeap* sdfSrvDescriptorHeaps[] = { mSdfSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(sdfSrvDescriptorHeaps), sdfSrvDescriptorHeaps);

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["ScreenSpaceShadowPass"].Get());

		mCommandList->SetPipelineState(mPSOs["ScreenSpaceShadowPass"].Get());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->SetGraphicsRoot32BitConstant(0, mSceneObjectSdfNum, 0);

		auto meshSdfDesBuffer = mMeshSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(1, meshSdfDesBuffer->GetGPUVirtualAddress());

		auto soSdfDesBuffer = mCurrFrameResource->SceneObjectSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, soSdfDesBuffer->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrv(mSdfTextrueIndex));

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(5, passCB->GetGPUVirtualAddress());

#if USE_SHADOW_MAP_PREFILTER
		mCommandList->SetGraphicsRootDescriptorTable(6, GetGpuSrv(mYPrefilteredShadowMapSrvIndex));
#else
		mCommandList->SetGraphicsRootDescriptorTable(6, GetGpuSrv(mCascadedShadowMapSrvIndex));
#endif

		mCommandList->SetGraphicsRootDescriptorTable(7, GetGpuSrv(mBlueNoiseSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(8, mUavs["SdfTilePass"]->GetGpuSrv());

		mCommandList->OMSetRenderTargets(1, &mRtvHeaps["ScreenSpaceShadowPass"]->mRtvHeap.handleCPU(0), false, nullptr);

		// Clear the render target.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[0],
		mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[1],
		mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[2],
		mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["ScreenSpaceShadowPass"]->mRtvHeap.handleCPU(0), clearColor, 0, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		//ID3D12DescriptorHeap* srvDescriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(srvDescriptorHeaps), srvDescriptorHeaps);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Screen Space Shadow Pass");
	}

#if USE_PCSS_TEMPORAL
	// Screen Space Shadow Temporal Filter Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Shadow Temporal Filter Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SSShadowTemporalPass"].Get());

		mCommandList->SetPipelineState(mPSOs["SSShadowTemporalPass"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["ScreenSpaceShadowPass"]->GetSrvGpu(0));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["SSShadowTemporalPass"]->GetSrvGpu(mShadowTemporalHistoryIndex));
		mShadowTemporalHistoryIndex = (mShadowTemporalHistoryIndex + 1) % 2;

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mVelocityBufferSrvIndex - mGBufferSrvIndex));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSShadowTemporalPass"]->mRtv[mShadowTemporalHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mProperties.mClearColor[0],
		mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mProperties.mClearColor[1],
		mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mProperties.mClearColor[2],
		mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSShadowTemporalPass"]->mRtvHeap.handleCPU(2), clearColor, 0, nullptr);

		DirectX::XMVECTORF32 hisClearColor = { mRtvHeaps["SSShadowTemporalPass"]->mRtv[mShadowTemporalHistoryIndex]->mProperties.mClearColor[0],
		mRtvHeaps["SSShadowTemporalPass"]->mRtv[mShadowTemporalHistoryIndex]->mProperties.mClearColor[1],
		mRtvHeaps["SSShadowTemporalPass"]->mRtv[mShadowTemporalHistoryIndex]->mProperties.mClearColor[2],
		mRtvHeaps["SSShadowTemporalPass"]->mRtv[mShadowTemporalHistoryIndex]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSShadowTemporalPass"]->mRtvHeap.handleCPU(mShadowTemporalHistoryIndex), hisClearColor, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE shadowTemporalRtvs[2] =
		{
			mRtvHeaps["SSShadowTemporalPass"]->mRtvHeap.handleCPU(2),
			mRtvHeaps["SSShadowTemporalPass"]->mRtvHeap.handleCPU(mShadowTemporalHistoryIndex)
		};
		//mCommandList->OMSetRenderTargets(2, taaRtvs, false, &DepthStencilView());
		mCommandList->OMSetRenderTargets(2, shadowTemporalRtvs, false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSShadowTemporalPass"]->mRtv[mShadowTemporalHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Shadow Temporal Filter Pass");
	}
#endif

	// Light Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Light Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["LightPass"].Get());

		mCommandList->SetPipelineState(mPSOs["LightPass"].Get());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[1]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		auto lightCB = mCurrFrameResource->LightCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, lightCB->GetGPUVirtualAddress());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mUavs["TileClusterPass"]->GetGpuSrv());

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpuStart());

		mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrv(mDepthBufferSrvIndex));

#if USE_PCSS_TEMPORAL
		mCommandList->SetGraphicsRootDescriptorTable(5, mRtvHeaps["SSShadowTemporalPass"]->GetSrvGpu(2));
#else
		mCommandList->SetGraphicsRootDescriptorTable(5, mRtvHeaps["ScreenSpaceShadowPass"]->GetSrvGpu(0));
#endif

		mCommandList->SetGraphicsRootDescriptorTable(6, GetGpuSrv(mIblIndex));

#if GTAO_USE_UPSAMPLE
		mCommandList->SetGraphicsRootDescriptorTable(7, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoUpsampleSrvIndexOffset));
#else
		mCommandList->SetGraphicsRootDescriptorTable(7, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoBlurYSrvIndexOffset));
#endif

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[0],
		mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[1],
		mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[2],
		mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(0), clearColor, 0, nullptr);

		DirectX::XMVECTORF32 clearColor2 = { mRtvHeaps["LightPass"]->mRtv[1]->mProperties.mClearColor[0],
		mRtvHeaps["LightPass"]->mRtv[1]->mProperties.mClearColor[1],
		mRtvHeaps["LightPass"]->mRtv[1]->mProperties.mClearColor[2],
		mRtvHeaps["LightPass"]->mRtv[1]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(1), clearColor2, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] =
		{
			mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(0),
			mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(1)
		};
		//mCommandList->OMSetRenderTargets(2, taaRtvs, false, &DepthStencilView());
		mCommandList->OMSetRenderTargets(2, rtvs, false, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[1]->mResource.Get(),
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Light Pass");
	}

	// Sky Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Sky Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["LightPass"]->mRtv[0]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["Sky"].Get());

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[mVelocityBufferSrvIndex - mGBufferSrvIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

		D3D12_CPU_DESCRIPTOR_HANDLE skyRtvs[2] =
		{
			mRtvHeaps["LightPass"]->mRtvHeap.handleCPU(0),
			mRtvHeaps["GBuffer"]->mRtvHeap.handleCPU(mVelocityBufferSrvIndex - mGBufferSrvIndex)
		};
		mCommandList->OMSetRenderTargets(2, skyRtvs, false, &DepthStencilView());

		auto passCB = mCurrFrameResource->SkyCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		// Sky cubemap SRV.
		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mSkyTexHeapIndex));
		//mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mIblIndex + 4)); //Irradiance cubemap debug.

		mCommandList->SetPipelineState(mPSOs["Sky"].Get());
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Sky, true, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["LightPass"]->mRtv[0]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["GBuffer"]->mRtv[mVelocityBufferSrvIndex - mGBufferSrvIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Sky Pass");
	}

	// TAA Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("TAA Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["TaaPass"]->mRtv[2]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["TaaPass"]->mRtv[2]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["TaaPass"].Get());

		mCommandList->SetPipelineState(mPSOs["TaaPass"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["LightPass"]->GetSrvGpu(0));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["TaaPass"]->GetSrvGpu(mTaaHistoryIndex));
		mTaaHistoryIndex = (mTaaHistoryIndex + 1) % 2;

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mVelocityBufferSrvIndex - mGBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[2]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 taaClearColor = { mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[0],
		mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[1],
		mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[2],
		mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(2), taaClearColor, 0, nullptr);

		DirectX::XMVECTORF32 hisClearColor = { mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[0],
		mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[1],
		mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[2],
		mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(mTaaHistoryIndex), hisClearColor, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE taaRtvs[2] =
		{
			mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(2),
			mRtvHeaps["TaaPass"]->mRtvHeap.handleCPU(mTaaHistoryIndex)
		};
		//mCommandList->OMSetRenderTargets(2, taaRtvs, false, &DepthStencilView());
		mCommandList->OMSetRenderTargets(2, taaRtvs, false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[2]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["TaaPass"]->mRtv[mTaaHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("TAA Pass");
	}

	// SSR Depth Unjitter Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Screen Space Reflection");

		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SSR Depth Unjitter Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SsrDepthUnjitter"].Get());

		mCommandList->SetPipelineState(mPSOs["SsrDepthUnjitter"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrUnjitteredDepthSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrUnjitteredDepthSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SSR Depth Unjitter Pass");
	}

	// SSR HiZ Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SSR HiZ Pass");

		for (int i = 0; i < SSR_MAX_MIP_LEVEL; i++)
		{
			mCommandList->RSSetViewports(1, &(mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mViewport));
			mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mScissorRect));

			mCommandList->SetGraphicsRootSignature(mRootSignatures["SsrHiz"].Get());

			mCommandList->SetPipelineState(mPSOs["SsrHiz"].Get());

			if (i == 0)
				mCommandList->SetGraphicsRootDescriptorTable(0, mRtvHeaps["SSR"]->GetSrvGpu(mSsrUnjitteredDepthSrvIndexOffset));
			else
				mCommandList->SetGraphicsRootDescriptorTable(0, mRtvHeaps["SSR"]->GetSrvGpu(mSsrHizSrvIndexOffset + i - 1));

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			// Clear RT.
			DirectX::XMVECTORF32 clearColor = { mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mProperties.mClearColor[0],
			mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mProperties.mClearColor[1],
			mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mProperties.mClearColor[2],
			mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mProperties.mClearColor[3]
			};

			mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrHizSrvIndexOffset + i), clearColor, 0, nullptr);

			mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrHizSrvIndexOffset + i)), false, nullptr);

			// For each render item...
			DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
		}

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SSR HiZ Pass");
	}

	// SSR Prefilter Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SSR Prefilter Pass");

		for (int i = 0; i < SSR_MAX_PREFILTER_LEVEL; i++)
		{
			mCommandList->RSSetViewports(1, &(mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mViewport));
			mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mScissorRect));

			mCommandList->SetGraphicsRootSignature(mRootSignatures["SsrPrefilter"].Get());

			mCommandList->SetPipelineState(mPSOs["SsrPrefilter"].Get());

			if (i == 0)
				mCommandList->SetGraphicsRootDescriptorTable(0, mRtvHeaps["TaaPass"]->GetSrvGpu(2));
			else
				mCommandList->SetGraphicsRootDescriptorTable(0, mRtvHeaps["SSR"]->GetSrvGpu(mSsrPrefilterSrvIndexOffset + i - 1));

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			// Clear RT.
			DirectX::XMVECTORF32 clearColor = { mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mProperties.mClearColor[0],
			mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mProperties.mClearColor[1],
			mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mProperties.mClearColor[2],
			mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mProperties.mClearColor[3]
			};

			mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrPrefilterSrvIndexOffset + i), clearColor, 0, nullptr);

			mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrPrefilterSrvIndexOffset + i)), false, nullptr);

			// For each render item...
			DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
		}

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SSR Prefilter Pass");
	}

	// SSR Trace Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SSR Trace Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SsrTraceResolve"].Get());

		mCommandList->SetPipelineState(mPSOs["SsrTrace"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferNormalSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferOrmSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(4, mRtvHeaps["TaaPass"]->GetSrvGpu(2));

		mCommandList->SetGraphicsRootDescriptorTable(5, GetGpuSrv(mBlueNoiseSrvIndex));

		float fovRad = FOV_Y * 2;
		float invHalfTanFov = 1 / tanf(fovRad * 0.5f);
		float focalLenX = invHalfTanFov * ((float)mClientHeight / (float)mClientWidth);
		float focalLenY = invHalfTanFov;
		float invFocalLenX = 1.0f / focalLenX;
		float invFocalLenY = 1.0f / focalLenY;

		float inputFloat[4] =
		{
			2.0f * invFocalLenX,
			2.0f * invFocalLenY,
			-1.0f * invFocalLenX,
			-1.0f * invFocalLenY
		};

		mCommandList->SetGraphicsRoot32BitConstants(6, 4, inputFloat, 0);

		mCommandList->SetGraphicsRootDescriptorTable(7, mRtvHeaps["SSR"]->GetSrvGpu(mSsrHizSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrTraceMaskSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrTraceSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		DirectX::XMVECTORF32 maskClearColor = { mRtvHeaps["SSR"]->mRtv[mSsrTraceMaskSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["SSR"]->mRtv[mSsrTraceMaskSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["SSR"]->mRtv[mSsrTraceMaskSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["SSR"]->mRtv[mSsrTraceMaskSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrTraceMaskSrvIndexOffset), maskClearColor, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] =
		{
			mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrTraceSrvIndexOffset),
			mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrTraceMaskSrvIndexOffset)
		};

		mCommandList->OMSetRenderTargets(2, rtvs, false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrTraceMaskSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SSR Trace Pass");
	}

	// SSR Resolve Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SSR Resolve Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SsrTraceResolve"].Get());

		mCommandList->SetPipelineState(mPSOs["SsrResolve"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferNormalSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferOrmSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(4, mRtvHeaps["TaaPass"]->GetSrvGpu(2));

		mCommandList->SetGraphicsRootDescriptorTable(5, GetGpuSrv(mBlueNoiseSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(7, mRtvHeaps["SSR"]->GetSrvGpu(mSsrHizSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(8, mRtvHeaps["SSR"]->GetSrvGpu(mSsrTraceSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(9, mRtvHeaps["SSR"]->GetSrvGpu(mSsrTraceMaskSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(10, mRtvHeaps["SSR"]->GetSrvGpu(mSsrPrefilterSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrResolveSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrResolveSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SSR Resolve Pass");
	}

	// SSR Temporal Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SSR Temporal Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SsrTemporal"].Get());

		mCommandList->SetPipelineState(mPSOs["SsrTemporal"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["SSR"]->GetSrvGpu(mSsrResolveSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["SSR"]->GetSrvGpu(mSsrTraceSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferVelocitySrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(4, mRtvHeaps["SSR"]->GetSrvGpu(mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex));
		mSsrTemporalHistoryIndex = (mSsrTemporalHistoryIndex + 1) % 2;

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrTemporalSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		DirectX::XMVECTORF32 maskClearColor = { mRtvHeaps["SSR"]->mRtv[mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex]->mProperties.mClearColor[0],
		mRtvHeaps["SSR"]->mRtv[mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex]->mProperties.mClearColor[1],
		mRtvHeaps["SSR"]->mRtv[mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex]->mProperties.mClearColor[2],
		mRtvHeaps["SSR"]->mRtv[mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex), maskClearColor, 0, nullptr);

		D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] =
		{
			mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrTemporalSrvIndexOffset),
			mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex)
		};

		mCommandList->OMSetRenderTargets(2, rtvs, false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrHistorySrvIndexOffset + mSsrTemporalHistoryIndex]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SSR Temporal Pass");
	}

	// SSR Combine Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SSR Combine Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SsrCombine"].Get());

		mCommandList->SetPipelineState(mPSOs["SsrCombine"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferNormalSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferOrmSrvIndexOffert));

		mCommandList->SetGraphicsRootDescriptorTable(4, GetGpuSrv(mIblIndex + 1));

		mCommandList->SetGraphicsRootDescriptorTable(5, mRtvHeaps["GTAO"]->GetSrvGpu(mGtaoBlurYSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(6, mRtvHeaps["TaaPass"]->GetSrvGpu(2));

		mCommandList->SetGraphicsRootDescriptorTable(7, mRtvHeaps["LightPass"]->GetSrvGpu(mLightingAmbientSpecularSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(8, mRtvHeaps["SSR"]->GetSrvGpu(mSsrTemporalSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(9, mRtvHeaps["GBuffer"]->GetSrvGpu(mGBufferAlbedoSrvIndexOffert));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrCombineSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["SSR"]->mRtvHeap.handleCPU(mSsrCombineSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SSR Combine Pass");

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Screen Space Reflection");
	}

	// DoF CoC Pass // DoF ����; CoC Circle of Confusion�� ��ɢԲ
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Depth of Field");

		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("DoF CoC Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["DoF"].Get());

		mCommandList->SetPipelineState(mPSOs["DofCoc"].Get());

		float scaledFilmHeight = mDofFilmHeight * (mClientHeight / 1080.0f);
		float f = DOF_FOCAL_LENGTH / 1000.0f;
		mDofDistance = max(DOF_FOCUS_DISTANCE, f); // DOF_FOCUS_DISTANCE, f
		mDofRcpAspect = (float)mClientHeight / (float)mClientWidth;
		mDofCoeff = f * f / (DOF_APERTURE * (mDofDistance - f) * scaledFilmHeight * 2.0f);
		float radiusInPixels = 1.0f * 4.0f + 6.0f;
		// �����쳣���������������ʱ�����ù���ģ����Ӱ��۲죻ԭ���Ǽ���ľ�����ɢԲֱ��̫С�ˣ����¾��Χ�����С
		mDofMaxCoc = 10; // min(0.05f, radiusInPixels / mClientHeight); 

		float inputFloat[5] =
		{
			mDofDistance,
			mDofCoeff,
			mDofMaxCoc,
			1.0f / mDofMaxCoc,
			mDofRcpAspect
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputFloat, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofCocSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofCocSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("DoF CoC Pass");
	}

	// DoF Prefilter Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("DoF Prefilter Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["DoF"].Get());

		mCommandList->SetPipelineState(mPSOs["DofPrefilter"].Get());
		
		float inputFloat[5] =
		{
			mDofDistance,
			mDofCoeff,
			mDofMaxCoc,
			1.0f / mDofMaxCoc,
			mDofRcpAspect
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputFloat, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["SSR"]->GetSrvGpu(mSsrCombineSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["DoF"]->GetSrvGpu(mDofCocSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofPrefilterSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofPrefilterSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("DoF Prefilter Pass");
	}

	// DoF Bokeh Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("DoF Bokeh Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["DoF"].Get());

		mCommandList->SetPipelineState(mPSOs["DofBokeh"].Get());

		float inputFloat[5] =
		{
			mDofDistance,
			mDofCoeff,
			mDofMaxCoc,
			1.0f / mDofMaxCoc,
			mDofRcpAspect
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputFloat, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["DoF"]->GetSrvGpu(mDofPrefilterSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofBokehSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofBokehSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("DoF Bokeh Pass");
	}

	// DoF Postfilter Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("DoF Postfilter Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["DoF"].Get());

		mCommandList->SetPipelineState(mPSOs["DofPostfilter"].Get());

		float inputFloat[5] =
		{
			mDofDistance,
			mDofCoeff,
			mDofMaxCoc,
			1.0f / mDofMaxCoc,
			mDofRcpAspect
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputFloat, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["DoF"]->GetSrvGpu(mDofBokehSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofPostfilterSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofPostfilterSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("DoF Postfilter Pass");
	}

	// DoF Combine Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("DoF Combine Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["DoF"].Get());

		mCommandList->SetPipelineState(mPSOs["DofCombine"].Get());

		float inputFloat[5] =
		{
			mDofDistance,
			mDofCoeff,
			mDofMaxCoc,
			1.0f / mDofMaxCoc,
			mDofRcpAspect
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputFloat, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["DoF"]->GetSrvGpu(mDofPostfilterSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["DoF"]->GetSrvGpu(mDofCocSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(4, mRtvHeaps["SSR"]->GetSrvGpu(mSsrCombineSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofCombineSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["DoF"]->mRtvHeap.handleCPU(mDofCombineSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("DoF Combine Pass");

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Depth of Field");
	}

	// Motion Blur Velocity Depth Packing Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Motion Blur");

		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Velocity Depth Packing Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurVdPacking"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurVdPacking"].Get());

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(0, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["GBuffer"]->GetSrvGpu(mVelocityBufferSrvIndex - mGBufferSrvIndex));

		mCommandList->SetGraphicsRootDescriptorTable(2, GetGpuSrv(mDepthBufferSrvIndex));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurVdBufferSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurVdBufferSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Velocity Depth Packing Pass");
	}

	// Motion Blur First Tile Max Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("First Tile Max Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurTileMax"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurFirstTileMax"].Get());

		float inputFloat[4] =
		{
			1.0f / mClientWidth,
			1.0f / mClientHeight,
			0.0f,
			0.0f
		};
		int inputInt = 0;

		char inputParams[4 * 5];
		memcpy(inputParams, &inputFloat[0], 4);
		memcpy(inputParams + 4, &inputFloat[1], 4);
		memcpy(inputParams + 8, &inputFloat[2], 4);
		memcpy(inputParams + 12, &inputFloat[3], 4);
		memcpy(inputParams + 16, &inputInt, 4);

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputParams, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurVdBufferSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurFirstTileMaxSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurFirstTileMaxSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("First Tile Max Pass");
	}

	// Motion Blur Second Tile Max Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Second Tile Max Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurTileMax"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurSecondThirdTileMax"].Get());

		float inputFloat[4] =
		{
			2.0f / mClientWidth,
			2.0f / mClientHeight,
			0.0f,
			0.0f
		};
		int inputInt = 0;

		char inputParams[4 * 5];
		memcpy(inputParams, &inputFloat[0], 4);
		memcpy(inputParams + 4, &inputFloat[1], 4);
		memcpy(inputParams + 8, &inputFloat[2], 4);
		memcpy(inputParams + 12, &inputFloat[3], 4);
		memcpy(inputParams + 16, &inputInt, 4);

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputParams, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurFirstTileMaxSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurSecondTileMaxSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurSecondTileMaxSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Second Tile Max Pass");
	}

	// Motion Blur Third Tile Max Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Third Tile Max Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurTileMax"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurSecondThirdTileMax"].Get());

		float inputFloat[4] =
		{
			4.0f / mClientWidth,
			4.0f / mClientHeight,
			0.0f,
			0.0f
		};
		int inputInt = 0;

		char inputParams[4 * 5];
		memcpy(inputParams, &inputFloat[0], 4);
		memcpy(inputParams + 4, &inputFloat[1], 4);
		memcpy(inputParams + 8, &inputFloat[2], 4);
		memcpy(inputParams + 12, &inputFloat[3], 4);
		memcpy(inputParams + 16, &inputInt, 4);

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputParams, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurSecondTileMaxSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurThirdTileMaxSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurThirdTileMaxSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurThirdTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Third Tile Max Pass");
	}

	// Motion Blur Fourth Tile Max Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Fourth Tile Max Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurTileMax"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurFourthTileMax"].Get());

		auto maxBlurRadius = (int)(MOTION_BLUR_MAX_RADIUS_SCALE * mClientHeight / 100);
		auto tileSize = (((maxBlurRadius - 1) / 8 + 1) * 8);
		auto tileMaxOffset = (tileSize / 8.0f - 1.0f) * -0.5f;

		float inputFloat[4] =
		{
			8.0f / mClientWidth,
			8.0f / mClientHeight,
			tileMaxOffset,
			tileMaxOffset
		};
		int inputInt = (int)(tileSize / 8.0f);

		char inputParams[4 * 5];
		memcpy(inputParams, &inputFloat[0], 4);
		memcpy(inputParams + 4, &inputFloat[1], 4);
		memcpy(inputParams + 8, &inputFloat[2], 4);
		memcpy(inputParams + 12, &inputFloat[3], 4);
		memcpy(inputParams + 16, &inputInt, 4);

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputParams, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurThirdTileMaxSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurFourthTileMaxSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurFourthTileMaxSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Fourth Tile Max Pass");
	}

	// Motion Blur Neighbor Max Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Neighbor Max Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurTileMax"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurNeighborMax"].Get());

		auto maxBlurRadius = (int)(MOTION_BLUR_MAX_RADIUS_SCALE * mClientHeight / 100);
		auto tileSize = (((maxBlurRadius - 1) / 8 + 1) * 8);

		float inputFloat[4] =
		{
			tileSize / mClientWidth,
			tileSize / mClientHeight,
			0.0f,
			0.0f
		};
		int inputInt = 0;

		char inputParams[4 * 5];
		memcpy(inputParams, &inputFloat[0], 4);
		memcpy(inputParams + 4, &inputFloat[1], 4);
		memcpy(inputParams + 8, &inputFloat[2], 4);
		memcpy(inputParams + 12, &inputFloat[3], 4);
		memcpy(inputParams + 16, &inputInt, 4);

		mCommandList->SetGraphicsRoot32BitConstants(0, 5, inputParams, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurFourthTileMaxSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurNeighborMaxSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurNeighborMaxSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Neighbor Max Pass");
	}

	// Motion Blur Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Motion Blur Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["MotionBlurPass"].Get());

		mCommandList->SetPipelineState(mPSOs["MotionBlurPass"].Get());

		auto maxBlurRadius = (int)(MOTION_BLUR_MAX_RADIUS_SCALE * mClientHeight / 100);
		auto tileSize = (((maxBlurRadius - 1) / 8 + 1) * 8);

		float inputFloat[2] =
		{
			tileSize / mClientWidth,
			tileSize / mClientHeight,
		};
		int inputInt = MOTION_BLUR_SAMPLE_COUNT / 2;

		char inputParams[4 * 3];
		memcpy(inputParams, &inputFloat[0], 4);
		memcpy(inputParams + 4, &inputFloat[1], 4);
		memcpy(inputParams + 8, &inputInt, 4);

		mCommandList->SetGraphicsRoot32BitConstants(0, 3, inputParams, 0);

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["DoF"]->GetSrvGpu(mDofCombineSrvIndexOffset));
		/*
		if(TestBool)
			mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["TaaPass"]->GetSrvGpu(2));
		else
			mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["SSR"]->GetSrvGpu(mSsrCombineSrvIndexOffset));
		*/

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurVdBufferSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(4, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurNeighborMaxSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 motionBlurClearColor = { mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurOutputSrvIndexOffset), motionBlurClearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["MotionBlur"]->mRtvHeap.handleCPU(mMotionBlurOutputSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Motion Blur Pass");

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Motion Blur");
	}

	// Bloom Prefilter Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Bloom");

		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Bloom Prefilter Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["Bloom"].Get());

		mCommandList->SetPipelineState(mPSOs["BloomPrefilter"].Get());

		BloomWidth = (int)floorf(mClientWidth / 2.0f);
		BloomHeight = (int)floorf(mClientHeight / 2.0f);
		int s = max(BloomWidth, BloomHeight);
		float logs = log2f(s) + min(BLOOM_DIFFUSION, 10.0f) - 10.0f;
		int logs_i = (int)floorf(logs);
		BloomSampleScale = 0.5f + logs - logs_i;
		if (logs_i > BloomChainLength)
			BloomIterations = BloomChainLength;
		else if (logs_i < 1)
			BloomIterations = 1;
		else
			BloomIterations = logs_i;
		float lthresh = BLOOM_THRESHOLD;
		float knee = lthresh * BLOOM_SOFT_KNEE + 1e-5f;

		float inputParams[7] =
		{
			1.0f / BloomWidth,
			1.0f / BloomHeight,
			lthresh,
			lthresh - knee,
			knee * 2.0f,
			0.25f / knee,
			BloomSampleScale
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 7, inputParams, 0);

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurOutputSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurOutputSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomDownSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomDownSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		BloomWidth = max(BloomWidth / 2, 1);
		BloomHeight = max(BloomHeight / 2, 1);

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Bloom Prefilter Pass");
	}

	// Bloom Downsample Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Bloom Downsample Pass");

		for (int i = 1; i < BloomIterations; i++)
		{
			mCommandList->RSSetViewports(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mViewport));
			mCommandList->RSSetScissorRects(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mScissorRect));

			mCommandList->SetGraphicsRootSignature(mRootSignatures["Bloom"].Get());

			mCommandList->SetPipelineState(mPSOs["BloomDownsample"].Get());

			float inputParams[7] =
			{
				1.0f / BloomWidth,
				1.0f / BloomHeight,
				0.0f,
				0.0f,
				0.0f,
				0.0f,
				BloomSampleScale
			};

			mCommandList->SetGraphicsRoot32BitConstants(0, 7, inputParams, 0);

			mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["Bloom"]->GetSrvGpu(mBloomDownSrvIndexOffset + i - 1));

			mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["Bloom"]->GetSrvGpu(mBloomDownSrvIndexOffset + i - 1));

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			// Clear RT.
			DirectX::XMVECTORF32 clearColor = { mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mProperties.mClearColor[0],
			mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mProperties.mClearColor[1],
			mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mProperties.mClearColor[2],
			mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mProperties.mClearColor[3]
			};

			mCommandList->ClearRenderTargetView(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomDownSrvIndexOffset + i), clearColor, 0, nullptr);

			mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomDownSrvIndexOffset + i)), false, nullptr);

			// For each render item...
			DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

			BloomWidth = max(BloomWidth / 2, 1);
			BloomHeight = max(BloomHeight / 2, 1);
		}

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Bloom Downsample Pass");
	}

	// Bloom Upsample Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Bloom Upsample Pass");

		for (int i = BloomIterations - 2; i >= 0; i--)
		{
			mCommandList->RSSetViewports(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mViewport));
			mCommandList->RSSetScissorRects(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mScissorRect));

			mCommandList->SetGraphicsRootSignature(mRootSignatures["Bloom"].Get());

			mCommandList->SetPipelineState(mPSOs["BloomUpsample"].Get());

			float inputParams[7] =
			{
				1.0f / BloomWidth,
				1.0f / BloomHeight,
				0.0f,
				0.0f,
				0.0f,
				0.0f,
				BloomSampleScale
			};

			mCommandList->SetGraphicsRoot32BitConstants(0, 7, inputParams, 0);

			if (i == BloomIterations - 2)
				mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["Bloom"]->GetSrvGpu(mBloomDownSrvIndexOffset + BloomIterations - 1));
			else
				mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["Bloom"]->GetSrvGpu(mBloomUpSrvIndexOffset + i + 1));

			mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["Bloom"]->GetSrvGpu(mBloomDownSrvIndexOffset + i));

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			// Clear RT.
			DirectX::XMVECTORF32 clearColor = { mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mProperties.mClearColor[0],
			mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mProperties.mClearColor[1],
			mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mProperties.mClearColor[2],
			mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mProperties.mClearColor[3]
			};

			mCommandList->ClearRenderTargetView(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomUpSrvIndexOffset + i), clearColor, 0, nullptr);

			mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomUpSrvIndexOffset + i)), false, nullptr);

			// For each render item...
			DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset + i]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

			BloomWidth *= 2;
			BloomHeight *= 2;
		}

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Bloom Upsample Pass");
	}

	// Bloom Upsample Pass
	{
		//GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Bloom Combine Pass");

		mCommandList->RSSetViewports(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mViewport));
		mCommandList->RSSetScissorRects(1, &(mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mScissorRect));

		mCommandList->SetGraphicsRootSignature(mRootSignatures["Bloom"].Get());

		mCommandList->SetPipelineState(mPSOs["BloomCombine"].Get());

		float inputParams[7] =
		{
			1.0f / BloomWidth,
			1.0f / BloomHeight,
			0.0f,
			0.0f,
			0.0f,
			0.0f,
			BloomSampleScale
		};

		mCommandList->SetGraphicsRoot32BitConstants(0, 7, inputParams, 0);

		mCommandList->SetGraphicsRootDescriptorTable(1, mRtvHeaps["Bloom"]->GetSrvGpu(mBloomUpSrvIndexOffset));

		mCommandList->SetGraphicsRootDescriptorTable(2, mRtvHeaps["MotionBlur"]->GetSrvGpu(mMotionBlurOutputSrvIndexOffset));

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

		// Clear RT.
		DirectX::XMVECTORF32 clearColor = { mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mProperties.mClearColor[0],
		mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mProperties.mClearColor[1],
		mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mProperties.mClearColor[2],
		mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mProperties.mClearColor[3]
		};

		mCommandList->ClearRenderTargetView(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomOutputSrvIndexOffset), clearColor, 0, nullptr);

		mCommandList->OMSetRenderTargets(1, &(mRtvHeaps["Bloom"]->mRtvHeap.handleCPU(mBloomOutputSrvIndexOffset)), false, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mRtvHeaps["Bloom"]->mRtv[mBloomOutputSrvIndexOffset]->mResource.Get(),
			D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

		//GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Bloom Combine Pass");

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Bloom");
	}

	// Output Pass
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Output Pass");

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["Output"].Get());

		mCommandList->SetPipelineState(mPSOs["Output"].Get());

		mCommandList->SetGraphicsRootDescriptorTable(0, mRtvHeaps["Bloom"]->GetSrvGpu(mBloomOutputSrvIndexOffset));

		// Specify the buffers we are going to render to.
		//mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());
		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Output Pass");
	}

	// Debug Pass
#if 0
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("Debug Pass");

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["GBufferDebug"].Get());

		mCommandList->SetPipelineState(mPSOs["GBufferDebug"].Get());

		UINT objCBByteSize = GDxUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(2, passCB->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, mRtvHeaps["GBuffer"]->GetSrvGpuStart());

		matBuffer = mCurrFrameResource->MaterialBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(4, matBuffer->GetGPUVirtualAddress());

		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

		// For each render item...
		DrawSceneObjects(mCommandList.Get(), RenderLayer::Debug, true, true);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("Debug Pass");
	}
#endif

	// SDF Debug Pass
#if 0
	{
		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("SDF Debug Pass");

		//ID3D12DescriptorHeap* sdfSrvDescriptorHeaps[] = { mSdfSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(sdfSrvDescriptorHeaps), sdfSrvDescriptorHeaps);

		mCommandList->RSSetViewports(1, &mScreenViewport);
		mCommandList->RSSetScissorRects(1, &mScissorRect);

		mCommandList->SetGraphicsRootSignature(mRootSignatures["SdfDebug"].Get());

		mCommandList->SetPipelineState(mPSOs["SdfDebug"].Get());

		mCommandList->SetGraphicsRoot32BitConstant(0, mSceneObjectSdfNum, 0);

		auto meshSdfDesBuffer = mMeshSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(1, meshSdfDesBuffer->GetGPUVirtualAddress());

		auto soSdfDesBuffer = mCurrFrameResource->SceneObjectSdfDescriptorBuffer->Resource();
		mCommandList->SetGraphicsRootShaderResourceView(2, soSdfDesBuffer->GetGPUVirtualAddress());

		mCommandList->SetGraphicsRootDescriptorTable(3, GetGpuSrv(mSdfTextrueIndex));

		auto passCB = mCurrFrameResource->PassCB->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(4, passCB->GetGPUVirtualAddress());

		mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::ScreenQuad, false, false);

		//ID3D12DescriptorHeap* srvDescriptorHeaps[] = { mSrvDescriptorHeap.Get() };
		//mCommandList->SetDescriptorHeaps(_countof(srvDescriptorHeaps), srvDescriptorHeaps);

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("SDF Debug Pass");
	}
#endif

	// Immediate Mode GUI Pass
	{

#ifdef USE_IMGUI

		GDxGpuProfiler::GetGpuProfiler().StartGpuProfile("GUI Pass");

		pImgui->Render(mCommandList.Get());

		GDxGpuProfiler::GetGpuProfiler().EndGpuProfile("GUI Pass");

#endif

	}

	GGiCpuProfiler::GetInstance().EndCpuProfile("Cpu Draw Call");

	// Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	// Done recording commands.
	ThrowIfFailed(mCommandList->Close());

	// Add the command list to the queue for execution.
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Swap the back and front buffers
	//ThrowIfFailed(mSwapChain->Present(1, 0)); // Present with vsync
	ThrowIfFailed(mSwapChain->Present(0, 0)); // Present without vsync
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

	// Advance the fence value to mark commands up to this fence point.
	mCurrFrameResource->Fence = ++mCurrentFence;

	// Add an instruction to the command queue to set a new fence point. 
	// Because we are on the GPU timeline, the new fence point won't be 
	// set until the GPU finishes processing all the commands prior to this Signal().
	mCommandQueue->Signal(mFence.Get(), mCurrentFence);

	GDxGpuProfiler::GetGpuProfiler().EndFrame();
}

void GDxRenderer::Update(const GGiGameTimer* gt)
{
	// Cycle through the circular frame resource array.
	mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % NUM_FRAME_RESOURCES;
	mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

	// Has the GPU finished processing the commands of the current frame resource?
	// If not, wait until the GPU has completed commands up to this fence point.
	if (mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}

	//
	// Animate the lights (and hence shadows).
	//

	mLightRotationAngle += 0.1f * gt->DeltaTime();

	XMMATRIX R = XMMatrixRotationY(mLightRotationAngle);
	for (int i = 0; i < 3; ++i)
	{
		XMVECTOR lightDir = XMLoadFloat3(&mBaseLightDirections[i]);
		lightDir = XMVector3TransformNormal(lightDir, R);
		XMStoreFloat3(&mRotatedLightDirections[i], lightDir);
	}

	GGiCpuProfiler::GetInstance().StartCpuProfile("Cpu Update Constant Buffers");

	ScriptUpdate(gt);

	UpdateObjectCBs(gt);
	UpdateMaterialBuffer(gt);
	UpdateShadowTransform(gt);
	UpdateSdfDescriptorBuffer(gt);
	UpdateMainPassCB(gt);
	UpdateSkyPassCB(gt);
	UpdateLightCB(gt);

	GGiCpuProfiler::GetInstance().EndCpuProfile("Cpu Update Constant Buffers");

	CullSceneObjects(gt);
}

// Modified by Ssi: ��������͸��/��͸�����Ժ�ˢ��pSceneObjectLayer��pSceneObjects�б��mSceneObjectLayer��mSceneObjects�б�
bool GDxRenderer::UpdateObjectTransparentOpaqueList()
{
	// �˶������Ƿ�ѡ��
	if (pickedSceneObjectforTO == nullptr) return false;
	// �˶�͸�����Ը�ѡ��Imgui���Ƿ񱻹�ѡ/ȡ����ѡ
	if (!pImgui->isTransparentCheckboxChanged) return false;
	// ��ʱ���Ѿ��ܱ�֤��ѡ�������ͬʱ�����������͸�����Խ������޸�

	// ���±�ѡ�������͸������
	pickedSceneObjectforTO->GRiIsTransparent = pImgui->isTransparent;

	// �������pSceneObjects�б��ж�Ӧ�����͸�����ԣ����Ҹ����޸ĺ��͸�����Զ�pSceneObjectLayer[transparent/opaque]�б���
	// ��Ԫ�ؽ�������/ɾ��

	// ����pSceneObjects�б��ж�Ӧ�������͸������
	if (pSceneObjects.find(pickedSceneObjectforTO->UniqueName) != pSceneObjects.end())
	{
		pSceneObjects[pickedSceneObjectforTO->UniqueName]->GRiIsTransparent = pImgui->isTransparent;
	}
	else
	{
		assert(0); // �����߼���������
	}

	// �����޸ĺ��͸�����Զ�pSceneObjectLayer[Transparent/Opaque]�б��е�Ԫ�ؽ�������/ɾ��
	// �������pSceneObjectLayer-Opaque��ɾ��������ӵ�pSceneObjectLayer-Transparent��
	if (pImgui->isTransparent) // �����͸�����Ա�����Ϊ��͸����
	{ 
		// �������pSceneObjectLayer-Opaque��ɾ���������ӦpSceneObjectLayer[(int)RenderLayer::Deferred]�б�
		std::vector<GRiSceneObject *>::iterator iter = std::find(pSceneObjectLayer[(int)RenderLayer::Deferred].begin(),
			pSceneObjectLayer[(int)RenderLayer::Deferred].end(), pickedSceneObjectforTO);
		if (iter == pSceneObjectLayer[(int)RenderLayer::Deferred].end()) // δ�ҵ�
		{
			assert(0); // δ��pSceneObjectLayer-Opaque���ҵ�ѡ�е�Ԫ�أ������߼���������
		}
		else
		{
			pSceneObjectLayer[(int)RenderLayer::Deferred].erase(iter); 
		}

		// ��������ӵ�pSceneObjectLayer-Transparent�У������ӦpSceneObjectLayer[(int)RenderLayer::Transparent]�б�
		pSceneObjectLayer[(int)RenderLayer::Transparent].push_back(pickedSceneObjectforTO);

	}
	else // �����͸�����Ա�����Ϊ����͸����
	{
		// �������pSceneObjectLayer-Transparent��ɾ���������ӦpSceneObjectLayer[(int)RenderLayer::Transparent]�б�
		std::vector<GRiSceneObject *>::iterator iter = std::find(pSceneObjectLayer[(int)RenderLayer::Transparent].begin(),
			pSceneObjectLayer[(int)RenderLayer::Transparent].end(), pickedSceneObjectforTO);
		if (iter == pSceneObjectLayer[(int)RenderLayer::Transparent].end()) // δ�ҵ�
		{
			assert(0); // δ��pSceneObjectLayer-Opaque���ҵ�ѡ�е�Ԫ�أ������߼���������
		}
		else
		{
			pSceneObjectLayer[(int)RenderLayer::Transparent].erase(iter);
		}

		// ��������ӵ�pSceneObjectLayer-Opaque�У������ӦpSceneObjectLayer[(int)RenderLayer::Deferred]�б�
		pSceneObjectLayer[(int)RenderLayer::Deferred].push_back(pickedSceneObjectforTO);
	}

	return true;
}


void GDxRenderer::OnResize()
{
	assert(md3dDevice);
	assert(mSwapChain);
	assert(mDirectCmdListAlloc);

	// Flush before changing any resources.
	FlushCommandQueue();

	ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

	// Release the previous resources we will be recreating.
	for (int i = 0; i < SwapChainBufferCount; ++i)
		mSwapChainBuffer[i].Reset();
	mDepthStencilBuffer.Reset();

	// Resize the swap chain.
	ThrowIfFailed(mSwapChain->ResizeBuffers(
		SwapChainBufferCount,
		mClientWidth, mClientHeight,
		mBackBufferFormat,
		DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH));

	mCurrBackBuffer = 0;

	CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHeapHandle(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	for (UINT i = 0; i < SwapChainBufferCount; i++)
	{
		ThrowIfFailed(mSwapChain->GetBuffer(i, IID_PPV_ARGS(&mSwapChainBuffer[i])));
		md3dDevice->CreateRenderTargetView(mSwapChainBuffer[i].Get(), nullptr, rtvHeapHandle);
		rtvHeapHandle.Offset(1, mRtvDescriptorSize);
	}

	D3D12_RESOURCE_DESC depthStencilDesc;
	depthStencilDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	depthStencilDesc.Alignment = 0;
	depthStencilDesc.Width = mClientWidth;
	depthStencilDesc.Height = mClientHeight;
	depthStencilDesc.DepthOrArraySize = 1;
	depthStencilDesc.MipLevels = 1;
	depthStencilDesc.Format = DXGI_FORMAT_R32G8X24_TYPELESS; //DXGI_FORMAT_R24G8_TYPELESS;
	depthStencilDesc.SampleDesc.Count = 1;
	depthStencilDesc.SampleDesc.Quality = 0;
	depthStencilDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	depthStencilDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

	D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
	dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
	dsv_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; //DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsv_desc.Texture2D.MipSlice = 0;

	D3D12_CLEAR_VALUE optClear; // ���������Դ���Ż�ֵ��������������ִ���ٶ�
	optClear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; //DXGI_FORMAT_D24_UNORM_S8_UINT;
	optClear.DepthStencil.Depth = 1.0f; // ��ʼ���ֵΪ1
	optClear.DepthStencil.Stencil = 0;  // ��ʼģ��ֵΪ0
	ThrowIfFailed(md3dDevice->CreateCommittedResource(
		&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
		D3D12_HEAP_FLAG_NONE,
		&depthStencilDesc,
		D3D12_RESOURCE_STATE_COMMON,
		&optClear,
		IID_PPV_ARGS(mDepthStencilBuffer.GetAddressOf())));

	if (mDepthBufferSrvIndex != 0)
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; //DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

		md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &srvDesc, GetCpuSrv(mDepthBufferSrvIndex));
	}

	// Create descriptor to mip level 0 of entire resource using the format of the resource.
	md3dDevice->CreateDepthStencilView(mDepthStencilBuffer.Get(), &dsv_desc, DepthStencilView());

	// Create depth readback buffer
	{
		// Free the old resources if they exist.
		if (mDepthReadbackBuffer != nullptr)
			mDepthReadbackBuffer.Reset();

		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(DEPTH_READBACK_BUFFER_SIZE * 4),
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(mDepthReadbackBuffer.GetAddressOf())));
	}

	// Transition the resource from its initial state to be used as a depth buffer.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mDepthStencilBuffer.Get(),
		D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_DEPTH_WRITE));

	// Execute the resize commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until resize is complete.
	FlushCommandQueue();

	// Update the viewport transform to cover the client area.
	mScreenViewport.TopLeftX = 0;
	mScreenViewport.TopLeftY = 0;
	mScreenViewport.Width = static_cast<float>(mClientWidth);
	mScreenViewport.Height = static_cast<float>(mClientHeight);
	mScreenViewport.MinDepth = 0.0f;
	mScreenViewport.MaxDepth = 1.0f;

	mScissorRect = { 0, 0, mClientWidth, mClientHeight };

	for (auto &rtvHeap : mRtvHeaps)
	{
		if (rtvHeap.second->mRtv.size() != 0 && rtvHeap.second->mRtv[0]->mResource != nullptr)
		{
			rtvHeap.second->OnResize(mClientWidth, mClientHeight);
		}
	}

	// UAV��unordered access view ���������ͼ�������������Դ����ͼ���������������������飩����ͨ������߳���ʱ���������/д����
	for (auto &uav : mUavs)
	{
		if (uav.second->ResourceNotNull())
		{
			if (uav.second->IsTexture())
				uav.second->OnResize(mClientWidth, mClientHeight);
			else
			{
				if (uav.first == "TileClusterPass")
				{
					UINT64 elementNum = 0;
#if USE_TBDR
					elementNum = UINT64((float)ceilf(mClientWidth / (float)TILE_SIZE_X) * ceilf((float)mClientHeight / (float)TILE_SIZE_Y) + 0.01);
#elif USE_CBDR
					elementNum = (UINT64)(ceilf((float)mClientWidth / (float)CLUSTER_SIZE_X) * ceilf((float)mClientHeight / (float)CLUSTER_SIZE_Y) * CLUSTER_NUM_Z + 0.01);
#else
					ThrowGGiException("TBDR/CBDR not enabled");
#endif
					uav.second->OnBufferResize((UINT)elementNum);
				}
			}
		}
	}
}

#pragma endregion

#pragma region Update

void GDxRenderer::ScriptUpdate(const GGiGameTimer* gt)
{
	if (pSceneObjects.find(L"MovingObject") != pSceneObjects.end())
	{
		std::vector<float> loc = pSceneObjects[L"MovingObject"]->GetLocation();
		pSceneObjects[L"MovingObject"]->SetLocation(1000.0f * DirectX::XMScalarSin(gt->TotalTime() * 2 * GGiEngineUtil::PI), loc[1], loc[2]);
	}

	//auto test1 = GGiFloat4x4::Identity();
	//auto test2 = GGiFloat4x4::Identity();
	//auto test3 = GGiFloat4x4::Identity();
	//auto test4 = test1 * test2;
	//auto test5 = test4 * test3;
	//test5 = test5;
}

void GDxRenderer::UpdateObjectCBs(const GGiGameTimer* gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();

	
	for (auto& e : pSceneObjects)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if (e.second->NumFramesDirty > 0 || pImgui->isAlphaChanged)
		{
			e.second->UpdateTransform();

			/*
			auto dxTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTransform());
			if (dxTrans == nullptr)
				ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");

			auto dxPrevTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetPrevTransform());
			if (dxPrevTrans == nullptr)
				ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");

			auto dxTexTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTexTransform());
			if (dxTexTrans == nullptr)
				ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");
			*/
			
			//XMMATRIX renderObjectTrans = XMLoadFloat4x4(&(dxTrans->GetValue())); 
			XMMATRIX renderObjectTrans = GDx::GGiToDxMatrix(e.second->GetTransform());
			XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(renderObjectTrans), renderObjectTrans);
			XMMATRIX invTransWorld = XMMatrixTranspose(invWorld);
			//XMMATRIX prevWorld = XMLoadFloat4x4(&(dxPrevTrans->GetValue()));
			XMMATRIX prevWorld = GDx::GGiToDxMatrix(e.second->GetPrevTransform());
			//auto tempSubTrans = e->GetSubmesh().Transform;
			//XMMATRIX submeshTrans = XMLoadFloat4x4(&tempSubTrans);
			//XMMATRIX texTransform = XMLoadFloat4x4(&(dxTexTrans->GetValue()));
			XMMATRIX texTransform = GDx::GGiToDxMatrix(e.second->GetTexTransform());
			//auto world = submeshTrans * renderObjectTrans;
			auto world = renderObjectTrans;

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
			XMStoreFloat4x4(&objConstants.PrevWorld, XMMatrixTranspose(prevWorld));
			XMStoreFloat4x4(&objConstants.InvTransWorld, XMMatrixTranspose(invTransWorld));
			XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
			
			// Modified by Ssi: ����������峣���������е�alphaֵ
			objConstants.blend_alpha = pImgui->adjustAlpha; // 0.0f;
			// Debug
			ImGui::Text(std::to_string(pImgui->adjustAlpha).c_str());


			/*
			if (e.second->GetMesh()->NumFramesDirty > 0)
			{
				objConstants.MaterialIndex = e.second->GetMaterial()->MatIndex;
			}
			*/

			currObjectCB->CopyData(e.second->GetObjIndex(), objConstants); // �������峣����������ֵ

			// Next FrameResource need to be updated too.
			e.second->NumFramesDirty--;
		}
	}
}

void GDxRenderer::UpdateLightCB(const GGiGameTimer* gt)
{
	LightConstants lightCB;

	auto pos = pCamera->GetPosition();
	lightCB.cameraPosition = DirectX::XMFLOAT3(pos[0], pos[1], pos[2]);

	lightCB.dirLight[0].Direction[0] = 0.57735f;
	lightCB.dirLight[0].Direction[1] = -0.57735f;
	lightCB.dirLight[0].Direction[2] = -0.57735f;
	lightCB.dirLight[0].DiffuseColor[0] = 0.7f;
	lightCB.dirLight[0].DiffuseColor[1] = 0.7f;
	lightCB.dirLight[0].DiffuseColor[2] = 0.6f;
	lightCB.dirLight[0].DiffuseColor[3] = 1.0f;
	lightCB.dirLight[0].AmbientColor[0] = 0.0f;
	lightCB.dirLight[0].AmbientColor[1] = 0.0f;
	lightCB.dirLight[0].AmbientColor[2] = 0.0f;
	lightCB.dirLight[0].AmbientColor[3] = 1.0f;
	lightCB.dirLight[0].Intensity = 3.0f;

	lightCB.dirLight[1].Direction[0] = -0.57735f;
	lightCB.dirLight[1].Direction[1] = -0.57735f;
	lightCB.dirLight[1].Direction[2] = -0.57735f;
	lightCB.dirLight[1].DiffuseColor[0] = 0.6f;
	lightCB.dirLight[1].DiffuseColor[1] = 0.6f;
	lightCB.dirLight[1].DiffuseColor[2] = 0.6f;
	lightCB.dirLight[1].DiffuseColor[3] = 1.0f;
	lightCB.dirLight[1].AmbientColor[0] = 0.0f;
	lightCB.dirLight[1].AmbientColor[1] = 0.0f;
	lightCB.dirLight[1].AmbientColor[2] = 0.0f;
	lightCB.dirLight[1].AmbientColor[3] = 1.0f;
	lightCB.dirLight[1].Intensity = 3.0f;

	lightCB.dirLight[2].Direction[0] = 0.0;
	lightCB.dirLight[2].Direction[1] = -0.707f;
	lightCB.dirLight[2].Direction[2] = 0.707f;
	lightCB.dirLight[2].DiffuseColor[0] = 0.5f;
	lightCB.dirLight[2].DiffuseColor[1] = 0.5f;
	lightCB.dirLight[2].DiffuseColor[2] = 0.5f;
	lightCB.dirLight[2].DiffuseColor[3] = 1.0f;
	lightCB.dirLight[2].AmbientColor[0] = 0.0f;
	lightCB.dirLight[2].AmbientColor[1] = 0.0f;
	lightCB.dirLight[2].AmbientColor[2] = 0.0f;
	lightCB.dirLight[2].AmbientColor[3] = 1.0f;
	lightCB.dirLight[2].Intensity = 3.0f;

	lightCB.pointLight[0].Color[0] = 1.0f;
	lightCB.pointLight[0].Color[1] = 1.0f;
	lightCB.pointLight[0].Color[2] = 1.0f;
	lightCB.pointLight[0].Color[3] = 1.0f;
	lightCB.pointLight[0].Intensity = 100.0f;
	lightCB.pointLight[0].Position[0] = 0.0f;
	lightCB.pointLight[0].Position[1] = -4.0f;
	lightCB.pointLight[0].Position[2] = 0.0f;
	lightCB.pointLight[0].Range = 100.0f;

	/*
	int lightCount = 16;
	for (int i = -lightCount; i < lightCount; i++)
	{
		for (int j = -lightCount; j < lightCount; j++)
		{
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[0] = ((abs(i * j + 1) % 8) * 0.1f + 0.2f);
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[1] = ((abs(i * j + 2) % 7) * 0.1f + 0.3f);
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[2] = ((abs(i * j) % 6) * 0.1f + 0.4f);
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Color[3] = 1.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Intensity = 500.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Position[0] = i * 30.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Position[1] = -15.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Position[2] = j * 30.0f;
			lightCB.pointLight[(i + lightCount) * 2 * lightCount + j + lightCount].Range = 50.0f;
		}
	}
	*/

	lightCB.pointLight[0].Color[0] = 1.0f;
	lightCB.pointLight[0].Color[1] = 1.0f;
	lightCB.pointLight[0].Color[2] = 1.0f;
	lightCB.pointLight[0].Color[3] = 1.0f;
	lightCB.pointLight[0].Intensity = 5000.0f;
	lightCB.pointLight[0].Position[0] = 0.0f;
	lightCB.pointLight[0].Position[1] = 0.0f;
	lightCB.pointLight[0].Position[2] = 0.0f;
	lightCB.pointLight[0].Range = 50000.0f;

	lightCB.pointLight[1].Color[0] = 1.0f;
	lightCB.pointLight[1].Color[1] = 1.0f;
	lightCB.pointLight[1].Color[2] = 1.0f;
	lightCB.pointLight[1].Color[3] = 1.0f;
	lightCB.pointLight[1].Intensity = 5000.0f;
	lightCB.pointLight[1].Position[0] = 800.0f;
	lightCB.pointLight[1].Position[1] = 0.0f;
	lightCB.pointLight[1].Position[2] = 800.0f;
	lightCB.pointLight[1].Range = 50000.0f;

	lightCB.pointLight[2].Color[0] = 1.0f;
	lightCB.pointLight[2].Color[1] = 1.0f;
	lightCB.pointLight[2].Color[2] = 1.0f;
	lightCB.pointLight[2].Color[3] = 1.0f;
	lightCB.pointLight[2].Intensity = 5000.0f;
	lightCB.pointLight[2].Position[0] = 800.0f;
	lightCB.pointLight[2].Position[1] = 0.0f;
	lightCB.pointLight[2].Position[2] = -800.0f;
	lightCB.pointLight[2].Range = 50000.0f;

	lightCB.dirLightCount = 3;
	lightCB.pointLightCount = 0;// 4 * lightCount * lightCount;

	auto LightCB = mCurrFrameResource->LightCB.get();
	LightCB->CopyData(0, lightCB);
}

void GDxRenderer::UpdateMaterialBuffer(const GGiGameTimer* gt)
{
	auto currMaterialBuffer = mCurrFrameResource->MaterialBuffer.get();
	for (auto& e : pMaterials)
	{
		// Only update the cbuffer data if the constants have changed.  If the cbuffer
		// data changes, it needs to be updated for each FrameResource.
		GRiMaterial* mat = e.second;
		if (mat->NumFramesDirty > 0)
		{
			MaterialData matData;
			int i;
			XMMATRIX matTransform = DirectX::XMMatrixScaling(mat->GetScaleX(), mat->GetScaleY(), 1.0f);
			//GGiFloat4x4* ggiMat = mat->MatTransform.get();
			//GDxFloat4x4* dxMat = dynamic_cast<GDxFloat4x4*>(ggiMat);
			//if (dxMat == nullptr)
				//ThrowDxException(L"Dynamic cast from GRiFloat4x4 to GDxFloat4x4 failed.");
			//XMMATRIX matTransform = XMLoadFloat4x4(&dxMat->GetValue());
			XMStoreFloat4x4(&matData.MatTransform, XMMatrixTranspose(matTransform));

			size_t texNum = mat->GetTextureNum();
			if (texNum > MATERIAL_MAX_TEXTURE_NUM)
				ThrowDxException(L"Material (CBIndex : " + std::to_wstring(mat->MatIndex) + L" ) texture number exceeds MATERIAL_MAX_TEXTURE_NUM.");
			for (i = 0; i < texNum; i++)
			{
				auto texName = mat->GetTextureUniqueNameByIndex(i);
				if (pTextures.find(texName) == pTextures.end())
					ThrowGGiException(L"Texture" + texName + L" not found.");
				matData.TextureIndex[i] = pTextures[texName]->texIndex;
			}

			size_t scalarNum = mat->GetScalarNum();
			if (scalarNum > MATERIAL_MAX_SCALAR_NUM)
				ThrowDxException(L"Material (CBIndex : " + std::to_wstring(mat->MatIndex) + L" ) scalar number exceeds MATERIAL_MAX_SCALAR_NUM.");
			for (i = 0; i < scalarNum; i++)
			{
				matData.ScalarParams[i] = mat->GetScalar(i);
			}

			size_t vectorNum = mat->GetVectorNum();
			if (vectorNum > MATERIAL_MAX_VECTOR_NUM)
				ThrowDxException(L"Material (CBIndex : " + std::to_wstring(mat->MatIndex) + L" ) vector number exceeds MATERIAL_MAX_VECTOR_NUM.");
			for (i = 0; i < vectorNum; i++)
			{
				XMVECTOR ggiVec = mat->GetVector(i);
				DirectX::XMStoreFloat4(&matData.VectorParams[i], ggiVec);
			}
			//matData.DiffuseMapIndex = mat->DiffuseSrvHeapIndex;
			//matData.NormalMapIndex = mat->NormalSrvHeapIndex;
			//matData.Roughness = mat->Roughness;
			//matData.DiffuseAlbedo = mat->DiffuseAlbedo;
			//matData.FresnelR0 = mat->FresnelR0;

			currMaterialBuffer->CopyData(mat->MatIndex, matData);

			// Next FrameResource need to be updated too.
			mat->NumFramesDirty--;
		}
	}
}

void GDxRenderer::UpdateSdfDescriptorBuffer(const GGiGameTimer* gt)
{
	auto currDescBuffer = mCurrFrameResource->SceneObjectSdfDescriptorBuffer.get();

	int soSdfIndex = 0;
	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		if (so->GetMesh()->GetSdf() != nullptr && so->GetMesh()->GetSdf()->size() > 0)
		{
			so->UpdateTransform();

			auto centerOffsetMat = DirectX::XMMatrixTranslation(
				so->GetMesh()->bounds.Center[0],
				so->GetMesh()->bounds.Center[1],
				so->GetMesh()->bounds.Center[2]
			);

			auto trans = GDx::GGiToDxMatrix(so->GetTransform());
			trans = DirectX::XMMatrixMultiply(centerOffsetMat, trans);

			auto worldCenter = XMVectorSet(so->GetMesh()->bounds.Center[0],
				so->GetMesh()->bounds.Center[1],
				so->GetMesh()->bounds.Center[2],
				1.0f);
			auto world = GDx::GGiToDxMatrix(so->GetTransform());
			worldCenter = XMVector4Transform(worldCenter, world);
			auto lightCenter = XMVector4Transform(worldCenter, mSdfTileSpaceView);

			int extAxis = so->GetMesh()->bounds.MaximumExtent();
			float boundX = so->GetScale()[0] * (so->GetMesh()->bounds.Extents[extAxis] * 1.4f + SDF_OUT_OF_BOX_RANGE);
			float boundY = so->GetScale()[1] * (so->GetMesh()->bounds.Extents[extAxis] * 1.4f + SDF_OUT_OF_BOX_RANGE);
			float boundZ = so->GetScale()[2] * (so->GetMesh()->bounds.Extents[extAxis] * 1.4f + SDF_OUT_OF_BOX_RANGE);
			float worldRadius = sqrt(boundX * boundX + boundY * boundY + boundZ * boundZ);

			mSceneObjectSdfDescriptors[soSdfIndex].SdfIndex = so->GetMesh()->mSdfIndex;

			XMStoreFloat4(&mSceneObjectSdfDescriptors[soSdfIndex].objWorldSpaceCenter, worldCenter);
			XMStoreFloat4(&mSceneObjectSdfDescriptors[soSdfIndex].objLightSpaceCenter, lightCenter);

			DirectX::XMStoreFloat4x4(&mSceneObjectSdfDescriptors[soSdfIndex].objWorld, XMMatrixTranspose(trans));
			auto invTrans = DirectX::XMMatrixInverse(&XMMatrixDeterminant(trans), trans);
			DirectX::XMStoreFloat4x4(&mSceneObjectSdfDescriptors[soSdfIndex].objInvWorld, XMMatrixTranspose(invTrans));
			auto invTrans_IT = XMMatrixTranspose(trans);
			DirectX::XMStoreFloat4x4(&mSceneObjectSdfDescriptors[soSdfIndex].objInvWorld_IT, XMMatrixTranspose(invTrans_IT));

			mSceneObjectSdfDescriptors[soSdfIndex].worldRadius = worldRadius;

			currDescBuffer->CopyData(soSdfIndex, mSceneObjectSdfDescriptors[soSdfIndex]);

			soSdfIndex++;
		}
	}
	mSceneObjectSdfNum = soSdfIndex;
}

void GDxRenderer::UpdateShadowTransform(const GGiGameTimer* gt)
{
	{
		int cascadeInd = mFrameCount % ShadowCascadeNum;

		// Calculate Frustum Corner Position.
		GGiFloat3 cornerPos[8];
		cornerPos[0] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd], true, true);
		cornerPos[1] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd], false, true);
		cornerPos[2] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd], true, false);
		cornerPos[3] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd], false, false);
		cornerPos[4] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd + 1], true, true);
		cornerPos[5] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd + 1], false, true);
		cornerPos[6] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd + 1], true, false);
		cornerPos[7] = pCamera->GetCornerPos(ShadowCascadeDistance[cascadeInd + 1], false, false);

		// Calculate Light Transform Matrix.
		auto cameraPos = pCamera->GetPosition();
		auto cameraLook = pCamera->GetLook();
		auto cameraPosVec = GGiFloat3(cameraPos[0], cameraPos[1], cameraPos[2]);
		auto cameraLookVec = GGiFloat3(cameraLook[0], cameraLook[1], cameraLook[2]);
		auto lightLoc = cameraPosVec;//MainDirectionalLightDir * ShadowMapCameraDis + cameraPosVec;

		/*
		auto lightT = DirectX::XMMatrixTranslation(lightLoc[0], lightLoc[1], lightLoc[2]);

		//auto lightR = DirectX::XMMatrixRotationRollPitchYaw(Rotation[0] * GGiEngineUtil::PI / 180.0f, Rotation[1] * GGiEngineUtil::PI / 180.0f, Rotation[2] * GGiEngineUtil::PI / 180.0f);
		auto upDir = GGiFloat3(0.0f, 1.0f, 0.0f);
		auto rightDir = GGiFloat3::Normalize(GGiFloat3::Cross(upDir, MainDirectionalLightDir));
		//XMMatrixSet(Right.x, Right.y, Right.z, 0.0f, Fwd.x, Fwd.y, Fwd.z, 0.0f, Up.x, Up.y, Up.z, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
		auto lightR = XMMatrixSet(rightDir.x, rightDir.y, rightDir.z, 0, MainDirectionalLightDir.x, MainDirectionalLightDir.y, MainDirectionalLightDir.z, 0, 0.0f, 1.0f, 0.0f, 0.0f, 0, 0, 0, 1);

		auto lightS = DirectX::XMMatrixScaling(1.0f, 1.0f, 1.0f);

		auto lightTransMat = DirectX::XMMatrixMultiply(DirectX::XMMatrixMultiply(lightS, lightR), lightT);
		auto invLightTransMat = XMMatrixInverse(&XMMatrixDeterminant(lightTransMat), lightTransMat);
		*/
		auto lightTargetPosVec = lightLoc + MainDirectionalLightDir * 100.0f;
		auto eyePos = XMVectorSet(lightLoc.x, lightLoc.y, lightLoc.z, 1.0f);
		auto focusPos = XMVectorSet(lightTargetPosVec.x, lightTargetPosVec.y, lightTargetPosVec.z, 1.0f);
		XMMATRIX invLightTransMat = XMMatrixLookAtLH(eyePos, focusPos, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		auto lightTransMat = XMMatrixInverse(&XMMatrixDeterminant(invLightTransMat), invLightTransMat);

		// Transform frustum corners to light space to get light space AABB.
		float xmin, xmax, ymin, ymax, zmin, zmax;
		for (int i = 0; i < 8; i++)
		{
			auto worldSpacePos = XMVectorSet(cornerPos[i].x, cornerPos[i].y, cornerPos[i].z, 1.0f);
			auto lightSpacePos = XMVector4Transform(worldSpacePos, invLightTransMat);
			XMFLOAT4 lightSpacePosVec;
			XMStoreFloat4(&lightSpacePosVec, lightSpacePos);
			if (i == 0)
			{
				xmin = lightSpacePosVec.x;
				xmax = lightSpacePosVec.x;
				ymin = lightSpacePosVec.y;
				ymax = lightSpacePosVec.y;
				zmin = lightSpacePosVec.z;
				zmax = lightSpacePosVec.z;
			}
			else
			{
				if (lightSpacePosVec.x < xmin)
					xmin = lightSpacePosVec.x;
				if (lightSpacePosVec.x > xmax)
					xmax = lightSpacePosVec.x;
				if (lightSpacePosVec.y < ymin)
					ymin = lightSpacePosVec.y;
				if (lightSpacePosVec.y > ymax)
					ymax = lightSpacePosVec.y;
				if (lightSpacePosVec.z < zmin)
					zmin = lightSpacePosVec.z;
				if (lightSpacePosVec.z > zmax)
					zmax = lightSpacePosVec.z;
			}
		}

		// Calculate shadow camera position.
		auto lightSpaceCenterPos = XMVectorSet((xmax + xmin) / 2.0f, (ymax + ymin) / 2.0f, 0.0f, 1.0f);
		auto worldSpaceCenterPos = XMVector4Transform(lightSpaceCenterPos, lightTransMat);
		auto targetPosVec = GGiFloat3(worldSpaceCenterPos.m128_f32[0], worldSpaceCenterPos.m128_f32[1], worldSpaceCenterPos.m128_f32[2]);
		auto lightPosVec = targetPosVec + MainDirectionalLightDir * (-ShadowMapCameraDis);

		XMVECTOR lightPos = XMVectorSet(lightPosVec.x, lightPosVec.y, lightPosVec.z, 1.0f);
		XMVECTOR targetPos = XMVectorSet(targetPosVec.x, targetPosVec.y, targetPosVec.z, 1.0f);
		XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

		float shadowWidth = xmax - xmin;
		float shadowHeight = ymax - ymin;

		// Calculate shadow view and projection matrix.
		XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);
#if USE_REVERSE_Z
		XMMATRIX lightProj = XMMatrixOrthographicLH(shadowWidth, shadowHeight, LIGHT_NEAR_Z, LIGHT_FAR_Z);
#else
		XMMATRIX lightProj = XMMatrixOrthographicLH(shadowWidth, shadowHeight, 0.0f, 2.0f * ShadowMapCameraDis);
#endif

		// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
		XMMATRIX T(
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f);

		XMMATRIX VP = lightView * lightProj;
		XMMATRIX S = VP * T;
		mShadowView[cascadeInd] = lightView;
		mShadowProj[cascadeInd] = lightProj;
		mShadowViewProj[cascadeInd] = VP;
		mShadowTransform[cascadeInd] = S;
	}

	//----------------------------------------------------------------------------------------------
	// Update SDF Tile Space Transform Matrix.
	//----------------------------------------------------------------------------------------------

	{
		// Calculate Frustum Corner Position.
		GGiFloat3 cornerPos[8];
		cornerPos[0] = pCamera->GetCornerPos(Z_LOWER_BOUND, true, true);
		cornerPos[1] = pCamera->GetCornerPos(Z_LOWER_BOUND, false, true);
		cornerPos[2] = pCamera->GetCornerPos(Z_LOWER_BOUND, true, false);
		cornerPos[3] = pCamera->GetCornerPos(Z_LOWER_BOUND, false, false);
		cornerPos[4] = pCamera->GetCornerPos(SDF_SHADOW_DISTANCE, true, true);
		cornerPos[5] = pCamera->GetCornerPos(SDF_SHADOW_DISTANCE, false, true);
		cornerPos[6] = pCamera->GetCornerPos(SDF_SHADOW_DISTANCE, true, false);
		cornerPos[7] = pCamera->GetCornerPos(SDF_SHADOW_DISTANCE, false, false);

		// Calculate Light Transform Matrix.
		auto cameraPos = pCamera->GetPosition();
		auto cameraLook = pCamera->GetLook();
		auto cameraPosVec = GGiFloat3(cameraPos[0], cameraPos[1], cameraPos[2]);
		auto cameraLookVec = GGiFloat3(cameraLook[0], cameraLook[1], cameraLook[2]);
		auto lightLoc = cameraPosVec;

		auto lightTargetPosVec = lightLoc + MainDirectionalLightDir * 100.0f;
		auto eyePos = XMVectorSet(lightLoc.x, lightLoc.y, lightLoc.z, 1.0f);
		auto focusPos = XMVectorSet(lightTargetPosVec.x, lightTargetPosVec.y, lightTargetPosVec.z, 1.0f);
		XMMATRIX invLightTransMat = XMMatrixLookAtLH(eyePos, focusPos, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		auto lightTransMat = XMMatrixInverse(&XMMatrixDeterminant(invLightTransMat), invLightTransMat);

		// Transform frustum corners to light space to get light space AABB.
		float xmin, xmax, ymin, ymax, zmin, zmax;
		for (int i = 0; i < 8; i++)
		{
			auto worldSpacePos = XMVectorSet(cornerPos[i].x, cornerPos[i].y, cornerPos[i].z, 1.0f);
			auto lightSpacePos = XMVector4Transform(worldSpacePos, invLightTransMat);
			XMFLOAT4 lightSpacePosVec;
			XMStoreFloat4(&lightSpacePosVec, lightSpacePos);
			if (i == 0)
			{
				xmin = lightSpacePosVec.x;
				xmax = lightSpacePosVec.x;
				ymin = lightSpacePosVec.y;
				ymax = lightSpacePosVec.y;
				zmin = lightSpacePosVec.z;
				zmax = lightSpacePosVec.z;
			}
			else
			{
				if (lightSpacePosVec.x < xmin)
					xmin = lightSpacePosVec.x;
				if (lightSpacePosVec.x > xmax)
					xmax = lightSpacePosVec.x;
				if (lightSpacePosVec.y < ymin)
					ymin = lightSpacePosVec.y;
				if (lightSpacePosVec.y > ymax)
					ymax = lightSpacePosVec.y;
				if (lightSpacePosVec.z < zmin)
					zmin = lightSpacePosVec.z;
				if (lightSpacePosVec.z > zmax)
					zmax = lightSpacePosVec.z;
			}
		}

		// Calculate shadow camera position.
		auto lightSpaceCenterPos = XMVectorSet((xmax + xmin) / 2.0f, (ymax + ymin) / 2.0f, 0.0f, 1.0f);
		auto worldSpaceCenterPos = XMVector4Transform(lightSpaceCenterPos, lightTransMat);
		auto targetPosVec = GGiFloat3(worldSpaceCenterPos.m128_f32[0], worldSpaceCenterPos.m128_f32[1], worldSpaceCenterPos.m128_f32[2]);
		auto lightPosVec = targetPosVec + MainDirectionalLightDir * (-ShadowMapCameraDis);

		XMVECTOR lightPos = XMVectorSet(lightPosVec.x, lightPosVec.y, lightPosVec.z, 1.0f);
		XMVECTOR targetPos = XMVectorSet(targetPosVec.x, targetPosVec.y, targetPosVec.z, 1.0f);
		XMVECTOR lightUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

		mSdfTileSpaceWidth = xmax - xmin;
		mSdfTileSpaceHeight = ymax - ymin;

		// Calculate shadow view and projection matrix.
		XMMATRIX lightView = XMMatrixLookAtLH(lightPos, targetPos, lightUp);
#if USE_REVERSE_Z
		XMMATRIX lightProj = XMMatrixOrthographicLH(mSdfTileSpaceWidth, mSdfTileSpaceHeight, LIGHT_NEAR_Z, LIGHT_FAR_Z);
#else
		XMMATRIX lightProj = XMMatrixOrthographicLH(mSdfTileSpaceWidth, mSdfTileSpaceHeight, 0.0f, 2.0f * ShadowMapCameraDis);
#endif

		// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
		XMMATRIX T(
			0.5f, 0.0f, 0.0f, 0.0f,
			0.0f, -0.5f, 0.0f, 0.0f,
			0.0f, 0.0f, 1.0f, 0.0f,
			0.5f, 0.5f, 0.0f, 1.0f);

		XMMATRIX VP = lightView * lightProj;
		XMMATRIX S = VP * T;

		mSdfTileSpaceView = lightView;
		mSdfTileSpaceTransform = S;
	}
}

void GDxRenderer::UpdateMainPassCB(const GGiGameTimer* gt)
{
	/*
	auto viewMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetView());
	if (viewMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto projMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetProj());
	if (projMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto prevViewProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetPrevViewProj());
	if (prevViewProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");
	*/

	UINT subsampIndex = mFrameCount % TAA_SAMPLE_COUNT;
	double JitterX = Halton_2[subsampIndex] / (double)mClientWidth * (double)TAA_JITTER_DISTANCE;
	double JitterY = Halton_3[subsampIndex] / (double)mClientHeight * (double)TAA_JITTER_DISTANCE;
	//XMMATRIX view = DirectX::XMLoadFloat4x4(&(viewMat->GetValue()));
	XMMATRIX view = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX proj = GDx::GGiToDxMatrix(pCamera->GetProj());
	proj.r[2].m128_f32[0] += (float)JitterX;//_31
	proj.r[2].m128_f32[1] += (float)JitterY;//_32
	//XMMATRIX proj = DirectX::XMLoadFloat4x4(&(projMat->GetValue()));
	//proj.r[2].m128_f32[0] += JitterX;
	//proj.r[2].m128_f32[1] += JitterY;

	XMMATRIX unjitteredProj = GDx::GGiToDxMatrix(pCamera->GetProj());
	XMMATRIX prevViewProj = GDx::GGiToDxMatrix(pCamera->GetPrevViewProj());

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX unjitteredViewProj = XMMatrixMultiply(view, unjitteredProj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX unjitteredInvProj = XMMatrixInverse(&XMMatrixDeterminant(unjitteredProj), unjitteredProj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);
	unjitteredProj = GDx::GGiToDxMatrix(pCamera->GetProj());

	// Transform NDC space [-1,+1]^2 to texture space [0,1]^2
	XMMATRIX T(
		0.5f, 0.0f, 0.0f, 0.0f,
		0.0f, -0.5f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f,
		0.5f, 0.5f, 0.0f, 1.0f
	);

	XMMATRIX viewProjTex = XMMatrixMultiply(viewProj, T);
	XMMATRIX lightTransform = XMLoadFloat4x4(&mLightTransform);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.UnjitteredProj, XMMatrixTranspose(unjitteredProj));
	XMStoreFloat4x4(&mMainPassCB.UnjitteredInvProj, XMMatrixTranspose(unjitteredInvProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.UnjitteredViewProj, XMMatrixTranspose(unjitteredViewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	XMStoreFloat4x4(&mMainPassCB.PrevViewProj, XMMatrixTranspose(prevViewProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProjTex, XMMatrixTranspose(viewProjTex));
	XMStoreFloat4x4(&mMainPassCB.LightTransform, XMMatrixTranspose(lightTransform));
	auto eyePos = pCamera->GetPosition();
	mMainPassCB.EyePosW = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt->TotalTime();
	mMainPassCB.DeltaTime = gt->DeltaTime();
	mMainPassCB.FrameCount = mFrameCount;
	mMainPassCB.Jitter = XMFLOAT2((float)(JitterX / 2), (float)(-JitterY / 2));//negate Y because world coord and tex coord have different Y axis.
	mMainPassCB.AmbientLight = { 0.4f, 0.4f, 0.6f, 1.0f };
	mMainPassCB.MainDirectionalLightDir = { 0.57735f, -0.57735f, -0.57735f, 0.0f };

	for (int i = 0; i < ShadowCascadeNum; i++)
	{
		XMStoreFloat4x4(&mMainPassCB.ShadowView[i], XMMatrixTranspose(mShadowView[i]));
		XMStoreFloat4x4(&mMainPassCB.ShadowProj[i], XMMatrixTranspose(mShadowProj[i]));
		XMStoreFloat4x4(&mMainPassCB.ShadowViewProj[i], XMMatrixTranspose(mShadowViewProj[i]));
		XMStoreFloat4x4(&mMainPassCB.ShadowTransform[i], XMMatrixTranspose(mShadowTransform[i]));
	}

	mMainPassCB.UniformRandom = XMFLOAT4((float)(rand() / double(RAND_MAX)), 
		(float)(rand() / double(RAND_MAX)), 
		(float)(rand() / double(RAND_MAX)), 
		(float)(rand() / double(RAND_MAX)));

	XMStoreFloat4x4(&mMainPassCB.SdfTileTransform, XMMatrixTranspose(mSdfTileSpaceTransform));
	mMainPassCB.gSdfTileSpaceSize = XMFLOAT4(mSdfTileSpaceWidth,
		mSdfTileSpaceHeight,
		1.0f / mSdfTileSpaceWidth,
		1.0f / mSdfTileSpaceHeight
		);

	mMainPassCB.HaltonUniform2D = GenerateRandomHaltonOffset();

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void GDxRenderer::UpdateSkyPassCB(const GGiGameTimer* gt)
{
	/*
	auto viewMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetView());
	if (viewMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto projMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetProj());
	if (projMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto prevViewProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetPrevViewProj());
	if (prevViewProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");
	*/

	XMMATRIX view = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX proj = GDx::GGiToDxMatrix(pCamera->GetProj());
	UINT subsampIndex = mFrameCount % TAA_SAMPLE_COUNT;
	double JitterX = Halton_2[subsampIndex] / (double)mClientWidth * (double)TAA_JITTER_DISTANCE;
	double JitterY = Halton_3[subsampIndex] / (double)mClientHeight * (double)TAA_JITTER_DISTANCE;
	proj.r[2].m128_f32[0] += (float)JitterX;
	proj.r[2].m128_f32[1] += (float)JitterY;

	XMMATRIX unjitteredProj = GDx::GGiToDxMatrix(pCamera->GetProj());
	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX unjitteredViewProj = XMMatrixMultiply(view, unjitteredProj);
	XMMATRIX prevViewProj = GDx::GGiToDxMatrix(pCamera->GetPrevViewProj());

	XMStoreFloat4x4(&mSkyPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mSkyPassCB.UnjitteredViewProj, XMMatrixTranspose(unjitteredViewProj));
	XMStoreFloat4x4(&mSkyPassCB.PrevViewProj, XMMatrixTranspose(prevViewProj));
	auto eyePos = pCamera->GetPosition();
	mSkyPassCB.EyePosW = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
	auto prevPos = pCamera->GetPrevPosition();
	mSkyPassCB.PrevPos = DirectX::XMFLOAT3(prevPos[0], prevPos[1], prevPos[2]);
	mSkyPassCB.roughness = 0.3f; // doesn't matter

	auto currPassCB = mCurrFrameResource->SkyCB.get();
	currPassCB->CopyData(0, mSkyPassCB);
}

void GDxRenderer::CullSceneObjects(const GGiGameTimer* gt)
{
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Reset cull state.
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		so->SetCullState(CullState::Visible);
	}

	numVisible = 0;
	numFrustumCulled = 0;
	numOcclusionCulled = 0;

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Frustum culling.
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	GGiCpuProfiler::GetInstance().StartCpuProfile("Frustum Culling");

	/*
	auto viewMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetView());
	if (viewMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto projMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetProj());
	if (projMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto revProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetReversedProj());
	if (revProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");

	auto prevViewProjMat = dynamic_cast<GDxFloat4x4*>(pCamera->GetPrevViewProj());
	if (prevViewProjMat == nullptr)
		ThrowGGiException("Cast failed from GGiFloat4x4* to GDxFloat4x4*.");
	*/

	XMMATRIX view = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX proj = GDx::GGiToDxMatrix(pCamera->GetProj());
	XMMATRIX revProj = GDx::GGiToDxMatrix(pCamera->GetReversedProj());
	XMMATRIX prevViewProj = GDx::GGiToDxMatrix(pCamera->GetPrevViewProj());
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	//proj.r[0].m128_f32[0] = (float)((double)proj.r[0].m128_f32[0] * ((double)mClientWidth / (double)mClientHeight) / ((double)DEPTH_READBACK_BUFFER_SIZE_X / (double)DEPTH_READBACK_BUFFER_SIZE_Y));

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invPrevViewProj = XMMatrixInverse(&XMMatrixDeterminant(prevViewProj), prevViewProj);

	BoundingFrustum cameraFrustum;
#if USE_REVERSE_Z
	BoundingFrustum::CreateFromMatrix(cameraFrustum, revProj);
#else
	BoundingFrustum::CreateFromMatrix(mCameraFrustum, proj);
#endif

	UINT32 fcStep;
	if (pSceneObjectLayer[(int)RenderLayer::Deferred].size() > 100)
		fcStep = (UINT32)(pSceneObjectLayer[(int)RenderLayer::Deferred].size() / mRendererThreadPool->GetThreadNum()) + 1;
	else
		fcStep = 100;
	for (auto i = 0u; i < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); i += fcStep)
	{
		mRendererThreadPool->Enqueue([&, i]//pSceneObjectLayer, &viewProj]
		{
			for (auto j = i; j < i + fcStep && j < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); j++)
			{
				auto so = pSceneObjectLayer[(int)RenderLayer::Deferred][j];

				XMMATRIX world = GDx::GGiToDxMatrix(so->GetTransform());

				XMMATRIX localToView = XMMatrixMultiply(world, view);

				BoundingBox bounds;
				bounds.Center = DirectX::XMFLOAT3(so->GetMesh()->bounds.Center);
				bounds.Extents = DirectX::XMFLOAT3(so->GetMesh()->bounds.Extents);

				BoundingBox worldBounds;
				bounds.Transform(worldBounds, localToView);

				// Perform the box/frustum intersection test in local space.
				if ((cameraFrustum.Contains(worldBounds) == DirectX::DISJOINT))
				{
					so->SetCullState(CullState::FrustumCulled);
				}
			}
		}
		);
	}

	mRendererThreadPool->Flush();

	GGiCpuProfiler::GetInstance().EndCpuProfile("Frustum Culling");

	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
	// Occlusion culling
	//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	if (mFrameCount != 0)
	{

		GGiCpuProfiler::GetInstance().StartCpuProfile("Occlusion Culling");

		// Map the data so we can read it on CPU.
		D3D12_RANGE readbackBufferRange = { 0, 4 * DEPTH_READBACK_BUFFER_SIZE };
		float* depthReadbackBuffer = nullptr;
		static float outputTest[DEPTH_READBACK_BUFFER_SIZE_X * DEPTH_READBACK_BUFFER_SIZE_Y];
		static float reprojectedDepthBuffer[DEPTH_READBACK_BUFFER_SIZE_X * DEPTH_READBACK_BUFFER_SIZE_Y];
		ThrowIfFailed(mDepthReadbackBuffer->Map(0, &readbackBufferRange, reinterpret_cast<void**>(&depthReadbackBuffer)));

#if 0
		std::ofstream fout;
#if 0
		for (auto i = 0u; i < DEPTH_READBACK_BUFFER_SIZE; i++)
			depthReadbackBuffer[i] *= 10;
#endif
		fout.open("depth.raw", ios::out | ios::binary);
		fout.write(reinterpret_cast<char*>(depthReadbackBuffer), DEPTH_READBACK_BUFFER_SIZE * 4);
		fout.close();
#if 0
		for (auto i = 0u; i < DEPTH_READBACK_BUFFER_SIZE; i++)
			depthReadbackBuffer[i] /= 10;
#endif
#endif

		D3D12_RANGE emptyRange = { 0, 0 };
		mDepthReadbackBuffer->Unmap(0, &emptyRange);

		// Reproject depth buffer.
		//GGiCpuProfiler::GetInstance().StartCpuProfile("Reprojection");

#if USE_MASKED_DEPTH_BUFFER
		GRiOcclusionCullingRasterizer::GetInstance().ReprojectToMaskedBufferMT(
			mRendererThreadPool.get(),
			depthReadbackBuffer,
			viewProj.r,
			invPrevViewProj.r
		);
#else
		GRiOcclusionCullingRasterizer::GetInstance().Reproject(
			depthReadbackBuffer,
			reprojectedDepthBuffer,
			viewProj.r,
			invPrevViewProj.r
		);
#endif

#if 0
		GRiOcclusionCullingRasterizer::GetInstance().GenerateMaskedBufferDebugImage(outputTest);
#endif

		//GGiCpuProfiler::GetInstance().EndCpuProfile("Reprojection");

		//XMMATRIX worldViewProj;

		//GGiCpuProfiler::GetInstance().StartCpuProfile("Rasterization");

		UINT32 ocStep;
		if (pSceneObjectLayer[(int)RenderLayer::Deferred].size() > 100)
			ocStep = (UINT32)(pSceneObjectLayer[(int)RenderLayer::Deferred].size() / mRendererThreadPool->GetThreadNum()) + 1;
		else
			ocStep = 100;
		for (auto i = 0u; i < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); i += ocStep)
		{
			mRendererThreadPool->Enqueue([&, i]//pSceneObjectLayer, &viewProj]
			{
				for (auto j = i; j < i + ocStep && j < pSceneObjectLayer[(int)RenderLayer::Deferred].size(); j++)
				{
					auto so = pSceneObjectLayer[(int)RenderLayer::Deferred][j];

					if (so->GetCullState() == CullState::FrustumCulled)
						continue;

					XMMATRIX sceneObjectTrans = GDx::GGiToDxMatrix(so->GetTransform());

					XMMATRIX worldViewProj = XMMatrixMultiply(sceneObjectTrans, viewProj);

#if USE_MASKED_DEPTH_BUFFER
					auto bOccCulled = !GRiOcclusionCullingRasterizer::GetInstance().RectTestBBoxMasked(
						so->GetMesh()->bounds,
						worldViewProj.r
					);
#else
					auto bOccCulled = !GRiOcclusionCullingRasterizer::GetInstance().RasterizeAndTestBBox(
						so->GetMesh()->bounds,
						worldViewProj.r,
						reprojectedDepthBuffer,
						outputTest
					);
#endif

					if (bOccCulled)
					{
						so->SetCullState(CullState::OcclusionCulled);
					}

				}
			}
			);
		}

		mRendererThreadPool->Flush();

		//GGiCpuProfiler::GetInstance().EndCpuProfile("Rasterization");

		for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
		{
			if (so->GetCullState() == CullState::OcclusionCulled)
			{
				numOcclusionCulled++;
			}
			else if (so->GetCullState() == CullState::FrustumCulled)
			{
				numFrustumCulled++;
			}
			else
			{
				numVisible++;
			}
		}

#if 0
		std::ofstream testOut;
		testOut.open("output.raw", ios::out | ios::binary);
#if 0
		for (auto i = 0u; i < DEPTH_READBACK_BUFFER_SIZE; i++)
			outputTest[i] *= 10;
#endif
		testOut.write(reinterpret_cast<char*>(outputTest), DEPTH_READBACK_BUFFER_SIZE * 4);
		testOut.close();
#endif

		GGiCpuProfiler::GetInstance().EndCpuProfile("Occlusion Culling");
	}
}

#pragma endregion

#pragma region Initialization

void GDxRenderer::SetImgui(GRiImgui* imguiPtr)
{
	GDxImgui* dxImgui = dynamic_cast<GDxImgui*>(imguiPtr);
	if (dxImgui == nullptr)
		ThrowGGiException("Cast failed from GRiImgui* to GDxImgui*.");
	pImgui = dxImgui;
}

// ������ǩ��
void GDxRenderer::BuildRootSignature()
{
	auto staticSamplers = GetStaticSamplers();

	// GBuffer root signature
	{
		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_TEXTURE_NUM, 0);

		CD3DX12_ROOT_PARAMETER gBufferRootParameters[5];
		gBufferRootParameters[0].InitAsConstantBufferView(0);
		gBufferRootParameters[1].InitAsConstants(1, 0, 1);
		gBufferRootParameters[2].InitAsConstantBufferView(1);
		gBufferRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gBufferRootParameters[4].InitAsShaderResourceView(0, 1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gBufferRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["GBuffer"].GetAddressOf())));
	}

	// Depth downsample pass root signature
	{
		//depth inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 0);

		//Output
		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)1, 0);

		CD3DX12_ROOT_PARAMETER depthDownsampleRootParameters[3];
		depthDownsampleRootParameters[0].InitAsConstantBufferView(1);
		depthDownsampleRootParameters[1].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		depthDownsampleRootParameters[2].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, depthDownsampleRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["DepthDownsamplePass"].GetAddressOf())));
	}

	// GBufferDebug root signature
	{
		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, 0);

		CD3DX12_ROOT_PARAMETER gBufferDebugRootParameters[5];
		gBufferDebugRootParameters[0].InitAsConstantBufferView(0);
		gBufferDebugRootParameters[1].InitAsConstants(1, 0, 1);
		gBufferDebugRootParameters[2].InitAsConstantBufferView(1);
		gBufferDebugRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gBufferDebugRootParameters[4].InitAsShaderResourceView(0, 1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gBufferDebugRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["GBufferDebug"].GetAddressOf())));
	}

	// Tile/cluster pass root signature �ֿ�/�ִ�
	{

		//Output
		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 0);

		CD3DX12_ROOT_PARAMETER gLightPassRootParameters[4];
		gLightPassRootParameters[0].InitAsConstantBufferView(0);
		gLightPassRootParameters[1].InitAsConstantBufferView(1);
		gLightPassRootParameters[2].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[3].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, gLightPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["TileClusterPass"].GetAddressOf())));
	}

	// Sdf tile pass root signature
	{
		//Output
		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)1, 0);

		CD3DX12_ROOT_PARAMETER gSdfTilePassRootParameters[5];
		gSdfTilePassRootParameters[0].InitAsConstants(1, 0);
		gSdfTilePassRootParameters[1].InitAsShaderResourceView(0, 0);
		gSdfTilePassRootParameters[2].InitAsShaderResourceView(1, 0);
		gSdfTilePassRootParameters[3].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);
		gSdfTilePassRootParameters[4].InitAsConstantBufferView(1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gSdfTilePassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SdfTilePass"].GetAddressOf())));
	}

	// GTAO raw root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeNormal;
		rangeNormal.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeOrm;
		rangeOrm.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_DESCRIPTOR_RANGE rangeAlbedo;
		rangeAlbedo.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_ROOT_PARAMETER rootParameters[6];
		rootParameters[0].InitAsConstants(7, 0);
		rootParameters[1].InitAsConstantBufferView(1);
		rootParameters[2].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[3].InitAsDescriptorTable(1, &rangeNormal, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[4].InitAsDescriptorTable(1, &rangeOrm, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[5].InitAsDescriptorTable(1, &rangeAlbedo, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(6, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["GtaoRaw"].GetAddressOf())));
	}

	// GTAO filter root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeInput;
		rangeInput.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeVelocity;
		rangeVelocity.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_DESCRIPTOR_RANGE rangeHistory;
		rangeHistory.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_ROOT_PARAMETER rootParameters[5];
		rootParameters[0].InitAsConstantBufferView(1);
		rootParameters[1].InitAsDescriptorTable(1, &rangeInput, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[2].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[3].InitAsDescriptorTable(1, &rangeVelocity, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[4].InitAsDescriptorTable(1, &rangeHistory, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["GtaoFilter"].GetAddressOf())));
	}

	// Shadow map root signature
	{
		CD3DX12_ROOT_PARAMETER gShadowMapRootParameters[2];
		gShadowMapRootParameters[0].InitAsConstantBufferView(0);
		gShadowMapRootParameters[1].InitAsConstantBufferView(1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, gShadowMapRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["ShadowMap"].GetAddressOf())));
	}

	// Shadow map prefilter root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeShadowMap;
		rangeShadowMap.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, (UINT)1, 0);

		CD3DX12_ROOT_PARAMETER gShadowMapPrefilterRootParameters[3];
		gShadowMapPrefilterRootParameters[0].InitAsConstants(1, 0);
		gShadowMapPrefilterRootParameters[1].InitAsDescriptorTable(1, &rangeShadowMap, D3D12_SHADER_VISIBILITY_ALL);
		gShadowMapPrefilterRootParameters[2].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, gShadowMapPrefilterRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["ShadowMapPrefilter"].GetAddressOf())));
	}

	// Screen space shadow pass root signature
	{
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_SCENE_OBJECT_NUM, 0, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 2);

		CD3DX12_DESCRIPTOR_RANGE rangeBlueNoise;
		rangeBlueNoise.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 3);

		CD3DX12_DESCRIPTOR_RANGE rangeTile;
		rangeTile.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 4);

		CD3DX12_DESCRIPTOR_RANGE rangeShadowMap;
		rangeShadowMap.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)ShadowCascadeNum, 5);

		CD3DX12_ROOT_PARAMETER gScreenSpaceShadowRootParameters[9];
		gScreenSpaceShadowRootParameters[0].InitAsConstants(1, 0);
		gScreenSpaceShadowRootParameters[1].InitAsShaderResourceView(0, 0);
		gScreenSpaceShadowRootParameters[2].InitAsShaderResourceView(1, 0);
		gScreenSpaceShadowRootParameters[3].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		gScreenSpaceShadowRootParameters[4].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gScreenSpaceShadowRootParameters[5].InitAsConstantBufferView(1);
		gScreenSpaceShadowRootParameters[6].InitAsDescriptorTable(1, &rangeShadowMap, D3D12_SHADER_VISIBILITY_ALL);
		gScreenSpaceShadowRootParameters[7].InitAsDescriptorTable(1, &rangeBlueNoise, D3D12_SHADER_VISIBILITY_ALL);
		gScreenSpaceShadowRootParameters[8].InitAsDescriptorTable(1, &rangeTile, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(9, gScreenSpaceShadowRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["ScreenSpaceShadowPass"].GetAddressOf())));
	}

	// Screen space shadow temporal filter pass root signature
	{
		//TAA inputs
		CD3DX12_DESCRIPTOR_RANGE rangeInput;
		rangeInput.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE rangeHistory;
		rangeHistory.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
		CD3DX12_DESCRIPTOR_RANGE rangeVelocity;
		rangeVelocity.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_ROOT_PARAMETER gSSShadowTemporalPassRootParameters[4];
		gSSShadowTemporalPassRootParameters[0].InitAsConstantBufferView(1);
		gSSShadowTemporalPassRootParameters[1].InitAsDescriptorTable(1, &rangeInput, D3D12_SHADER_VISIBILITY_ALL);
		gSSShadowTemporalPassRootParameters[2].InitAsDescriptorTable(1, &rangeHistory, D3D12_SHADER_VISIBILITY_ALL);
		gSSShadowTemporalPassRootParameters[3].InitAsDescriptorTable(1, &rangeVelocity, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, gSSShadowTemporalPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP
		);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SSShadowTemporalPass"].GetAddressOf())));
	}

	// Light pass root signature
	{
		//Output
		CD3DX12_DESCRIPTOR_RANGE rangeUav;
		rangeUav.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, 0);

		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors, 1);

		//Depth inputs
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors + 1);

		//Shadow inputs
		CD3DX12_DESCRIPTOR_RANGE rangeShadow;
		rangeShadow.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors + 2);

		//Shadow inputs
		CD3DX12_DESCRIPTOR_RANGE rangeOcclusion;
		rangeOcclusion.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)1, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors + 3);

		//IBL inputs
		CD3DX12_DESCRIPTOR_RANGE rangeIBL;
		rangeIBL.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)mPrefilterLevels + (UINT)1 + (UINT)1, mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors + 4);

		CD3DX12_ROOT_PARAMETER gLightPassRootParameters[8];
		gLightPassRootParameters[0].InitAsConstantBufferView(0);
		gLightPassRootParameters[1].InitAsConstantBufferView(1);
		gLightPassRootParameters[2].InitAsDescriptorTable(1, &rangeUav, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[4].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[5].InitAsDescriptorTable(1, &rangeShadow, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[6].InitAsDescriptorTable(1, &rangeIBL, D3D12_SHADER_VISIBILITY_ALL);
		gLightPassRootParameters[7].InitAsDescriptorTable(1, &rangeOcclusion, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(8, gLightPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;
		
		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["LightPass"].GetAddressOf())));
	}

	// Taa pass root signature
	{
		//TAA inputs
		CD3DX12_DESCRIPTOR_RANGE rangeLight;
		rangeLight.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE rangeHistory;
		rangeHistory.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);
		CD3DX12_DESCRIPTOR_RANGE rangeVelocity;
		rangeVelocity.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_ROOT_PARAMETER gTaaPassRootParameters[5];
		gTaaPassRootParameters[0].InitAsConstantBufferView(1);
		gTaaPassRootParameters[1].InitAsDescriptorTable(1, &rangeLight, D3D12_SHADER_VISIBILITY_ALL);
		gTaaPassRootParameters[2].InitAsDescriptorTable(1, &rangeHistory, D3D12_SHADER_VISIBILITY_ALL);
		gTaaPassRootParameters[3].InitAsDescriptorTable(1, &rangeVelocity, D3D12_SHADER_VISIBILITY_ALL);
		gTaaPassRootParameters[4].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gTaaPassRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
			D3D12_TEXTURE_ADDRESS_MODE_CLAMP
			);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["TaaPass"].GetAddressOf())));
	}

	// SSR depth unjitter root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_ROOT_PARAMETER rootParameters[2];
		rootParameters[0].InitAsConstantBufferView(1);
		rootParameters[1].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SsrDepthUnjitter"].GetAddressOf())));
	}

	// SSR hiz root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_ROOT_PARAMETER rootParameters[1];
		rootParameters[0].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SsrHiz"].GetAddressOf())));
	}

	// SSR prefilter root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeInput;
		rangeInput.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_ROOT_PARAMETER rootParameters[1];
		rootParameters[0].InitAsDescriptorTable(1, &rangeInput, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SsrPrefilter"].GetAddressOf())));
	}

	// SSR trace resolve root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeNormal;
		rangeNormal.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeOrm;
		rangeOrm.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_DESCRIPTOR_RANGE rangeColor;
		rangeColor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_DESCRIPTOR_RANGE rangeBlueNoise;
		rangeBlueNoise.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);

		CD3DX12_DESCRIPTOR_RANGE rangeTrace;
		rangeTrace.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);

		CD3DX12_DESCRIPTOR_RANGE rangeTraceMask;
		rangeTraceMask.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);

		CD3DX12_DESCRIPTOR_RANGE rangeHiZ;
		rangeHiZ.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, SSR_MAX_MIP_LEVEL, 0, 1);

		CD3DX12_DESCRIPTOR_RANGE rangePrefilter;
		rangePrefilter.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, SSR_MAX_PREFILTER_LEVEL, 0, 2);

		CD3DX12_ROOT_PARAMETER rootParameters[11];
		rootParameters[0].InitAsConstantBufferView(1);
		rootParameters[1].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[2].InitAsDescriptorTable(1, &rangeNormal, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[3].InitAsDescriptorTable(1, &rangeOrm, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[4].InitAsDescriptorTable(1, &rangeColor, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[5].InitAsDescriptorTable(1, &rangeBlueNoise, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[6].InitAsConstants(4, 0);
		rootParameters[7].InitAsDescriptorTable(1, &rangeHiZ, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[8].InitAsDescriptorTable(1, &rangeTrace, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[9].InitAsDescriptorTable(1, &rangeTraceMask, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[10].InitAsDescriptorTable(1, &rangePrefilter, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(11, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SsrTraceResolve"].GetAddressOf())));
	}

	// SSR temporal root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeInput;
		rangeInput.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeTrace;
		rangeTrace.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeVelocity;
		rangeVelocity.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_DESCRIPTOR_RANGE rangeHistory;
		rangeHistory.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_ROOT_PARAMETER rootParameters[5];
		rootParameters[0].InitAsConstantBufferView(1);
		rootParameters[1].InitAsDescriptorTable(1, &rangeInput, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[2].InitAsDescriptorTable(1, &rangeTrace, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[3].InitAsDescriptorTable(1, &rangeVelocity, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[4].InitAsDescriptorTable(1, &rangeHistory, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SsrTemporal"].GetAddressOf())));
	}

	// SSR combine root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeNormal;
		rangeNormal.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeOrm;
		rangeOrm.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_DESCRIPTOR_RANGE rangeGf;
		rangeGf.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3);

		CD3DX12_DESCRIPTOR_RANGE rangeOcclusion;
		rangeOcclusion.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 4);

		CD3DX12_DESCRIPTOR_RANGE rangeSceneColor;
		rangeSceneColor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 5);

		CD3DX12_DESCRIPTOR_RANGE AmbientSpecular;
		AmbientSpecular.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 6);

		CD3DX12_DESCRIPTOR_RANGE SsrColor;
		SsrColor.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 7);

		CD3DX12_DESCRIPTOR_RANGE rangeAlbedo;
		rangeAlbedo.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 8);

		CD3DX12_ROOT_PARAMETER rootParameters[10];
		rootParameters[0].InitAsConstantBufferView(1);
		rootParameters[1].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[2].InitAsDescriptorTable(1, &rangeNormal, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[3].InitAsDescriptorTable(1, &rangeOrm, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[4].InitAsDescriptorTable(1, &rangeGf, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[5].InitAsDescriptorTable(1, &rangeOcclusion, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[6].InitAsDescriptorTable(1, &rangeSceneColor, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[7].InitAsDescriptorTable(1, &AmbientSpecular, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[8].InitAsDescriptorTable(1, &SsrColor, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[9].InitAsDescriptorTable(1, &rangeAlbedo, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(10, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SsrCombine"].GetAddressOf())));
	}

	// DoF root signature
	{
		//Motion blur inputs
		CD3DX12_DESCRIPTOR_RANGE rangeInput1;
		rangeInput1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeInput2;
		rangeInput2.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeInput3;
		rangeInput3.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_ROOT_PARAMETER rootParameters[5];
		rootParameters[0].InitAsConstants(5, 0);
		rootParameters[1].InitAsConstantBufferView(1);
		rootParameters[2].InitAsDescriptorTable(1, &rangeInput1, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[3].InitAsDescriptorTable(1, &rangeInput2, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[4].InitAsDescriptorTable(1, &rangeInput3, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["DoF"].GetAddressOf())));
	}

	// Motion blur velocity depth packing pass root signature
	{
		//Motion blur inputs
		CD3DX12_DESCRIPTOR_RANGE rangeVelocity;
		rangeVelocity.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeDepth;
		rangeDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_ROOT_PARAMETER rootParameters[3];
		rootParameters[0].InitAsConstantBufferView(1);
		rootParameters[1].InitAsDescriptorTable(1, &rangeVelocity, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[2].InitAsDescriptorTable(1, &rangeDepth, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["MotionBlurVdPacking"].GetAddressOf())));
	}

	// Motion blur tile max root signature
	{
		//Motion blur inputs
		CD3DX12_DESCRIPTOR_RANGE rangeInputTexture;
		rangeInputTexture.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_ROOT_PARAMETER rootParameters[3];
		rootParameters[0].InitAsConstants(5, 0);
		rootParameters[1].InitAsConstantBufferView(1);
		rootParameters[2].InitAsDescriptorTable(1, &rangeInputTexture, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["MotionBlurTileMax"].GetAddressOf())));
	}

	// Motion blur pass root signature
	{
		//Motion blur inputs
		CD3DX12_DESCRIPTOR_RANGE rangeInput;
		rangeInput.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		CD3DX12_DESCRIPTOR_RANGE rangeVelocityDepth;
		rangeVelocityDepth.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_DESCRIPTOR_RANGE rangeNeighborMax;
		rangeNeighborMax.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2);

		CD3DX12_ROOT_PARAMETER rootParameters[5];
		rootParameters[0].InitAsConstants(3, 0);
		rootParameters[1].InitAsConstantBufferView(1);
		rootParameters[2].InitAsDescriptorTable(1, &rangeInput, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[3].InitAsDescriptorTable(1, &rangeVelocityDepth, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[4].InitAsDescriptorTable(1, &rangeNeighborMax, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["MotionBlurPass"].GetAddressOf())));
	}

	// Bloom root signature
	{
		CD3DX12_DESCRIPTOR_RANGE rangeBloomChainTexture;
		rangeBloomChainTexture.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		CD3DX12_DESCRIPTOR_RANGE rangeInputTexture;
		rangeInputTexture.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1);

		CD3DX12_ROOT_PARAMETER rootParameters[3];
		rootParameters[0].InitAsConstants(7, 0);
		rootParameters[1].InitAsDescriptorTable(1, &rangeBloomChainTexture, D3D12_SHADER_VISIBILITY_ALL);
		rootParameters[2].InitAsDescriptorTable(1, &rangeInputTexture, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, rootParameters,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["Bloom"].GetAddressOf())));
	}

	// Output root signature
	{
		//G-Buffer inputs
		CD3DX12_DESCRIPTOR_RANGE ppInputRange;
		ppInputRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
		//CD3DX12_DESCRIPTOR_RANGE skyRange;
		//skyRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, mRtvHeaps["LightPass"]->mRtvHeap.HeapDesc.NumDescriptors, 1);

		CD3DX12_ROOT_PARAMETER gPostProcessRootParameters[1];
		gPostProcessRootParameters[0].InitAsDescriptorTable(1, &ppInputRange, D3D12_SHADER_VISIBILITY_ALL);
		//gPostProcessRootParameters[1].InitAsDescriptorTable(1, &skyRange, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(1, gPostProcessRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["Output"].GetAddressOf())));
	}

	// SDF debug root signature
	{
		CD3DX12_DESCRIPTOR_RANGE range;
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_SCENE_OBJECT_NUM, 0, 1);

		CD3DX12_ROOT_PARAMETER gSdfDebugRootParameters[5];
		gSdfDebugRootParameters[0].InitAsConstants(1, 0);
		gSdfDebugRootParameters[1].InitAsShaderResourceView(0, 0);
		gSdfDebugRootParameters[2].InitAsShaderResourceView(1, 0);
		gSdfDebugRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		gSdfDebugRootParameters[4].InitAsConstantBufferView(1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, gSdfDebugRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["SdfDebug"].GetAddressOf())));
	}

	// Sky root signature
	{
		CD3DX12_DESCRIPTOR_RANGE texTable0;
		texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0);

		// Root parameter can be a table, root descriptor or root constants.
		CD3DX12_ROOT_PARAMETER slotRootParameter[3];

		// Perfomance TIP: Order from most frequent to least frequent.
		slotRootParameter[0].InitAsConstantBufferView(0);
		slotRootParameter[1].InitAsConstantBufferView(1);
		slotRootParameter[2].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_ALL);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(3, slotRootParameter,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		//ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["Sky"].GetAddressOf())));
	}

	// Forward root signature
	{
		CD3DX12_DESCRIPTOR_RANGE texTable0;
		texTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0);

		CD3DX12_DESCRIPTOR_RANGE texTable1;
		texTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, (UINT)pTextures.size(), 3, 0);//10,3,0

		// Root parameter can be a table, root descriptor or root constants.
		CD3DX12_ROOT_PARAMETER slotRootParameter[5];

		// Perfomance TIP: Order from most frequent to least frequent.
		slotRootParameter[0].InitAsConstantBufferView(0);
		slotRootParameter[1].InitAsConstantBufferView(1);
		slotRootParameter[2].InitAsShaderResourceView(0, 1);
		slotRootParameter[3].InitAsDescriptorTable(1, &texTable0, D3D12_SHADER_VISIBILITY_PIXEL);
		slotRootParameter[4].InitAsDescriptorTable(1, &texTable1, D3D12_SHADER_VISIBILITY_PIXEL);

		auto staticSamplers = GetStaticSamplers();

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, slotRootParameter,
			(UINT)staticSamplers.size(), staticSamplers.data(),
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		//ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["Forward"].GetAddressOf())));
	}

	// Modified by Ssi:
	// Transparent root signature
	{
		CD3DX12_DESCRIPTOR_RANGE range; // ��ǩ���ĸ�����֮��������
		range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, MAX_TEXTURE_NUM, 0); // ��ɫ����Դ��ͼ

		CD3DX12_ROOT_PARAMETER transparentRootParameters[5];	  // ������
		transparentRootParameters[0].InitAsConstantBufferView(0); // ������CBV��ָ���󶨵���ɫ����������Ĵ�����b0��
		transparentRootParameters[1].InitAsConstants(1, 0, 1);
		transparentRootParameters[2].InitAsConstantBufferView(1); // ������CBV��ָ���󶨵���ɫ����������Ĵ�����b1��
		transparentRootParameters[3].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_ALL);
		transparentRootParameters[4].InitAsShaderResourceView(0, 1);

		// A root signature is an array of root parameters.
		CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(5, transparentRootParameters,
			0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		CD3DX12_STATIC_SAMPLER_DESC StaticSamplers[2];
		StaticSamplers[0].Init(0, D3D12_FILTER_ANISOTROPIC);
		StaticSamplers[1].Init(1, D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			D3D12_TEXTURE_ADDRESS_MODE_BORDER,
			0.f, 16u, D3D12_COMPARISON_FUNC_LESS_EQUAL);
		rootSigDesc.NumStaticSamplers = 2;
		rootSigDesc.pStaticSamplers = StaticSamplers;

		// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		ComPtr<ID3DBlob> serializedRootSig = nullptr;
		ComPtr<ID3DBlob> errorBlob = nullptr;
		HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
			serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		if (errorBlob != nullptr)
		{
			::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		}
		ThrowIfFailed(hr);

		ThrowIfFailed(md3dDevice->CreateRootSignature(
			0,
			serializedRootSig->GetBufferPointer(),
			serializedRootSig->GetBufferSize(),
			IID_PPV_ARGS(mRootSignatures["Transparent"].GetAddressOf())));

		//CD3DX12_DESCRIPTOR_RANGE texTable;
		//texTable.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

		//// Root parameter can be a table, root descriptor or root constants.
		//CD3DX12_ROOT_PARAMETER slotRootParameter[4];

		//// Perfomance TIP: Order from most frequent to least frequent.
		//slotRootParameter[0].InitAsDescriptorTable(1, &texTable, D3D12_SHADER_VISIBILITY_PIXEL);
		//slotRootParameter[1].InitAsConstantBufferView(0);
		//slotRootParameter[2].InitAsConstantBufferView(1);
		//slotRootParameter[3].InitAsConstantBufferView(2);

		//auto staticSamplers = GetStaticSamplers();

		//// A root signature is an array of root parameters.
		//CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(4, slotRootParameter,
		//	(UINT)staticSamplers.size(), staticSamplers.data(),
		//	D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

		//// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
		//ComPtr<ID3DBlob> serializedRootSig = nullptr;
		//ComPtr<ID3DBlob> errorBlob = nullptr;
		//HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		//	serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

		//if (errorBlob != nullptr)
		//{
		//	::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
		//}
		//ThrowIfFailed(hr);

		//ThrowIfFailed(md3dDevice->CreateRootSignature(
		//	0,
		//	serializedRootSig->GetBufferPointer(),
		//	serializedRootSig->GetBufferSize(),
		//	IID_PPV_ARGS(mRootSignatures["Transparent"].GetAddressOf())));
	}
}

std::array<const CD3DX12_STATIC_SAMPLER_DESC, 7> GDxRenderer::GetStaticSamplers()
{
	// Applications usually only need a handful of samplers.  So just define them all up front
	// and keep them available as part of the root signature.  

	const CD3DX12_STATIC_SAMPLER_DESC pointWrap(
		0, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC pointClamp(
		1, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearWrap(
		2, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC linearClamp(
		3, // shaderRegister
		D3D12_FILTER_MIN_MAG_MIP_LINEAR, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP); // addressW

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicWrap(
		4, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_WRAP,  // addressW
		0.0f,                             // mipLODBias
		8);                               // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC anisotropicClamp(
		5, // shaderRegister
		D3D12_FILTER_ANISOTROPIC, // filter
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  // addressW
		0.0f,                              // mipLODBias
		8);                                // maxAnisotropy

	const CD3DX12_STATIC_SAMPLER_DESC shadow(
		6, // shaderRegister
		D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, // filter
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressU
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressV
		D3D12_TEXTURE_ADDRESS_MODE_BORDER,  // addressW
		0.0f,                               // mipLODBias
		16,                                 // maxAnisotropy
		D3D12_COMPARISON_FUNC_LESS_EQUAL,
		D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK);

	return {
		pointWrap, pointClamp,
		linearWrap, linearClamp,
		anisotropicWrap, anisotropicClamp,
		shadow
	};
}

void GDxRenderer::BuildDescriptorHeaps()
{
	//
	// Create the SRV heap.
	//
	D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
	srvHeapDesc.NumDescriptors = MAX_TEXTURE_NUM
		+ MAX_SCENE_OBJECT_NUM //sdf textures
		+ 1 //imgui
		+ 1 //sky cubemap
		+ 1 //depth buffer
		+ 2 //downsampled depth buffer
		+ 1 //stencil buffer
		+ 4 //g-buffer
		+ 2 //tile/cluster pass srv and uav
		+ 2 //sdf tile pass srv and uav
		+ ShadowCascadeNum //cascaded shadow map �㼶��Ӱ
		+ ShadowCascadeNum * 2 * 2 //cascaded shadow map prefilter X and Y srv and uav
		+ 1 //screen space shadow
		+ 3 //screen space shadow temporal filter
		+ 7 //gtao
		+ 2 //light pass
		+ 3 //taa
		+ SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 12 //ssr
		+ 5 //dof ����
		+ 7 //motion blur
		+ BloomChainLength * 2 + 1 //bloom
		+ 1 //blue noise
		+ (2 + mPrefilterLevels);//IBL
	srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&mSrvDescriptorHeap)));
	
	//
	// ��ʵ�ʵ�����������
	// Fill out the heap with actual descriptors.
	//

	mSkyTexHeapIndex = 1; // 0 is for imgui.

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());

	//auto skyCubeMap = mTextures["skyCubeMap"]->Resource;
	D3D12_SHADER_RESOURCE_VIEW_DESC skySrvDesc = {};
	skySrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	skySrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
	skySrvDesc.TextureCube.MostDetailedMip = 0;
	GDxTexture* tex = dynamic_cast<GDxTexture*>(pTextures[L"skyCubeMap"]);
	if (tex == nullptr)
		ThrowDxException(L"Dynamic cast from GRiTexture to GDxTexture failed.");
	skySrvDesc.TextureCube.MipLevels = tex->Resource->GetDesc().MipLevels;
	skySrvDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	skySrvDesc.Format = tex->Resource->GetDesc().Format;
	md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &skySrvDesc, GetCpuSrv(mSkyTexHeapIndex));

	// Build SRV for depth/stencil buffer
	{
		mDepthBufferSrvIndex = mSkyTexHeapIndex + 1;
		//mStencilBufferSrvIndex = mDepthBufferSrvIndex + 1;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; //DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

		md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &srvDesc, GetCpuSrv(mDepthBufferSrvIndex));

		D3D12_SHADER_RESOURCE_VIEW_DESC stencilSrvDesc = {};
		stencilSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		//stencilSrvDesc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(1,1,1,1);
		stencilSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		stencilSrvDesc.Texture2D.MipLevels = 1;
		stencilSrvDesc.Texture2D.MostDetailedMip = 0;
		stencilSrvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		//stencilSrvDesc.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT;
		stencilSrvDesc.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

		//md3dDevice->CreateShaderResourceView(mDepthStencilBuffer.Get(), &stencilSrvDesc, GetCpuSrv(mStencilBufferSrvIndex));
	}

	// Build SRV for depth readback buffer.
	{
		mDepthDownsampleSrvIndex = mDepthBufferSrvIndex + 1;

		GDxUavProperties prop;
		prop.mUavFormat = DXGI_FORMAT_R32_FLOAT;
		prop.mClearColor[0] = 0;
		prop.mClearColor[1] = 0;
		prop.mClearColor[2] = 0;
		prop.mClearColor[3] = 1;

		UINT64 elementNum = DEPTH_READBACK_BUFFER_SIZE_X * DEPTH_READBACK_BUFFER_SIZE_Y;

		auto depthDownsamplePassUav = std::make_unique<GDxUav>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mDepthDownsampleSrvIndex), GetGpuSrv(mDepthDownsampleSrvIndex), prop, false, false, false, sizeof(float), elementNum);
		mUavs["DepthDownsamplePass"] = std::move(depthDownsamplePassUav);
	}

	// Build RTV heap and SRV for GBuffers.
	{
		mGBufferSrvIndex = mDepthDownsampleSrvIndex + mUavs["DepthDownsamplePass"]->GetSize();
		mVelocityBufferSrvIndex = mGBufferSrvIndex + 2;

		mGBufferAlbedoSrvIndexOffert = 0;
		mGBufferNormalSrvIndexOffert = 1;
		mGBufferVelocitySrvIndexOffert = 2;
		mGBufferOrmSrvIndexOffert = 3;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R8G8B8A8_UNORM,//Albedo
			DXGI_FORMAT_R8G8B8A8_SNORM, //Normal
			//DXGI_FORMAT_R32G32B32A32_FLOAT, //WorldPos
			DXGI_FORMAT_R16G16_FLOAT, //Velocity
			DXGI_FORMAT_R8G8B8A8_UNORM //OcclusionRoughnessMetallic
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0,0,0,0 },
			{ 0,0,0,0 },
			//{ 0,0,0,0 },
			{ 0,0,0,0 },
			{ 0,0.3f,0,0 }
		};
		std::vector<GRtvProperties> propVec;
		for (size_t i = 0; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto gBufferRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mGBufferSrvIndex), GetGpuSrv(mGBufferSrvIndex), propVec);
		mRtvHeaps["GBuffer"] = std::move(gBufferRtvHeap);
	}

	// Build UAV and SRV for Tile/Cluster pass.
	{
		mTileClusterSrvIndex = mGBufferSrvIndex + mRtvHeaps["GBuffer"]->mRtvHeap.HeapDesc.NumDescriptors;

		GDxUavProperties prop;
		prop.mUavFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		prop.mClearColor[0] = 0;
		prop.mClearColor[1] = 0;
		prop.mClearColor[2] = 0;
		prop.mClearColor[3] = 1;

		UINT64 elementNum = 0;
#if USE_TBDR
		elementNum = UINT64(ceilf((float)mClientWidth / (float)TILE_SIZE_X) * ceilf((float)mClientHeight / (float)TILE_SIZE_Y) + 0.01);
#elif USE_CBDR
		elementNum = (UINT64)(ceilf((float)mClientWidth / (float)CLUSTER_SIZE_X) * ceilf((float)mClientHeight / (float)CLUSTER_SIZE_Y) * CLUSTER_NUM_Z + 0.01);
#else
		ThrowGGiException("TBDR/CBDR not enabled");
#endif

		auto tileClusterPassUav = std::make_unique<GDxUav>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mTileClusterSrvIndex), GetGpuSrv(mTileClusterSrvIndex), prop, false, false, false, sizeof(LightList), elementNum);
		mUavs["TileClusterPass"] = std::move(tileClusterPassUav);
	}

	// Build UAV and SRV for SDF tile pass.
	{
		mSdfTileSrvIndex = mTileClusterSrvIndex + mUavs["TileClusterPass"]->GetSize();

		GDxUavProperties prop;
		prop.mUavFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		prop.mClearColor[0] = 0;
		prop.mClearColor[1] = 0;
		prop.mClearColor[2] = 0;
		prop.mClearColor[3] = 1;

		UINT64 elementNum = SDF_GRID_NUM * SDF_GRID_NUM;

		auto tileClusterPassUav = std::make_unique<GDxUav>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mSdfTileSrvIndex), GetGpuSrv(mSdfTileSrvIndex), prop, false, false, false, sizeof(SdfList), elementNum);
		mUavs["SdfTilePass"] = std::move(tileClusterPassUav);
	}

	// Build DSV and SRV for cascaded shadow map pass.
	{
		mCascadedShadowMapSrvIndex = mSdfTileSrvIndex + mUavs["SdfTilePass"]->GetSize();
		mShadowMapDsvIndex = mDepthDsvIndex + 1;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; //DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

		D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
		dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
		dsv_desc.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT; //DXGI_FORMAT_D24_UNORM_S8_UINT;
		dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
		dsv_desc.Texture2D.MipSlice = 0;

		for (int i = 0; i < ShadowCascadeNum; i++)
		{
			mCascadedShadowMap.push_back(std::make_unique<GDxCascadedShadowMap>(md3dDevice.Get(), ShadowMapResolution));
			md3dDevice->CreateShaderResourceView(mCascadedShadowMap[i]->GetShadowmapResource(), &srvDesc, GetCpuSrv(mCascadedShadowMapSrvIndex + i));
			md3dDevice->CreateDepthStencilView(mCascadedShadowMap[i]->GetShadowmapResource(), &dsv_desc, GetDsv(mShadowMapDsvIndex + i));
		}
	}

	// Build UAV and SRV for shadow map prefilter pass.
	{
		mXPrefilteredShadowMapUavIndex = mCascadedShadowMapSrvIndex + ShadowCascadeNum;
		mYPrefilteredShadowMapUavIndex = mXPrefilteredShadowMapUavIndex + ShadowCascadeNum;
		mXPrefilteredShadowMapSrvIndex = mYPrefilteredShadowMapUavIndex + ShadowCascadeNum;
		mYPrefilteredShadowMapSrvIndex = mXPrefilteredShadowMapSrvIndex + ShadowCascadeNum;

		// Create UAV.
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		// Create SRV.
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;

		ZeroMemory(&srvDesc, sizeof(srvDesc));
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
		srvDesc.Texture2D.MipLevels = 1;

		for (int i = 0; i < ShadowCascadeNum; i++)
		{
			md3dDevice->CreateUnorderedAccessView(mCascadedShadowMap[i]->GetXPrefilteredResource(), nullptr, &uavDesc, GetCpuSrv(mXPrefilteredShadowMapUavIndex + i));
			md3dDevice->CreateUnorderedAccessView(mCascadedShadowMap[i]->GetYPrefilteredResource(), nullptr, &uavDesc, GetCpuSrv(mYPrefilteredShadowMapUavIndex + i));
			md3dDevice->CreateShaderResourceView(mCascadedShadowMap[i]->GetXPrefilteredResource(), &srvDesc, GetCpuSrv(mXPrefilteredShadowMapSrvIndex + i));
			md3dDevice->CreateShaderResourceView(mCascadedShadowMap[i]->GetYPrefilteredResource(), &srvDesc, GetCpuSrv(mYPrefilteredShadowMapSrvIndex + i));
		}
	}

	// Build RTV and SRV for screen space shadow pass.
	{
		mScreenSpaceShadowPassSrvIndex = mYPrefilteredShadowMapSrvIndex + ShadowCascadeNum;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R32_FLOAT// Direct light and ambient light
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 1,1,1,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto screenSpaceShadowPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mScreenSpaceShadowPassSrvIndex), GetGpuSrv(mScreenSpaceShadowPassSrvIndex), propVec);
		mRtvHeaps["ScreenSpaceShadowPass"] = std::move(screenSpaceShadowPassRtvHeap);
	}

	// Build RTV heap and SRV for screen space shadow temporal filter pass.
	{
		mSSShadowTemporalSrvIndex = mScreenSpaceShadowPassSrvIndex + (UINT)mRtvHeaps["ScreenSpaceShadowPass"]->mRtv.size();

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R32_FLOAT,// History 1
			DXGI_FORMAT_R32_FLOAT,// History 2
			DXGI_FORMAT_R32_FLOAT// Output
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 1,1,1,1 },
			{ 1,1,1,1 },
			{ 1,1,1,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto SSShadowTemporalPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mSSShadowTemporalSrvIndex), GetGpuSrv(mSSShadowTemporalSrvIndex), propVec);
		mRtvHeaps["SSShadowTemporalPass"] = std::move(SSShadowTemporalPassRtvHeap);
	}

	// Build RTV heap and SRV for ground truth ambient occlusion pass.
	{
		mGtaoSrvIndex = mSSShadowTemporalSrvIndex + (UINT)mRtvHeaps["SSShadowTemporalPass"]->mRtv.size();

		mGtaoRawSrvIndexOffset = 0;
		mGtaoHistory1SrvIndexOffset = 1;
		mGtaoHistory2SrvIndexOffset = 2;
		mGtaoTemporalSrvIndexOffset = 3;
		mGtaoBlurXSrvIndexOffset = 4;
		mGtaoBlurYSrvIndexOffset = 5;
		mGtaoUpsampleSrvIndexOffset = 6;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R8G8_UNORM,// GTAO Raw
			DXGI_FORMAT_R8G8_UNORM,// History 1
			DXGI_FORMAT_R8G8_UNORM,// History 2
			DXGI_FORMAT_R8G8_UNORM,// Temporal
			DXGI_FORMAT_R8G8_UNORM,// Blur X
			DXGI_FORMAT_R8G8_UNORM,// Blur Y
			DXGI_FORMAT_R8G8_UNORM// Upsample
		};

		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f }
		};

		std::vector<float> rtvResolutionScale =
		{
			GTAO_RESOLUTION_SCALE,
			GTAO_RESOLUTION_SCALE,
			GTAO_RESOLUTION_SCALE,
			GTAO_RESOLUTION_SCALE,
			GTAO_RESOLUTION_SCALE,
			GTAO_RESOLUTION_SCALE,
			1.0f
		};

		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			prop.mWidthPercentage = rtvResolutionScale[i];
			prop.mHeightPercentage = rtvResolutionScale[i];
			propVec.push_back(prop);
		}

		auto rtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mGtaoSrvIndex), GetGpuSrv(mGtaoSrvIndex), propVec);
		mRtvHeaps["GTAO"] = std::move(rtvHeap);
	}

	// Build RTV and SRV for light pass.
	{
		mLightPassSrvIndex = mGtaoSrvIndex + (UINT)mRtvHeaps["GTAO"]->mRtv.size();

		mLightingSrvIndexOffset = 0;
		mLightingAmbientSpecularSrvIndexOffset = 1;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R16G16B16A16_FLOAT,// Direct light and ambient diffuse
			DXGI_FORMAT_R16G16B16A16_FLOAT// ambient specular
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0,0,0,1 },
			{ 0,0,0,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto lightPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mLightPassSrvIndex), GetGpuSrv(mLightPassSrvIndex), propVec);
		mRtvHeaps["LightPass"] = std::move(lightPassRtvHeap);
	}

	// Build RTV heap and SRV for TAA pass.
	{
		mTaaPassSrvIndex = mLightPassSrvIndex + (UINT)mRtvHeaps["LightPass"]->mRtv.size();

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R16G16B16A16_FLOAT,// TAA History 1
			DXGI_FORMAT_R16G16B16A16_FLOAT,// TAA History 2
			DXGI_FORMAT_R16G16B16A16_FLOAT// TAA Output
		};
		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0,0,0,1 },
			{ 0,0,0,1 },
			{ 0,0,0,1 }
		};
		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			propVec.push_back(prop);
		}
		auto taaPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mTaaPassSrvIndex), GetGpuSrv(mTaaPassSrvIndex), propVec);
		mRtvHeaps["TaaPass"] = std::move(taaPassRtvHeap);
	}

	// Build RTV heap and SRV for stochastic screen space reflection pass.
	{
		mSsrSrvIndex = mTaaPassSrvIndex + mRtvHeaps["TaaPass"]->mRtvHeap.HeapDesc.NumDescriptors;

		mSsrHizSrvIndexOffset = 0;
		mSsrPrefilterSrvIndexOffset = SSR_MAX_MIP_LEVEL;
		mSsrTileImportanceSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL;
		mSsrTileImportanceHistorySrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 1;
		mSsrTileImportanceTemporalSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 3;
		mSsrTraceSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 4;
		mSsrTraceMaskSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 5;
		mSsrResolveSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 6;
		mSsrHistorySrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 7;
		mSsrTemporalSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 9;
		mSsrCombineSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 10;
		mSsrUnjitteredDepthSrvIndexOffset = SSR_MAX_MIP_LEVEL + SSR_MAX_PREFILTER_LEVEL + 11;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R8_UNORM,// Tile Importance
			DXGI_FORMAT_R8_UNORM,// Tile Importance History 1
			DXGI_FORMAT_R8_UNORM,// Tile Importance History 2
			DXGI_FORMAT_R8_UNORM,// Tile Importance Temporal
			DXGI_FORMAT_R8G8B8A8_UNORM,// Trace
			DXGI_FORMAT_R8_UNORM,// Mask
			DXGI_FORMAT_R16G16B16A16_FLOAT,// Resolve
			DXGI_FORMAT_R16G16B16A16_FLOAT,// History 1
			DXGI_FORMAT_R16G16B16A16_FLOAT,// History 2
			DXGI_FORMAT_R16G16B16A16_FLOAT,// Temporal
			DXGI_FORMAT_R16G16B16A16_FLOAT,// Combine
			DXGI_FORMAT_R32_FLOAT// Unjittered Depth
		};

		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f }
		};

		std::vector<float> rtvResolutionScale =
		{
			0.1f,
			0.1f,
			0.1f,
			0.1f,
			0.5f,
			0.5f,
			1.0f,
			1.0f,
			1.0f,
			1.0f,
			1.0f,
			1.0f
		};

		auto res = 1.0f;
		for (int i = 0; i < SSR_MAX_PREFILTER_LEVEL; i++)
		{
			res /= 2.0f;
			rtvFormats.insert(rtvFormats.begin() + i, DXGI_FORMAT_R16G16B16A16_FLOAT);// Prefilter
			rtvClearColor.insert(rtvClearColor.begin() + i, { 0.0f, 0.0f, 0.0f, 1.0f });
			rtvResolutionScale.insert(rtvResolutionScale.begin() + i, res);
		}

		res = 1.0f;
		for (int i = 0; i < SSR_MAX_MIP_LEVEL; i++)
		{
			rtvFormats.insert(rtvFormats.begin() + i, DXGI_FORMAT_R32_FLOAT);// HiZ
			rtvClearColor.insert(rtvClearColor.begin() + i, { 0.0f, 0.0f, 0.0f, 1.0f });
			res /= 2.0f;
			rtvResolutionScale.insert(rtvResolutionScale.begin() + i, res);
		}

		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			prop.mWidthPercentage = rtvResolutionScale[i];
			prop.mHeightPercentage = rtvResolutionScale[i];
			propVec.push_back(prop);
		}

		auto rtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mSsrSrvIndex), GetGpuSrv(mSsrSrvIndex), propVec);
		mRtvHeaps["SSR"] = std::move(rtvHeap);
	}

	// Build RTV heap and SRV for depth of field pass.
	{
		mDofSrvIndex = mSsrSrvIndex + mRtvHeaps["SSR"]->mRtvHeap.HeapDesc.NumDescriptors;

		mDofCocSrvIndexOffset = 0;
		mDofPrefilterSrvIndexOffset = 1;
		mDofBokehSrvIndexOffset = 2;
		mDofPostfilterSrvIndexOffset = 3;
		mDofCombineSrvIndexOffset = 4;

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R8_UNORM,// CoC
			DXGI_FORMAT_R16G16B16A16_FLOAT,// Prefilter
			DXGI_FORMAT_R16G16B16A16_FLOAT,// Bokeh
			DXGI_FORMAT_R16G16B16A16_FLOAT,// Postfilter
			DXGI_FORMAT_R16G16B16A16_FLOAT// Combine
		};

		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0.5f, 0.5f, 0.5f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f }
		};

		std::vector<float> rtvResolutionScale =
		{
			1.0f,
			0.5f,
			0.5f,
			0.5f,
			1.0f
		};

		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			prop.mWidthPercentage = rtvResolutionScale[i];
			prop.mHeightPercentage = rtvResolutionScale[i];
			propVec.push_back(prop);
		}

		auto rtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mDofSrvIndex), GetGpuSrv(mDofSrvIndex), propVec);
		mRtvHeaps["DoF"] = std::move(rtvHeap);
	}

	// Build RTV heap and SRV for motion blur pass.
	{
		mMotionBlurSrvIndex = mDofSrvIndex + mRtvHeaps["DoF"]->mRtvHeap.HeapDesc.NumDescriptors;

		mMotionBlurVdBufferSrvIndexOffset = 0;
		mMotionBlurFirstTileMaxSrvIndexOffset = 1;
		mMotionBlurSecondTileMaxSrvIndexOffset = 2;
		mMotionBlurThirdTileMaxSrvIndexOffset = 3;
		mMotionBlurFourthTileMaxSrvIndexOffset = 4;
		mMotionBlurNeighborMaxSrvIndexOffset = 5;
		mMotionBlurOutputSrvIndexOffset = 6;

		auto maxBlurRadius = (int)(MOTION_BLUR_MAX_RADIUS_SCALE * mClientHeight / 100);
		auto tileSize = (((maxBlurRadius - 1) / 8 + 1) * 8);

		std::vector<DXGI_FORMAT> rtvFormats =
		{
			DXGI_FORMAT_R10G10B10A2_UNORM,// Velocity Depth Buffer
			DXGI_FORMAT_R16G16_FLOAT,// First Tile Max
			DXGI_FORMAT_R16G16_FLOAT,// Second Tile Max
			DXGI_FORMAT_R16G16_FLOAT,// Third Tile Max
			DXGI_FORMAT_R16G16_FLOAT,// Fourth Tile Max
			DXGI_FORMAT_R16G16_FLOAT,// Neighbor Max
			DXGI_FORMAT_R16G16B16A16_FLOAT// Motion Blur Output
		};

		std::vector<std::vector<FLOAT>> rtvClearColor =
		{
			{ 0.5f, 0.5f, 1.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f },
			{ 0.0f, 0.0f, 0.0f, 1.0f }
		};

		std::vector<float> rtvResolutionScale =
		{
			1.0f,
			0.5f,
			0.25f,
			0.125f,
			1.0f / tileSize,
			1.0f / tileSize,
			1.0f
		};

		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			prop.mWidthPercentage = rtvResolutionScale[i];
			prop.mHeightPercentage = rtvResolutionScale[i];
			propVec.push_back(prop);
		}

		auto motionBlurPassRtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mMotionBlurSrvIndex), GetGpuSrv(mMotionBlurSrvIndex), propVec);
		mRtvHeaps["MotionBlur"] = std::move(motionBlurPassRtvHeap);
	}

	// Build RTV heap and SRV for bloom pass.
	{
		mBloomSrvIndex = mMotionBlurSrvIndex + mRtvHeaps["MotionBlur"]->mRtvHeap.HeapDesc.NumDescriptors;

		mBloomDownSrvIndexOffset = 0;
		mBloomUpSrvIndexOffset = BloomChainLength;
		mBloomOutputSrvIndexOffset = 2 * BloomChainLength;

		auto maxBlurRadius = (int)(MOTION_BLUR_MAX_RADIUS_SCALE * mClientHeight / 100);
		auto tileSize = (((maxBlurRadius - 1) / 8 + 1) * 8);

		std::vector<DXGI_FORMAT> rtvFormats;
		std::vector<std::vector<FLOAT>> rtvClearColor;
		std::vector<float> rtvResolutionScale;

		int i = 0;
		float scale = 0.5f;
		for (i = 0; i < BloomChainLength; i++)
		{
			rtvFormats.push_back(DXGI_FORMAT_R16G16B16A16_FLOAT);
			std::vector<FLOAT> color = { 0.0f, 0.0f, 0.0f, 1.0f };
			rtvClearColor.push_back(color);
			rtvResolutionScale.push_back(scale);
			scale /= 2.0f;
		}

		scale = 0.5f;
		for (i = BloomChainLength; i < 2 * BloomChainLength; i++)
		{
			rtvFormats.push_back(DXGI_FORMAT_R16G16B16A16_FLOAT);
			std::vector<FLOAT> color = { 0.0f, 0.0f, 0.0f, 1.0f };
			rtvClearColor.push_back(color);
			rtvResolutionScale.push_back(scale);
			scale /= 2.0f;
		}

		rtvFormats.push_back(DXGI_FORMAT_R16G16B16A16_FLOAT);
		std::vector<FLOAT> color = { 0.0f, 0.0f, 0.0f, 1.0f };
		rtvClearColor.push_back(color);
		rtvResolutionScale.push_back(1.0f);

		std::vector<GRtvProperties> propVec;
		for (auto i = 0u; i < rtvFormats.size(); i++)
		{
			GRtvProperties prop;
			prop.mRtvFormat = rtvFormats[i];
			prop.mClearColor[0] = rtvClearColor[i][0];
			prop.mClearColor[1] = rtvClearColor[i][1];
			prop.mClearColor[2] = rtvClearColor[i][2];
			prop.mClearColor[3] = rtvClearColor[i][3];
			prop.mWidthPercentage = rtvResolutionScale[i];
			prop.mHeightPercentage = rtvResolutionScale[i];
			propVec.push_back(prop);
		}

		auto rtvHeap = std::make_unique<GDxRtvHeap>(md3dDevice.Get(), mClientWidth, mClientHeight, GetCpuSrv(mBloomSrvIndex), GetGpuSrv(mBloomSrvIndex), propVec);
		mRtvHeaps["Bloom"] = std::move(rtvHeap);
	}

	// Build SRV for blue noise.
	{
		mBlueNoiseSrvIndex = mBloomSrvIndex + mRtvHeaps["Bloom"]->mRtvHeap.HeapDesc.NumDescriptors;

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		GDxTexture* tex = dynamic_cast<GDxTexture*>(pTextures[L"Resource\\Textures\\BlueNoise.png"]);
		if (tex == nullptr)
			ThrowDxException(L"Dynamic cast from GRiTexture to GDxTexture failed.");

		srvDesc.Format = tex->Resource->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, GetCpuSrv(mBlueNoiseSrvIndex));
	}

	// Build cubemap SRV and RTVs for irradiance pre-integration.
	{
		mIblIndex = mBlueNoiseSrvIndex + 1;

		GRtvProperties prop;
		//prop.mRtvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
		prop.mRtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		prop.mClearColor[0] = 0;
		prop.mClearColor[1] = 0;
		prop.mClearColor[2] = 0;
		prop.mClearColor[3] = 1;

		auto gIrradianceCubemap = std::make_unique<GDxCubeRtv>(md3dDevice.Get(), SKY_CUBEMAP_SIZE, GetCpuSrv(mIblIndex), GetGpuSrv(mIblIndex), prop);
		mCubeRtvs["Irradiance"] = std::move(gIrradianceCubemap);
	}

	// Build SRV for LUT
	{
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

		GDxTexture* tex = dynamic_cast<GDxTexture*>(pTextures[L"Resource\\Textures\\IBL_BRDF_LUT.png"]);
		if (tex == nullptr)
			ThrowDxException(L"Dynamic cast from GRiTexture to GDxTexture failed.");

		srvDesc.Format = tex->Resource->GetDesc().Format;
		srvDesc.Texture2D.MipLevels = tex->Resource->GetDesc().MipLevels;
		md3dDevice->CreateShaderResourceView(tex->Resource.Get(), &srvDesc, GetCpuSrv(mIblIndex + 1));
	}

	// Build cubemap SRV and RTVs for prefilter cubemap pre-integration.
	{
		for (auto i = 0u; i < mPrefilterLevels; i++)
		{
			GRtvProperties prop;
			//prop.mRtvFormat = DXGI_FORMAT_R32G32B32A32_FLOAT;
			prop.mRtvFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
			prop.mClearColor[0] = 0;
			prop.mClearColor[1] = 0;
			prop.mClearColor[2] = 0;
			prop.mClearColor[3] = 1;

			auto gPrefilterCubemap = std::make_unique<GDxCubeRtv>(md3dDevice.Get(), (UINT)(SKY_CUBEMAP_SIZE / pow(2, i)), GetCpuSrv(mIblIndex + 2 + i), GetGpuSrv(mIblIndex + 2 + i), prop);
			mCubeRtvs["Prefilter_" + std::to_string(i)] = std::move(gPrefilterCubemap);
		}
	}

	// Build SRV for ordinary textures.
	{
		mTextrueHeapIndex = mIblIndex + 2 + mPrefilterLevels;

		for (auto i = 0u; i < MAX_TEXTURE_NUM; i++)
			mTexturePoolFreeIndex.push_back(i);

		for (auto tex : pTextures)
		{
			RegisterTexture(tex.second);
		}
	}

	// Build SRV for SDF textures.
	{
		mSdfTextrueIndex = mTextrueHeapIndex + MAX_TEXTURE_NUM;
	}
}

void GDxRenderer::BuildPSOs()
{
	// PSO for GBuffers.
	{
		D3D12_DEPTH_STENCIL_DESC gBufferDSD;
		gBufferDSD.DepthEnable = true;
		gBufferDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		gBufferDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		gBufferDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		gBufferDSD.StencilEnable = true;
		gBufferDSD.StencilReadMask = 0xff;
		gBufferDSD.StencilWriteMask = 0xff;
		gBufferDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gBufferDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		// We are not rendering backfacing polygons, so these settings do not matter. 
		gBufferDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gBufferDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC gBufferPsoDesc;
		ZeroMemory(&gBufferPsoDesc, sizeof(gBufferPsoDesc));
		gBufferPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\DefaultVS.cso");
		gBufferPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\DeferredPS.cso");
		gBufferPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		gBufferPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		gBufferPsoDesc.pRootSignature = mRootSignatures["GBuffer"].Get();
		//gBufferPsoDesc.pRootSignature = mRootSignatures["Forward"].Get();
		gBufferPsoDesc.DepthStencilState = gBufferDSD;
		gBufferPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		gBufferPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		gBufferPsoDesc.SampleMask = UINT_MAX;
		gBufferPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		gBufferPsoDesc.NumRenderTargets = (UINT)mRtvHeaps["GBuffer"]->mRtv.size();
		for (size_t i = 0; i < mRtvHeaps["GBuffer"]->mRtv.size(); i++)
		{
			gBufferPsoDesc.RTVFormats[i] = mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mRtvFormat;
		}
		gBufferPsoDesc.DSVFormat = mDepthStencilFormat;
		gBufferPsoDesc.SampleDesc.Count = 1;// don't use msaa in deferred rendering.
		//deferredPSO = sysRM->CreatePSO(StringID("deferredPSO"), descPipelineState);
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&gBufferPsoDesc, IID_PPV_ARGS(&mPSOs["GBuffer"])));
	}

	// PSO for depth downsample pass
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
		computePsoDesc.pRootSignature = mRootSignatures["DepthDownsamplePass"].Get();
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\DepthDownsampleCS.cso");
		computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["DepthDownsamplePass"])));
	}

	// PSO for tile/cluster pass.
	{
		/*
		D3D12_DEPTH_STENCIL_DESC lightPassDSD;
		lightPassDSD.DepthEnable = false;
		lightPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		lightPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		lightPassDSD.StencilEnable = true;
		lightPassDSD.StencilReadMask = 0xff;
		lightPassDSD.StencilWriteMask = 0x0;
		lightPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		// We are not rendering backfacing polygons, so these settings do not matter.
		lightPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		auto blendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		blendState.AlphaToCoverageEnable = false;
		blendState.IndependentBlendEnable = false;

		blendState.RenderTarget[0].BlendEnable = true;
		blendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		blendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		blendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;

		auto rasterizer = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		//rasterizer.CullMode = D3D12_CULL_MODE_FRONT; // Front culling for point light
		rasterizer.CullMode = D3D12_CULL_MODE_NONE;
		rasterizer.DepthClipEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPipelineState;
		ZeroMemory(&descPipelineState, sizeof(descPipelineState));

		descPipelineState.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPipelineState.PS = GDxShaderManager::LoadShader(L"Shaders\\DirectLightPassPS.cso");
		descPipelineState.pRootSignature = mRootSignatures["LightPass"].Get();
		descPipelineState.BlendState = blendState;
		descPipelineState.DepthStencilState = lightPassDSD;
		descPipelineState.DepthStencilState.DepthEnable = false;
		descPipelineState.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPipelineState.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPipelineState.RasterizerState = rasterizer;
		descPipelineState.NumRenderTargets = 1;
		descPipelineState.RTVFormats[0] = mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mRtvFormat;
		descPipelineState.SampleMask = UINT_MAX;
		descPipelineState.SampleDesc.Count = 1;
		descPipelineState.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPipelineState, IID_PPV_ARGS(&mPSOs["DirectLightPass"])));
		*/

		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
		computePsoDesc.pRootSignature = mRootSignatures["TileClusterPass"].Get();
#if USE_TBDR
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\TiledDeferredCS.cso");
#elif USE_CBDR
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\ClusteredDeferredCS.cso");
#else
		ThrowGGiException("TBDR/CBDR not enabled.");
#endif
		computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["TileClusterPass"])));
	}

	// PSO for SDF tile pass.
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
		computePsoDesc.pRootSignature = mRootSignatures["SdfTilePass"].Get();
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\SdfTileCS.cso");
		computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["SdfTilePass"])));
	}

	// PSO for GTAO raw pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\GtaoPS.cso");
		descPSO.pRootSignature = mRootSignatures["GtaoRaw"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["GTAO"]->mRtv[mGtaoRawSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["GtaoRaw"])));
	}

	// PSO for GTAO upsample pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\GtaoUpsamplePS.cso");
		descPSO.pRootSignature = mRootSignatures["GtaoFilter"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["GTAO"]->mRtv[mGtaoUpsampleSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["GtaoUpsample"])));
	}

	// PSO for GTAO bilateral filter x pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\GtaoBilateralXPS.cso");
		descPSO.pRootSignature = mRootSignatures["GtaoFilter"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["GTAO"]->mRtv[mGtaoBlurXSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["GtaoBilateralX"])));
	}

	// PSO for GTAO bilateral filter y pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\GtaoBilateralYPS.cso");
		descPSO.pRootSignature = mRootSignatures["GtaoFilter"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["GTAO"]->mRtv[mGtaoBlurYSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["GtaoBilateralY"])));
	}

	// PSO for GTAO temporal filter pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\GtaoTemporalPS.cso");
		descPSO.pRootSignature = mRootSignatures["GtaoFilter"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 2;
		descPSO.RTVFormats[0] = mRtvHeaps["GTAO"]->mRtv[mGtaoTemporalSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.RTVFormats[1] = mRtvHeaps["GTAO"]->mRtv[mGtaoHistory1SrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["GtaoTemporal"])));
	}

	// PSO for shadow map pass.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC ShadowMapPsoDesc;

		D3D12_DEPTH_STENCIL_DESC ShadowMapDSD;
		ShadowMapDSD.DepthEnable = true;
		ShadowMapDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		ShadowMapDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		ShadowMapDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		ShadowMapDSD.StencilEnable = false;

		ZeroMemory(&ShadowMapPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		ShadowMapPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		ShadowMapPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		ShadowMapPsoDesc.pRootSignature = mRootSignatures["ShadowMap"].Get();
		ShadowMapPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\ShadowMapVS.cso");
		ShadowMapPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\ShadowMapPS.cso");
		ShadowMapPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
#if USE_REVERSE_Z
		ShadowMapPsoDesc.RasterizerState.DepthBias = -100;
		ShadowMapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
		ShadowMapPsoDesc.RasterizerState.SlopeScaledDepthBias = -0.0f;
#else
		ShadowMapPsoDesc.RasterizerState.DepthBias = 100;
		ShadowMapPsoDesc.RasterizerState.DepthBiasClamp = 0.0f;
		ShadowMapPsoDesc.RasterizerState.SlopeScaledDepthBias = 1.0f;
#endif
		ShadowMapPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		ShadowMapPsoDesc.DepthStencilState = ShadowMapDSD;
		ShadowMapPsoDesc.SampleMask = UINT_MAX;
		ShadowMapPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ShadowMapPsoDesc.NumRenderTargets = 0;
		ShadowMapPsoDesc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
		ShadowMapPsoDesc.SampleDesc.Count = 1;//m4xMsaaState ? 4 : 1;
		ShadowMapPsoDesc.SampleDesc.Quality = 0;//m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		ShadowMapPsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ShadowMapPsoDesc, IID_PPV_ARGS(&mPSOs["ShadowMap"])));
	}

	// PSO for shadow map prefilter pass.
	{
		D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
		computePsoDesc.pRootSignature = mRootSignatures["ShadowMapPrefilter"].Get();
		computePsoDesc.CS = GDxShaderManager::LoadShader(L"Shaders\\ShadowMapPrefilterCS.cso");
		computePsoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
		ThrowIfFailed(md3dDevice->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&mPSOs["ShadowMapPrefilter"])));
	}

	// PSO for screen space shadow pass.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC ScreenSpaceShadowPsoDesc;

		D3D12_DEPTH_STENCIL_DESC ScreenSpaceShadowDSD;
		ScreenSpaceShadowDSD.DepthEnable = true;
		ScreenSpaceShadowDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
#if USE_REVERSE_Z
		ScreenSpaceShadowDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		ScreenSpaceShadowDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		ScreenSpaceShadowDSD.StencilEnable = false;

		ZeroMemory(&ScreenSpaceShadowPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		ScreenSpaceShadowPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		ScreenSpaceShadowPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		ScreenSpaceShadowPsoDesc.pRootSignature = mRootSignatures["ScreenSpaceShadowPass"].Get();
		ScreenSpaceShadowPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		ScreenSpaceShadowPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\ScreenSpaceShadowPS.cso");
		ScreenSpaceShadowPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		ScreenSpaceShadowPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		ScreenSpaceShadowPsoDesc.DepthStencilState = ScreenSpaceShadowDSD;
		ScreenSpaceShadowPsoDesc.SampleMask = UINT_MAX;
		ScreenSpaceShadowPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ScreenSpaceShadowPsoDesc.NumRenderTargets = 1;
		ScreenSpaceShadowPsoDesc.RTVFormats[0] = mRtvHeaps["ScreenSpaceShadowPass"]->mRtv[0]->mProperties.mRtvFormat;//light pass output
		ScreenSpaceShadowPsoDesc.SampleDesc.Count = 1;//m4xMsaaState ? 4 : 1;
		ScreenSpaceShadowPsoDesc.SampleDesc.Quality = 0;//m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		ScreenSpaceShadowPsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&ScreenSpaceShadowPsoDesc, IID_PPV_ARGS(&mPSOs["ScreenSpaceShadowPass"])));
	}

	// PSO for screen space shadow temporal filter pass.
	{
		D3D12_DEPTH_STENCIL_DESC SSShadowTemporalPassDSD;
		SSShadowTemporalPassDSD.DepthEnable = true;
		SSShadowTemporalPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		SSShadowTemporalPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		SSShadowTemporalPassDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descSSShadowTemporalPSO;
		ZeroMemory(&descSSShadowTemporalPSO, sizeof(descSSShadowTemporalPSO));

		descSSShadowTemporalPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descSSShadowTemporalPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SSShadowTemporalPS.cso");
		descSSShadowTemporalPSO.pRootSignature = mRootSignatures["SSShadowTemporalPass"].Get();
		descSSShadowTemporalPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descSSShadowTemporalPSO.DepthStencilState = SSShadowTemporalPassDSD;
		descSSShadowTemporalPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descSSShadowTemporalPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descSSShadowTemporalPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descSSShadowTemporalPSO.NumRenderTargets = 2;
		descSSShadowTemporalPSO.RTVFormats[0] = mRtvHeaps["SSShadowTemporalPass"]->mRtv[2]->mProperties.mRtvFormat;//temporal output
		descSSShadowTemporalPSO.RTVFormats[1] = mRtvHeaps["SSShadowTemporalPass"]->mRtv[0]->mProperties.mRtvFormat;//history
		descSSShadowTemporalPSO.SampleMask = UINT_MAX;
		descSSShadowTemporalPSO.SampleDesc.Count = 1;
		descSSShadowTemporalPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descSSShadowTemporalPSO, IID_PPV_ARGS(&mPSOs["SSShadowTemporalPass"])));
	}

	// PSO for light pass.
	{
		D3D12_DEPTH_STENCIL_DESC lightPassDSD;
		lightPassDSD.DepthEnable = true;
		lightPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		lightPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		lightPassDSD.StencilEnable = false;
		lightPassDSD.StencilReadMask = 0xff;
		lightPassDSD.StencilWriteMask = 0x0;
		lightPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		lightPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		lightPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descLightPSO;
		ZeroMemory(&descLightPSO, sizeof(descLightPSO));

		descLightPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descLightPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\LightPassPS.cso");
		descLightPSO.pRootSignature = mRootSignatures["LightPass"].Get();
		descLightPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descLightPSO.DepthStencilState = lightPassDSD;
		//descLightPSO.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		descLightPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descLightPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descLightPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descLightPSO.NumRenderTargets = 2;
		descLightPSO.RTVFormats[0] = mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mRtvFormat;//light pass output
		descLightPSO.RTVFormats[1] = mRtvHeaps["LightPass"]->mRtv[1]->mProperties.mRtvFormat;//light pass output
		descLightPSO.SampleMask = UINT_MAX;
		descLightPSO.SampleDesc.Count = 1;
		descLightPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descLightPSO, IID_PPV_ARGS(&mPSOs["LightPass"])));
	}

	// PSO for TAA pass.
	{
		D3D12_DEPTH_STENCIL_DESC taaPassDSD;
		taaPassDSD.DepthEnable = true;
		taaPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		taaPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		taaPassDSD.StencilEnable = false;
		taaPassDSD.StencilReadMask = 0xff;
		taaPassDSD.StencilWriteMask = 0x0;
		taaPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		taaPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		taaPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descTaaPSO;
		ZeroMemory(&descTaaPSO, sizeof(descTaaPSO));

		descTaaPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descTaaPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\TaaPassPS.cso"); 
		descTaaPSO.pRootSignature = mRootSignatures["TaaPass"].Get();
		descTaaPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descTaaPSO.DepthStencilState = taaPassDSD;
		//descTaaPSO.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		descTaaPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descTaaPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descTaaPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descTaaPSO.NumRenderTargets = 2;
		descTaaPSO.RTVFormats[0] = mRtvHeaps["TaaPass"]->mRtv[2]->mProperties.mRtvFormat;//taa output
		descTaaPSO.RTVFormats[1] = mRtvHeaps["TaaPass"]->mRtv[0]->mProperties.mRtvFormat;//history
		descTaaPSO.SampleMask = UINT_MAX;
		descTaaPSO.SampleDesc.Count = 1;
		descTaaPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descTaaPSO, IID_PPV_ARGS(&mPSOs["TaaPass"])));
	}

	// PSO for SSR depth unjitter pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SsrDepthUnjitterPS.cso");
		descPSO.pRootSignature = mRootSignatures["SsrDepthUnjitter"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["SSR"]->mRtv[mSsrUnjitteredDepthSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["SsrDepthUnjitter"])));
	}

	// PSO for SSR hiz pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SsrHizPS.cso");
		descPSO.pRootSignature = mRootSignatures["SsrHiz"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["SSR"]->mRtv[mSsrHizSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["SsrHiz"])));
	}

	// PSO for SSR prefilter pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SsrPrefilterPS.cso");
		descPSO.pRootSignature = mRootSignatures["SsrPrefilter"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["SSR"]->mRtv[mSsrPrefilterSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["SsrPrefilter"])));
	}

	// PSO for SSR trace pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SsrTracePS.cso");
		descPSO.pRootSignature = mRootSignatures["SsrTraceResolve"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 2;
		descPSO.RTVFormats[0] = mRtvHeaps["SSR"]->mRtv[mSsrTraceSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.RTVFormats[1] = mRtvHeaps["SSR"]->mRtv[mSsrTraceMaskSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["SsrTrace"])));
	}

	// PSO for SSR resolve pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SsrResolvePS.cso");
		descPSO.pRootSignature = mRootSignatures["SsrTraceResolve"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["SSR"]->mRtv[mSsrResolveSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["SsrResolve"])));
	}

	// PSO for SSR temporal pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SsrTemporalPS.cso");
		descPSO.pRootSignature = mRootSignatures["SsrTemporal"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 2;
		descPSO.RTVFormats[0] = mRtvHeaps["SSR"]->mRtv[mSsrTemporalSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.RTVFormats[1] = mRtvHeaps["SSR"]->mRtv[mSsrHistorySrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["SsrTemporal"])));
	}

	// PSO for SSR combine pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\SsrCombinePS.cso");
		descPSO.pRootSignature = mRootSignatures["SsrCombine"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["SSR"]->mRtv[mSsrCombineSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["SsrCombine"])));
	}

	// PSO for DoF CoC pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\DofCocPS.cso");
		descPSO.pRootSignature = mRootSignatures["DoF"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["DoF"]->mRtv[mDofCocSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["DofCoc"])));
	}

	// PSO for DoF prefilter pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\DofPrefilterPS.cso");
		descPSO.pRootSignature = mRootSignatures["DoF"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["DoF"]->mRtv[mDofPrefilterSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["DofPrefilter"])));
	}

	// PSO for DoF bokeh pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\DofPostfilterPS.cso");
		descPSO.pRootSignature = mRootSignatures["DoF"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["DoF"]->mRtv[mDofPostfilterSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["DofPostfilter"])));
	}

	// PSO for DoF postfilter pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\DofBokehPS.cso");
		descPSO.pRootSignature = mRootSignatures["DoF"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["DoF"]->mRtv[mDofBokehSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["DofBokeh"])));
	}

	// PSO for DoF combine pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\DofCombinePS.cso");
		descPSO.pRootSignature = mRootSignatures["DoF"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["DoF"]->mRtv[mDofCombineSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["DofCombine"])));
	}

	// PSO for motion blur velocity depth packing pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\MotionBlurVdPackingPS.cso");
		descPSO.pRootSignature = mRootSignatures["MotionBlurVdPacking"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurVdBufferSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["MotionBlurVdPacking"])));
	}

	// PSO for motion blur first tile max pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\MotionBlurFirstTileMaxPS.cso");
		descPSO.pRootSignature = mRootSignatures["MotionBlurTileMax"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFirstTileMaxSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["MotionBlurFirstTileMax"])));
	}

	// PSO for motion blur second third tile max pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\MotionBlurSecondThirdTileMaxPS.cso");
		descPSO.pRootSignature = mRootSignatures["MotionBlurTileMax"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurSecondTileMaxSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["MotionBlurSecondThirdTileMax"])));
	}

	// PSO for motion blur fourth tile max pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\MotionBlurFourthTileMaxPS.cso");
		descPSO.pRootSignature = mRootSignatures["MotionBlurTileMax"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurFourthTileMaxSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["MotionBlurFourthTileMax"])));
	}

	// PSO for motion blur neighbor max pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\MotionBlurNeighborMaxPS.cso");
		descPSO.pRootSignature = mRootSignatures["MotionBlurTileMax"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurNeighborMaxSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["MotionBlurNeighborMax"])));
	}

	// PSO for motion blur pass.
	{
		D3D12_DEPTH_STENCIL_DESC motionBlurPassDSD;
		motionBlurPassDSD.DepthEnable = true;
		motionBlurPassDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		motionBlurPassDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		motionBlurPassDSD.StencilEnable = false;
		motionBlurPassDSD.StencilReadMask = 0xff;
		motionBlurPassDSD.StencilWriteMask = 0x0;
		motionBlurPassDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		motionBlurPassDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		motionBlurPassDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descMotionBlurPSO;
		ZeroMemory(&descMotionBlurPSO, sizeof(descMotionBlurPSO));

		descMotionBlurPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descMotionBlurPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\MotionBlurPassPS.cso");
		descMotionBlurPSO.pRootSignature = mRootSignatures["MotionBlurPass"].Get();
		descMotionBlurPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descMotionBlurPSO.DepthStencilState = motionBlurPassDSD;
		//descMotionBlurPSO.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		descMotionBlurPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descMotionBlurPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descMotionBlurPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descMotionBlurPSO.NumRenderTargets = 1;
		descMotionBlurPSO.RTVFormats[0] = mRtvHeaps["MotionBlur"]->mRtv[mMotionBlurOutputSrvIndexOffset]->mProperties.mRtvFormat;//motion blur output
		descMotionBlurPSO.SampleMask = UINT_MAX;
		descMotionBlurPSO.SampleDesc.Count = 1;
		descMotionBlurPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descMotionBlurPSO, IID_PPV_ARGS(&mPSOs["MotionBlurPass"])));
	}

	// PSO for bloom prefilter pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\BloomPrefilterPS.cso");
		descPSO.pRootSignature = mRootSignatures["Bloom"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["BloomPrefilter"])));
	}

	// PSO for bloom downsample pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\BloomDownsamplePS.cso");
		descPSO.pRootSignature = mRootSignatures["Bloom"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["Bloom"]->mRtv[mBloomDownSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["BloomDownsample"])));
	}

	// PSO for bloom upsample pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\BloomUpsamplePS.cso");
		descPSO.pRootSignature = mRootSignatures["Bloom"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["BloomUpsample"])));
	}

	// PSO for bloom upsample pass.
	{
		D3D12_DEPTH_STENCIL_DESC passDSD;
		passDSD.DepthEnable = true;
		passDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
		passDSD.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		passDSD.StencilEnable = false;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC descPSO;
		ZeroMemory(&descPSO, sizeof(descPSO));

		descPSO.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		descPSO.PS = GDxShaderManager::LoadShader(L"Shaders\\BloomCombinePS.cso");
		descPSO.pRootSignature = mRootSignatures["Bloom"].Get();
		descPSO.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		descPSO.DepthStencilState = passDSD;
		descPSO.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		descPSO.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		descPSO.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		descPSO.NumRenderTargets = 1;
		descPSO.RTVFormats[0] = mRtvHeaps["Bloom"]->mRtv[mBloomUpSrvIndexOffset]->mProperties.mRtvFormat;
		descPSO.SampleMask = UINT_MAX;
		descPSO.SampleDesc.Count = 1;
		descPSO.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&descPSO, IID_PPV_ARGS(&mPSOs["BloomCombine"])));
	}

	// PSO for output.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC PostProcessPsoDesc;

		D3D12_DEPTH_STENCIL_DESC postProcessDSD;
		postProcessDSD.DepthEnable = true;
		postProcessDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
#if USE_REVERSE_Z
		postProcessDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		postProcessDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		postProcessDSD.StencilEnable = false;
		postProcessDSD.StencilReadMask = 0xff;
		postProcessDSD.StencilWriteMask = 0x0;
		postProcessDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		// We are not rendering backfacing polygons, so these settings do not matter. 
		postProcessDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		postProcessDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;

		ZeroMemory(&PostProcessPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		PostProcessPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		PostProcessPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		PostProcessPsoDesc.pRootSignature = mRootSignatures["Output"].Get();
		PostProcessPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		PostProcessPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\OutputPS.cso");
		PostProcessPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		PostProcessPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		PostProcessPsoDesc.DepthStencilState = postProcessDSD;
		PostProcessPsoDesc.SampleMask = UINT_MAX;
		PostProcessPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		PostProcessPsoDesc.NumRenderTargets = 1;
		PostProcessPsoDesc.RTVFormats[0] = mBackBufferFormat;
		PostProcessPsoDesc.SampleDesc.Count = 1;//m4xMsaaState ? 4 : 1;
		PostProcessPsoDesc.SampleDesc.Quality = 0;//m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		PostProcessPsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&PostProcessPsoDesc, IID_PPV_ARGS(&mPSOs["Output"])));
	}

	// PSO for SDF debug.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC SdfDebugPsoDesc;

		D3D12_DEPTH_STENCIL_DESC SdfDebugDSD;
		SdfDebugDSD.DepthEnable = true;
		SdfDebugDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
#if USE_REVERSE_Z
		SdfDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		SdfDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		SdfDebugDSD.StencilEnable = false;

		ZeroMemory(&SdfDebugPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		SdfDebugPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		SdfDebugPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		SdfDebugPsoDesc.pRootSignature = mRootSignatures["SdfDebug"].Get();
		SdfDebugPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\FullScreenVS.cso");
		SdfDebugPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\SdfDebugPS.cso");
		SdfDebugPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		SdfDebugPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		SdfDebugPsoDesc.DepthStencilState = SdfDebugDSD;
		SdfDebugPsoDesc.SampleMask = UINT_MAX;
		SdfDebugPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		SdfDebugPsoDesc.NumRenderTargets = 1;
		SdfDebugPsoDesc.RTVFormats[0] = mBackBufferFormat;
		SdfDebugPsoDesc.SampleDesc.Count = 1;//m4xMsaaState ? 4 : 1;
		SdfDebugPsoDesc.SampleDesc.Quality = 0;//m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		SdfDebugPsoDesc.DSVFormat = mDepthStencilFormat;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&SdfDebugPsoDesc, IID_PPV_ARGS(&mPSOs["SdfDebug"])));
	}

	// PSO for GBuffer debug layer.
	{
		D3D12_DEPTH_STENCIL_DESC gBufferDebugDSD;
		gBufferDebugDSD.DepthEnable = true;
		gBufferDebugDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		gBufferDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		gBufferDebugDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		gBufferDebugDSD.StencilEnable = false;
		gBufferDebugDSD.StencilReadMask = 0xff;
		gBufferDebugDSD.StencilWriteMask = 0x0;
		gBufferDebugDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		// We are not rendering backfacing polygons, so these settings do not matter. 
		gBufferDebugDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		gBufferDebugDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC debugPsoDesc;

		ZeroMemory(&debugPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		debugPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		debugPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		//debugPsoDesc.pRootSignature = mRootSignatures["Forward"].Get();
		debugPsoDesc.pRootSignature = mRootSignatures["GBufferDebug"].Get();
		debugPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		debugPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		debugPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		debugPsoDesc.SampleMask = UINT_MAX;
		debugPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		debugPsoDesc.NumRenderTargets = 1;
		debugPsoDesc.RTVFormats[0] = mBackBufferFormat;
		debugPsoDesc.SampleDesc.Count = 1;
		debugPsoDesc.SampleDesc.Quality = 0;
		debugPsoDesc.DSVFormat = mDepthStencilFormat;
		debugPsoDesc.pRootSignature = mRootSignatures["GBufferDebug"].Get();
		debugPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\ScreenVS.cso");
		debugPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\GBufferDebugPS.cso");
		debugPsoDesc.DepthStencilState = gBufferDebugDSD;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&debugPsoDesc, IID_PPV_ARGS(&mPSOs["GBufferDebug"])));
	}

	// PSO for sky.
	{
		D3D12_DEPTH_STENCIL_DESC gskyDSD;
		gskyDSD.DepthEnable = true;
		gskyDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		gskyDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		gskyDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		gskyDSD.StencilEnable = false;
		gskyDSD.StencilReadMask = 0xff;
		gskyDSD.StencilWriteMask = 0x0;
		gskyDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gskyDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		gskyDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		gskyDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		gskyDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		auto skyBlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		skyBlendState.AlphaToCoverageEnable = false;
		skyBlendState.IndependentBlendEnable = false;
		skyBlendState.RenderTarget[0].BlendEnable = true;
		skyBlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		skyBlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
		skyBlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC skyPsoDesc;

		ZeroMemory(&skyPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		skyPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		skyPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		skyPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		//skyPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		skyPsoDesc.BlendState = skyBlendState;
		//skyPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//skyPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		//skyPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		skyPsoDesc.DepthStencilState = gskyDSD;
		skyPsoDesc.SampleMask = UINT_MAX;
		skyPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		skyPsoDesc.NumRenderTargets = 2;
		skyPsoDesc.RTVFormats[0] = mRtvHeaps["LightPass"]->mRtv[0]->mProperties.mRtvFormat;
		skyPsoDesc.RTVFormats[1] = mRtvHeaps["GBuffer"]->mRtv[mVelocityBufferSrvIndex - mGBufferSrvIndex]->mProperties.mRtvFormat;
		skyPsoDesc.SampleDesc.Count = 1;
		skyPsoDesc.SampleDesc.Quality = 0;
		skyPsoDesc.DSVFormat = mDepthStencilFormat;

		// The camera is inside the sky sphere, so just turn off culling.
		//skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		skyPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;

		// Make sure the depth function is LESS_EQUAL and not just LESS.  
		// Otherwise, the normalized depth values at z = 1 (NDC) will 
		// fail the depth test if the depth buffer was cleared to 1.
		skyPsoDesc.pRootSignature = mRootSignatures["Sky"].Get();
		skyPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\SkyVS.cso");
		skyPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\SkyPS.cso");
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&skyPsoDesc, IID_PPV_ARGS(&mPSOs["Sky"])));
	}

	// PSO for irradiance pre-integration.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC irradiancePsoDesc;

		ZeroMemory(&irradiancePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		irradiancePsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		irradiancePsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		irradiancePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		irradiancePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		irradiancePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		irradiancePsoDesc.SampleMask = UINT_MAX;
		irradiancePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		irradiancePsoDesc.NumRenderTargets = 1;
		irradiancePsoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
		irradiancePsoDesc.SampleDesc.Count = 1;
		irradiancePsoDesc.SampleDesc.Quality = 0;
		irradiancePsoDesc.DSVFormat = mDepthStencilFormat;

		// The camera is inside the sky sphere, so just turn off culling.
		irradiancePsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		// Make sure the depth function is LESS_EQUAL and not just LESS.  
		// Otherwise, the normalized depth values at z = 1 (NDC) will 
		// fail the depth test if the depth buffer was cleared to 1.
#if USE_REVERSE_Z
		irradiancePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		irradiancePsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		irradiancePsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		irradiancePsoDesc.pRootSignature = mRootSignatures["Sky"].Get();
		irradiancePsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\SkyVS.cso");
		irradiancePsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\IrradianceCubemapPS.cso");
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&irradiancePsoDesc, IID_PPV_ARGS(&mPSOs["Irradiance"])));
	}

	// PSO for prefilter pre-integration.
	{
		D3D12_GRAPHICS_PIPELINE_STATE_DESC prefilterPsoDesc;

		ZeroMemory(&prefilterPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		prefilterPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		prefilterPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		prefilterPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		prefilterPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		prefilterPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		prefilterPsoDesc.SampleMask = UINT_MAX;
		prefilterPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		prefilterPsoDesc.NumRenderTargets = 1;
		prefilterPsoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32B32A32_FLOAT;
		prefilterPsoDesc.SampleDesc.Count = 1;
		prefilterPsoDesc.SampleDesc.Quality = 0;
		prefilterPsoDesc.DSVFormat = mDepthStencilFormat;

		// The camera is inside the sky sphere, so just turn off culling.
		prefilterPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		// Make sure the depth function is LESS_EQUAL and not just LESS.  
		// Otherwise, the normalized depth values at z = 1 (NDC) will 
		// fail the depth test if the depth buffer was cleared to 1.
#if USE_REVERSE_Z
		prefilterPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
		prefilterPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
#endif
		prefilterPsoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		prefilterPsoDesc.pRootSignature = mRootSignatures["Sky"].Get();
		prefilterPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\SkyVS.cso");
		prefilterPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\PrefilterCubemapPS.cso");
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&prefilterPsoDesc, IID_PPV_ARGS(&mPSOs["Prefilter"])));
	}

	// Modified by Ssi: 
	// PSO for transpatent objects
	{
		D3D12_DEPTH_STENCIL_DESC transparentDSD;
		transparentDSD.DepthEnable = true;
		transparentDSD.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
#if USE_REVERSE_Z
		transparentDSD.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
		transparentDSD.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
#endif
		transparentDSD.StencilEnable = true;
		transparentDSD.StencilReadMask = 0xff;
		transparentDSD.StencilWriteMask = 0xff;
		transparentDSD.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		transparentDSD.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		transparentDSD.FrontFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		transparentDSD.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		// We are not rendering backfacing polygons, so these settings do not matter. 
		transparentDSD.BackFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		transparentDSD.BackFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		transparentDSD.BackFace.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
		transparentDSD.BackFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;

		D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc;
		ZeroMemory(&transparentPsoDesc, sizeof(transparentPsoDesc));
		transparentPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\DefaultVS.cso");
		transparentPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\TransparentPS.cso");
		transparentPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::DefaultLayout;
		transparentPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::DefaultLayout);
		transparentPsoDesc.pRootSignature = mRootSignatures["Transparent"].Get();
		//gBufferPsoDesc.pRootSignature = mRootSignatures["Forward"].Get();
		transparentPsoDesc.DepthStencilState = transparentDSD;
		transparentPsoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;

		// ����BlendState
		D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
		transparencyBlendDesc.BlendEnable = true;
		transparencyBlendDesc.LogicOpEnable = false;
		transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
		transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE; //  D3D12_BLEND_INV_SRC_ALPHA;
		transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
		transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
		transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
		// gBufferPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		transparentPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		transparentPsoDesc.SampleMask = UINT_MAX;
		transparentPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

		// Modified by Ssi: ��ȾĿ�������͸�ʽ
		//gBufferPsoDesc.NumRenderTargets = (UINT)mRtvHeaps["GBuffer"]->mRtv.size();
		//for (size_t i = 0; i < mRtvHeaps["GBuffer"]->mRtv.size(); i++)
		//{
		//	gBufferPsoDesc.RTVFormats[i] = mRtvHeaps["GBuffer"]->mRtv[i]->mProperties.mRtvFormat;
		//}
		transparentPsoDesc.NumRenderTargets = 1;
		transparentPsoDesc.RTVFormats[0] = mBackBufferFormat;

		transparentPsoDesc.DSVFormat = mDepthStencilFormat;
		transparentPsoDesc.SampleDesc.Count = 1;// don't use msaa in deferred rendering.
		//deferredPSO = sysRM->CreatePSO(StringID("deferredPSO"), descPipelineState);
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["Transparent"])));


		//D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc;

		////
		//// PSO for opaque objects.
		////
		//ZeroMemory(&transparentPsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
		//transparentPsoDesc.InputLayout.pInputElementDescs = GDxInputLayout::TransparentLayout; // ��������
		//transparentPsoDesc.InputLayout.NumElements = _countof(GDxInputLayout::TransparentLayout);
		//transparentPsoDesc.pRootSignature = mRootSignatures["Transparent"].Get(); // ��ǩ��
		//transparentPsoDesc.VS = GDxShaderManager::LoadShader(L"Shaders\\DefaultVS.cso");
		//transparentPsoDesc.PS = GDxShaderManager::LoadShader(L"Shaders\\DeferredPS.cso");
		//transparentPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
		//transparentPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
		//transparentPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
		//transparentPsoDesc.SampleMask = UINT_MAX;
		//transparentPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
		//transparentPsoDesc.NumRenderTargets = 1;
		//transparentPsoDesc.RTVFormats[0] = mBackBufferFormat;
		//transparentPsoDesc.SampleDesc.Count = 1; // m4xMsaaState ? 4 : 1;
		//transparentPsoDesc.SampleDesc.Quality = 0; // m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
		//transparentPsoDesc.DSVFormat = mDepthStencilFormat;
		//// ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));
		//ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["transparent"])));

		//
		// PSO for transparent objects
		//

		// D3D12_GRAPHICS_PIPELINE_STATE_DESC transparentPsoDesc = transparencyBlendDesc;

		/*D3D12_RENDER_TARGET_BLEND_DESC transparencyBlendDesc;
		transparencyBlendDesc.BlendEnable = true;
		transparencyBlendDesc.LogicOpEnable = false;
		transparencyBlendDesc.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		transparencyBlendDesc.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		transparencyBlendDesc.BlendOp = D3D12_BLEND_OP_ADD;
		transparencyBlendDesc.SrcBlendAlpha = D3D12_BLEND_ONE;
		transparencyBlendDesc.DestBlendAlpha = D3D12_BLEND_ZERO;
		transparencyBlendDesc.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		transparencyBlendDesc.LogicOp = D3D12_LOGIC_OP_NOOP;
		transparencyBlendDesc.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		transparentPsoDesc.BlendState.RenderTarget[0] = transparencyBlendDesc;
		ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&transparentPsoDesc, IID_PPV_ARGS(&mPSOs["transparent"])));*/
	}

}

void GDxRenderer::BuildFrameResources()
{
	for (int i = 0; i < NUM_FRAME_RESOURCES; ++i)
	{
		mFrameResources.push_back(std::make_unique<GDxFrameResource>(md3dDevice.Get(),
			2, MAX_SCENE_OBJECT_NUM, MAX_MATERIAL_NUM));//(UINT)pSceneObjects.size(), (UINT)pMaterials.size()));
	}

	for (auto i = 0u; i < (6 * mPrefilterLevels); i++)
	{
		PreIntegrationPassCbs.push_back(std::make_unique<GDxUploadBuffer<SkyPassConstants>>(md3dDevice.Get(), 1, true));
	}
}

void GDxRenderer::CubemapPreIntegration()
{
	ID3D12DescriptorHeap* descriptorHeaps[] = { mSrvDescriptorHeap.Get() };
	mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	//
	// Irradiance cubemap pre-integration
	//

	// Reset root parameters and PSO.
	mCommandList->RSSetViewports(1, &mCubeRtvs["Irradiance"]->mViewport);
	mCommandList->RSSetScissorRects(1, &mCubeRtvs["Irradiance"]->mScissorRect);
	
	mCommandList->SetGraphicsRootSignature(mRootSignatures["Sky"].Get());

	mCommandList->SetPipelineState(mPSOs["Irradiance"].Get());

	// Load object CB.
	mCurrFrameResource = mFrameResources[0].get();
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for (auto& e : pSceneObjects)
	{
		e.second->UpdateTransform();

		/*
		auto dxTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTransform());
		if (dxTrans == nullptr)
			ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");

		auto dxTexTrans = dynamic_pointer_cast<GDxFloat4x4>(e.second->GetTexTransform());
		if (dxTexTrans == nullptr)
			ThrowGGiException("Cast failed from shared_ptr<GGiFloat4x4> to shared_ptr<GDxFloat4x4>.");
		*/

		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		XMMATRIX world = GDx::GGiToDxMatrix(e.second->GetTransform());
		XMMATRIX texTransform = GDx::GGiToDxMatrix(e.second->GetTexTransform());

		ObjectConstants objConstants;
		XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));
		XMStoreFloat4x4(&objConstants.PrevWorld, XMMatrixTranspose(world));
		XMStoreFloat4x4(&objConstants.InvTransWorld, XMMatrixTranspose(world));
		XMStoreFloat4x4(&objConstants.TexTransform, XMMatrixTranspose(texTransform));
		//objConstants.MaterialIndex = e.second->GetMaterial()->MatIndex;
		//objConstants.ObjPad0 = 0;
		//objConstants.ObjPad1 = 0;
		//objConstants.ObjPad2 = 0;

		currObjectCB->CopyData(e.second->GetObjIndex(), objConstants); // ���³����������е�ֵ
	}

	// Load sky pass CB.
	for (auto i = 0u; i < mPrefilterLevels; i++)
	{
		for (auto j = 0u; j < 6u; j++)
		{
			auto view = pCubemapSampleCamera[j]->GetView();
			auto proj = pCubemapSampleCamera[j]->GetProj();
			GGiFloat4x4 viewProj = view * proj;

			viewProj.Transpose();
			XMStoreFloat4x4(&mSkyPassCB.ViewProj, GDx::GGiToDxMatrix(viewProj));
			XMStoreFloat4x4(&mSkyPassCB.PrevViewProj, GDx::GGiToDxMatrix(viewProj));
			mSkyPassCB.pad1 = 0.0f;
			auto eyePos = pCamera->GetPosition();
			mSkyPassCB.EyePosW = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
			mSkyPassCB.PrevPos = DirectX::XMFLOAT3(eyePos[0], eyePos[1], eyePos[2]);
			if (i == 0)
			{
				mSkyPassCB.roughness = 0.01f;
			}
			else
			{
				mSkyPassCB.roughness = ((float)i / (float)mPrefilterLevels);
			}
			auto uploadCB = PreIntegrationPassCbs[i * 6 + j].get();
			uploadCB->CopyData(0, mSkyPassCB); // ���³����������е�ֵ
		}
	}

	for (auto i = 0u; i < 6; i++)
	{
		mCommandList->ClearRenderTargetView(mCubeRtvs["Irradiance"]->mRtvHeap.handleCPU(i), Colors::LightSteelBlue, 0, nullptr);

		auto skyCB = PreIntegrationPassCbs[i]->Resource();
		mCommandList->SetGraphicsRootConstantBufferView(1, skyCB->GetGPUVirtualAddress());

		CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
		skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvUavDescriptorSize);
		mCommandList->SetGraphicsRootDescriptorTable(2, skyTexDescriptor);

		mCommandList->OMSetRenderTargets(1, &(mCubeRtvs["Irradiance"]->mRtvHeap.handleCPU(i)), true, nullptr);

		DrawSceneObjects(mCommandList.Get(), RenderLayer::Sky, true, false);
	}

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Irradiance"]->mResource.Get(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));

	//
	// Prefilter cubemap pre-integration
	//

	// Reset root parameters and PSO.
	mCommandList->SetGraphicsRootSignature(mRootSignatures["Sky"].Get());

	mCommandList->SetPipelineState(mPSOs["Prefilter"].Get());

	for (auto i = 0u; i < mPrefilterLevels; i++)
	{
		for (auto j = 0u; j < 6; j++)
		{
			mCommandList->RSSetViewports(1, &mCubeRtvs["Prefilter_" + std::to_string(i)]->mViewport);
			mCommandList->RSSetScissorRects(1, &mCubeRtvs["Prefilter_" + std::to_string(i)]->mScissorRect);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
				D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_RENDER_TARGET));

			mCommandList->ClearRenderTargetView(mCubeRtvs["Prefilter_" + std::to_string(i)]->mRtvHeap.handleCPU(j), Colors::LightSteelBlue, 0, nullptr);

			//SetPassCbByCamera(PreIntegrationPassCB[i].get(), 0.0f, 0.0f, mCubemapSampleCamera[i]);
			auto passCB = PreIntegrationPassCbs[i * 6 + j]->Resource();
			mCommandList->SetGraphicsRootConstantBufferView(1, passCB->GetGPUVirtualAddress());

			CD3DX12_GPU_DESCRIPTOR_HANDLE skyTexDescriptor(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
			skyTexDescriptor.Offset(mSkyTexHeapIndex, mCbvSrvUavDescriptorSize);
			mCommandList->SetGraphicsRootDescriptorTable(2, skyTexDescriptor);

			mCommandList->OMSetRenderTargets(1, &(mCubeRtvs["Prefilter_" + std::to_string(i)]->mRtvHeap.handleCPU(j)), true, nullptr);

			DrawSceneObjects(mCommandList.Get(), RenderLayer::Sky, true, false);

			mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
				D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_GENERIC_READ));
		}
	}
}

/*
void GDxRenderer::SaveBakedCubemap(std::wstring workDir, std::wstring CubemapPath)
{
	std::wstring originalPath = workDir + CubemapPath;
	std::wstring savePathPrefix = originalPath.substr(0, originalPath.rfind(L"."));

	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Irradiance"]->mResource.Get(),
		D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE));
	ThrowIfFailed(
		DirectX::SaveDDSTextureToFile(mCommandQueue.Get(),
			mCubeRtvs["Irradiance"]->mResource.Get(),
			(savePathPrefix + L"_Irradiance.dds").c_str(),
			D3D12_RESOURCE_STATE_COPY_SOURCE,
			D3D12_RESOURCE_STATE_COPY_SOURCE)
	);
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Irradiance"]->mResource.Get(),
		D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));
	
	for (auto i = 0u; i < mPrefilterLevels; i++)
	{
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
			D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE));
		ThrowIfFailed(
			DirectX::SaveDDSTextureToFile(mCommandQueue.Get(),
				mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
				(savePathPrefix + L"_Prefilter_" + std::to_wstring(i) + L".dds").c_str(),
				D3D12_RESOURCE_STATE_COPY_SOURCE,
				D3D12_RESOURCE_STATE_COPY_SOURCE)
		);
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mCubeRtvs["Prefilter_" + std::to_string(i)]->mResource.Get(),
			D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ));
	}
}
*/

// ������Ⱦ���������󣨺�������ͼ��������ͼ���������������ʡ��������񡢴��������������������������塢����Imgui����ȳ�Ա������
void GDxRenderer::CreateRendererFactory()
{
	GDxRendererFactory fac(md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get());
	mFactory = std::make_unique<GDxRendererFactory>(fac); // ��make_unique����ָ����ʽ����RendererFactory���󣬶���new������delete������ȫ
}

// ����FBX������
void GDxRenderer::CreateFilmboxManager()
{
	mFilmboxManager = std::make_unique<GDxFilmboxManager>();
	mFilmboxManager->SetRendererFactory(mFactory.get());
}

void GDxRenderer::CreateCommandObjects()
{
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	ThrowIfFailed(md3dDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&mCommandQueue)));

	ThrowIfFailed(md3dDevice->CreateCommandAllocator(
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		IID_PPV_ARGS(mDirectCmdListAlloc.GetAddressOf())));

	ThrowIfFailed(md3dDevice->CreateCommandList(
		0,
		D3D12_COMMAND_LIST_TYPE_DIRECT,
		mDirectCmdListAlloc.Get(), // Associated command allocator
		nullptr,                   // Initial PipelineStateObject
		IID_PPV_ARGS(mCommandList.GetAddressOf())));

	// Start off in a closed state.  This is because the first time we refer 
	// to the command list we will Reset it, and it needs to be closed before
	// calling Reset.
	mCommandList->Close();
}

void GDxRenderer::CreateSwapChain()
{
	// Release the previous swapchain we will be recreating.
	mSwapChain.Reset();

	DXGI_SWAP_CHAIN_DESC sd;
	sd.BufferDesc.Width = mClientWidth;
	sd.BufferDesc.Height = mClientHeight;
	sd.BufferDesc.RefreshRate.Numerator = 60;
	sd.BufferDesc.RefreshRate.Denominator = 1;
	sd.BufferDesc.Format = mBackBufferFormat;
	sd.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
	sd.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = SwapChainBufferCount;
	sd.OutputWindow = mhMainWnd;
	sd.Windowed = true;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

	// Note: Swap chain uses queue to perform flush.
	ThrowIfFailed(mdxgiFactory->CreateSwapChain(
		mCommandQueue.Get(),
		&sd,
		mSwapChain.GetAddressOf()));
}

void GDxRenderer::CreateRtvAndDsvDescriptorHeaps()
{
	// Add +1 for screen normal map, +2 for ambient maps.
	D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc;
	rtvHeapDesc.NumDescriptors = SwapChainBufferCount + 3;
	rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	rtvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&rtvHeapDesc, IID_PPV_ARGS(mRtvHeap.GetAddressOf())));

	// Add +1 DSV for shadow map.
	D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc;
	dsvHeapDesc.NumDescriptors = 2;
	dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
	dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
	dsvHeapDesc.NodeMask = 0;
	ThrowIfFailed(md3dDevice->CreateDescriptorHeap(
		&dsvHeapDesc, IID_PPV_ARGS(mDsvHeap.GetAddressOf())));
}

void GDxRenderer::InitializeGpuProfiler()
{
	GDxGpuProfiler::GetGpuProfiler().Initialize(md3dDevice.Get(), mCommandList.Get(), mCommandQueue.Get());
}

#pragma endregion

#pragma region Draw

void GDxRenderer::DrawSceneObjects(ID3D12GraphicsCommandList* cmdList, const RenderLayer layer, bool bSetObjCb, bool bSetSubmeshCb, bool bCheckCullState)
{
	// For each render item...
	for (size_t i = 0; i < pSceneObjectLayer[((int)layer)].size(); ++i)
	{
		auto sObject = pSceneObjectLayer[((int)layer)][i];
		if (!bCheckCullState || (bCheckCullState && (sObject->GetCullState() == CullState::Visible)))
			DrawSceneObject(cmdList, sObject, bSetObjCb, bSetSubmeshCb);
	}
}

void GDxRenderer::DrawSceneObject(ID3D12GraphicsCommandList* cmdList, GRiSceneObject* sObject, bool bSetObjCb, bool bSetSubmeshCb, bool bCheckCullState)
{
	GDxSceneObject* dxSO = dynamic_cast<GDxSceneObject*>(sObject);
	if (dxSO == NULL)
	{
		ThrowGGiException("Cast failed : from GRiSceneObject* to GDxSceneObject*.")
	}

	GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(sObject->GetMesh());
	if (dxMesh == NULL)
	{
		ThrowGGiException("Cast failed : from GRiMesh* to GDxMesh*.")
	}

	cmdList->IASetVertexBuffers(0, 1, &dxMesh->mVIBuffer->VertexBufferView());
	cmdList->IASetIndexBuffer(&dxMesh->mVIBuffer->IndexBufferView());
	cmdList->IASetPrimitiveTopology(dxSO->GetPrimitiveTopology());

	// �������峣������������������Դ��
	if (bSetObjCb)
	{
		UINT objCBByteSize = GDxUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
		auto objectCB = mCurrFrameResource->ObjectCB->Resource();
		D3D12_GPU_VIRTUAL_ADDRESS objCBAddress = objectCB->GetGPUVirtualAddress() + sObject->GetObjIndex() * objCBByteSize;
		cmdList->SetGraphicsRootConstantBufferView(0, objCBAddress); // ��������������Ϊ�����󶨵���������
	}

	if (!bCheckCullState || (bCheckCullState && (sObject->GetCullState() == CullState::Visible)))
	{
		//cmdList->DrawIndexedInstanced(dxMesh->mVIBuffer->IndexCount, 1, 0, 0, 0);
		for (auto& submesh : dxMesh->Submeshes)
		{
			if (bSetSubmeshCb)
			{
				auto overrideMat = dxSO->GetOverrideMaterial(submesh.first);
				if (overrideMat != nullptr)
					cmdList->SetGraphicsRoot32BitConstants(1, 1, &(overrideMat->MatIndex), 0);
				else
					cmdList->SetGraphicsRoot32BitConstants(1, 1, &(submesh.second.GetMaterial()->MatIndex), 0);
			}
			cmdList->DrawIndexedInstanced(submesh.second.IndexCount, 1, submesh.second.StartIndexLocation, submesh.second.BaseVertexLocation, 0);
		}
	}
}

#pragma endregion

#pragma region Runtime

void GDxRenderer::RegisterTexture(GRiTexture* text)
{
	GDxTexture* dxTex = dynamic_cast<GDxTexture*>(text);
	if (dxTex == nullptr)
		ThrowDxException(L"Dynamic cast from GRiTexture to GDxTexture failed.");

	D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;
	srvDesc.Format = dxTex->Resource->GetDesc().Format;
	srvDesc.Texture2D.MipLevels = dxTex->Resource->GetDesc().MipLevels;

	// if srv is previously created
	if (dxTex->texIndex != -1)
	{
		md3dDevice->CreateShaderResourceView(dxTex->Resource.Get(), &srvDesc, GetCpuSrv(mTextrueHeapIndex + dxTex->texIndex));
	}
	else
	{
		if (mTexturePoolFreeIndex.empty())
			ThrowGGiException("Texture pool has run out.");
		auto it = mTexturePoolFreeIndex.begin();
		dxTex->texIndex = *it;
		md3dDevice->CreateShaderResourceView(dxTex->Resource.Get(), &srvDesc, GetCpuSrv(mTextrueHeapIndex + *it));
		mTexturePoolFreeIndex.erase(it);
	}
}

// ѡ�г����е����塾��͸������/͸�����塿
GRiSceneObject* GDxRenderer::SelectSceneObject(int sx, int sy)
{
	GGiFloat4x4 P = pCamera->GetProj();

	// Compute picking ray in view space.
	float vx = (+2.0f*sx / mClientWidth - 1.0f) / P.GetElement(0, 0);
	float vy = (-2.0f*sy / mClientHeight + 1.0f) / P.GetElement(1, 1);

	// Ray definition in view space.
	XMVECTOR viewRayOrigin = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
	XMVECTOR viewRayDir = XMVectorSet(vx, vy, 1.0f, 0.0f);
			 
	XMMATRIX dxView = GDx::GGiToDxMatrix(pCamera->GetView());
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(dxView), dxView);

	GRiSceneObject* pickedSceneObject = nullptr;
	float tPicked = GGiEngineUtil::Infinity;

	// ͸���Ͳ�͸�������ѡ��
	// Check if we picked an opaque render item.  A real app might keep a separate "picking list"
	// of objects that can be selected.   
	// for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])

	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		auto mesh = so->GetMesh();

		XMMATRIX W = GDx::GGiToDxMatrix(so->GetTransform());

		XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(W), W);

		// Tranform ray to vi space of Mesh.
		XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);

		XMVECTOR rayOrigin = XMVector3TransformCoord(viewRayOrigin, toLocal);
		XMVECTOR rayDir = XMVector3TransformNormal(viewRayDir, toLocal);

		// Make the ray direction unit length for the intersection tests.
		rayDir = XMVector3Normalize(rayDir);

		// If we hit the bounding box of the Mesh, then we might have picked a Mesh triangle,
		// so do the ray/triangle tests.
		//
		// If we did not hit the bounding box, then it is impossible that we hit 
		// the Mesh, so do not waste effort doing ray/triangle tests.
		BoundingBox bBox;
		bBox.Center.x = /*so->GetLocation()[0] +*/ so->GetMesh()->bounds.Center[0];
		bBox.Center.y = /*so->GetLocation()[1] +*/ so->GetMesh()->bounds.Center[1];
		bBox.Center.z = /*so->GetLocation()[2] +*/ so->GetMesh()->bounds.Center[2];
		bBox.Extents.x = so->GetMesh()->bounds.Extents[0];
		bBox.Extents.y = so->GetMesh()->bounds.Extents[1];
		bBox.Extents.z = so->GetMesh()->bounds.Extents[2];
		float tmin = 0.0f;
		if (bBox.Intersects(rayOrigin, rayDir, tmin))
		{
			// NOTE: For the demo, we know what to cast the vertex/index data to.  If we were mixing
			// formats, some metadata would be needed to figure out what to cast it to.
			GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(so->GetMesh());
			if (dxMesh == nullptr)
				ThrowGGiException("cast failed from GRiMesh* to GDxMesh*.");
			shared_ptr<GDxStaticVIBuffer> dxViBuffer = dynamic_pointer_cast<GDxStaticVIBuffer>(dxMesh->mVIBuffer);
			if (dxViBuffer == nullptr)
				ThrowGGiException("cast failed from shared_ptr<GDxStaticVIBuffer> to shared_ptr<GDxStaticVIBuffer>.");
			
			auto vertices = (GRiVertex*)dxViBuffer->VertexBufferCPU->GetBufferPointer();
			auto indices = (std::uint32_t*)dxViBuffer->IndexBufferCPU->GetBufferPointer();
			UINT triCount = dxMesh->mVIBuffer->IndexCount / 3;

			// Find the nearest ray/triangle intersection.
			tmin = GGiEngineUtil::Infinity;
			for (auto submesh : so->GetMesh()->Submeshes)
			{
				auto startIndexLocation = submesh.second.StartIndexLocation;
				auto baseVertexLocation = submesh.second.BaseVertexLocation;

				for (size_t i = 0; i < (submesh.second.IndexCount / 3); i++)
				{
					// Indices for this triangle.
					UINT i0 = indices[startIndexLocation + i * 3 + 0] + baseVertexLocation;
					UINT i1 = indices[startIndexLocation + i * 3 + 1] + baseVertexLocation;
					UINT i2 = indices[startIndexLocation + i * 3 + 2] + baseVertexLocation;

					// Vertices for this triangle.
					XMFLOAT3 v0f;
					XMFLOAT3 v1f;
					XMFLOAT3 v2f;
					v0f.x = vertices[i0].Position[0];
					v0f.y = vertices[i0].Position[1];
					v0f.z = vertices[i0].Position[2];
					v1f.x = vertices[i1].Position[0];
					v1f.y = vertices[i1].Position[1];
					v1f.z = vertices[i1].Position[2];
					v2f.x = vertices[i2].Position[0];
					v2f.y = vertices[i2].Position[1];
					v2f.z = vertices[i2].Position[2];
					XMVECTOR v0 = XMLoadFloat3(&v0f);
					XMVECTOR v1 = XMLoadFloat3(&v1f);
					XMVECTOR v2 = XMLoadFloat3(&v2f);

					// We have to iterate over all the triangles in order to find the nearest intersection.
					float t = 0.0f;
					if (TriangleTests::Intersects(rayOrigin, rayDir, v0, v1, v2, t))
					{
						if (t < tmin)
						{
							// This is the new nearest picked triangle.
							tmin = t;
						}
					}
				}
			}

			std::vector<float> soScale = so->GetScale();
			float relSize = (float)pow(soScale[0] * soScale[0] + soScale[1] * soScale[1] + soScale[2] * soScale[2], 0.5);
			tmin *= relSize;

			if (tmin < tPicked)
			{
				tPicked = tmin;
				pickedSceneObject = so;

				// Modified by Ssi: ��ѡ�е����壬��������͸������
				pickedSceneObjectforTO = so;
			}
		}
	}

	// Modified by Ssi: ͸�������ѡ��
	for (auto so : pSceneObjectLayer[(int)RenderLayer::Transparent])
	{
		auto mesh = so->GetMesh();

		XMMATRIX W = GDx::GGiToDxMatrix(so->GetTransform());

		XMMATRIX invWorld = XMMatrixInverse(&XMMatrixDeterminant(W), W);

		// Tranform ray to vi space of Mesh.
		XMMATRIX toLocal = XMMatrixMultiply(invView, invWorld);

		XMVECTOR rayOrigin = XMVector3TransformCoord(viewRayOrigin, toLocal);
		XMVECTOR rayDir = XMVector3TransformNormal(viewRayDir, toLocal);

		// Make the ray direction unit length for the intersection tests.
		rayDir = XMVector3Normalize(rayDir);

		// If we hit the bounding box of the Mesh, then we might have picked a Mesh triangle,
		// so do the ray/triangle tests.
		//
		// If we did not hit the bounding box, then it is impossible that we hit 
		// the Mesh, so do not waste effort doing ray/triangle tests.
		BoundingBox bBox;
		bBox.Center.x = /*so->GetLocation()[0] +*/ so->GetMesh()->bounds.Center[0];
		bBox.Center.y = /*so->GetLocation()[1] +*/ so->GetMesh()->bounds.Center[1];
		bBox.Center.z = /*so->GetLocation()[2] +*/ so->GetMesh()->bounds.Center[2];
		bBox.Extents.x = so->GetMesh()->bounds.Extents[0];
		bBox.Extents.y = so->GetMesh()->bounds.Extents[1];
		bBox.Extents.z = so->GetMesh()->bounds.Extents[2];
		float tmin = 0.0f;
		if (bBox.Intersects(rayOrigin, rayDir, tmin))
		{
			// NOTE: For the demo, we know what to cast the vertex/index data to.  If we were mixing
			// formats, some metadata would be needed to figure out what to cast it to.
			GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(so->GetMesh());
			if (dxMesh == nullptr)
				ThrowGGiException("cast failed from GRiMesh* to GDxMesh*.");
			shared_ptr<GDxStaticVIBuffer> dxViBuffer = dynamic_pointer_cast<GDxStaticVIBuffer>(dxMesh->mVIBuffer);
			if (dxViBuffer == nullptr)
				ThrowGGiException("cast failed from shared_ptr<GDxStaticVIBuffer> to shared_ptr<GDxStaticVIBuffer>.");

			auto vertices = (GRiVertex*)dxViBuffer->VertexBufferCPU->GetBufferPointer();
			auto indices = (std::uint32_t*)dxViBuffer->IndexBufferCPU->GetBufferPointer();
			UINT triCount = dxMesh->mVIBuffer->IndexCount / 3;

			// Find the nearest ray/triangle intersection.
			tmin = GGiEngineUtil::Infinity;
			for (auto submesh : so->GetMesh()->Submeshes)
			{
				auto startIndexLocation = submesh.second.StartIndexLocation;
				auto baseVertexLocation = submesh.second.BaseVertexLocation;

				for (size_t i = 0; i < (submesh.second.IndexCount / 3); i++)
				{
					// Indices for this triangle.
					UINT i0 = indices[startIndexLocation + i * 3 + 0] + baseVertexLocation;
					UINT i1 = indices[startIndexLocation + i * 3 + 1] + baseVertexLocation;
					UINT i2 = indices[startIndexLocation + i * 3 + 2] + baseVertexLocation;

					// Vertices for this triangle.
					XMFLOAT3 v0f;
					XMFLOAT3 v1f;
					XMFLOAT3 v2f;
					v0f.x = vertices[i0].Position[0];
					v0f.y = vertices[i0].Position[1];
					v0f.z = vertices[i0].Position[2];
					v1f.x = vertices[i1].Position[0];
					v1f.y = vertices[i1].Position[1];
					v1f.z = vertices[i1].Position[2];
					v2f.x = vertices[i2].Position[0];
					v2f.y = vertices[i2].Position[1];
					v2f.z = vertices[i2].Position[2];
					XMVECTOR v0 = XMLoadFloat3(&v0f);
					XMVECTOR v1 = XMLoadFloat3(&v1f);
					XMVECTOR v2 = XMLoadFloat3(&v2f);

					// We have to iterate over all the triangles in order to find the nearest intersection.
					float t = 0.0f;
					if (TriangleTests::Intersects(rayOrigin, rayDir, v0, v1, v2, t))
					{
						if (t < tmin)
						{
							// This is the new nearest picked triangle.
							tmin = t;
						}
					}
				}
			}

			std::vector<float> soScale = so->GetScale();
			float relSize = (float)pow(soScale[0] * soScale[0] + soScale[1] * soScale[1] + soScale[2] * soScale[2], 0.5);
			tmin *= relSize;

			if (tmin < tPicked)
			{
				tPicked = tmin;
				pickedSceneObject = so;

				// Modified by Ssi: ��¼��ѡ�е����壬��������͸������
				pickedSceneObjectforTO = so;
			}
		}
	}

	// �����屻ѡ�У���ȡ�����͸������ΪImgui�ϵ�͸�����Ը�ѡ�򸳳�ֵ
	if (pickedSceneObject != nullptr)
	{
		pImgui->isTransparent = pickedSceneObject->GRiIsTransparent;
	}

	return pickedSceneObject;
}

std::vector<ProfileData> GDxRenderer::GetGpuProfiles()
{
	return GDxGpuProfiler::GetGpuProfiler().GetProfiles();
}

void GDxRenderer::BuildMeshSDF()
{
	std::vector<std::shared_ptr<GRiKdPrimitive>> prims;
	prims.clear();

	CD3DX12_CPU_DESCRIPTOR_HANDLE hDescriptor = GetCpuSrv(mSdfTextrueIndex);

	int sdfIndex = 0;

	mMeshSdfDescriptorBuffer = std::make_unique<GDxUploadBuffer<MeshSdfDescriptor>>(md3dDevice.Get(), MAX_MESH_NUM, false);

	for (auto mesh : pMeshes)
	{

		GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(mesh.second);
		if (dxMesh == nullptr)
			ThrowGGiException("cast failed from GRiMesh* to GDxMesh*.");

		bool find = false;
		std::map<std::wstring, int> SdfGenerateList =
		{
			{L"Stool", 64},
			{L"Cube", 64}
		};

		bool discard = false;
		std::vector<std::wstring> SdfDiscardList =
		{
			L"Stool",
			L"Cube"
		};

		int resolutionToGenerate = 0;
		for (auto iter = SdfGenerateList.begin(); iter != SdfGenerateList.end(); iter++)
		{
			if (mesh.second->Name == (*iter).first)
			{
				find = true;
				resolutionToGenerate = (*iter).second;
			}
		}

		for (auto iter2 = SdfDiscardList.begin(); iter2 != SdfDiscardList.end(); iter2++)
		{
			if (mesh.second->Name == (*iter2))
			{
				discard = true;
			}
		}

		if (discard)
		{
			mesh.second->ClearSdf();
			continue;
		}

		int sdfRes;
		float sdfExtent;
		auto loadedSdf = mesh.second->GetSdf();
		auto loadedRes = mesh.second->GetSdfResolution();

		// if we can't find sdf from serialized scene file.
		if ((loadedSdf == nullptr) || // if we can't find sdf from serialized scene file.
			(loadedSdf->size() != (loadedRes * loadedRes * loadedRes)) || // if the sdf doesn't matche the loaded resolution.
			loadedRes != resolutionToGenerate // if we need to generate sdf in another resolution.
			)
		{
			// if we don't need to generate sdf for this mesh, skip it.
			if (!find)
				continue;

			std::vector<float> sdf;
			sdf.clear();

			// generate mesh SDF.
			shared_ptr<GDxStaticVIBuffer> dxViBuffer = dynamic_pointer_cast<GDxStaticVIBuffer>(dxMesh->mVIBuffer);
			if (dxViBuffer == nullptr)
				ThrowGGiException("cast failed from shared_ptr<GDxStaticVIBuffer> to shared_ptr<GDxStaticVIBuffer>.");

			auto vertices = (GRiVertex*)dxViBuffer->VertexBufferCPU->GetBufferPointer();
			auto indices = (std::uint32_t*)dxViBuffer->IndexBufferCPU->GetBufferPointer();
			UINT triCount = dxMesh->mVIBuffer->IndexCount / 3;

			// Collect primitives.
			for (auto &submesh : dxMesh->Submeshes)
			{
				auto startIndexLocation = submesh.second.StartIndexLocation;
				auto baseVertexLocation = submesh.second.BaseVertexLocation;

				for (size_t i = 0; i < (submesh.second.IndexCount / 3); i++)
				{
					// Indices for this triangle.
					UINT i0 = indices[startIndexLocation + i * 3 + 0] + baseVertexLocation;

					auto prim = std::make_shared<GRiKdPrimitive>(&vertices[i0], &vertices[i0 + 1], &vertices[i0 + 2]);

					prims.push_back(prim);
				}
			}

			int isectCost = 80;
			int travCost = 1;
			float emptyBonus = 0.5f;
			int maxPrims = 1;
			int maxDepth = -1;

			auto pAcceleratorTree = std::make_shared<GRiKdTree>(std::move(prims), isectCost, travCost, emptyBonus,
				maxPrims, maxDepth);

			dxMesh->SetSdfResolution(resolutionToGenerate);
			sdfRes = dxMesh->GetSdfResolution();
			sdf = std::vector<float>(sdfRes * sdfRes * sdfRes);

			/*
			float maxExtent = 0.0f;
			for (int dim = 0; dim < 3; dim++)
			{
				float range = abs(dxMesh->bounds.Center[dim] + dxMesh->bounds.Extents[dim]);
				if (range > maxExtent)
					maxExtent = range;
				range = abs(dxMesh->bounds.Center[dim] - dxMesh->bounds.Extents[dim]);
				if (range > maxExtent)
					maxExtent = range;
			}
			auto sdfExtent = maxExtent * 1.4f * 2.0f;
			*/
			sdfExtent = dxMesh->bounds.Extents[dxMesh->bounds.MaximumExtent()] * 1.4f * 2.0f;
			auto sdfCenter = GGiFloat3(dxMesh->bounds.Center[0], dxMesh->bounds.Center[1], dxMesh->bounds.Center[2]);
			auto sdfUnit = sdfExtent / (float)sdfRes;
			auto initMinDisFront = 1.414f * sdfExtent;
			auto initMaxDisBack = -1.414f * sdfExtent;

			for (int z = 0; z < sdfRes; z++)
			{
				for (int y = 0; y < sdfRes; y++)
				{
					for (int x = 0; x < sdfRes; x++)
					{
						mRendererThreadPool->Enqueue([&, x, y, z]
							{
								int index = z * sdfRes * sdfRes + y * sdfRes + x;
								sdf[index] = 0.0f;

								GGiFloat3 rayOrigin(
									((float)x - sdfRes / 2 + 0.5f) * sdfUnit + sdfCenter.x,
									((float)y - sdfRes / 2 + 0.5f) * sdfUnit + sdfCenter.y,
									((float)z - sdfRes / 2 + 0.5f) * sdfUnit + sdfCenter.z
								);

								static int rayNum = 128;
								static float fibParam = 2 * GGiEngineUtil::PI * 0.618f;
								float fibInter = 0.0f;
								GRiRay ray;
								float minDist = initMinDisFront;
								float outDis = 999.0f;
								int numFront = 0;
								int numBack = 0;
								bool bBackFace;

								ray.Origin[0] = rayOrigin.x;
								ray.Origin[1] = rayOrigin.y;
								ray.Origin[2] = rayOrigin.z;

								// Fibonacci lattices.
								for (int n = 0; n < rayNum; n++)
								{
									ray.Direction[1] = (float)(2 * n + 1) / (float)rayNum - 1;
									fibInter = sqrt(1.0f - ray.Direction[1] * ray.Direction[1]);
									ray.Direction[0] = fibInter * cos(fibParam * n);
									ray.Direction[2] = fibInter * sin(fibParam * n);

									ray.tMax = 99999.0f;

									if (pAcceleratorTree->IntersectDis(ray, &outDis, bBackFace))
									{
										if (bBackFace)
										{
											numBack++;
										}
										else
										{
											numFront++;
										}
										if (outDis < minDist)
											minDist = outDis;
									}
								}

								sdf[index] = minDist;
								if (numBack > numFront)
									sdf[index] *= -1;
							}
						);
					}
				}
			}

			mRendererThreadPool->Flush();

			dxMesh->InitializeSdf(sdf);
		}
		else
		{
			// if we load sdf from serialized file successfully.
			sdfExtent = dxMesh->bounds.Extents[dxMesh->bounds.MaximumExtent()] * 1.4f * 2.0f;
			sdfRes = loadedRes;
		}

#if USE_FIXED_POINT_SDF_TEXTURE
		auto pSDF = dxMesh->GetSdf();
		int index = 0;
		float SdfScale = sdfExtent * SDF_DISTANCE_RANGE_SCALE;
		UINT8* fixedPointSDF = new UINT8[sdfRes * sdfRes * sdfRes];
		for (int z = 0; z < sdfRes; z++)
		{
			for (int y = 0; y < sdfRes; y++)
			{
				for (int x = 0; x < sdfRes; x++)
				{
					index = z * sdfRes * sdfRes + y * sdfRes + x;
					fixedPointSDF[index] = (UINT8)(((*pSDF)[index] / SdfScale + 0.5f) * 256);
				}
			}
		}
#endif

		ResetCommandList();

		D3D12_RESOURCE_DESC texDesc;
		ZeroMemory(&texDesc, sizeof(D3D12_RESOURCE_DESC));
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
		texDesc.Alignment = 0;
		texDesc.DepthOrArraySize = 1;
		texDesc.MipLevels = 1;
		texDesc.SampleDesc.Count = 1;
		texDesc.SampleDesc.Quality = 0;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
		texDesc.Width = (UINT)(sdfRes);
		texDesc.Height = (UINT)(sdfRes);
		texDesc.DepthOrArraySize = (UINT)(sdfRes);
#if USE_FIXED_POINT_SDF_TEXTURE
		texDesc.Format = DXGI_FORMAT::DXGI_FORMAT_R8_UNORM;
#else
		texDesc.Format = DXGI_FORMAT::DXGI_FORMAT_R32_FLOAT;
#endif

		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_COPY_DEST,
			nullptr,
			IID_PPV_ARGS(&mSdfTextures[sdfIndex])));

		const UINT64 uploadBufferSize = GetRequiredIntermediateSize(mSdfTextures[sdfIndex].Get(), 0, 1);

		ThrowIfFailed(md3dDevice->CreateCommittedResource(
			&CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
			D3D12_HEAP_FLAG_NONE,
			&CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize),
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&mSdfTextureUploadBuffer[sdfIndex])));

		D3D12_SUBRESOURCE_DATA textureData = {};
#if USE_FIXED_POINT_SDF_TEXTURE
		textureData.pData = fixedPointSDF;
		textureData.RowPitch = static_cast<LONG_PTR>((sdfRes));
#else
		textureData.pData = dxMesh->GetSdf()->data();
		textureData.RowPitch = static_cast<LONG_PTR>((4 * sdfRes));
#endif
		textureData.SlicePitch = textureData.RowPitch * sdfRes;

		UpdateSubresources(mCommandList.Get(), mSdfTextures[sdfIndex].Get(), mSdfTextureUploadBuffer[sdfIndex].Get(), 0, 0, 1, &textureData);
		mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(mSdfTextures[sdfIndex].Get(),
			D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));

		D3D12_SHADER_RESOURCE_VIEW_DESC sdfSrvDesc = {};
		sdfSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
#if USE_FIXED_POINT_SDF_TEXTURE
		sdfSrvDesc.Format = DXGI_FORMAT_R8_UNORM;
#else
		sdfSrvDesc.Format = DXGI_FORMAT_R32_FLOAT;
#endif
		sdfSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		sdfSrvDesc.Texture3D.MipLevels = 1;
		md3dDevice->CreateShaderResourceView(mSdfTextures[sdfIndex].Get(), &sdfSrvDesc, hDescriptor);
		hDescriptor.Offset(mCbvSrvUavDescriptorSize);

		dxMesh->mSdfIndex = sdfIndex;
		mMeshSdfDescriptors[sdfIndex].Center = DirectX::XMFLOAT3(dxMesh->bounds.Center[0], dxMesh->bounds.Center[1], dxMesh->bounds.Center[2]);
		mMeshSdfDescriptors[sdfIndex].HalfExtent = 0.5f * sdfExtent;
		mMeshSdfDescriptors[sdfIndex].Radius = 0.866f * sdfExtent;
		mMeshSdfDescriptors[sdfIndex].Resolution = sdfRes;
		sdfIndex++;

		ExecuteCommandList();
	}

	auto meshSdfBuffer = mMeshSdfDescriptorBuffer.get();
	for (int i = 0; i < sdfIndex; i++)
	{
		meshSdfBuffer->CopyData(i, mMeshSdfDescriptors[i]);
	}
	
	/*
	for (auto so : pSceneObjectLayer[(int)RenderLayer::Deferred])
	{
		auto mesh = so->GetMesh();

		XMMATRIX soWorld = GDx::GGiToDxMatrix(so->GetTransform());

		GDxMesh* dxMesh = dynamic_cast<GDxMesh*>(so->GetMesh());
		if (dxMesh == nullptr)
			ThrowGGiException("cast failed from GRiMesh* to GDxMesh*.");
		shared_ptr<GDxStaticVIBuffer> dxViBuffer = dynamic_pointer_cast<GDxStaticVIBuffer>(dxMesh->mVIBuffer);
		if (dxViBuffer == nullptr)
			ThrowGGiException("cast failed from shared_ptr<GDxStaticVIBuffer> to shared_ptr<GDxStaticVIBuffer>.");

		auto vertices = (GRiVertex*)dxViBuffer->VertexBufferCPU->GetBufferPointer();
		auto indices = (std::uint32_t*)dxViBuffer->IndexBufferCPU->GetBufferPointer();
		UINT triCount = dxMesh->mVIBuffer->IndexCount / 3;
 
		for (auto &submesh : so->GetMesh()->Submeshes)
		{
			auto startIndexLocation = submesh.second.StartIndexLocation;
			auto baseVertexLocation = submesh.second.BaseVertexLocation;

			for (size_t i = 0; i < (submesh.second.IndexCount / 3); i++)
			{
				// Indices for this triangle.
				UINT i0 = indices[startIndexLocation + i * 3 + 0] + baseVertexLocation;
				//UINT i1 = indices[startIndexLocation + i * 3 + 1] + baseVertexLocation;
				//UINT i2 = indices[startIndexLocation + i * 3 + 2] + baseVertexLocation;

				GRiVertex verticesWorld[3];
				auto vert1 = XMFLOAT4(vertices[i0].Position[0], vertices[i0].Position[1], vertices[i0].Position[2], 1.0f);
				auto vert2 = XMFLOAT4(vertices[i0 + 1].Position[0], vertices[i0 + 1].Position[1], vertices[i0 + 1].Position[2], 1.0f);
				auto vert3 = XMFLOAT4(vertices[i0 + 2].Position[0], vertices[i0 + 2].Position[1], vertices[i0 + 2].Position[2], 1.0f);
				auto vertVec1 = XMLoadFloat4(&vert1);
				auto vertVec2 = XMLoadFloat4(&vert2);
				auto vertVec3 = XMLoadFloat4(&vert3);
				auto vert1WorldPos = DirectX::XMVector4Transform(vertVec1, soWorld);
				auto vert2WorldPos = DirectX::XMVector4Transform(vertVec2, soWorld);
				auto vert3WorldPos = DirectX::XMVector4Transform(vertVec3, soWorld);
				GRiVertex vert1World, vert2World, vert3World;
				vert1World.Position[0] = vert1WorldPos.m128_f32[0];
				vert1World.Position[1] = vert1WorldPos.m128_f32[1];
				vert1World.Position[2] = vert1WorldPos.m128_f32[2];
				vert2World.Position[0] = vert2WorldPos.m128_f32[0];
				vert2World.Position[1] = vert2WorldPos.m128_f32[1];
				vert2World.Position[2] = vert2WorldPos.m128_f32[2];
				vert3World.Position[0] = vert3WorldPos.m128_f32[0];
				vert3World.Position[1] = vert3WorldPos.m128_f32[1];
				vert3World.Position[2] = vert3WorldPos.m128_f32[2];
				verticesWorld[0] = vert1World;
				verticesWorld[1] = vert2World;
				verticesWorld[2] = vert3World;

				auto prim = std::make_shared<GRiKdPrimitive>(verticesWorld[0], verticesWorld[1], verticesWorld[2]);

				prims.push_back(prim);
			}
		}
	}
	int isectCost = 80;
	int travCost = 1;
	float emptyBonus = 0.5f;
	int maxPrims = 1;
	int maxDepth = -1;

	auto mAcceleratorTree = std::make_shared<GRiKdTree>(std::move(prims), isectCost, travCost, emptyBonus,
		maxPrims, maxDepth);

	GRiRay ray;
	ray.Direction[0] = 0.0f;
	ray.Direction[1] = -1.0f;
	ray.Direction[2] = 0.0f;
	ray.Origin[0] = 0.0f;
	ray.Origin[1] = 100.0f;
	ray.Origin[2] = 0.0f;
	float distance = -1.0f;
	bool bBackface = false;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//80

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = 0.0f;
	ray.Direction[2] = 1.0f;
	ray.Origin[0] = 0.0f;
	ray.Origin[1] = 70.0f;
	ray.Origin[2] = 0.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//~100

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = -1.0f;
	ray.Direction[2] = 0.0f;
	ray.Origin[0] = -80.0f;
	ray.Origin[1] = 65.0f;
	ray.Origin[2] = -80.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//15

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = 1.0f;
	ray.Direction[2] = 0.0f;
	ray.Origin[0] = 100.0f;
	ray.Origin[1] = 0.0f;
	ray.Origin[2] = 0.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//-25

	ray.Direction[0] = 0.0f;
	ray.Direction[1] = 0.717f;
	ray.Direction[2] = 0.717f;
	ray.Origin[0] = 100.0f;
	ray.Origin[1] = 0.0f;
	ray.Origin[2] = 0.0f;
	distance = -1.0f;
	ray.tMax = 99999.0f;
	mAcceleratorTree->IntersectDis(ray, &distance, bBackface);
	distance = distance;//-35
	*/
}

#pragma endregion

#pragma region Util

bool GDxRenderer::IsRunning()
{
	if (md3dDevice)
		return true;
	else
		return false;
}

ID3D12Resource* GDxRenderer::CurrentBackBuffer()const
{
	return mSwapChainBuffer[mCurrBackBuffer].Get();
}

D3D12_CPU_DESCRIPTOR_HANDLE GDxRenderer::CurrentBackBufferView()const
{
	return CD3DX12_CPU_DESCRIPTOR_HANDLE(
		mRtvHeap->GetCPUDescriptorHandleForHeapStart(),
		mCurrBackBuffer,
		mRtvDescriptorSize);
}

D3D12_CPU_DESCRIPTOR_HANDLE GDxRenderer::DepthStencilView()const
{
	return mDsvHeap->GetCPUDescriptorHandleForHeapStart();
}

void GDxRenderer::LogAdapters()
{
	UINT i = 0;
	IDXGIAdapter* adapter = nullptr;
	std::vector<IDXGIAdapter*> adapterList;
	while (mdxgiFactory->EnumAdapters(i, &adapter) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_ADAPTER_DESC desc;
		adapter->GetDesc(&desc);

		std::wstring text = L"***Adapter: ";
		text += desc.Description;
		text += L"\n";

		OutputDebugString(text.c_str());

		adapterList.push_back(adapter);

		++i;
	}

	for (size_t i = 0; i < adapterList.size(); ++i)
	{
		LogAdapterOutputs(adapterList[i]);
		ReleaseCom(adapterList[i]);
	}
}

void GDxRenderer::LogAdapterOutputs(IDXGIAdapter* adapter)
{
	UINT i = 0;
	IDXGIOutput* output = nullptr;
	while (adapter->EnumOutputs(i, &output) != DXGI_ERROR_NOT_FOUND)
	{
		DXGI_OUTPUT_DESC desc;
		output->GetDesc(&desc);

		std::wstring text = L"***Output: ";
		text += desc.DeviceName;
		text += L"\n";
		OutputDebugString(text.c_str());

		LogOutputDisplayModes(output, mBackBufferFormat);

		ReleaseCom(output);

		++i;
	}
}

void GDxRenderer::LogOutputDisplayModes(IDXGIOutput* output, DXGI_FORMAT format)
{
	UINT count = 0;
	UINT flags = 0;

	// Call with nullptr to get list count.
	output->GetDisplayModeList(format, flags, &count, nullptr);

	std::vector<DXGI_MODE_DESC> modeList(count);
	output->GetDisplayModeList(format, flags, &count, &modeList[0]);

	for (auto& x : modeList)
	{
		UINT n = x.RefreshRate.Numerator;
		UINT d = x.RefreshRate.Denominator;
		std::wstring text =
			L"Width = " + std::to_wstring(x.Width) + L" " +
			L"Height = " + std::to_wstring(x.Height) + L" " +
			L"Refresh = " + std::to_wstring(n) + L"/" + std::to_wstring(d) +
			L"\n";

		::OutputDebugString(text.c_str());
	}
}

void GDxRenderer::FlushCommandQueue()
{
	// Advance the fence value to mark commands up to this fence point.
	mCurrentFence++;

	// Add an instruction to the command queue to set a new fence point.  Because we 
	// are on the GPU timeline, the new fence point won't be set until the GPU finishes
	// processing all the commands prior to this Signal().
	ThrowIfFailed(mCommandQueue->Signal(mFence.Get(), mCurrentFence));

	// Wait until the GPU has completed commands up to this fence point.
	if (mFence->GetCompletedValue() < mCurrentFence)
	{
		HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);

		// Fire event when GPU hits current fence.  
		ThrowIfFailed(mFence->SetEventOnCompletion(mCurrentFence, eventHandle));

		// Wait until the GPU hits current fence event is fired.
		WaitForSingleObject(eventHandle, INFINITE);
		CloseHandle(eventHandle);
	}
}

void GDxRenderer::ResetCommandList()
{
	auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;
	ThrowIfFailed(cmdListAlloc->Reset());
	ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["GBuffer"].Get()));
}

void GDxRenderer::ExecuteCommandList()
{
	// Execute the commands.
	ThrowIfFailed(mCommandList->Close());
	ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
	mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

	// Wait until command queue is complete.
	FlushCommandQueue();
}

CD3DX12_CPU_DESCRIPTOR_HANDLE GDxRenderer::GetCpuSrv(int index)const
{
	auto srv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetCPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_GPU_DESCRIPTOR_HANDLE GDxRenderer::GetGpuSrv(int index)const
{
	auto srv = CD3DX12_GPU_DESCRIPTOR_HANDLE(mSrvDescriptorHeap->GetGPUDescriptorHandleForHeapStart());
	srv.Offset(index, mCbvSrvUavDescriptorSize);
	return srv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE GDxRenderer::GetDsv(int index)const
{
	auto dsv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mDsvHeap->GetCPUDescriptorHandleForHeapStart());
	dsv.Offset(index, mDsvDescriptorSize);
	return dsv;
}

CD3DX12_CPU_DESCRIPTOR_HANDLE GDxRenderer::GetRtv(int index)const
{
	auto rtv = CD3DX12_CPU_DESCRIPTOR_HANDLE(mRtvHeap->GetCPUDescriptorHandleForHeapStart());
	rtv.Offset(index, mRtvDescriptorSize);
	return rtv;
}

float GDxRenderer::GetHaltonValue(int index, int radix)
{
	float result = 0.0f;
	float fraction = 1.0f / (float)radix;

	while (index > 0)
	{
		result += (float)(index % radix) * fraction;

		index /= radix;
		fraction /= (float)radix;
	}

	return result;
}

DirectX::XMFLOAT2 GDxRenderer::GenerateRandomHaltonOffset()
{
	auto offset = DirectX::XMFLOAT2(
		GetHaltonValue(mHaltonSampleIndex & 1023, 2),
		GetHaltonValue(mHaltonSampleIndex & 1023, 3)
	);

	if (++mHaltonSampleIndex >= mHaltonSampleCount)
		mHaltonSampleIndex = 0;

	return offset;
}

#pragma endregion
