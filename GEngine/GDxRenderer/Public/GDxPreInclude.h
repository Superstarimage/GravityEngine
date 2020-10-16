#pragma once

#include <windows.h>
#include <comdef.h>
#include <wrl.h>
#include <dxgi1_4.h>
#include <d3d12.h>
#include <D3Dcompiler.h>
#include <DirectXMath.h>
#include <math.h>
#include <DirectXPackedVector.h>
#include <DirectXColors.h>
#include <DirectXCollision.h>
#include <string>
#include <memory>
#include <algorithm>
#include <vector>
#include <array>
#include <unordered_map>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <cassert>
#include "d3dx12.h"
#include "DDSTextureLoader.h"
#include "ResourceUploadBatch.h"
#include <WICTextureLoader.h>
#include <fbxsdk.h>
#include "GRiInclude.h"
#include "GGiInclude.h"
#include "DirectXTex.h"
//#include <wrl/wrappers/corewrappers.h>
//#include <ScreenGrab.h>

//extern const int gNumFrameResources;

#define MaxLights 16

#if defined(_DEBUG)
#ifndef Assert
#define Assert(x, description)                                  \
	{                                                               \
		static bool ignoreAssert = false;                           \
		if(!ignoreAssert && !(x))                                   \
		{                                                           \
			Debug::AssertResult result = Debug::ShowAssertDialog(   \
			(L#x), description, AnsiToWString(__FILE__), __LINE__); \
		if(result == Debug::AssertIgnore)                           \
		{                                                           \
			ignoreAssert = true;                                    \
		}                                                           \
					else if(result == Debug::AssertBreak)           \
		{                                                           \
			__debugbreak();                                         \
		}                                                           \
		}                                                           \
	}
#endif
#else
#ifndef Assert
#define Assert(x, description)
#endif
#endif

inline void d3dSetDebugName(IDXGIObject* obj, const char* name)
{
	if (obj)
	{
		obj->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
	}
}

inline void d3dSetDebugName(ID3D12Device* obj, const char* name)
{
	if (obj)
	{
		obj->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
	}
}

inline void d3dSetDebugName(ID3D12DeviceChild* obj, const char* name)
{
	if (obj)
	{
		obj->SetPrivateData(WKPDID_D3DDebugObjectName, (UINT)strlen(name), name);
	}
}

inline std::wstring AnsiToWString(const std::string& str)
{
	WCHAR buffer[512];
	MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, buffer, 512);
	return std::wstring(buffer);
}

class DxException
{
public:
	DxException() = default;
	DxException(HRESULT hr, const std::wstring& functionName, const std::wstring& filename, int lineNumber) :
		ErrorCode(hr),
		FunctionName(functionName),
		Filename(filename),
		LineNumber(lineNumber)
	{
	}

	std::wstring ToString()const
	{
		// Get the string description of the error code.
		_com_error err(ErrorCode);
		std::wstring msg = err.ErrorMessage();

		return FunctionName + L" failed in " + Filename + L"; line " + std::to_wstring(LineNumber) + L"; error: " + msg;
	}

	HRESULT ErrorCode = S_OK;
	std::wstring FunctionName;
	std::wstring Filename;
	int LineNumber = -1;
};

#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                              \
{                                                                     \
    HRESULT hr__ = (x);                                               \
    std::wstring wfn = AnsiToWString(__FILE__);                       \
    if(FAILED(hr__)) { throw DxException(hr__, L#x, wfn, __LINE__); } \
}
#endif

#ifndef ThrowDxException
#define ThrowDxException(wInfo)                                       \
{                                                                     \
    std::wstring wfn = AnsiToWString(__FILE__);                       \
    throw DxException(0, wInfo, wfn, __LINE__);					      \
}
#endif

#ifndef ReleaseCom
#define ReleaseCom(x) { if(x){ x->Release(); x = 0; } }
#endif

namespace GDx
{
	inline GGiFloat4x4 _GGI_VECTOR_CALL DxToGGiMatrix
	(
		DirectX::XMMATRIX mat
	)
	{
		GGiFloat4x4 ret;
		ret.SetRow(0, mat.r[0]);
		ret.SetRow(1, mat.r[1]);
		ret.SetRow(2, mat.r[2]);
		ret.SetRow(3, mat.r[3]);
		return ret;
	}

	//------------------------------------------------------------------------------

	inline DirectX::XMMATRIX _GGI_VECTOR_CALL GGiToDxMatrix
	(
		GGiFloat4x4 mat
	)
	{
		DirectX::XMMATRIX ret;
		ret.r[0] = mat.GetRow(0);
		ret.r[1] = mat.GetRow(1);
		ret.r[2] = mat.GetRow(2);
		ret.r[3] = mat.GetRow(3);
		return ret;
	}
}

