#pragma once

#include <d3d12.h>
#include <wrl.h>
#include <filesystem>
#include <vector>
#include <string>

#include <glm/glm.hpp>
#include "edgefriend.h"

class EdgefriendDX12 {
public:
    ~EdgefriendDX12();

    void Run();
    bool RunAndCompareWithCpu(float positionEpsilon = 2e-5f);
    void SetIters(int i);

private:
    static constexpr UINT kComputeThreadsPerGroup = 32;
    static constexpr float kDefaultSharpnessFactor = 1.0f;

    struct ConstantBufferCS {
        int F;
        int V;
        float sharpnessFactor;
    };

    enum ComputeRootParameters : UINT32 {
        ComputeRootCBV = 0,
        ComputeRootUAVTable,
        ComputeRootSRVTable,
        ComputeRootParametersCount
    };

    enum DescriptorHeapIndex : UINT32 {
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

    struct GeometryBufferSet {
        Microsoft::WRL::ComPtr<ID3D12Resource> position;
        Microsoft::WRL::ComPtr<ID3D12Resource> index;
        Microsoft::WRL::ComPtr<ID3D12Resource> friendSharpness;
        Microsoft::WRL::ComPtr<ID3D12Resource> valence;
    };

    // --- Configuration ---
    int m_iters = 1;
    std::filesystem::path m_objPath = "spot_quadrangulated.obj";

    // --- Geometry data ---
    Edgefriend::EdgefriendGeometry m_inputGeometry;
    Edgefriend::EdgefriendGeometry m_resultGeometry;

    // --- DX12 core objects ---
    Microsoft::WRL::ComPtr<ID3D12Device>              m_device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature>       m_rootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState>       m_pipelineState;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    m_commandAllocator;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue>        m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList;

    // --- Constant buffer ---
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_constantBufferUpload;

    // --- Transfer heaps ---
    Microsoft::WRL::ComPtr<ID3D12Resource> m_uploadHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_readbackHeap;

    // --- Double-buffered geometry resources (swapped each iteration) ---
    GeometryBufferSet m_buffersIn;
    GeometryBufferSet m_buffersOut;

    // --- Descriptor heap ---
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_srvUavHeap;
    UINT m_srvUavDescriptorSize = 0;

    // --- Synchronization ---
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    UINT64              m_fenceValue = 0;
    HANDLE              m_fenceEvent = nullptr;

    // --- Initialization ---
    void InitDevice();
    void CreateRootSignatureAndPipeline();
    void CreateBuffers();
    void InitFence();

    // --- GPU execution ---
    void UploadInputGeometry();
    void ExecuteSubdivisions();
    void ReadBackResults();

    // --- Resource creation helpers ---
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateDefaultBuffer(UINT64 size, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateUploadBuffer(UINT64 size);
    Microsoft::WRL::ComPtr<ID3D12Resource> CreateReadbackBuffer(UINT64 size);

    // --- View creation helpers ---
    void CreateSrvUavViews();
    void CreateStructuredSrv(ID3D12Resource* resource, UINT count, UINT stride, DescriptorHeapIndex index);
    void CreateRawSrv(ID3D12Resource* resource, UINT wordCount, DescriptorHeapIndex index);
    void CreateStructuredUav(ID3D12Resource* resource, UINT count, UINT stride, DescriptorHeapIndex index);
    void CreateRawUav(ID3D12Resource* resource, UINT wordCount, DescriptorHeapIndex index);

    // --- Command helpers ---
    void BindComputeState();
    void SwapGeometryBuffers();
    void WaitForGpu();
    void ExecuteCommandList();
    void ResetCommandList(ID3D12PipelineState* pso = nullptr);

    // --- Data management ---
    void LoadObj();
    void PreallocateResult(int iterations, const Edgefriend::EdgefriendGeometry& input);
    Edgefriend::EdgefriendGeometry RunCpuSubdivision() const;
};
