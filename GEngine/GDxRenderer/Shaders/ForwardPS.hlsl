#include "ShaderDefinition.h"
#include "Lighting.hlsli"
#include "MainPassCB.hlsli"
#include "ObjectCB.hlsli"
#include "Material.hlsli"

StructuredBuffer<LightList> gLightList : register(t0); // 光源列表

// G-Buffer
Texture2D gAlbedoTexture			: register(t1);
Texture2D gNormalTexture			: register(t2);
Texture2D gVelocityTexture			: register(t3);
Texture2D gOrmTexture				: register(t4);

// Depth
Texture2D gDepthBuffer				: register(t5);

// Shadow
Texture2D gShadowTexture			: register(t6);

// Occlusion
Texture2D gOcclusionTexture			: register(t7);

#define PREFILTER_MIP_LEVEL 5

// IBL
TextureCube skyIrradianceTexture	: register(t8);
Texture2D	brdfLUTTexture			: register(t9);
TextureCube skyPrefilterTexture[PREFILTER_MIP_LEVEL]	: register(t10);

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

Texture2D gTextureMaps[MAX_TEXTURE_NUM] : register(t15);

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
	
	// 深度检测
	//float linearDepth = (depth - NEAR_Z) / (FAR_Z - NEAR_Z);
	//if (linearDepth <= 0.0f)
	//{
	//	PixelOutput o;
	//	o.color = float4(0.0f, 0.0f, 0.0f, 1.0f);
	//	// Modified by Ssi:
	//	return o.color;
	//}

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

	//for (i = 0; i < numPointLight; i++)
	//{
	//	shadowAmount = 1.f;
	//	// 光源随距离的衰减系数
	//	float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
	//	// 衰减后的光强
	//	float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
	//	// 光向量（指向光源）
	//	float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
	//	// 光源的颜色
	//	float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;

	//	// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
	//	// finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
	//	// finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
	//	float ndotl = max(dot(toLight, normal), 0.0f);
	//	finalColor = float3(1.f, 1.f, 1.f) *  ndotl;
	//}


	// Directional light.
	for (i = 0; i < 1; i++) // dirLightCount
	{
		float shadowAmount = gShadowTexture.Sample(basicSampler, pIn.TexC).r;

		float lightIntensity = dirLight[i].Intensity;
		float3 toLight = normalize(-dirLight[i].Direction);
		// float3 toLight = normalize(-float3(1.f, 1.f, 1.f));
		float3 toCam = normalize(gEyePosW - pIn.PosW);
		float3 h = normalize(toLight + toCam);
		float roughnessFactor = (256.0f*(1 - roughness) + 8.0f) * pow(max(dot(h, normal), 0.0f), 256.0f*(1 - roughness)) / 8.0f; // roughness
		float3 f0 = lerp(F0_NON_METAL.rrr, diffuseAlbedo.rgb, metal);
		// float3 f0 = float3(0.01f, 0.01f, 0.01f);
		float3 fresnelFactor = SchlickFresnel(f0, normal, toLight);

		float3 lightColor = dirLight[i].DiffuseColor.rgb;

		// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
		// finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
		// finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
		
		// ndotl
		float ndotl = max(dot(toLight, normal), 0.0f);
		// float3 ndotlColor = float3(1.f, 1.f, 1.f) *  ndotl;
		// 漫反射部分 max(L·n, 0)·BL×md
		float3 diffuseColor = ndotl * dot(lightIntensity*lightColor, diffuseAlbedo);
		// 镜面高光部分
		float3 specAlbedo = fresnelFactor * roughnessFactor;
		// Our spec formula goes outside [0,1] range, but we are 
		// doing LDR rendering.  So scale it down a bit.
		specAlbedo = specAlbedo / (specAlbedo + 1.0f);
		

		float3 specColor = ndotl * dot(lightIntensity*lightColor, specAlbedo);
		// float3 specColor = ndotl * dot(lightIntensity*lightColor, fresnelFactor);
		// float3 specColor = ndotl * lightIntensity * lightColor * roughnessFactor; 
		// float3 specColor = ndotlColor;
		
		finalColor += specColor + diffuseColor;
	}

	// 环境光
	float4 ambientColor = gAmbientLight * float4(diffuseAlbedo, 1.0f);
	finalColor += ambientColor.xyz;

	PixelOutput o;
	o.color = float4(finalColor , blend_alpha);
	// o.color = float4(finalColor , 0.0f);

	return o.color;
	// return float4(0.5f, 0.5f, 0.5f, blend_alpha);
}



