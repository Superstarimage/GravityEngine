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

	static float adjustAlpha;	 // ��̬ȫ�ֱ���(Ĭ�ϳ�ֵΪ0)��������GDxRenderer.cpp�еĳ�������������
	static float isAlphaChanged; // �ж�imgui�Ƿ��޸���alphaֵ
	
private:
	
	void Manipulation(bool bShowGizmo, const float *cameraView, float *cameraProjection, float* objectLocation, float* objectRotation, float* objectScale, float& cameraSpeed);

	static ImU8 mCameraSpeedUpperBound;
	static ImU8 mCameraSpeedLowerBound;
	static float mInitialCameraSpeed;

	// Modified by Ssi: Imgui����Alphaֵ
	static float mAlphaUpperBound;
	static float mAlphaLowerBound;
	static float mAlpha;
};

