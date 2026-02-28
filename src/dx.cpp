#include "dx.h"
#include "DXSampleHelper.h"
#include <glm/gtx/hash.hpp>
#include <stdexcept>
#include <algorithm>
#include <limits>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace {
constexpr float kDefaultSharpnessFactor = 1.0f;

struct BufferLayout {
    UINT64 positionBytes = 0;
    UINT64 indexBytes = 0;
    UINT64 sharpnessBytes = 0;
    UINT64 valenceBytes = 0;

    UINT64 indexOffset = 0;
    UINT64 sharpnessOffset = 0;
    UINT64 valenceOffset = 0;
    UINT64 totalBytes = 0;
};

UINT64 Align256(UINT64 size) {
    return (size + 255ull) & ~255ull;
}

UINT64 ByteSize(std::size_t elementCount, std::size_t elementSize) {
    return static_cast<UINT64>(elementCount) * static_cast<UINT64>(elementSize);
}

BufferLayout BuildBufferLayout(const Edgefriend::EdgefriendGeometry& geometry) {
    BufferLayout layout;
    layout.positionBytes = ByteSize(geometry.positions.size(), sizeof(glm::vec3));
    layout.indexBytes = ByteSize(geometry.indices.size(), sizeof(int));
    layout.sharpnessBytes = ByteSize(geometry.friendsAndSharpnesses.size(), sizeof(glm::uvec4));
    layout.valenceBytes = ByteSize(geometry.valenceStartInfos.size(), sizeof(int));

    layout.indexOffset = layout.positionBytes;
    layout.sharpnessOffset = layout.indexOffset + layout.indexBytes;
    layout.valenceOffset = layout.sharpnessOffset + layout.sharpnessBytes;
    layout.totalBytes = layout.valenceOffset + layout.valenceBytes;

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
        throw std::runtime_error("Negative dispatch dimensions are invalid.");
    }
    const UINT64 dispatchThreads = static_cast<UINT64>(std::max(vertexCount, faceCount));
    const UINT64 groups64 = (dispatchThreads + threadsPerGroup - 1u) / threadsPerGroup;
    return ToUintChecked(groups64, "Dispatch group count");
}

struct ObjData {
    std::vector<glm::vec3> vertices;
    std::vector<glm::ivec4> faces;
};

int ParseObjIndexToken(const std::string& token) {
    const auto slashPos = token.find('/');
    const std::string numberPart = (slashPos == std::string::npos) ? token : token.substr(0, slashPos);
    return std::stoi(numberPart);
}

ObjData LoadObjData(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open OBJ file for comparison: " + path.string());
    }

    ObjData data;
    std::string line;
    while (std::getline(input, line)) {
        if (line.size() > 2 && line[0] == 'v' && line[1] == ' ') {
            std::istringstream lineStream(line.substr(2));
            glm::vec3 vertex{};
            lineStream >> vertex.x >> vertex.y >> vertex.z;
            if (!lineStream.fail()) {
                data.vertices.push_back(vertex);
            }
        }
        else if (line.size() > 2 && line[0] == 'f' && line[1] == ' ') {
            std::istringstream lineStream(line.substr(2));
            std::string token0, token1, token2, token3;
            lineStream >> token0 >> token1 >> token2 >> token3;
            if (!lineStream.fail()) {
                data.faces.emplace_back(
                    ParseObjIndexToken(token0),
                    ParseObjIndexToken(token1),
                    ParseObjIndexToken(token2),
                    ParseObjIndexToken(token3)
                );
            }
        }
    }

    return data;
}
} // namespace

EdgefriendDX12::~EdgefriendDX12() {
    if (m_renderContextFenceEvent != nullptr) {
        CloseHandle(m_renderContextFenceEvent);
        m_renderContextFenceEvent = nullptr;
    }
}