//float4 main(VertexToPixel pIn) : SV_TARGET
//{
//	// 获取材质数据
//	MaterialData matData = gMaterialData[gMaterialIndex];
//
//	uint diffuseMapIndex = matData.TextureIndex[0];
//	uint normalMapIndex = matData.TextureIndex[1];
//	uint OrmMapIndex = matData.TextureIndex[2];
//
//	float3 albedoFromTexture = gTextureMaps[diffuseMapIndex].Sample(basicSampler, pIn.uv).rgb;
//	float3 normalFromTexture = gTextureMaps[normalMapIndex].Sample(basicSampler, pIn.uv).rgb;
//	float3 ormFromTexture = gTextureMaps[OrmMapIndex].Sample(basicSampler, pIn.uv).rgb;
//	//if (matData.TextureSrgb[0] == 1)
//		//albedoFromTexture = pow(albedoFromTexture, 2.2f);
//	//if (matData.TextureSrgb[1] == 1)
//		//normalFromTexture = pow(normalFromTexture, 2.2f);
//	//if (matData.TextureSrgb[2] == 1)
//		//ormFromTexture = pow(ormFromTexture, 2.2f);
//
//	float4 prevPos = pIn.prevPos;
//	prevPos = prevPos / prevPos.w;
//	prevPos.xy = prevPos.xy / float2(2.0f, -2.0f) + float2(0.5f, 0.5f);//negate Y because world coord and tex coord have different Y axis.
//	float4 curPos = pIn.curPos;
//	curPos = curPos / curPos.w;
//	curPos.xy = curPos.xy / float2(2.0f, -2.0f) + float2(0.5f, 0.5f);//negate Y because world coord and tex coord have different Y axis.
//
//	float3 normal = calculateNormalFromMap(normalFromTexture, normalize(pIn.normal), pIn.tangent);
//	PixelOutput output;
//	output.albedo = float4(albedoFromTexture, 1.0f);;
//	output.normal = float4(normalize(normal), 1.0f);
//	//output.worldPos = float4(pIn.worldPos, 0.0f);
//	output.velocity = float2(curPos.x - prevPos.x, curPos.y - prevPos.y);
//	//output.velocity = float2(prevPos.x, prevPos.y);
//	float roughness = ormFromTexture.g;
//	float metal = ormFromTexture.b;
//	output.occlusionRoughnessMetallic = float4(0, roughness, metal, 0);
//	output.albedo.a = pIn.linearZ;
//
//
//
//
//
//	// LightPass
//	float depthBuffer = gDepthBuffer.Sample(basicSampler, pIn.uv).r;
//	float depth = ViewDepth(depthBuffer);
//	float linearDepth = (depth - NEAR_Z) / (FAR_Z - NEAR_Z);
//
//	//if (linearDepth <= 0.0f)
//	//{
//	//	PixelOutput o;
//	//	o.color = float4(0.0f, 0.0f, 0.0f, 1.0f);
//	//	o.ambientSpecular = float4(0.0f, 0.0f, 0.0f, 1.0f);
//	//	// Modified by Ssi:
//	//	return o.color;
//	//}
//
//	// 计算girdId
//	uint gridId = 0;
//
//	uint clusterZ = 0;
//	for (clusterZ = 0; ((depth > DepthSlicing_16[clusterZ + 1]) && (clusterZ < CLUSTER_NUM_Z - 1)); clusterZ++)
//	{
//		;
//	}
//	uint offsetX = floor(pIn.uv.x * gRenderTargetSize.x / CLUSTER_SIZE_X);
//	uint offsetY = floor(pIn.uv.y * gRenderTargetSize.y / CLUSTER_SIZE_Y);
//	gridId = (offsetY * ceil(gRenderTargetSize.x / CLUSTER_SIZE_X) + offsetX) * CLUSTER_NUM_Z + clusterZ;
//
//
//	// 获取点光源、聚光灯数量
//	uint numPointLight = gLightList[gridId].NumPointLights;
//	uint numSpotlight = gLightList[gridId].NumSpotlights;
//	if (numPointLight > MAX_GRID_POINT_LIGHT_NUM)
//		numPointLight = MAX_GRID_POINT_LIGHT_NUM;
//	if (numSpotlight > MAX_GRID_SPOTLIGHT_NUM)
//		numSpotlight = MAX_GRID_SPOTLIGHT_NUM;
//
//	float3 finalColor = 0.f;
//
//	float4 packedAlbedo = gAlbedoTexture.Sample(basicSampler, pIn.uv);
//	// float3 albedo = packedAlbedo.rgb;
//	float3 albedo = albedoFromTexture;
//	// float3 normal = gNormalTexture.Sample(basicSampler, pIn.uv).rgb; // pIn.normal
//	// float3 normal;  【已定义】
//	// float2 metalRoughness = gOrmTexture.Sample(basicSampler, pIn.uv).gb;
//	// float roughness = metalRoughness.r;
//	// float roughness; 【已定义】 
//	// float metal = metalRoughness.g;
//	// float metal; 【已定义】 
//	float2 AoRo = gOcclusionTexture.Sample(basicSampler, pIn.uv).rg;
//	float3 worldPos = ReconstructWorldPos(pIn.uv, depthBuffer);
//
//
//	//clamp roughness
//	roughness = max(ROUGHNESS_CLAMP, roughness);
//
//	float shadowAmount = 1.f;
//	int i = 0;
//
//	for (i = 0; i < numPointLight; i++)
//	{
//		shadowAmount = 1.f;
//		// 光源随距离的衰减系数
//		float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
//		// 衰减后的光强
//		float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
//		// 光向量（指向光源）
//		float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
//		// 光源的颜色
//		float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;
//
//		// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		// finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//
//
//	// Directional light.
//	for (i = 0; i < dirLightCount; i++)
//	{
//		 float shadowAmount = gShadowTexture.Sample(basicSampler, pIn.uv).r;
//
//		float lightIntensity = dirLight[i].Intensity;
//		float3 toLight = normalize(-dirLight[i].Direction);
//		float3 lightColor = dirLight[i].DiffuseColor.rgb;
//
//		// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		// finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//
//	// Ambient light.
//	float3 viewDir = normalize(cameraPosition - worldPos);
//	float3 prefilter = PrefilteredColor(viewDir, normal, roughness);
//	float2 brdf = BrdfLUT(normal, viewDir, roughness);
//	float3 irradiance = skyIrradianceTexture.Sample(basicSampler, normal).rgb;
//
//	float3 ambientDiffuse = float3(0.0f, 0.0f, 0.0f);
//	float3 ambientSpecular = float3(0.0f, 0.0f, 0.0f);
//
//	AmbientPBR(normalize(normal), worldPos,
//		cameraPosition, roughness, metal, albedo,
//		irradiance, prefilter, brdf, shadowAmount, AoRo,d
//		ambientDiffuse, ambientSpecular);
//
//	 finalColor = finalColor + ambientDiffuse;
//
//
//	PixelOutput o;
//	// o.color = float4(finalColor + ambientSpecular, 1.0f);
//	o.color = float4(finalColor + ambientSpecular, blend_alpha);
//	// o.color = float4(finalColor , blend_alpha);
//	o.ambientSpecular = float4(ambientSpecular, 1.0f);
//
//	// Modified by Ssi:
//	// o.color = (finalColor, blend_alpha);
//	// o.color = float4(0.0f, 0.0f, 1.0f, 0.5f);
//	return o.color;
//	// return float4(1.0f, 1.0f, 1.0f, blend_alpha);
//}



