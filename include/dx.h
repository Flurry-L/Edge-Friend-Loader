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
    void Init();
    void CreateResources();

    void Dispatch(int faceCount, int vertexCount, float sharpness);

private:
    ID3D12Device* m_device;
    ID3D12GraphicsCommandList* m_cmdList;
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

    // Methods to create and initialize the resources
    void CreateDevice();
    void CreateRootSignature();
    void CreateComputePipelineStateObject();
    void CreateComputeCommands();
    void UpdateConstantBuffer(int faceCount, int vertexCount, float sharpness);
};

// Constructor
//EdgefriendDX12::EdgefriendDX12(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList)
//    : m_device(device), m_cmdList(cmdList) {
//    CreateResources();
//    CreateRootSignature();
//    CreatePipelineState();
//}

// Method implementations would go here...
