#include "dx.h"
#include "obj_io.h"
#include "DXSampleHelper.h"

#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <d3dx12.h>
#include <glm/gtx/hash.hpp>

#include <stdexcept>
#include <algorithm>
#include <limits>
#include <fstream>
#include <sstream>
#include <iostream>

using Microsoft::WRL::ComPtr;

namespace {

struct BufferLayout {
    UINT64 positionBytes  = 0;
    UINT64 indexBytes     = 0;
    UINT64 sharpnessBytes = 0;
    UINT64 valenceBytes   = 0;

    UINT64 indexOffset     = 0;
    UINT64 sharpnessOffset = 0;
    UINT64 valenceOffset   = 0;
    UINT64 totalBytes      = 0;
};

UINT64 Align256(UINT64 size) {
    return (size + 255ull) & ~255ull;
}

UINT64 ByteSize(std::size_t count, std::size_t elementSize) {
    return static_cast<UINT64>(count) * static_cast<UINT64>(elementSize);
}

BufferLayout BuildBufferLayout(const Edgefriend::EdgefriendGeometry& geometry) {
    BufferLayout layout;
    layout.positionBytes  = ByteSize(geometry.positions.size(), sizeof(glm::vec3));
    layout.indexBytes     = ByteSize(geometry.indices.size(), sizeof(int));
    layout.sharpnessBytes = ByteSize(geometry.friendsAndSharpnesses.size(), sizeof(glm::uvec4));
    layout.valenceBytes   = ByteSize(geometry.valenceStartInfos.size(), sizeof(int));

    layout.indexOffset     = layout.positionBytes;
    layout.sharpnessOffset = layout.indexOffset + layout.indexBytes;
    layout.valenceOffset   = layout.sharpnessOffset + layout.sharpnessBytes;
    layout.totalBytes      = layout.valenceOffset + layout.valenceBytes;
    return layout;
}

UINT ToUintChecked(UINT64 value, const char* label) {
    if (value > static_cast<UINT64>(std::numeric_limits<UINT>::max())) {
        throw std::runtime_error(std::string(label) + " exceeds UINT range.");
    }
    return static_cast<UINT>(value);
}

UINT ComputeDispatchGroupCount(int vertexCount, int faceCount, UINT threadsPerGroup) {
    if (vertexCount < 0 || faceCount < 0) {
        throw std::runtime_error("Negative dispatch dimensions.");
    }
    const UINT64 threads = static_cast<UINT64>(std::max(vertexCount, faceCount));
    const UINT64 groups  = (threads + threadsPerGroup - 1u) / threadsPerGroup;
    return ToUintChecked(groups, "Dispatch group count");
}

} // anonymous namespace

// ============================================================================
// Lifecycle
// ============================================================================