void EdgefriendDX12::LoadObj() {
    auto model = rapidobj::ParseFile(file);
    if (model.error) {
        throw std::runtime_error("OBJ file could not be loaded: " + file.string());
    }

    if (model.shapes.size() == 0) {
        throw std::runtime_error("OBJ file does not contain a mesh: " + file.string());
    }

    const auto& objmesh = model.shapes.front().mesh;
    if (model.shapes.size() != 1) {
        std::cerr << "Warning: demo will only process the first shape/object!";
    }

    auto positionSpan = std::span<glm::vec3>(
        reinterpret_cast<glm::vec3*>(model.attributes.positions.data()), model.attributes.positions.size() / 3);

    std::vector<glm::vec3> positions(positionSpan.begin(), positionSpan.end());

    std::vector<std::int32_t> indices;
    std::transform(objmesh.indices.begin(), objmesh.indices.end(), std::back_inserter(indices),
        [](const rapidobj::Index& idx) {
            return idx.position_index;
        });

    std::vector<std::int32_t> indicesOffsets;

    indicesOffsets.reserve(objmesh.num_face_vertices.size());
    std::size_t startIndex = 0;
    for (const auto faceSize : objmesh.num_face_vertices) {
        indicesOffsets.push_back(startIndex);
        startIndex += faceSize;
    }

    ankerl::unordered_dense::map<glm::ivec2, float> creases;
    creases.reserve(objmesh.creases.size());
    for (const auto& crease : objmesh.creases) {
        const auto [min, max] = std::minmax(crease.position_index_from, crease.position_index_to);
        creases.emplace(glm::ivec2(min, max), crease.sharpness);
    }

    PreProcess(positions, indices, indicesOffsets, creases);
}

void EdgefriendDX12::PreProcess(std::vector<glm::vec3> oldPositions,
    std::vector<int> oldIndices,
    std::vector<int> oldIndicesOffsets,
    ankerl::unordered_dense::map<glm::ivec2, float> oldCreases) {
    
    orig_geometry = Edgefriend::SubdivideToEdgefriendGeometry(oldPositions, oldIndices, oldIndicesOffsets, oldCreases);
}

void EdgefriendDX12::OnInit() {
    LoadObj();
    ComputeMemory(iters, orig_geometry);
    LoadPipeline();
    LoadAssets();
    ReadBack();
    WriteObj();
}

bool EdgefriendDX12::RunAndCompareWithCpu(float positionEpsilon) {
    if (positionEpsilon <= 0.0f) {
        throw std::invalid_argument("positionEpsilon must be > 0.");
    }

    OnInit();

    const std::filesystem::path dx12Path = "output_" + std::to_string(iters) + "iter.obj";
    const std::filesystem::path cpuPath = "output_cpp_" + std::to_string(iters) + "iter.obj";

    const auto cpuResult = RunCpuSubdivision();
    WriteObj(cpuPath, cpuResult);

    const bool isConsistent = CompareObjFiles(dx12Path, cpuPath, positionEpsilon);
    std::cout << "[Check] Compared files: " << dx12Path.string() << " vs " << cpuPath.string()
              << " (epsilon=" << positionEpsilon << ")\n";
    std::cout << (isConsistent ? "[Check] DX12 and C++ OBJ outputs are consistent.\n"
                               : "[Check] DX12 and C++ OBJ outputs differ.\n");
    return isConsistent;
}


