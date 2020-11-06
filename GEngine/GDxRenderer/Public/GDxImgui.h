#pragma once
#include "GDxPreInclude.h"



class GDxImgui : public GRiImgui
{

public:

	GDxImgui();
	~GDxImgui();

	void Initialize(HWND handleWnd, ID3D12Device* pDevice, int NumFrameResources, ID3D12DescriptorHeap* pDescriptorHeap);

	virtual void BeginFrame() override;

	virtual void SetGUIContent(
		bool bShowGizmo, 
		const float *cameraView,
		float *cameraProjection,
		float* objectLocation,
		float* objectRotation,
		float* objectScale,
		float& cameraSpeed,
		std::vector<CpuProfileData> cpuProfiles,
		std::vector<ProfileData> gpuProfiles,
		int clientWidth,
		int clientHeight
	) override;

	void Render(ID3D12GraphicsCommandList* cmdList);

	virtual void ShutDown() override;

	// Modified by Ssi: Imgui控制Alpha值——滑动条部分
	static float adjustAlpha;	// 静态全局变量(默认初值为0)，调出给GDxRenderer.cpp中的常量缓冲区接收
	static bool isAlphaChanged; // 判断imgui是否修改了alpha值

	// Modified by Ssi: Imgui控制Alpha值——复选框部分
	static bool isTransparent;  // 物体透明属性的实际值
	static bool isTransparentCheckboxChanged; // 检测透明属性是否被修改——复选框是否被勾选/取消勾选
	
private:
	
	void Manipulation(bool bShowGizmo, const float *cameraView, float *cameraProjection, float* objectLocation, float* objectRotation, float* objectScale, float& cameraSpeed);

	static ImU8 mCameraSpeedUpperBound;
	static ImU8 mCameraSpeedLowerBound;
	static float mInitialCameraSpeed;

	// Modified by Ssi: Imgui控制Alpha值
	static float mAlphaUpperBound;
	static float mAlphaLowerBound;
	static float mAlpha;

};