EdgefriendDX12::~EdgefriendDX12() {
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

void EdgefriendDX12::SetIters(int i) {
    if (i <= 0) throw std::invalid_argument("iters must be > 0.");
    m_iters = i;
}

// ============================================================================
// Public entry points
// ============================================================================

void EdgefriendDX12::Run() {
    LoadObj();
    PreallocateResult(m_iters, m_inputGeometry);

    InitDevice();
    CreateRootSignatureAndPipeline();
    CreateBuffers();
    InitFence();

    ExecuteSubdivisions();
    ReadBackResults();

    const auto outputPath = "output_" + std::to_string(m_iters) + "iter.obj";
    ObjIO::WriteGeometry(outputPath, m_resultGeometry);
}

bool EdgefriendDX12::RunAndCompareWithCpu(float positionEpsilon) {
    if (positionEpsilon <= 0.0f) {
        throw std::invalid_argument("positionEpsilon must be > 0.");
    }

    Run();

    const auto dx12Path = "output_" + std::to_string(m_iters) + "iter.obj";
    const auto cpuPath  = "output_cpp_" + std::to_string(m_iters) + "iter.obj";

    const auto cpuResult = RunCpuSubdivision();
    ObjIO::WriteGeometry(cpuPath, cpuResult);

    const bool match = ObjIO::CompareFiles(dx12Path, cpuPath, positionEpsilon);
    std::cout << "[Check] Compared: " << dx12Path << " vs " << cpuPath
              << " (epsilon=" << positionEpsilon << ")\n"
              << (match ? "[Check] DX12 and C++ outputs are consistent.\n"
                        : "[Check] DX12 and C++ outputs differ.\n");
    return match;
}

// ============================================================================
// Data loading & preparation
// ============================================================================

void EdgefriendDX12::LoadObj() {
    auto raw = ObjIO::LoadRawMesh(m_objPath);
    m_inputGeometry = Edgefriend::SubdivideToEdgefriendGeometry(
        std::move(raw.positions), std::move(raw.indices),
        std::move(raw.indicesOffsets), std::move(raw.creases));
}

void EdgefriendDX12::PreallocateResult(int iterations, const Edgefriend::EdgefriendGeometry& input) {
    if (iterations <= 0) {
        m_resultGeometry = input;
        return;
    }

    auto geom = input;
    for (int i = 0; i < iterations; ++i) {
        Edgefriend::EdgefriendGeometry next;
        next.positions.resize(geom.positions.size() + 3 * geom.valenceStartInfos.size());
        next.indices.resize(geom.indices.size() * 4);
        next.friendsAndSharpnesses.resize(geom.indices.size());
        next.valenceStartInfos.resize(next.positions.size());
        geom = std::move(next);
    }
    m_resultGeometry = std::move(geom);
}

Edgefriend::EdgefriendGeometry EdgefriendDX12::RunCpuSubdivision() const {
    auto cpuGeometry = m_inputGeometry;
    for (int i = 0; i < m_iters; ++i) {
        cpuGeometry = Edgefriend::SubdivideEdgefriendGeometry(cpuGeometry);
    }
    return cpuGeometry;
}

// ============================================================================
// DX12 initialization
// ============================================================================

void EdgefriendDX12::InitDevice() {
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    {
        ComPtr<ID3D12Debug6> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
            debugController->EnableDebugLayer();
            debugController->SetEnableGPUBasedValidation(true);
            debugController->SetEnableSynchronizedCommandQueueValidation(true);
            debugController->SetEnableAutoName(true);
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        adapters.push_back(adapter);
    }
    if (adapters.empty()) {
        throw std::runtime_error("No DXGI adapters found.");
    }

    std::cout << "Select a device:\n";
    for (size_t i = 0; i < adapters.size(); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapters[i]->GetDesc1(&desc);
        std::wcout << i << ": " << desc.Description << "\n";
    }

    std::cout << "Enter device number: ";
    size_t choice;
    std::cin >> choice;
    if (!std::cin || choice >= adapters.size()) {
        throw std::runtime_error("Invalid adapter selection.");
    }

    ThrowIfFailed(D3D12CreateDevice(adapters[choice].Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)));

    DXGI_ADAPTER_DESC1 desc;
    ThrowIfFailed(adapters[choice]->GetDesc1(&desc));
    std::wcout << L"Using: " << desc.Description << L"\n";
    std::cout << "VRAM: " << desc.DedicatedVideoMemory / (1024 * 1024) << " MB\n";

    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    NAME_D3D12_OBJECT(m_commandQueue);

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.NumDescriptors = DescriptorCount;
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvUavHeap)));
    NAME_D3D12_OBJECT(m_srvUavHeap);
    m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));
}

void EdgefriendDX12::CreateRootSignatureAndPipeline() {
    CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE);
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE);

    CD3DX12_ROOT_PARAMETER1 rootParams[ComputeRootParametersCount];
    rootParams[ComputeRootCBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
    rootParams[ComputeRootSRVTable].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);
    rootParams[ComputeRootUAVTable].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc;
    rootSigDesc.Init_1_1(_countof(rootParams), rootParams, 0, nullptr);

    ComPtr<ID3DBlob> signature, error;
    ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
    ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
    NAME_D3D12_OBJECT(m_rootSignature);

    UINT compileFlags = 0;
#if defined(_DEBUG)
    compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    std::ifstream shaderFile("hlsl/edgefriend.hlsl");
    if (!shaderFile.is_open()) {
        throw std::runtime_error("Failed to open hlsl/edgefriend.hlsl");
    }
    std::stringstream ss;
    ss << shaderFile.rdbuf();
    std::string shaderCode = ss.str();

    ComPtr<ID3DBlob> computeShader;
    ThrowIfFailed(D3DCompile(shaderCode.data(), shaderCode.size(),
        nullptr, nullptr, nullptr, "CSEdgefriend", "cs_5_1", compileFlags, 0, &computeShader, nullptr));

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());
    psoDesc.pRootSignature = m_rootSignature.Get();
    ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
    NAME_D3D12_OBJECT(m_pipelineState);
}

