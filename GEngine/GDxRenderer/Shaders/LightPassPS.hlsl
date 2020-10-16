
#include "ShaderDefinition.h"
#include "Lighting.hlsli"
#include "MainPassCB.hlsli"


#define DEBUG 0

#define USE_CBDR 1

#if USE_CBDR
#define USE_TBDR 0
#else
#define USE_TBDR 1
#endif

#define VISUALIZE_GRID_LIGHT_NUM 0

#define VISUALIZE_CLUSTER_DISTRIBUTION 0

//#define CLUSTER_SIZE_X 32
//#define CLUSTER_SIZE_Y 16
//#define CLUSTER_NUM_Z 16

//#define TILE_SIZE_X 16
//#define TILE_SIZE_Y 16

#define DEBUG_CASCADE_RANGE 0

StructuredBuffer<LightList> gLightList : register(t0);

//G-Buffer
Texture2D gAlbedoTexture			: register(t1);
Texture2D gNormalTexture			: register(t2);
Texture2D gVelocityTexture			: register(t3);
Texture2D gOrmTexture				: register(t4);

Texture2D gDepthBuffer				: register(t5);

Texture2D gShadowTexture			: register(t6);

Texture2D gOcclusionTexture			: register(t7);

#define PREFILTER_MIP_LEVEL 5

TextureCube skyIrradianceTexture	: register(t8);
Texture2D	brdfLUTTexture			: register(t9);
TextureCube skyPrefilterTexture[PREFILTER_MIP_LEVEL]	: register(t10);

struct VertexToPixel
{
	float4 position		: SV_POSITION;
	float2 uv           : TEXCOORD0;
};

struct PixelOutput
{
	float4	color				: SV_TARGET0;
	float4	ambientSpecular		: SV_TARGET1;
};

SamplerState			basicSampler	: register(s0);
SamplerComparisonState	shadowSampler	: register(s1);

float LinearDepth(float depth)
{
	return (depth * NEAR_Z) / (FAR_Z - depth * (FAR_Z - NEAR_Z));
}

float ViewDepth(float depth)
{
	return (FAR_Z * NEAR_Z) / (FAR_Z - depth * (FAR_Z - NEAR_Z));
}

float3 PrefilteredColor(float3 viewDir, float3 normal, float roughness)
{
	float roughnessLevel = roughness * PREFILTER_MIP_LEVEL;
	int fl = floor(roughnessLevel);
	int cl = ceil(roughnessLevel);
	float3 R = reflect(-viewDir, normal);
	float3 flSample = skyPrefilterTexture[fl].Sample(basicSampler, R).rgb;
	float3 clSample = skyPrefilterTexture[cl].Sample(basicSampler, R).rgb;
	float3 prefilterColor = lerp(flSample, clSample, (roughnessLevel - fl));
	return prefilterColor;
}

float2 BrdfLUT(float3 normal, float3 viewDir, float roughness)
{
	float NdotV = dot(normal, viewDir);
	NdotV = max(NdotV, 0.0f);
	float2 uv = float2(NdotV, roughness);
	uv = clamp(uv, float2(0.0f, 0.0f), float2(0.99f, 0.99f));
	return brdfLUTTexture.Sample(basicSampler, uv).rg;
}

float3 ReconstructWorldPos(float2 uv, float depth)
{
	float ndcX = uv.x * 2 - 1;
	float ndcY = 1 - uv.y * 2;//remember to flip y!!!
	float4 viewPos = mul(float4(ndcX, ndcY, depth, 1.0f), gInvProj);
	viewPos = viewPos / viewPos.w;
	return mul(viewPos, gInvView).xyz;
}

