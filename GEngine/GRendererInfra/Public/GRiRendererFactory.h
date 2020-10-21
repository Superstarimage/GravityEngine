#pragma once
#include "GRiPreInclude.h"
#include "GRiTexture.h"
#include "GRiTextureLoader.h"
#include "GRiMaterial.h"
#include "GGiInclude.h"
#include "GRiMesh.h"
#include "GRiMeshData.h"
#include "GRiGeometryGenerator.h"
#include "GRiSceneObject.h"
#include "GRiImgui.h"

// 创建材质队形、创建材质加载器对象、创建材质对象、创建Mesh对象、创建几何体生成器对象、创建场景物体对象、创建Imgui对象
class GRiRendererFactory
{

public:
	GRiRendererFactory();
	~GRiRendererFactory();

	virtual GRiTexture* CreateTexture() = 0;

	virtual GRiTextureLoader* CreateTextureLoader() = 0;

	virtual GRiMaterial* CreateMaterial() = 0;

	//virtual GGiFloat4* CreateFloat4() = 0;

	//virtual GGiFloat4* CreateFloat4(float x, float y, float z, float w) = 0;

	//virtual GGiFloat4x4* CreateFloat4x4() = 0;

	virtual GRiMesh* CreateMesh(std::vector<GRiMeshData> meshData) = 0;

	virtual GRiGeometryGenerator* CreateGeometryGenerator() = 0;

	virtual GRiSceneObject* CreateSceneObject() = 0;

	virtual GRiImgui* CreateImgui() = 0;

};