void EdgefriendDX12::LoadPipeline() {
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ComPtr<ID3D12Debug6> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            debugController->SetEnableGPUBasedValidation(true);
            debugController->SetEnableSynchronizedCommandQueueValidation(true);
            debugController->SetEnableAutoName(true);
            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    ComPtr<IDXGIFactory4> dxgiFactory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&dxgiFactory)));
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> adapter;

    for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != dxgiFactory->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex) {
        adapters.push_back(adapter);
    }
    if (adapters.empty()) {
        throw std::runtime_error("No DXGI adapters found.");
    }

    // List the adapters and let the user choose
    std::cout << "Select a device to create:\n";
    for (size_t i = 0; i < adapters.size(); ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapters[i]->GetDesc1(&desc);

        std::wcout << i << ": " << desc.Description << "\n";
    }

    std::cout << "Enter the number of the device you want to create: ";
    size_t choice;
    std::cin >> choice;
    if (!std::cin) {
        throw std::runtime_error("Failed to read adapter selection.");
    }

    if (choice >= adapters.size()) {
        throw std::runtime_error("Invalid adapter selection.");
    }

    // Device creation logic using the selected adapter
    const HRESULT createDeviceResult = D3D12CreateDevice(adapters[choice].Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device));
    if (FAILED(createDeviceResult)) {
        throw std::runtime_error("Failed to create D3D12 device.");
    }

    // m_device now contains the created device
    // Further initialization can be done here

    std::cout << "Device created successfully with adapter " << choice << ".\n";
    ComPtr<IDXGIAdapter1> c_adapter = adapters[choice];
    DXGI_ADAPTER_DESC1 adapterDesc;
    if (SUCCEEDED(c_adapter->GetDesc1(&adapterDesc))) {
        // 输出设备名称
        std::wcout << L"Adapter Name: " << adapterDesc.Description << std::endl;

        // 输出其他有用的信息，比如设备ID，供应商ID等
        std::cout << "Device ID: " << adapterDesc.DeviceId << std::endl;
        std::cout << "Vendor ID: " << adapterDesc.VendorId << std::endl;
        std::cout << "SubSys ID: " << adapterDesc.SubSysId << std::endl;
        std::cout << "Revision: " << adapterDesc.Revision << std::endl;
        std::cout << "Dedicated Video Memory: " << adapterDesc.DedicatedVideoMemory << std::endl;
        std::cout << "Dedicated System Memory: " << adapterDesc.DedicatedSystemMemory << std::endl;
        std::cout << "Shared System Memory: " << adapterDesc.SharedSystemMemory << std::endl;
    }
    else {
        throw std::runtime_error("Failed to get adapter description.");
    }

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue)));
    NAME_D3D12_OBJECT(m_commandQueue);

    // create descriptor heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC srvUavHeapDesc = {};
        srvUavHeapDesc.NumDescriptors = DescriptorCount;
        srvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        srvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(m_device->CreateDescriptorHeap(&srvUavHeapDesc, IID_PPV_ARGS(&m_srvUavHeap)));
        NAME_D3D12_OBJECT(m_srvUavHeap);

        m_srvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    // create allocator
    ThrowIfFailed(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_commandAllocator)));

}

