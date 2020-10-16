#include "stdafx.h"
#include "EGCore.h"





void __stdcall InitD3D(HWND hWnd, double width, double height)
{
	GCore::GetCore().Initialize(hWnd, width, height);
}

int __stdcall Run(void)
{
	GCore::GetCore().Run();
	return 0;
}

void __stdcall MsgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	// GDxRenderer::GetRenderer().MsgProc(hwnd, msg, wParam, lParam);
	GCore::GetCore().MsgProc(hwnd, msg, wParam, lParam);
}

void __stdcall SetWorkDirectory(wchar_t* dir)
{
	GCore::GetCore().SetWorkDirectory(dir);
}

int __stdcall GetSceneObjectNum(void)
{
	return GCore::GetCore().GetSceneObjectNum();
}

const wchar_t* __stdcall GetSceneObjectName(int index)
{
	return GCore::GetCore().GetSceneObjectName(index);
}

void __stdcall GetSceneObjectTransform(wchar_t* objName, float* trans)
{
	GCore::GetCore().GetSceneObjectTransform(objName, trans);
}

void __stdcall SetSceneObjectTransform(wchar_t* objName, float* trans)
{
	GCore::GetCore().SetSceneObjectTransform(objName, trans);
}

bool __stdcall GetTextureSrgb(wchar_t* txtName)
{
	return GCore::GetCore().GetTextureSrgb(txtName);
}

void __stdcall SetTextureSrgb(wchar_t* txtName, bool bSrgb)
{
	GCore::GetCore().SetTextureSrgb(txtName, bSrgb);
}

void __stdcall SetProjectName(wchar_t* projName)
{
	return GCore::GetCore().SetProjectName(projName);
}

void __stdcall SaveProject()
{
	return GCore::GetCore().SaveProject();
}

void __stdcall CreateMaterial(wchar_t* cUniqueName)
{
	GCore::GetCore().CreateMaterial(cUniqueName);
}

void __stdcall GetMaterialScale(wchar_t* matUniqueName, float* scale)
{
	GCore::GetCore().GetMaterialScale(matUniqueName, scale);
}

void __stdcall SetMaterialScale(wchar_t* matUniqueName, float* scale)
{
	GCore::GetCore().SetMaterialScale(matUniqueName, scale);
}

const wchar_t* __stdcall GetMaterialTextureUniqueName(wchar_t* matUniqueName, int index)
{
	return GCore::GetCore().GetMaterialTextureUniqueName(matUniqueName, index);
}

bool __stdcall SetMaterialTexture(wchar_t* matUniqueName, int index, wchar_t* texUniqueName)
{
	return GCore::GetCore().SetMaterialTexture(matUniqueName, index, texUniqueName);
}

void __stdcall SetMaterialTextureToDefaultValue(wchar_t* matUniqueName, int index)
{
	GCore::GetCore().SetMaterialTextureToDefaultValue(matUniqueName, index);
}

void __stdcall RenameMaterial(wchar_t* oldUniqueName, wchar_t* newUniqueName)
{
	GCore::GetCore().RenameMaterial(oldUniqueName, newUniqueName);
}

void __stdcall SetSceneObjectMesh(wchar_t* sceneObjectName, wchar_t* meshUniqueName)
{
	GCore::GetCore().SetSceneObjectMesh(sceneObjectName, meshUniqueName);
}

//void __stdcall SetSceneObjectMaterial(wchar_t* sceneObjectName, wchar_t* matUniqueName)
//{
//	GCore::GetCore().SetSceneObjectMaterial(sceneObjectName, matUniqueName);
//}

const wchar_t* __stdcall GetSceneObjectMeshName(wchar_t* sceneObjectName)
{
	return GCore::GetCore().GetSceneObjectMeshName(sceneObjectName);
}

//const wchar_t* __stdcall GetSceneObjectMaterialName(wchar_t* sceneObjectName)
//{
//	return GCore::GetCore().GetSceneObjectMaterialName(sceneObjectName);
//}

bool __stdcall SceneObjectExists(wchar_t* sceneObjectName)
{
	return GCore::GetCore().SceneObjectExists(sceneObjectName);
}

void __stdcall CreateSceneObject(wchar_t* sceneObjectName, wchar_t* meshUniqueName)
{
	return GCore::GetCore().CreateSceneObject(sceneObjectName, meshUniqueName);
}

void __stdcall RenameSceneObject(wchar_t* oldName, wchar_t* newName)
{
	GCore::GetCore().RenameSceneObject(oldName, newName);
}

void __stdcall DeleteSceneObject(wchar_t* sceneObjectName)
{
	GCore::GetCore().DeleteSceneObject(sceneObjectName);
}

const wchar_t* __stdcall GetSkyCubemapUniqueName()
{
	return GCore::GetCore().GetSkyCubemapUniqueName();
}

const wchar_t* __stdcall GetDefaultSkyCubemapUniqueName()
{
	return GCore::GetCore().GetDefaultSkyCubemapUniqueName();
}

void __stdcall SetSkyCubemapUniqueName(wchar_t* newName)
{
	GCore::GetCore().SetSkyCubemapUniqueName(newName);
}

bool __stdcall SkyCubemapNameAvailable(wchar_t* cubemapName)
{
	return GCore::GetCore().SkyCubemapNameAvailable(cubemapName);
}

void __stdcall SetSelectSceneObjectCallback(VoidWstringFuncPointerType pSelectSceneObjectCallback)
{
	GCore::GetCore().SetSelectSceneObjectCallback(pSelectSceneObjectCallback);
}

void __stdcall SelectSceneObject(wchar_t* sceneObjectName)
{
	GCore::GetCore().SelectSceneObject(sceneObjectName);
}

void __stdcall SetRefreshSceneObjectTransformCallback(VoidFuncPointerType pRefreshSceneObjectTransformCallback)
{
	GCore::GetCore().SetRefreshSceneObjectTransformCallback(pRefreshSceneObjectTransformCallback);
}

int __stdcall GetMeshSubmeshCount(wchar_t* meshName)
{
	return GCore::GetCore().GetMeshSubmeshCount(meshName);
}

wchar_t** __stdcall GetMeshSubmeshNames(wchar_t* meshName)
{
	return GCore::GetCore().GetMeshSubmeshNames(meshName);
}

const wchar_t* __stdcall GetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName)
{
	return GCore::GetCore().GetMeshSubmeshMaterialUniqueName(meshName, submeshName);
}

void __stdcall SetMeshSubmeshMaterialUniqueName(wchar_t* meshName, wchar_t* submeshName, wchar_t* materialName)
{
	GCore::GetCore().SetMeshSubmeshMaterialUniqueName(meshName, submeshName, materialName);
}

const wchar_t* __stdcall GetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName)
{
	return GCore::GetCore().GetSceneObjectOverrideMaterial(soName, submeshName);
}

void __stdcall SetSceneObjectOverrideMaterial(wchar_t* soName, wchar_t* submeshName, wchar_t* materialName)
{
	GCore::GetCore().SetSceneObjectOverrideMaterial(soName, submeshName, materialName);
}

void __stdcall SetTestValue(int index, float value)
{
	return GCore::GetCore().SetTestValue(index, value);
}

void __stdcall SetTestBool(bool value)
{
	GCore::GetCore().SetTestBool(value);
}





