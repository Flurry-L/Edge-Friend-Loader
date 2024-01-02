#include <d3d12.h>
#include <wrl.h>
#include <dxgi.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <iostream>
#include <vector>
#include <d3dx12.h>
#include <fstream>
#include <string>
#include <sstream>
#include <DirectXMath.h>

#include "rapidobj.hpp"
#include <span>
#include <glm/glm.hpp>
#include "unordered_dense.h"

#include "edgefriend.h"

using Microsoft::WRL::ComPtr;
using namespace DirectX;

class EdgefriendDX12 {
public:

    struct ConstantBufferCS
    {
        int F; // face count
        int V; // vertex count
        float sharpnessFactor; 
    };

    void OnInit();
    

    void Dispatch(int faceCount, int vertexCount, float sharpness);

private:
    std::filesystem::path file = "C:/Users/17480/Desktop/test/spot_quadrangulated.obj";

    Edgefriend::EdgefriendGeometry orig_geometry;
    Edgefriend::EdgefriendGeometry new_geometry;

    struct oldSize {
        int F;
        int V;
        float sharpnessFactor;
    };

    struct newSize {
        int F;
        int V;
        float sharpnessFactor;
    };

    enum ComputeRootParameters : UINT32
    {
        ComputeRootCBV = 0,
        ComputeRootSRVTable,
        ComputeRootUAVTable,
        ComputeRootParametersCount
    };

    // Indices of shader resources in the descriptor heap.
    enum DescriptorHeapIndex : UINT32
    {
        UavPosOut = 0,
        UavIndexOut,
        UavFriendOut,
        UavValenceOut,
        SrvPosIn,
        SrvIndexIn,
        SrvFriendIn,
        SrvValenceIn,
        DescriptorCount
    };

    ID3D12Device* m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandQueue> m_commandQueue;
    ComPtr<ID3D12GraphicsCommandList> m_computeCommandList;

    // Resources
    ComPtr<ID3D12Resource> m_constantBufferCS;

    ComPtr<ID3D12Resource> m_uploadHeap;
    ComPtr<ID3D12Resource> m_readbackHeap;

    ComPtr<ID3D12Resource> m_positionBufferIn;
    ComPtr<ID3D12Resource> m_indexBufferIn;
    ComPtr<ID3D12Resource> m_friendAndSharpnessBufferIn;
    ComPtr<ID3D12Resource> m_valenceStartInfoBufferIn;

    ComPtr<ID3D12Resource> m_positionBufferOut;
    ComPtr<ID3D12Resource> m_indexBufferOut;
    ComPtr<ID3D12Resource> m_friendAndSharpnessBufferOut;
    ComPtr<ID3D12Resource> m_valenceStartInfoBufferOut;

    ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
    UINT m_srvUavDescriptorSize;

    ComPtr<ID3D12Fence> m_renderContextFence;
    UINT64 m_renderContextFenceValue;
    HANDLE m_renderContextFenceEvent;

    ComPtr<ID3D12Fence> m_threadFences;
    volatile HANDLE m_threadFenceEvents;

    

    // Methods to create and initialize the resources
    void LoadPipeline();
    void LoadAssets();


    void CreateBuffers();
    void WaitForRenderContext();
    void ReadBack();

    void LoadObj();
    void WriteObj();
    void PreProcess(std::vector<glm::vec3> oldPositions,
        std::vector<int> oldIndices,
        std::vector<int> oldIndicesOffsets,
        ankerl::unordered_dense::map<glm::ivec2, float> oldCreases);
};