void EdgefriendDX12::LoadAssets() {
    // Create the root signature
    {
        CD3DX12_DESCRIPTOR_RANGE1 ranges[2];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE);   

        CD3DX12_ROOT_PARAMETER1 rootParameters[ComputeRootParametersCount];
        rootParameters[ComputeRootCBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[ComputeRootSRVTable].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[ComputeRootUAVTable].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC computeRootSignatureDesc;
        computeRootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&computeRootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error));
        ThrowIfFailed(m_device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature)));
        NAME_D3D12_OBJECT(m_rootSignature);
    }

    // Create the pipeline states, which includes compiling and loading shaders.
    {
#if defined(_DEBUG)
        // Enable better shader debugging with the graphics debugging tools.
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = 0;
#endif

        std::ifstream shaderFile("hlsl/edgefriend.hlsl");
        if (!shaderFile.is_open()) {
            throw std::runtime_error("Failed to open hlsl/edgefriend.hlsl");
        }
        std::stringstream shaderStream;
        shaderStream << shaderFile.rdbuf();
        std::string shaderCode = shaderStream.str();
        //std::cout << shaderCode;

        ComPtr<ID3DBlob> computeShader;
        ThrowIfFailed(D3DCompile(shaderCode.data(), shaderCode.size(),
            nullptr, nullptr, nullptr,
            "CSEdgefriend", "cs_5_1", compileFlags, 0, &computeShader, nullptr));
        //std::cout << shaderCode;

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());
        psoDesc.pRootSignature = m_rootSignature.Get();

        ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
        NAME_D3D12_OBJECT(m_pipelineState);
    }

    CreateHeapAndViews();

    // Create the command list.
    m_computeCommandList = nullptr;
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_computeCommandList)));
    BindComputeState();

    SetBuffers();

    // initial constantbuffer
    ConstantBufferCS* constantBufferCS = {};
    ThrowIfFailed(constantBufferCSUpload->Map(0, nullptr, reinterpret_cast<void**>(&constantBufferCS)));
    constantBufferCS->F = orig_geometry.friendsAndSharpnesses.size();
    constantBufferCS->V = orig_geometry.positions.size();
    constantBufferCS->sharpnessFactor = kDefaultSharpnessFactor;
    
    {
        ThrowIfFailed(m_device->CreateFence(m_renderContextFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_renderContextFence)));
        m_renderContextFenceValue++;

        m_renderContextFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_renderContextFenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
    }

    CD3DX12_RESOURCE_BARRIER resBarrier[4];
    resBarrier[0] = CD3DX12_RESOURCE_BARRIER::Transition(m_positionBufferIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    resBarrier[1] = CD3DX12_RESOURCE_BARRIER::Transition(m_indexBufferIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    resBarrier[2] = CD3DX12_RESOURCE_BARRIER::Transition(m_friendAndSharpnessBufferIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    resBarrier[3] = CD3DX12_RESOURCE_BARRIER::Transition(m_valenceStartInfoBufferIn.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    m_computeCommandList->ResourceBarrier(_countof(resBarrier), resBarrier);

    for (int i = 0; i < iters; i++) {
        m_computeCommandList->CopyBufferRegion(m_constantBufferCS.Get(), 0, constantBufferCSUpload.Get(), 0, sizeof(ConstantBufferCS));
        CD3DX12_RESOURCE_BARRIER barrier;
        barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_constantBufferCS.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        m_computeCommandList->ResourceBarrier(1, &barrier);

        // Thread groups must scale with the current iteration's working set, not the initial mesh.
        const UINT dispatchGroups = ComputeDispatchGroupCount(constantBufferCS->V, constantBufferCS->F, kComputeThreadsPerGroup);
        m_computeCommandList->Dispatch(dispatchGroups, 1, 1);
        ThrowIfFailed(m_computeCommandList->Close());

        ID3D12CommandList* ppCommandLists[] = { m_computeCommandList.Get() };
        m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

        WaitForRenderContext();

        SwapGeometryBuffers();
        CreateSrvUavViews();

        ThrowIfFailed(m_commandAllocator->Reset());
        ThrowIfFailed(m_computeCommandList->Reset(m_commandAllocator.Get(), m_pipelineState.Get()));
        BindComputeState();

        constantBufferCS->F *= 4;
        constantBufferCS->V *= 4;
        
    }

    SwapGeometryBuffers();
    ThrowIfFailed(m_computeCommandList->Close());
    constantBufferCSUpload->Unmap(0, nullptr);
    CreateSrvUavViews();

}

void EdgefriendDX12::BindComputeState() {
    m_computeCommandList->SetComputeRootSignature(m_rootSignature.Get());
    ID3D12DescriptorHeap* descriptorHeaps[] = { m_srvUavHeap.Get() };
    m_computeCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), SrvPosIn, m_srvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), UavPosOut, m_srvUavDescriptorSize);

    m_computeCommandList->SetComputeRootConstantBufferView(ComputeRootCBV, m_constantBufferCS->GetGPUVirtualAddress());
    m_computeCommandList->SetComputeRootDescriptorTable(ComputeRootSRVTable, srvHandle);
    m_computeCommandList->SetComputeRootDescriptorTable(ComputeRootUAVTable, uavHandle);
    NAME_D3D12_OBJECT(m_computeCommandList);
}

