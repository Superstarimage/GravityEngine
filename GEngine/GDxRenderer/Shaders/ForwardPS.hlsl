#include "ShaderDefinition.h"
#include "Lighting.hlsli"
#include "MainPassCB.hlsli"
#include "ObjectCB.hlsli"
#include "Material.hlsli"

StructuredBuffer<LightList> gLightList : register(t0); // 光源列表

//// G-Buffer
//Texture2D gAlbedoTexture			: register(t1);
//Texture2D gNormalTexture			: register(t2);
//Texture2D gVelocityTexture			: register(t3);
//Texture2D gOrmTexture				: register(t4);

// Depth
Texture2D gDepthBuffer				: register(t1);

//// Shadow
//Texture2D gShadowTexture			: register(t6);

//// Occlusion
//Texture2D gOcclusionTexture			: register(t7);

//#define PREFILTER_MIP_LEVEL 5
//
//// IBL
//TextureCube skyIrradianceTexture	: register(t8);
//Texture2D	brdfLUTTexture			: register(t9);
//TextureCube skyPrefilterTexture[PREFILTER_MIP_LEVEL]	: register(t10);

struct VertexToPixel
{
	float4 PosH    : SV_POSITION;
	float3 PosW    : POSITION;
	float3 NormalW : NORMAL;
	float2 TexC    : TEXCOORD;
};

struct PixelOutput
{
	float4	color;				// : SV_TARGET0;
	// DeferredPS:
	float4	albedo;				// : SV_TARGET0;
	float4	normal;				// : SV_TARGET1;
	float2	velocity;			// : SV_TARGET2;
	float4	occlusionRoughnessMetallic;	// : SV_TARGET3;
};

Texture2D gTextureMaps[MAX_TEXTURE_NUM] : register(t2);

SamplerState			basicSampler	: register(s0);
SamplerComparisonState	shadowSampler	: register(s1);

float ViewDepth(float depth)
{
	return (FAR_Z * NEAR_Z) / (FAR_Z - depth * (FAR_Z - NEAR_Z));
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
	float ndcX = uv.x * 2 - 1;
	float ndcY = 1 - uv.y * 2;//remember to flip y!!!
	float4 viewPos = mul(float4(ndcX, ndcY, depth, 1.0f), gInvProj);
	viewPos = viewPos / viewPos.w;
	return mul(viewPos, gInvView).xyz;
}

float3 SchlickFresnel(float3 R0, float3 normal, float3 lightVec)
{
	float cosIncidentAngle = saturate(dot(normal, lightVec)); // saturate限制到[0,1]

	float f0 = 1.0f - cosIncidentAngle;
	float3 reflectPercent = R0 + (1.0f - R0)*(f0*f0*f0*f0*f0);

	return reflectPercent;
}

float3 sd_BlinnPhong(float3 diffuseAlbedo, float3 toLight, float3 normal, float lightIntensity, float lightColor, float metal, float roughness, float3 PosW)
{
	// Prepare for Specular
	float3 toCam = normalize(gEyePosW - PosW);
	float3 h = normalize(toLight + toCam);
	float roughnessFactor = (256.0f*(1 - roughness) + 8.0f) * pow(max(dot(h, normal), 0.0f), 256.0f*(1 - roughness)) / 8.0f; // roughness
	float3 f0 = lerp(F0_NON_METAL.rrr, diffuseAlbedo.rgb, metal);
	float3 fresnelFactor = SchlickFresnel(f0, normal, toLight);

	// ndotl
	float ndotl = max(dot(toLight, normal), 0.0f);
	// float3 ndotlColor = float3(1.f, 1.f, 1.f) *  ndotl;

	// Diffuse part: max(L·n, 0)·BL×md
	float3 diffuseColor = ndotl * dot(lightIntensity*lightColor, diffuseAlbedo);

	// Specular part:
	float3 specAlbedo = fresnelFactor * roughnessFactor;
	// Our spec formula goes outside [0,1] range, but we are 
	// doing LDR rendering.  So scale it down a bit.
	specAlbedo = specAlbedo / (specAlbedo + 1.0f);
	float3 specColor = ndotl * dot(lightIntensity*lightColor, specAlbedo);

	return specColor + diffuseColor;
}