void EdgefriendDX12::InitFence() {
    ThrowIfFailed(m_device->CreateFence(m_fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    m_fenceValue++;
    m_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) {
        ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
    }
}

// ============================================================================
// Resource creation helpers
// ============================================================================

ComPtr<ID3D12Resource> EdgefriendDX12::CreateDefaultBuffer(UINT64 size, D3D12_RESOURCE_FLAGS flags) {
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(Align256(size));
    desc.Flags = flags;
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

ComPtr<ID3D12Resource> EdgefriendDX12::CreateUploadBuffer(UINT64 size) {
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(Align256(size));
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

ComPtr<ID3D12Resource> EdgefriendDX12::CreateReadbackBuffer(UINT64 size) {
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
    auto desc = CD3DX12_RESOURCE_DESC::Buffer(Align256(size));
    ComPtr<ID3D12Resource> resource;
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&resource)));
    return resource;
}

void EdgefriendDX12::CreateBuffers() {
    const auto layout = BuildBufferLayout(m_resultGeometry);
    constexpr auto kUavFlag = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    m_constantBuffer       = CreateDefaultBuffer(sizeof(ConstantBufferCS));
    m_constantBufferUpload = CreateUploadBuffer(sizeof(ConstantBufferCS));
    NAME_D3D12_OBJECT(m_constantBuffer);
    NAME_D3D12_OBJECT(m_constantBufferUpload);

    m_buffersOut.position       = CreateDefaultBuffer(layout.positionBytes, kUavFlag);
    m_buffersOut.index          = CreateDefaultBuffer(layout.indexBytes, kUavFlag);
    m_buffersOut.friendSharpness = CreateDefaultBuffer(layout.sharpnessBytes, kUavFlag);
    m_buffersOut.valence        = CreateDefaultBuffer(layout.valenceBytes, kUavFlag);
    NAME_D3D12_OBJECT(m_buffersOut.position);
    NAME_D3D12_OBJECT(m_buffersOut.index);
    NAME_D3D12_OBJECT(m_buffersOut.friendSharpness);
    NAME_D3D12_OBJECT(m_buffersOut.valence);

    m_uploadHeap = CreateUploadBuffer(layout.totalBytes);
    NAME_D3D12_OBJECT(m_uploadHeap);

    m_buffersIn.position       = CreateDefaultBuffer(layout.positionBytes, kUavFlag);
    m_buffersIn.index          = CreateDefaultBuffer(layout.indexBytes, kUavFlag);
    m_buffersIn.friendSharpness = CreateDefaultBuffer(layout.sharpnessBytes, kUavFlag);
    m_buffersIn.valence        = CreateDefaultBuffer(layout.valenceBytes, kUavFlag);
    NAME_D3D12_OBJECT(m_buffersIn.position);
    NAME_D3D12_OBJECT(m_buffersIn.index);
    NAME_D3D12_OBJECT(m_buffersIn.friendSharpness);
    NAME_D3D12_OBJECT(m_buffersIn.valence);

    CreateSrvUavViews();

    m_readbackHeap = CreateReadbackBuffer(layout.totalBytes);
    NAME_D3D12_OBJECT(m_readbackHeap);
}

// ============================================================================
// View creation
// ============================================================================

void EdgefriendDX12::CreateStructuredSrv(ID3D12Resource* resource, UINT count, UINT stride, DescriptorHeapIndex index) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Format                  = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    desc.Buffer.NumElements      = count;
    desc.Buffer.StructureByteStride = stride;
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), index, m_srvUavDescriptorSize);
    m_device->CreateShaderResourceView(resource, &desc, handle);
}

void EdgefriendDX12::CreateRawSrv(ID3D12Resource* resource, UINT wordCount, DescriptorHeapIndex index) {
    D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension           = D3D12_SRV_DIMENSION_BUFFER;
    desc.Buffer.NumElements      = wordCount;
    desc.Buffer.Flags            = D3D12_BUFFER_SRV_FLAG_RAW;
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), index, m_srvUavDescriptorSize);
    m_device->CreateShaderResourceView(resource, &desc, handle);
}