void EdgefriendDX12::SwapGeometryBuffers() {
    std::swap(m_positionBufferIn, m_positionBufferOut);
    std::swap(m_indexBufferIn, m_indexBufferOut);
    std::swap(m_friendAndSharpnessBufferIn, m_friendAndSharpnessBufferOut);
    std::swap(m_valenceStartInfoBufferIn, m_valenceStartInfoBufferOut);
}

void EdgefriendDX12::CreateSrvUavViews() {
    const auto layout = BuildBufferLayout(result);
    const UINT positionCount = ToUintChecked(static_cast<UINT64>(result.positions.size()), "Position element count");
    const UINT indexCount = ToUintChecked(static_cast<UINT64>(result.indices.size()), "Index element count");
    const UINT sharpnessWordCount = ToUintChecked(layout.sharpnessBytes / sizeof(UINT32), "Sharpness word count");
    const UINT valenceCount = ToUintChecked(static_cast<UINT64>(result.valenceStartInfos.size()), "Valence element count");

    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_posIn = {};
        srvDesc_posIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_posIn.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc_posIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_posIn.Buffer.FirstElement = 0;
        srvDesc_posIn.Buffer.NumElements = positionCount;
        srvDesc_posIn.Buffer.StructureByteStride = sizeof(glm::vec3);
        srvDesc_posIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandlePosIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvPosIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_positionBufferIn.Get(), &srvDesc_posIn, srvHandlePosIn);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_indexIn = {};
        srvDesc_indexIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_indexIn.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc_indexIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_indexIn.Buffer.FirstElement = 0;
        srvDesc_indexIn.Buffer.NumElements = indexCount;
        srvDesc_indexIn.Buffer.StructureByteStride = 0;
        srvDesc_indexIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandleIndexIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvIndexIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_indexBufferIn.Get(), &srvDesc_indexIn, srvHandleIndexIn);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_sharpnessIn = {};
        srvDesc_sharpnessIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_sharpnessIn.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc_sharpnessIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_sharpnessIn.Buffer.FirstElement = 0;
        srvDesc_sharpnessIn.Buffer.NumElements = sharpnessWordCount;
        srvDesc_sharpnessIn.Buffer.StructureByteStride = 0;
        srvDesc_sharpnessIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandleSharpnessIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvFriendIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_friendAndSharpnessBufferIn.Get(), &srvDesc_sharpnessIn, srvHandleSharpnessIn);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_valenceIn = {};
        srvDesc_valenceIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_valenceIn.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc_valenceIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_valenceIn.Buffer.FirstElement = 0;
        srvDesc_valenceIn.Buffer.NumElements = valenceCount;
        srvDesc_valenceIn.Buffer.StructureByteStride = sizeof(int);
        srvDesc_valenceIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandleValenceIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvValenceIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_valenceStartInfoBufferIn.Get(), &srvDesc_valenceIn, srvHandleValenceIn);
    }

    {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_posOut = {};
        uavDesc_posOut.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc_posOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_posOut.Buffer.FirstElement = 0;
        uavDesc_posOut.Buffer.NumElements = positionCount;
        uavDesc_posOut.Buffer.StructureByteStride = sizeof(glm::vec3);
        uavDesc_posOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_posOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandlePosOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavPosOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_positionBufferOut.Get(), nullptr, &uavDesc_posOut, uavHandlePosOut);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_indexOut = {};
        uavDesc_indexOut.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc_indexOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_indexOut.Buffer.FirstElement = 0;
        uavDesc_indexOut.Buffer.NumElements = indexCount;
        uavDesc_indexOut.Buffer.StructureByteStride = 0;
        uavDesc_indexOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_indexOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandleIndexOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavIndexOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_indexBufferOut.Get(), nullptr, &uavDesc_indexOut, uavHandleIndexOut);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_sharpnessOut = {};
        uavDesc_sharpnessOut.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc_sharpnessOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_sharpnessOut.Buffer.FirstElement = 0;
        uavDesc_sharpnessOut.Buffer.NumElements = sharpnessWordCount;
        uavDesc_sharpnessOut.Buffer.StructureByteStride = 0;
        uavDesc_sharpnessOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_sharpnessOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandleSharpnessOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavFriendOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_friendAndSharpnessBufferOut.Get(), nullptr, &uavDesc_sharpnessOut, uavHandleSharpnessOut);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_valenceOut = {};
        uavDesc_valenceOut.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc_valenceOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_valenceOut.Buffer.FirstElement = 0;
        uavDesc_valenceOut.Buffer.NumElements = valenceCount;
        uavDesc_valenceOut.Buffer.StructureByteStride = sizeof(int);
        uavDesc_valenceOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_valenceOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandleValenceOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavValenceOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_valenceStartInfoBufferOut.Get(), nullptr, &uavDesc_valenceOut, uavHandleValenceOut);
    }
}

