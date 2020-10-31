#pragma once

#include "GDxRenderer.h"

#include "GProject.h"
#include "GScene.h"
#include "GMaterial.h"
#include "GGuiCallback.h"
#include "Shaders/ShaderDefinition.h"




using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

// Win32 message handler
#ifdef USE_IMGUI
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

class GCore
{
public:
	GCore(const GCore& rhs) = delete;
	GCore& operator=(const GCore& rhs) = delete;
	~GCore();

	static GCore& GetCore();

	void Run();

	void Initialize(HWND OutputWindow, double width, double height);

	void MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

#pragma region export

	void SaveProject();

	int GetSceneObjectNum();

	const wchar_t* GetSceneObjectName(int index);

	void GetSceneObjectTransform(wchar_t* objName, float* trans);

	void SetSceneObjectTransform(wchar_t* objName, float* trans);

	void SetWorkDirectory(wchar_t* dir);

	void SetProjectName(wchar_t* projName);

	bool GetTextureSrgb(wchar_t* txtName);

	void SetTextureSrgb(wchar_t* txtName, bool bSrgb);

	void CreateMaterial(wchar_t* cUniqueName);

	void GetMaterialScale(wchar_t* matUniqueName, float* scale);

	void SetMaterialScale(wchar_t* matUniqueName, float* scale);

	const wchar_t* GetMaterialTextureUniqueName(wchar_t* matUniqueName, int index);

	bool SetMaterialTexture(wchar_t* matUniqueName, int index, wchar_t* texUniqueName);

	void SetMaterialTextureToDefaultValue(wchar_t* matUniqueName, int index);

	void RenameMaterial(wchar_t* oldUniqueName, wchar_t* newUniqueName);

	void SetSceneObjectMesh(wchar_t* sceneObjectName, wchar_t* meshUniqueName);

	//void SetSceneObjectMaterial(wchar_t* sceneObjectName, wchar_t* matUniqueName);

	const wchar_t* GetSceneObjectMeshName(wchar_t* sceneObjectName);

	//const wchar_t* GetSceneObjectMaterialName(wchar_t* sceneObjectName);

	bool SceneObjectExists(wchar_t* sceneObjectName);

	void CreateSceneObject(wchar_t* sceneObjectName, wchar_t* meshUniqueName);

	void RenameSceneObject(wchar_t* oldName, wchar_t* newName);

	void DeleteSceneObject(wchar_t* sceneObjectName);

	const wchar_t* GetSkyCubemapUniqueName();

	const wchar_t* GetDefaultSkyCubemapUniqueName();

	void SetSkyCubemapUniqueName(wchar_t* newName);

	bool SkyCubemapNameAvailable(wchar_t* cubemapName);

	void SetSelectSceneObjectCallback(VoidWstringFuncPointerType callback);

	void SelectSceneObject(wchar_t* sceneObjectName);

	void SetRefreshSceneObjectTransformCallback(VoidFuncPointerType pRefreshSceneObjectTransformCallback);

	int GetMeshSubmeshCount(wchar_t* meshName);

	wchar_t** GetMeshSubmeshNames(wchar_t* meshName);

	const wchar_t* GetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName);

	void SetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName, wchar_t* materialName);

	const wchar_t* GetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName);

	void SetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName, wchar_t* materialName);

	void SetTestValue(int index, float value);

	void SetTestBool(bool value);

#pragma endregion

private:

	bool      mAppPaused = false;  // is the application paused?
	bool      mMinimized = false;  // is the application minimized?
	bool      mMaximized = false;  // is the application maximized?
	bool      mResizing = false;   // are the resize bars being dragged?
	bool      mFullscreenState = false;// fullscreen enabled

	bool	  mboolSceneLoad = false;  // 载入场景开关；场景是否已导入 modified by Ssi

	std::wstring WorkDirectory;   // 工作目录

	std::wstring EngineDirectory; // 引擎工作目录

	std::wstring ProjectName;	  // 项目名字

	GRiRenderer* mRenderer;

	GRiRendererFactory* pRendererFactory; // 包含成员函数：创建材质队形、创建材质加载器对象、创建材质对象、创建Mesh对象、创建几何体生成器对象、创建场景物体对象、创建Imgui对象


	std::unique_ptr<GGiGameTimer> mTimer;

	std::unordered_map<std::wstring, std::unique_ptr<GRiTexture>> mTextures;         // 纹理
	std::unordered_map<std::wstring, std::unique_ptr<GRiMaterial>> mMaterials;		 // 材质
	std::unordered_map<std::wstring, std::unique_ptr<GRiMesh>> mMeshes;				 // 网格
	std::unordered_map<std::wstring, std::unique_ptr<GRiSceneObject>> mSceneObjects; // 场景对象
	std::vector<GRiSceneObject*> mSceneObjectLayer[(int)RenderLayer::Count];		 // 场景中不同类别的对象数组
	// Modified by Ssi: 
	std::vector<GRiSceneObject*> mSceneObjectLayer_Deferred_Transparent;			 // 场景中Deferred和Transparent对象 -- 用于集中操作

	std::unordered_map<std::wstring, std::unique_ptr<GMaterial>> mMaterialFiles;

	std::unique_ptr<GRiCamera> mCamera;

	std::unique_ptr<GRiCamera> mCubemapSampleCamera[6];

	POINT mLastMousePos;

	GProject* mProject;

	std::unique_ptr<GRiImgui> mImgui;

	UINT mMaterialIndex = 0;

	UINT mSceneObjectIndex = 0;

	std::wstring mSkyCubemapUniqueName;

	GRiSceneObject* mSelectedSceneObject = nullptr;

	GGuiCallback* mGuiCallback;

	float mCameraSpeed = 150.0f;

private:

	GCore();
	
	void OnResize();

	void Update();

	void RecordPrevFrame(const GGiGameTimer* gt);
	void OnKeyboardInput(const GGiGameTimer* gt);
	void UpdateGui(const GGiGameTimer* gt);
	void OnMouseDown(WPARAM btnState, int x, int y);
	void OnMouseUp(WPARAM btnState, int x, int y);
	void OnMouseMove(WPARAM btnState, int x, int y);

	void CreateImgui();
	void LoadTextures();
	void LoadMaterials();
	void LoadMeshes();
	void LoadSceneObjects();
	void LoadCameras();

	void LoadProject();

	void Pick(int sx, int sy);

	//Util
	std::vector<std::wstring> GetAllFilesInFolder(std::wstring path, bool bCheckFormat, std::vector<std::wstring> format);
	std::vector<std::wstring> GetAllFilesUnderFolder(std::wstring path, bool bCheckFormat, std::vector<std::wstring> format);

};