//float4 main1(VertexToPixel pIn) : SV_TARGET
//{
//	// 获取材质数据
//	MaterialData matData = gMaterialData[gMaterialIndex];
//
//	uint diffuseMapIndex = matData.TextureIndex[0];
//	uint normalMapIndex = matData.TextureIndex[1];
//	uint OrmMapIndex = matData.TextureIndex[2];
//
//	float3 albedoFromTexture = gTextureMaps[diffuseMapIndex].Sample(basicSampler, pIn.uv).rgb;
//	float3 normalFromTexture = gTextureMaps[normalMapIndex].Sample(basicSampler, pIn.uv).rgb;
//	float3 ormFromTexture = gTextureMaps[OrmMapIndex].Sample(basicSampler, pIn.uv).rgb;
//	//if (matData.TextureSrgb[0] == 1)
//		//albedoFromTexture = pow(albedoFromTexture, 2.2f);
//	//if (matData.TextureSrgb[1] == 1)
//		//normalFromTexture = pow(normalFromTexture, 2.2f);
//	//if (matData.TextureSrgb[2] == 1)
//		//ormFromTexture = pow(ormFromTexture, 2.2f);
//
//	float4 prevPos = pIn.prevPos;
//	prevPos = prevPos / prevPos.w;
//	prevPos.xy = prevPos.xy / float2(2.0f, -2.0f) + float2(0.5f, 0.5f);//negate Y because world coord and tex coord have different Y axis.
//	float4 curPos = pIn.curPos;
//	curPos = curPos / curPos.w;
//	curPos.xy = curPos.xy / float2(2.0f, -2.0f) + float2(0.5f, 0.5f);//negate Y because world coord and tex coord have different Y axis.
//
//	float3 normal = calculateNormalFromMap(normalFromTexture, normalize(pIn.normal), pIn.tangent);
//	PixelOutput output;
//	output.albedo = float4(albedoFromTexture, 1.0f);;
//	output.normal = float4(normalize(normal), 1.0f);
//	//output.worldPos = float4(pIn.worldPos, 0.0f);
//	output.velocity = float2(curPos.x - prevPos.x, curPos.y - prevPos.y);
//	//output.velocity = float2(prevPos.x, prevPos.y);
//	float roughness = ormFromTexture.g;
//	float metal = ormFromTexture.b;
//	output.occlusionRoughnessMetallic = float4(0, roughness, metal, 0);
//	output.albedo.a = pIn.linearZ;
//
//
//
//
//
//	// LightPass
//	float depthBuffer = gDepthBuffer.Sample(basicSampler, pIn.uv).r;
//	float depth = ViewDepth(depthBuffer);
//	float linearDepth = (depth - NEAR_Z) / (FAR_Z - NEAR_Z);
//
//	if (linearDepth <= 0.0f)
//	{
//		PixelOutput o;
//		o.color = float4(0.0f, 0.0f, 0.0f, 1.0f);
//		o.ambientSpecular = float4(0.0f, 0.0f, 0.0f, 1.0f);
//		// Modified by Ssi:
//		return o.color;
//	}
//
//	// 计算girdId
//	uint gridId = 0;
//#if USE_TBDR // == 0
//	uint offsetX = floor(pIn.uv.x * gRenderTargetSize.x / TILE_SIZE_X);
//	uint offsetY = floor(pIn.uv.y * gRenderTargetSize.y / TILE_SIZE_Y);
//	gridId = offsetY * ceil(gRenderTargetSize.x / TILE_SIZE_X) + offsetX;
//#elif USE_CBDR
//	uint clusterZ = 0;
//	for (clusterZ = 0; ((depth > DepthSlicing_16[clusterZ + 1]) && (clusterZ < CLUSTER_NUM_Z - 1)); clusterZ++)
//	{
//		;
//	}
//	uint offsetX = floor(pIn.uv.x * gRenderTargetSize.x / CLUSTER_SIZE_X);
//	uint offsetY = floor(pIn.uv.y * gRenderTargetSize.y / CLUSTER_SIZE_Y);
//	gridId = (offsetY * ceil(gRenderTargetSize.x / CLUSTER_SIZE_X) + offsetX) * CLUSTER_NUM_Z + clusterZ;
//#endif
//
//	// 获取点光源、聚光灯数量
//	uint numPointLight = gLightList[gridId].NumPointLights;
//	uint numSpotlight = gLightList[gridId].NumSpotlights;
//	if (numPointLight > MAX_GRID_POINT_LIGHT_NUM)
//		numPointLight = MAX_GRID_POINT_LIGHT_NUM;
//	if (numSpotlight > MAX_GRID_SPOTLIGHT_NUM)
//		numSpotlight = MAX_GRID_SPOTLIGHT_NUM;
//
//#if VISUALIZE_GRID_LIGHT_NUM // == 0
//	float lightNum = float(numPointLight + numSpotlight) / 30.0f;
//	{
//		PixelOutput o;
//		o.color = float4(lightNum, lightNum, lightNum, 1.0f);
//		o.ambientSpecular = float4(lightNum, lightNum, lightNum, 1.0f);
//		return o.color;
//	}
//#elif VISUALIZE_CLUSTER_DISTRIBUTION // == 0
//	float clusterColor = float(clusterZ) / CLUSTER_NUM_Z;
//	{
//		PixelOutput o;
//		o.color = float4(clusterColor, clusterColor, clusterColor, 1.0f);
//		o.ambientSpecular = float4(clusterColor, clusterColor, clusterColor, 1.0f);
//		return o.color;
//	}
//#else
//	float3 finalColor = 0.f;
//
//	float4 packedAlbedo = gAlbedoTexture.Sample(basicSampler, pIn.uv);
//	// float3 albedo = packedAlbedo.rgb;
//	float3 albedo = albedoFromTexture;
//	// float3 normal = gNormalTexture.Sample(basicSampler, pIn.uv).rgb; // pIn.normal
//	// float3 normal;  【已定义】
//	// float2 metalRoughness = gOrmTexture.Sample(basicSampler, pIn.uv).gb;
//	// float roughness = metalRoughness.r;
//	// float roughness; 【已定义】 
//	// float metal = metalRoughness.g;
//	// float metal; 【已定义】 
//	float2 AoRo = gOcclusionTexture.Sample(basicSampler, pIn.uv).rg;
//	float3 worldPos = ReconstructWorldPos(pIn.uv, depthBuffer);
//
//
//	//clamp roughness
//	roughness = max(ROUGHNESS_CLAMP, roughness);
//
//	float shadowAmount = 1.f;
//	int i = 0;
//
//#if USE_CBDR
//	// Point light.
//	for (i = 0; i < numPointLight; i++)
//	{
//		//shadowAmount = 1.f;
//		//// 光源随距离的衰减系数
//		//float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
//		//// 衰减后的光强
//		//float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
//		//// 光向量（指向光源）
//		//float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
//		//// 光源的颜色
//		//float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;
//
//		//// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		//// finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		//finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//#elif USE_TBDR // == 0
//	// Point light.
//	for (i = 0; i < (int)numPointLight; i++)
//	{
//		shadowAmount = 1.f;
//		float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
//		float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
//		float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
//		float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;
//
//		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//#else
//	for (i = 0; i < MAX_POINT_LIGHT_NUM; i++)
//	{
//		shadowAmount = 1.f;
//		float atten = Attenuate(pointLight[i].Position, pointLight[i].Range, worldPos);
//		float lightIntensity = pointLight[i].Intensity * atten;
//		float3 toLight = normalize(pointLight[i].Position - worldPos);
//		float3 lightColor = pointLight[i].Color.rgb;
//
//		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//#endif
//
//	// Directional light.
//	for (i = 0; i < dirLightCount; i++)
//	{
//#if DEBUG_CASCADE_RANGE // == 0
//		float shadowAmount = 1.0f;
//#else
//		// float shadowAmount = gShadowTexture.Sample(basicSampler, pIn.uv).r;
//#endif
//		//float lightIntensity = dirLight[i].Intensity;
//		//float3 toLight = normalize(-dirLight[i].Direction);
//		//float3 lightColor = dirLight[i].DiffuseColor.rgb;
//
//		//// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		//// finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		//finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//
//	// Ambient light.
//	//float3 viewDir = normalize(cameraPosition - worldPos);
//	//float3 prefilter = PrefilteredColor(viewDir, normal, roughness);
//	//float2 brdf = BrdfLUT(normal, viewDir, roughness);
//	//float3 irradiance = skyIrradianceTexture.Sample(basicSampler, normal).rgb;
//
//	//float3 ambientDiffuse = float3(0.0f, 0.0f, 0.0f);
//	float3 ambientSpecular = float3(0.0f, 0.0f, 0.0f);
//
//	//AmbientPBR(normalize(normal), worldPos,
//	//	cameraPosition, roughness, metal, albedo,
//	//	irradiance, prefilter, brdf, shadowAmount, AoRo,
//	//	ambientDiffuse, ambientSpecular);
//
//	//finalColor = finalColor + ambientDiffuse;
//
//
//#if DEBUG_CASCADE_RANGE // == 0
//	float testShadow = gShadowTexture.Sample(basicSampler, pIn.uv).r;
//	if (testShadow < 0.1f)
//		finalColor *= float3(1.0f, 0.25f, 0.25f);
//	if (testShadow > 0.4f && testShadow < 0.9f)
//		finalColor *= float3(0.25f, 1.0f, 0.25f);
//	if (testShadow > 0.9f)
//		finalColor *= float3(0.25f, 0.25f, 1.0f);
//#endif
//
//#if DEBUG // DEBUG == 0
//	finalColor = gOcclusionTexture.Sample(basicSampler, pIn.uv).rrr;
//#endif
//
//	PixelOutput o;
//	o.color = float4(finalColor + ambientSpecular, 1.0f);
//	o.ambientSpecular = float4(ambientSpecular, 1.0f);
//
//	// Modified by Ssi:
//	// o.color = (finalColor, blend_alpha);
//	// o.color = float4(0.0f, 0.0f, 1.0f, 0.5f);
//	// return o.color;
//	return float4(1.0f, 1.0f, 1.0f, blend_alpha);
//
//#endif
//}