void EdgefriendDX12::WaitForRenderContext() {
    if (m_renderContextFenceEvent == nullptr) {
        throw std::runtime_error("Fence event is not initialized.");
    }

    // Add a signal command to the queue.
    ThrowIfFailed(m_commandQueue->Signal(m_renderContextFence.Get(), m_renderContextFenceValue));

    // Instruct the fence to set the event object when the signal command completes.
    ThrowIfFailed(m_renderContextFence->SetEventOnCompletion(m_renderContextFenceValue, m_renderContextFenceEvent));
    m_renderContextFenceValue++;

    // Wait until the signal command has been processed.
    WaitForSingleObject(m_renderContextFenceEvent, INFINITE);
}


void EdgefriendDX12::SetBuffers() {
    const auto inputLayout = BuildBufferLayout(orig_geometry);

    // upload heap
    UINT8* pUploadHeapData;
    ThrowIfFailed(m_uploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&pUploadHeapData)));

    memcpy(pUploadHeapData, orig_geometry.positions.data(), static_cast<std::size_t>(inputLayout.positionBytes));
    memcpy(pUploadHeapData + inputLayout.indexOffset, orig_geometry.indices.data(), static_cast<std::size_t>(inputLayout.indexBytes));
    memcpy(pUploadHeapData + inputLayout.sharpnessOffset, orig_geometry.friendsAndSharpnesses.data(), static_cast<std::size_t>(inputLayout.sharpnessBytes));
    memcpy(pUploadHeapData + inputLayout.valenceOffset, orig_geometry.valenceStartInfos.data(), static_cast<std::size_t>(inputLayout.valenceBytes));

    m_uploadHeap->Unmap(0, nullptr);

    // input resources
    m_computeCommandList->CopyBufferRegion(m_positionBufferIn.Get(), 0, m_uploadHeap.Get(), 0, inputLayout.positionBytes);
    m_computeCommandList->CopyBufferRegion(m_indexBufferIn.Get(), 0, m_uploadHeap.Get(), inputLayout.indexOffset, inputLayout.indexBytes);
    m_computeCommandList->CopyBufferRegion(m_friendAndSharpnessBufferIn.Get(), 0, m_uploadHeap.Get(), inputLayout.sharpnessOffset, inputLayout.sharpnessBytes);
    m_computeCommandList->CopyBufferRegion(m_valenceStartInfoBufferIn.Get(), 0, m_uploadHeap.Get(), inputLayout.valenceOffset, inputLayout.valenceBytes);

}

