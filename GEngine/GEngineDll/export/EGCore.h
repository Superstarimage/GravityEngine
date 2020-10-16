#pragma once
#include "GCore.h"
// ���嵼������

extern "C"
{
	__declspec(dllexport) void __stdcall InitD3D(HWND hWnd, double width, double height);
}

extern "C"
{
	__declspec(dllexport) int __stdcall Run(void);
}

extern "C"
{
	__declspec(dllexport) void __stdcall MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetWorkDirectory(wchar_t* dir);
}

extern "C"
{
	__declspec(dllexport) int __stdcall GetSceneObjectNum(void);
}

extern "C"
{
	__declspec(dllexport) const wchar_t* __stdcall GetSceneObjectName(int index);
}

extern "C"
{
	__declspec(dllexport) void __stdcall GetSceneObjectTransform(wchar_t* objName, float* trans);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetSceneObjectTransform(wchar_t* objName, float* trans);
}

extern "C"
{
	__declspec(dllexport) bool __stdcall GetTextureSrgb(wchar_t* txtName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetTextureSrgb(wchar_t* txtName, bool bSrgb);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetProjectName(wchar_t* projName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SaveProject();
}

extern "C"
{
	__declspec(dllexport) void __stdcall CreateMaterial(wchar_t* cUniqueName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall GetMaterialScale(wchar_t* matUniqueName, float* scale);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetMaterialScale(wchar_t* matUniqueName, float* scale);
}

extern "C"
{
	__declspec(dllexport) const wchar_t* __stdcall GetMaterialTextureUniqueName(wchar_t* matUniqueName, int index);
}

extern "C"
{
	__declspec(dllexport) bool __stdcall SetMaterialTexture(wchar_t* matUniqueName, int index, wchar_t* texUniqueName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetMaterialTextureToDefaultValue(wchar_t* matUniqueName, int index);
}

extern "C"
{
	__declspec(dllexport) void __stdcall RenameMaterial(wchar_t* oldUniqueName, wchar_t* newUniqueName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetSceneObjectMesh(wchar_t* sceneObjectName, wchar_t* meshUniqueName);
}

//extern "C"
//{
//	__declspec(dllexport) void __stdcall SetSceneObjectMaterial(wchar_t* sceneObjectName, wchar_t* matUniqueName);
//}

extern "C"
{
	__declspec(dllexport) const wchar_t* __stdcall GetSceneObjectMeshName(wchar_t* sceneObjectName);
}

//extern "C"
//{
//	__declspec(dllexport) const wchar_t* __stdcall GetSceneObjectMaterialName(wchar_t* sceneObjectName);
//}

extern "C"
{
	__declspec(dllexport) bool __stdcall SceneObjectExists(wchar_t* sceneObjectName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall CreateSceneObject(wchar_t* sceneObjectName, wchar_t* meshUniqueName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall RenameSceneObject(wchar_t* oldName, wchar_t* newName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall DeleteSceneObject(wchar_t* sceneObjectName);
}

extern "C"
{
	__declspec(dllexport) const wchar_t* __stdcall GetSkyCubemapUniqueName();
}

extern "C"
{
	__declspec(dllexport) const wchar_t* __stdcall GetDefaultSkyCubemapUniqueName();
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetSkyCubemapUniqueName(wchar_t* newName);
}

extern "C"
{
	__declspec(dllexport) bool __stdcall SkyCubemapNameAvailable(wchar_t* cubemapName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetSelectSceneObjectCallback(VoidWstringFuncPointerType pSelectSceneObjectCallback);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SelectSceneObject(wchar_t* sceneObjectName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetRefreshSceneObjectTransformCallback(VoidFuncPointerType pRefreshSceneObjectTransformCallback);
}

extern "C"
{
	__declspec(dllexport) int __stdcall GetMeshSubmeshCount(wchar_t* meshName);
}

extern "C"
{
	__declspec(dllexport) wchar_t** __stdcall GetMeshSubmeshNames(wchar_t* meshName);
}

extern "C"
{
	__declspec(dllexport) const wchar_t* __stdcall GetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName, wchar_t* materialName);
}

extern "C"
{
	__declspec(dllexport) const wchar_t* __stdcall GetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName, wchar_t* materialName);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetTestValue(int index, float value);
}

extern "C"
{
	__declspec(dllexport) void __stdcall SetTestBool(bool value);
}


/*
class EGCore
{
public:
	EGCore();
	~EGCore();
};
*/