void EdgefriendDX12::CreateStructuredUav(ID3D12Resource* resource, UINT count, UINT stride, DescriptorHeapIndex index) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format                  = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension           = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements      = count;
    desc.Buffer.StructureByteStride = stride;
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), index, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void EdgefriendDX12::CreateRawUav(ID3D12Resource* resource, UINT wordCount, DescriptorHeapIndex index) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc = {};
    desc.Format                  = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension           = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.NumElements      = wordCount;
    desc.Buffer.Flags            = D3D12_BUFFER_UAV_FLAG_RAW;
    CD3DX12_CPU_DESCRIPTOR_HANDLE handle(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), index, m_srvUavDescriptorSize);
    m_device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void EdgefriendDX12::CreateSrvUavViews() {
    const auto layout     = BuildBufferLayout(m_resultGeometry);
    const UINT posCount   = ToUintChecked(m_resultGeometry.positions.size(), "Position count");
    const UINT idxCount   = ToUintChecked(m_resultGeometry.indices.size(), "Index count");
    const UINT sharpWords = ToUintChecked(layout.sharpnessBytes / sizeof(UINT32), "Sharpness words");
    const UINT valCount   = ToUintChecked(m_resultGeometry.valenceStartInfos.size(), "Valence count");

    CreateStructuredSrv(m_buffersIn.position.Get(),       posCount,   sizeof(glm::vec3), SrvPosIn);
    CreateRawSrv       (m_buffersIn.index.Get(),           idxCount,                      SrvIndexIn);
    CreateRawSrv       (m_buffersIn.friendSharpness.Get(), sharpWords,                    SrvFriendIn);
    CreateStructuredSrv(m_buffersIn.valence.Get(),         valCount,   sizeof(int),       SrvValenceIn);

    CreateStructuredUav(m_buffersOut.position.Get(),       posCount,   sizeof(glm::vec3), UavPosOut);
    CreateRawUav       (m_buffersOut.index.Get(),           idxCount,                      UavIndexOut);
    CreateRawUav       (m_buffersOut.friendSharpness.Get(), sharpWords,                    UavFriendOut);
    CreateStructuredUav(m_buffersOut.valence.Get(),         valCount,   sizeof(int),       UavValenceOut);
}

// ============================================================================
// Command helpers
// ============================================================================

void EdgefriendDX12::BindComputeState() {
    m_commandList->SetComputeRootSignature(m_rootSignature.Get());

    ID3D12DescriptorHeap* heaps[] = { m_srvUavHeap.Get() };
    m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), SrvPosIn, m_srvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), UavPosOut, m_srvUavDescriptorSize);

    m_commandList->SetComputeRootConstantBufferView(ComputeRootCBV, m_constantBuffer->GetGPUVirtualAddress());
    m_commandList->SetComputeRootDescriptorTable(ComputeRootSRVTable, srvHandle);
    m_commandList->SetComputeRootDescriptorTable(ComputeRootUAVTable, uavHandle);
}

void EdgefriendDX12::SwapGeometryBuffers() {
    std::swap(m_buffersIn, m_buffersOut);
}

void EdgefriendDX12::WaitForGpu() {
    if (!m_fenceEvent) {
        throw std::runtime_error("Fence event not initialized.");
    }
    ThrowIfFailed(m_commandQueue->Signal(m_fence.Get(), m_fenceValue));
    ThrowIfFailed(m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent));
    m_fenceValue++;
    WaitForSingleObject(m_fenceEvent, INFINITE);
}

void EdgefriendDX12::ExecuteCommandList() {
    ThrowIfFailed(m_commandList->Close());
    ID3D12CommandList* lists[] = { m_commandList.Get() };
    m_commandQueue->ExecuteCommandLists(1, lists);
}

void EdgefriendDX12::ResetCommandList(ID3D12PipelineState* pso) {
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_commandList->Reset(m_commandAllocator.Get(), pso));
}

// ============================================================================
// GPU execution
// ============================================================================