void EdgefriendDX12::CreateHeapAndViews() {
    const auto layout = BuildBufferLayout(result);

    D3D12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_HEAP_PROPERTIES readbackHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    D3D12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(layout.totalBytes));
    D3D12_RESOURCE_DESC readbackBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(layout.totalBytes));

    D3D12_RESOURCE_DESC posBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(layout.positionBytes));
    posBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC indexBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(layout.indexBytes));
    indexBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC sharpnessBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(layout.sharpnessBytes));
    sharpnessBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    D3D12_RESOURCE_DESC valenceBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(layout.valenceBytes));
    valenceBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    {
        CD3DX12_HEAP_PROPERTIES constantBufferHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(Align256(sizeof(ConstantBufferCS)));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &constantBufferHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &constantBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_constantBufferCS)));

        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC resourceDescUpload = CD3DX12_RESOURCE_DESC::Buffer(Align256(sizeof(ConstantBufferCS)));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescUpload,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&constantBufferCSUpload)));

        NAME_D3D12_OBJECT(m_constantBufferCS);
        NAME_D3D12_OBJECT(constantBufferCSUpload);
    }

    {
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &posBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_positionBufferOut)));
        NAME_D3D12_OBJECT(m_positionBufferOut);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &indexBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_indexBufferOut)));
        NAME_D3D12_OBJECT(m_indexBufferOut);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &sharpnessBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_friendAndSharpnessBufferOut)));
        NAME_D3D12_OBJECT(m_friendAndSharpnessBufferOut);
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &valenceBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_valenceStartInfoBufferOut)));
        NAME_D3D12_OBJECT(m_valenceStartInfoBufferOut);

        
    }



    {
        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &uploadBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_uploadHeap)));
        NAME_D3D12_OBJECT(m_uploadHeap);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &posBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_positionBufferIn)
        ));
        NAME_D3D12_OBJECT(m_positionBufferIn);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &indexBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_indexBufferIn)
        ));
        NAME_D3D12_OBJECT(m_indexBufferIn);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &sharpnessBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_friendAndSharpnessBufferIn)
        ));
        NAME_D3D12_OBJECT(m_friendAndSharpnessBufferIn);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &valenceBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_valenceStartInfoBufferIn)
        ));
        NAME_D3D12_OBJECT(m_valenceStartInfoBufferIn);

    }

    CreateSrvUavViews();

    {
        ThrowIfFailed(m_device->CreateCommittedResource(
            &readbackHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &readbackBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_readbackHeap)));
        NAME_D3D12_OBJECT(m_readbackHeap);
    }
}


void EdgefriendDX12::ReadBack() {
    const auto outputLayout = BuildBufferLayout(result);

    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_computeCommandList->Reset(m_commandAllocator.Get(), nullptr));


    // 拷贝资源到 readbackHeap
    m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), 0, m_positionBufferOut.Get(), 0, outputLayout.positionBytes);
    m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), outputLayout.indexOffset, m_indexBufferOut.Get(), 0, outputLayout.indexBytes);
    m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), outputLayout.sharpnessOffset, m_friendAndSharpnessBufferOut.Get(), 0, outputLayout.sharpnessBytes);
    m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), outputLayout.valenceOffset, m_valenceStartInfoBufferOut.Get(), 0, outputLayout.valenceBytes);
    

    ThrowIfFailed(m_computeCommandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_computeCommandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    WaitForRenderContext();


    // 把 readbackHeap 里的内容拷贝到 pReadbackHeapData
    void* pReadbackHeapData;
    ThrowIfFailed(m_readbackHeap->Map(0, nullptr, reinterpret_cast<void**>(&pReadbackHeapData)));
    auto* readbackBytes = reinterpret_cast<UINT8*>(pReadbackHeapData);
    memcpy(result.positions.data(), readbackBytes, static_cast<std::size_t>(outputLayout.positionBytes));
    memcpy(result.indices.data(), readbackBytes + outputLayout.indexOffset, static_cast<std::size_t>(outputLayout.indexBytes));
    memcpy(result.friendsAndSharpnesses.data(), readbackBytes + outputLayout.sharpnessOffset, static_cast<std::size_t>(outputLayout.sharpnessBytes));
    memcpy(result.valenceStartInfos.data(), readbackBytes + outputLayout.valenceOffset, static_cast<std::size_t>(outputLayout.valenceBytes));
    m_readbackHeap->Unmap(0, nullptr);
}

void EdgefriendDX12::WriteObj() {
    const std::string outputFile = "output_" + std::to_string(iters) + "iter.obj";
    WriteObj(outputFile, result);
}

