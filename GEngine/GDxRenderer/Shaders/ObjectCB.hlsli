
#ifndef _OBJECTCB_HLSLI
#define _OBJECTCB_HLSLI



// Constant data that varies per frame.

cbuffer cbPerObject : register(b0)
{
	float4x4 gWorld;
	float4x4 gPrevWorld;
	float4x4 gInvTransWorld;
	float4x4 gTexTransform;
	float blend_alpha; // blend技术实现透明的alpha值；手动修改之来调节透明物体的透明度
	//uint gMaterialIndex;
	//uint gObjPad0;
	//uint gObjPad1;
	//uint gObjPad2;
};

#endif 