void EdgefriendDX12::UploadInputGeometry() {
    const auto layout = BuildBufferLayout(m_inputGeometry);

    UINT8* mapped;
    ThrowIfFailed(m_uploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
    memcpy(mapped,                          m_inputGeometry.positions.data(),              static_cast<std::size_t>(layout.positionBytes));
    memcpy(mapped + layout.indexOffset,     m_inputGeometry.indices.data(),                static_cast<std::size_t>(layout.indexBytes));
    memcpy(mapped + layout.sharpnessOffset, m_inputGeometry.friendsAndSharpnesses.data(),  static_cast<std::size_t>(layout.sharpnessBytes));
    memcpy(mapped + layout.valenceOffset,   m_inputGeometry.valenceStartInfos.data(),      static_cast<std::size_t>(layout.valenceBytes));
    m_uploadHeap->Unmap(0, nullptr);

    m_commandList->CopyBufferRegion(m_buffersIn.position.Get(),       0, m_uploadHeap.Get(), 0,                      layout.positionBytes);
    m_commandList->CopyBufferRegion(m_buffersIn.index.Get(),          0, m_uploadHeap.Get(), layout.indexOffset,      layout.indexBytes);
    m_commandList->CopyBufferRegion(m_buffersIn.friendSharpness.Get(), 0, m_uploadHeap.Get(), layout.sharpnessOffset, layout.sharpnessBytes);
    m_commandList->CopyBufferRegion(m_buffersIn.valence.Get(),        0, m_uploadHeap.Get(), layout.valenceOffset,    layout.valenceBytes);
}

void EdgefriendDX12::ExecuteSubdivisions() {
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_commandList)));
    BindComputeState();
    UploadInputGeometry();

    ConstantBufferCS* cb = nullptr;
    ThrowIfFailed(m_constantBufferUpload->Map(0, nullptr, reinterpret_cast<void**>(&cb)));
    cb->F = static_cast<int>(m_inputGeometry.friendsAndSharpnesses.size());
    cb->V = static_cast<int>(m_inputGeometry.positions.size());
    cb->sharpnessFactor = kDefaultSharpnessFactor;

    CD3DX12_RESOURCE_BARRIER initBarriers[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(m_buffersIn.position.Get(),       D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_buffersIn.index.Get(),          D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_buffersIn.friendSharpness.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
        CD3DX12_RESOURCE_BARRIER::Transition(m_buffersIn.valence.Get(),        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON),
    };
    m_commandList->ResourceBarrier(_countof(initBarriers), initBarriers);

    for (int i = 0; i < m_iters; ++i) {
        m_commandList->CopyBufferRegion(m_constantBuffer.Get(), 0, m_constantBufferUpload.Get(), 0, sizeof(ConstantBufferCS));

        auto cbBarrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_constantBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        m_commandList->ResourceBarrier(1, &cbBarrier);

        const UINT groups = ComputeDispatchGroupCount(cb->V, cb->F, kComputeThreadsPerGroup);
        m_commandList->Dispatch(groups, 1, 1);

        ExecuteCommandList();
        WaitForGpu();

        SwapGeometryBuffers();
        CreateSrvUavViews();

        ResetCommandList(m_pipelineState.Get());
        BindComputeState();

        cb->F *= 4;
        cb->V *= 4;
    }

    SwapGeometryBuffers();
    m_constantBufferUpload->Unmap(0, nullptr);
    CreateSrvUavViews();
    ThrowIfFailed(m_commandList->Close());
}

void EdgefriendDX12::ReadBackResults() {
    const auto layout = BuildBufferLayout(m_resultGeometry);

    ResetCommandList();

    m_commandList->CopyBufferRegion(m_readbackHeap.Get(), 0,                      m_buffersOut.position.Get(),       0, layout.positionBytes);
    m_commandList->CopyBufferRegion(m_readbackHeap.Get(), layout.indexOffset,      m_buffersOut.index.Get(),          0, layout.indexBytes);
    m_commandList->CopyBufferRegion(m_readbackHeap.Get(), layout.sharpnessOffset,  m_buffersOut.friendSharpness.Get(), 0, layout.sharpnessBytes);
    m_commandList->CopyBufferRegion(m_readbackHeap.Get(), layout.valenceOffset,    m_buffersOut.valence.Get(),        0, layout.valenceBytes);

    ExecuteCommandList();
    WaitForGpu();

    void* mapped;
    ThrowIfFailed(m_readbackHeap->Map(0, nullptr, &mapped));
    auto* bytes = reinterpret_cast<UINT8*>(mapped);
    memcpy(m_resultGeometry.positions.data(),             bytes,                          static_cast<std::size_t>(layout.positionBytes));
    memcpy(m_resultGeometry.indices.data(),               bytes + layout.indexOffset,     static_cast<std::size_t>(layout.indexBytes));
    memcpy(m_resultGeometry.friendsAndSharpnesses.data(), bytes + layout.sharpnessOffset, static_cast<std::size_t>(layout.sharpnessBytes));
    memcpy(m_resultGeometry.valenceStartInfos.data(),     bytes + layout.valenceOffset,   static_cast<std::size_t>(layout.valenceBytes));
    m_readbackHeap->Unmap(0, nullptr);
}