float4 main(VertexToPixel pIn) : SV_TARGET
{
	// 获取漫反射反照率
	MaterialData matData = gMaterialData[gMaterialIndex];
	uint diffuseMapIndex = matData.TextureIndex[0];
	uint OrmMapIndex = matData.TextureIndex[2];

	float3 diffuseAlbedo = gTextureMaps[diffuseMapIndex].Sample(basicSampler, pIn.TexC).rgb;
	float3 ormFromTexture = gTextureMaps[OrmMapIndex].Sample(basicSampler, pIn.TexC).rgb;
	
	float roughness = ormFromTexture.g;
	float metal = ormFromTexture.b;

	// 在这确定：normal、roughness、metal、albedo、shadowamount

	// 法向量：
	float3 normal = normalize(pIn.NormalW);

	// LightPass
	float depthBuffer = gDepthBuffer.Sample(basicSampler, pIn.TexC).r;
	float depth = ViewDepth(depthBuffer);

	// 计算girdId
	uint gridId = 0;

	uint clusterZ = 0;
	for (clusterZ = 0; ((depth > DepthSlicing_16[clusterZ + 1]) && (clusterZ < CLUSTER_NUM_Z - 1)); clusterZ++)
	{
		;
	}
	uint offsetX = floor(pIn.TexC.x * gRenderTargetSize.x / CLUSTER_SIZE_X);
	uint offsetY = floor(pIn.TexC.y * gRenderTargetSize.y / CLUSTER_SIZE_Y);
	gridId = (offsetY * ceil(gRenderTargetSize.x / CLUSTER_SIZE_X) + offsetX) * CLUSTER_NUM_Z + clusterZ;


	// 获取点光源、聚光灯数量
	uint numPointLight = gLightList[gridId].NumPointLights;
	uint numSpotlight = gLightList[gridId].NumSpotlights;
	if (numPointLight > MAX_GRID_POINT_LIGHT_NUM)
		numPointLight = MAX_GRID_POINT_LIGHT_NUM;
	if (numSpotlight > MAX_GRID_SPOTLIGHT_NUM)
		numSpotlight = MAX_GRID_SPOTLIGHT_NUM;

	float3 finalColor = 0.f;

	float shadowAmount = 1.f;
	int i = 0;

	for (i = 0; i < numPointLight; i++)
	{
		// shadowAmount = 1.f;
		// 光源随距离的衰减系数
		float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, pIn.PosW);
		// 衰减后的光强
		float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
		// 光向量（指向光源）
		float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - pIn.PosW);
		// 光源的颜色
		float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;

		// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
		// finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
		// finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);

		// finalColor += sd_BlinnPhong(diffuseAlbedo, toLight, normal, lightIntensity, lightColor, metal, roughness, pIn.PosW);
		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normal, pIn.PosW, gEyePosW, roughness, metal, diffuseAlbedo, shadowAmount);
	}


	// Directional light.
	for (i = 0; i < dirLightCount; i++) // dirLightCount
	{
		// float shadowAmount = gShadowTexture.Sample(basicSampler, pIn.TexC).r;

		float lightIntensity = dirLight[i].Intensity;
		float3 toLight = normalize(-dirLight[i].Direction);
		float3 lightColor = dirLight[i].DiffuseColor.rgb;

		// finalColor += sd_BlinnPhong(diffuseAlbedo, toLight, normal, lightIntensity, lightColor, metal, roughness, pIn.PosW);
		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normal, pIn.PosW, gEyePosW, roughness, metal, diffuseAlbedo, shadowAmount);
	}

	// 环境光
	float4 ambientColor = gAmbientLight * float4(diffuseAlbedo, 1.0f);
	// finalColor += ambientColor.xyz;

	PixelOutput o;
	o.color = float4(finalColor , blend_alpha);
	// o.color = float4(finalColor , 0.0f);

	return o.color;
	// return float4(0.5f, 0.5f, 0.5f, blend_alpha);
}