//// Modified by Ssi:
//float4 main(VertexToPixel pIn) : SV_TARGET
//{
//	float depthBuffer = gDepthBuffer.Sample(basicSampler, pIn.uv).r;
//	float depth = ViewDepth(depthBuffer);
//	float linearDepth = (depth - NEAR_Z) / (FAR_Z - NEAR_Z);
//
//	if (linearDepth <= 0.0f)
//	{
//		PixelOutput o;
//		o.color = float4(0.0f, 0.0f, 0.0f, 1.0f);
//		o.ambientSpecular = float4(0.0f, 0.0f, 0.0f, 1.0f);
//		// Modified by Ssi:
//		return o.color;
//	}
//
//	// 计算girdId
//	uint gridId = 0;
//#if USE_TBDR // == 0
//	uint offsetX = floor(pIn.uv.x * gRenderTargetSize.x / TILE_SIZE_X);
//	uint offsetY = floor(pIn.uv.y * gRenderTargetSize.y / TILE_SIZE_Y);
//	gridId = offsetY * ceil(gRenderTargetSize.x / TILE_SIZE_X) + offsetX;
//#elif USE_CBDR
//	uint clusterZ = 0;
//	for (clusterZ = 0; ((depth > DepthSlicing_16[clusterZ + 1]) && (clusterZ < CLUSTER_NUM_Z - 1)); clusterZ++)
//	{
//		;
//	}
//	uint offsetX = floor(pIn.uv.x * gRenderTargetSize.x / CLUSTER_SIZE_X);
//	uint offsetY = floor(pIn.uv.y * gRenderTargetSize.y / CLUSTER_SIZE_Y);
//	gridId = (offsetY * ceil(gRenderTargetSize.x / CLUSTER_SIZE_X) + offsetX) * CLUSTER_NUM_Z + clusterZ;
//#endif
//
//	// 获取点光源、聚光灯数量
//	uint numPointLight = gLightList[gridId].NumPointLights;
//	uint numSpotlight = gLightList[gridId].NumSpotlights;
//	if (numPointLight > MAX_GRID_POINT_LIGHT_NUM)
//		numPointLight = MAX_GRID_POINT_LIGHT_NUM;
//	if (numSpotlight > MAX_GRID_SPOTLIGHT_NUM)
//		numSpotlight = MAX_GRID_SPOTLIGHT_NUM;
//
//#if VISUALIZE_GRID_LIGHT_NUM // == 0
//	float lightNum = float(numPointLight + numSpotlight) / 30.0f;
//	{
//		PixelOutput o;
//		o.color = float4(lightNum, lightNum, lightNum, 1.0f);
//		o.ambientSpecular = float4(lightNum, lightNum, lightNum, 1.0f);
//		return o.color;
//	}
//#elif VISUALIZE_CLUSTER_DISTRIBUTION // == 0
//	float clusterColor = float(clusterZ) / CLUSTER_NUM_Z;
//	{
//		PixelOutput o;
//		o.color = float4(clusterColor, clusterColor, clusterColor, 1.0f);
//		o.ambientSpecular = float4(clusterColor, clusterColor, clusterColor, 1.0f);
//		return o.color;
//	}
//#else
//	float3 finalColor = 0.f;
//
//	float4 packedAlbedo = gAlbedoTexture.Sample(basicSampler, pIn.uv);
//	float3 albedo = packedAlbedo.rgb;
//	float3 normal = gNormalTexture.Sample(basicSampler, pIn.uv).rgb; // pIn.normal
//	float2 metalRoughness = gOrmTexture.Sample(basicSampler, pIn.uv).gb;
//	float roughness = metalRoughness.r;
//	float metal = metalRoughness.g;
//	float2 AoRo = gOcclusionTexture.Sample(basicSampler, pIn.uv).rg;
//	float3 worldPos = ReconstructWorldPos(pIn.uv, depthBuffer);
//
//	//clamp roughness
//	roughness = max(ROUGHNESS_CLAMP, roughness);
//
//	float shadowAmount = 1.f;
//	int i = 0;
//
//#if USE_CBDR
//	// Point light.
//	for (i = 0; i < numPointLight; i++)
//	{
//		shadowAmount = 1.f;
//		// 光源随距离的衰减系数
//		float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
//		// 衰减后的光强
//		float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
//		// 光向量（指向光源）
//		float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
//		// 光源的颜色
//		float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;
//
//		// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//#elif USE_TBDR // == 0
//	// Point light.
//	for (i = 0; i < (int)numPointLight; i++)
//	{
//		shadowAmount = 1.f;
//		float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
//		float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
//		float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
//		float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;
//
//		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//#else
//	for (i = 0; i < MAX_POINT_LIGHT_NUM; i++)
//	{
//		shadowAmount = 1.f;
//		float atten = Attenuate(pointLight[i].Position, pointLight[i].Range, worldPos);
//		float lightIntensity = pointLight[i].Intensity * atten;
//		float3 toLight = normalize(pointLight[i].Position - worldPos);
//		float3 lightColor = pointLight[i].Color.rgb;
//
//		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//#endif
//
//	// Directional light.
//	for (i = 0; i < dirLightCount; i++)
//	{
//#if DEBUG_CASCADE_RANGE // == 0
//		float shadowAmount = 1.0f;
//#else
//		float shadowAmount = gShadowTexture.Sample(basicSampler, pIn.uv).r;
//#endif
//		float lightIntensity = dirLight[i].Intensity;
//		float3 toLight = normalize(-dirLight[i].Direction);
//		float3 lightColor = dirLight[i].DiffuseColor.rgb;
//
//		// finalColor = finalColor + DirectPBR_diff(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//		finalColor = finalColor + DirectPBR_spec(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
//	}
//
//	// Ambient light.
//	float3 viewDir = normalize(cameraPosition - worldPos);
//	float3 prefilter = PrefilteredColor(viewDir, normal, roughness);
//	float2 brdf = BrdfLUT(normal, viewDir, roughness);
//	float3 irradiance = skyIrradianceTexture.Sample(basicSampler, normal).rgb;
//
//	float3 ambientDiffuse = float3(0.0f, 0.0f, 0.0f);
//	float3 ambientSpecular = float3(0.0f, 0.0f, 0.0f);
//
//	AmbientPBR(normalize(normal), worldPos,
//		cameraPosition, roughness, metal, albedo,
//		irradiance, prefilter, brdf, shadowAmount, AoRo,
//		ambientDiffuse, ambientSpecular);
//
//	// finalColor = finalColor + ambientDiffuse;
//
//
//#if DEBUG_CASCADE_RANGE // == 0
//	float testShadow = gShadowTexture.Sample(basicSampler, pIn.uv).r;
//	if (testShadow < 0.1f)
//		finalColor *= float3(1.0f, 0.25f, 0.25f);
//	if (testShadow > 0.4f && testShadow < 0.9f)
//		finalColor *= float3(0.25f, 1.0f, 0.25f);
//	if (testShadow > 0.9f)
//		finalColor *= float3(0.25f, 0.25f, 1.0f);
//#endif
//
//#if DEBUG // DEBUG == 0
//	finalColor = gOcclusionTexture.Sample(basicSampler, pIn.uv).rrr;
//#endif
//
//	PixelOutput o;
//	o.color = float4(finalColor + ambientSpecular, 1.0f);
//	o.ambientSpecular = float4(ambientSpecular, 1.0f);
//
//	// Modified by Ssi:
//	o.color.a = blend_alpha;
//	// o.color = float4(0.0f, 0.0f, 1.0f, 0.5f);
//	return o.color;
//	// return float4(1.0f, 1.0f, 1.0f, blend_alpha);
//
//#endif
//}