void EdgefriendDX12::WriteObj(const std::filesystem::path& outputPath, const Edgefriend::EdgefriendGeometry& geometry) const {
    std::ofstream obj(outputPath);
    if (!obj.is_open()) {
        throw std::runtime_error("Failed to open output file: " + outputPath.string());
    }

    for (const auto& position : geometry.positions) {
        obj << "v " << position.x << ' ' << position.y << ' ' << position.z << '\n';
    }
    for (int i = 0; i < static_cast<int>(geometry.friendsAndSharpnesses.size()); ++i) {
        obj << 'f';
        for (int j = 0; j < 4; ++j) {
            obj << ' ' << geometry.indices[4 * i + j] + 1;
        }
        obj << '\n';
    }
    obj.close();
}

Edgefriend::EdgefriendGeometry EdgefriendDX12::RunCpuSubdivision() const {
    auto cpuGeometry = orig_geometry;
    for (int i = 0; i < iters; ++i) {
        cpuGeometry = Edgefriend::SubdivideEdgefriendGeometry(cpuGeometry);
    }
    return cpuGeometry;
}

bool EdgefriendDX12::CompareObjFiles(const std::filesystem::path& dx12Path, const std::filesystem::path& cpuPath, float positionEpsilon) const {
    const ObjData dx12Data = LoadObjData(dx12Path);
    const ObjData cpuData = LoadObjData(cpuPath);

    if (dx12Data.vertices.size() != cpuData.vertices.size()) {
        std::cerr << "[Check] Vertex count mismatch: DX12=" << dx12Data.vertices.size()
                  << ", CPU=" << cpuData.vertices.size() << '\n';
        return false;
    }
    if (dx12Data.faces.size() != cpuData.faces.size()) {
        std::cerr << "[Check] Face count mismatch: DX12=" << dx12Data.faces.size()
                  << ", CPU=" << cpuData.faces.size() << '\n';
        return false;
    }

    for (std::size_t i = 0; i < dx12Data.vertices.size(); ++i) {
        const glm::vec3& a = dx12Data.vertices[i];
        const glm::vec3& b = cpuData.vertices[i];
        if (std::abs(a.x - b.x) > positionEpsilon ||
            std::abs(a.y - b.y) > positionEpsilon ||
            std::abs(a.z - b.z) > positionEpsilon) {
            std::cerr << "[Check] Vertex mismatch at index " << i
                      << ": DX12=(" << a.x << ", " << a.y << ", " << a.z
                      << "), CPU=(" << b.x << ", " << b.y << ", " << b.z << ")\n";
            return false;
        }
    }

    for (std::size_t i = 0; i < dx12Data.faces.size(); ++i) {
        const glm::ivec4& a = dx12Data.faces[i];
        const glm::ivec4& b = cpuData.faces[i];
        if (a.x != b.x || a.y != b.y || a.z != b.z || a.w != b.w) {
            std::cerr << "[Check] Face mismatch at index " << i
                      << ": DX12=(" << a.x << ", " << a.y << ", " << a.z << ", " << a.w
                      << "), CPU=(" << b.x << ", " << b.y << ", " << b.z << ", " << b.w << ")\n";
            return false;
        }
    }

    return true;
}

void EdgefriendDX12::SetIters(int i) {
    if (i <= 0) {
        throw std::invalid_argument("iters must be > 0.");
    }
    iters = i;
}

void EdgefriendDX12::ComputeMemory(int iter, Edgefriend::EdgefriendGeometry orig){
    if (iter <= 0) {
        result = std::move(orig);
        return;
    }

    for (int i = 0; i < iter; i++) {
        Edgefriend::EdgefriendGeometry next;
        const auto oldVertexCount = orig.positions.size();
        next.positions.resize(oldVertexCount + 3 * orig.valenceStartInfos.size());
        next.indices.resize(orig.indices.size() * 4);
        next.friendsAndSharpnesses.resize(orig.indices.size());
        next.valenceStartInfos.resize(next.positions.size());
        orig = std::move(next);
    }
    result = std::move(orig);
}
