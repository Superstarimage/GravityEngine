#include "stdafx.h"
#include "GDxVertexIndexBuffer.h"


GDxVertexIndexBuffer::GDxVertexIndexBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, std::vector<GRiVertex> vertices, std::vector<uint32_t> indices)
{
	VertexByteStride = sizeof(GRiVertex);
	IndexFormat = DXGI_FORMAT_R32_UINT;
	VertexBufferByteSize = (UINT)vertices.size() * sizeof(GRiVertex);
	IndexBufferByteSize = (UINT)indices.size() * sizeof(std::uint32_t);
	VertexCount = (UINT)vertices.size();
	IndexCount = (UINT)indices.size();
}


