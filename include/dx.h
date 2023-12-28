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


using Microsoft::WRL::ComPtr;


class EdgefriendDX12 {
public:

    struct OldSizeConstants
    {
        int F; // face count
        int V; // vertex count
        float sharpnessFactor;
        float padding; // Padding to ensure the buffer size is a multiple of 16 bytes
    };

    struct float3 {
        float x, y, z;
    };
    
    void Init();
    

    void Dispatch(int faceCount, int vertexCount, float sharpness);

private:
    static const UINT ThreadCount = 1;

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
        UavIndexOut = UavPosOut + ThreadCount,
        UavFriendOut = UavIndexOut + ThreadCount,
        UavValenceOut = UavFriendOut + ThreadCount,
        SrvPosIn = UavValenceOut + ThreadCount,
        SrvIndexIn = SrvPosIn + ThreadCount,
        SrvFriendIn = SrvIndexIn + ThreadCount,
        SrvValenceIn = SrvFriendIn + ThreadCount,
        DescriptorCount = SrvValenceIn + ThreadCount
    };

    ID3D12Device* m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_pipelineState;
    ComPtr<ID3D12CommandAllocator> m_commandAllocator;
    ComPtr<ID3D12CommandQueue> m_computeCommandQueue;
    ComPtr<ID3D12GraphicsCommandList> m_computeCommandList;

    // Resources
    ComPtr<ID3D12Resource> m_constantBuffer;
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

    // Methods to create and initialize the resources
    void LoadPipeline();
    void LoadAssets();


};

// Constructor
//EdgefriendDX12::EdgefriendDX12(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
//    : m_device(device), m_cmdList(cmdList) {
//    CreateResources();
//    CreateRootSignature();
//    CreatePipelineState();
//}

// Method implementations would go here...