PixelOutput main(VertexToPixel pIn)
{
	float depthBuffer = gDepthBuffer.Sample(basicSampler, pIn.uv).r;
	float depth = ViewDepth(depthBuffer);
	float linearDepth = (depth - NEAR_Z) / (FAR_Z - NEAR_Z);

	if (linearDepth <= 0.0f)
	{
		PixelOutput o;
		o.color = float4(0.0f, 0.0f, 0.0f, 1.0f);
		o.ambientSpecular = float4(0.0f, 0.0f, 0.0f, 1.0f);
		return o;
	}

	uint gridId = 0;
#if USE_TBDR
	uint offsetX = floor(pIn.uv.x * gRenderTargetSize.x / TILE_SIZE_X);
	uint offsetY = floor(pIn.uv.y * gRenderTargetSize.y / TILE_SIZE_Y);
	gridId = offsetY * ceil(gRenderTargetSize.x / TILE_SIZE_X) + offsetX;
#elif USE_CBDR
	uint clusterZ = 0;
	for (clusterZ = 0; ((depth > DepthSlicing_16[clusterZ + 1]) && (clusterZ < CLUSTER_NUM_Z - 1)); clusterZ++)
	{
		;
	}
	uint offsetX = floor(pIn.uv.x * gRenderTargetSize.x / CLUSTER_SIZE_X);
	uint offsetY = floor(pIn.uv.y * gRenderTargetSize.y / CLUSTER_SIZE_Y);
	gridId = (offsetY * ceil(gRenderTargetSize.x / CLUSTER_SIZE_X) + offsetX) * CLUSTER_NUM_Z + clusterZ;
#endif

	uint numPointLight = gLightList[gridId].NumPointLights;
	uint numSpotlight = gLightList[gridId].NumSpotlights;
	if (numPointLight > MAX_GRID_POINT_LIGHT_NUM)
		numPointLight = MAX_GRID_POINT_LIGHT_NUM;
	if (numSpotlight > MAX_GRID_SPOTLIGHT_NUM)
		numSpotlight = MAX_GRID_SPOTLIGHT_NUM;

#if VISUALIZE_GRID_LIGHT_NUM
	float lightNum = float(numPointLight + numSpotlight) / 30.0f;
	{
		PixelOutput o;
		o.color = float4(lightNum, lightNum, lightNum, 1.0f);
		o.ambientSpecular = float4(lightNum, lightNum, lightNum, 1.0f);
		return o;
	}
#elif VISUALIZE_CLUSTER_DISTRIBUTION
	float clusterColor = float(clusterZ) / CLUSTER_NUM_Z;
	{
		PixelOutput o;
		o.color = float4(clusterColor, clusterColor, clusterColor, 1.0f);
		o.ambientSpecular = float4(clusterColor, clusterColor, clusterColor, 1.0f);
		return o;
	}
#else
	float3 finalColor = 0.f;

	float4 packedAlbedo = gAlbedoTexture.Sample(basicSampler, pIn.uv);
	float3 albedo = packedAlbedo.rgb;
	float3 normal = gNormalTexture.Sample(basicSampler, pIn.uv).rgb;
	float2 metalRoughness = gOrmTexture.Sample(basicSampler, pIn.uv).gb;
	float roughness = metalRoughness.r;
	float metal = metalRoughness.g;
	float2 AoRo = gOcclusionTexture.Sample(basicSampler, pIn.uv).rg;
	float3 worldPos = ReconstructWorldPos(pIn.uv, depthBuffer);

	//clamp roughness
	roughness = max(ROUGHNESS_CLAMP, roughness);

	float shadowAmount = 1.f;
	int i = 0;

#if USE_CBDR
	// Point light.
	for (i = 0; i < numPointLight; i++)
	{
		shadowAmount = 1.f;
		float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
		float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
		float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
		float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;

		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
	}
#elif USE_TBDR
	// Point light.
	for (i = 0; i < (int)numPointLight; i++)
	{
		shadowAmount = 1.f;
		float atten = Attenuate(pointLight[gLightList[gridId].PointLightIndices[i]].Position, pointLight[gLightList[gridId].PointLightIndices[i]].Range, worldPos);
		float lightIntensity = pointLight[gLightList[gridId].PointLightIndices[i]].Intensity * atten;
		float3 toLight = normalize(pointLight[gLightList[gridId].PointLightIndices[i]].Position - worldPos);
		float3 lightColor = pointLight[gLightList[gridId].PointLightIndices[i]].Color.rgb;

		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
	}
#else
	for (i = 0; i < MAX_POINT_LIGHT_NUM; i++)
	{
		shadowAmount = 1.f;
		float atten = Attenuate(pointLight[i].Position, pointLight[i].Range, worldPos);
		float lightIntensity = pointLight[i].Intensity * atten;
		float3 toLight = normalize(pointLight[i].Position - worldPos);
		float3 lightColor = pointLight[i].Color.rgb;

		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
	}
#endif

	// Directional light.
	for (i = 0; i < dirLightCount; i++)
	{
#if DEBUG_CASCADE_RANGE
		float shadowAmount = 1.0f;
#else
		float shadowAmount = gShadowTexture.Sample(basicSampler, pIn.uv).r;
#endif
		float lightIntensity = dirLight[i].Intensity;
		float3 toLight = normalize(-dirLight[i].Direction);
		float3 lightColor = dirLight[i].DiffuseColor.rgb;

		finalColor = finalColor + DirectPBR(lightIntensity, lightColor, toLight, normalize(normal), worldPos, cameraPosition, roughness, metal, albedo, shadowAmount);
	}

	// Ambient light.
	float3 viewDir = normalize(cameraPosition - worldPos);
	float3 prefilter = PrefilteredColor(viewDir, normal, roughness);
	float2 brdf = BrdfLUT(normal, viewDir, roughness);
	float3 irradiance = skyIrradianceTexture.Sample(basicSampler, normal).rgb;

	float3 ambientDiffuse = float3(0.0f, 0.0f, 0.0f);
	float3 ambientSpecular = float3(0.0f, 0.0f, 0.0f);

	AmbientPBR(normalize(normal), worldPos,
		cameraPosition, roughness, metal, albedo,
		irradiance, prefilter, brdf, shadowAmount, AoRo,
		ambientDiffuse, ambientSpecular);

	finalColor = finalColor + ambientDiffuse;


#if DEBUG_CASCADE_RANGE
	float testShadow = gShadowTexture.Sample(basicSampler, pIn.uv).r;
	if (testShadow < 0.1f)
		finalColor *= float3(1.0f, 0.25f, 0.25f);
	if (testShadow > 0.4f && testShadow < 0.9f)
		finalColor *= float3(0.25f, 1.0f, 0.25f);
	if (testShadow > 0.9f)
		finalColor *= float3(0.25f, 0.25f, 1.0f);
#endif

#if DEBUG
	finalColor = gOcclusionTexture.Sample(basicSampler, pIn.uv).rrr;
#endif

	PixelOutput o;
	o.color = float4(finalColor + ambientSpecular, 1.0f);
	o.ambientSpecular = float4(ambientSpecular, 1.0f);

	return o;
#endif
}

