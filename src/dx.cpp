#include "dx.h"
#include "DXSampleHelper.h"

#include <stdexcept>

using Microsoft::WRL::ComPtr;

void EdgefriendDX12::OnInit() {
    LoadPipeline();
    LoadAssets();
}


int EdgefriendDX12::LoadObj(const std::filesystem::path& file) {
    auto model = rapidobj::ParseFile(file);
    if (model.error) {
        std::cerr << "Error: OBJ file" << file << "could not be loaded.";
        return 2;
    }
    if (model.shapes.size() == 0) {
        std::cerr << "Error: OBJ file" << file << "does not contain a mesh.";
        return 3;
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

}

void EdgefriendDX12::LoadPipeline() {
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    // Enable the debug layer (requires the Graphics Tools "optional feature").
    // NOTE: Enabling the debug layer after device creation will invalidate the active device.
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            // Enable additional debug layers.
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif
    ComPtr<IDXGIFactory4> dxgiFactory;
    ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)));
    std::vector<ComPtr<IDXGIAdapter1>> adapters;
    ComPtr<IDXGIAdapter1> adapter;

    for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != dxgiFactory->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex) {
        adapters.push_back(adapter);
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

    if (choice >= adapters.size()) {
        std::cerr << "Invalid selection.\n";
        return;
    }

    // Device creation logic using the selected adapter
    //ComPtr<ID3D12Device> m_device;
    if (D3D12CreateDevice(adapters[choice].Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)) != S_OK) {
        std::cerr << "Failed to create D3D12 device.\n";
        return;
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
        std::cerr << "Failed to get adapter description.\n";
    }

    // Describe and create the command queue.
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_computeCommandQueue)));
    NAME_D3D12_OBJECT(m_computeCommandQueue);

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
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE);
        ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 4, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);

        CD3DX12_ROOT_PARAMETER1 rootParameters[ComputeRootParametersCount];
        rootParameters[ComputeRootCBV].InitAsConstantBufferView(0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC, D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[ComputeRootSRVTable].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);
        rootParameters[ComputeRootUAVTable].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);

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

        std::ifstream shaderFile("C:/Users/17480/Desktop/dx12/hlsl/edgefriend.hlsl");
        if (!shaderFile.is_open()) {
            std::cerr << "Failed to open hlsl." << std::endl;
            return;
        }
        std::stringstream shaderStream;
        shaderStream << shaderFile.rdbuf();
        std::string shaderCode = shaderStream.str();
        //std::cout << shaderCode;

        ComPtr<ID3DBlob> computeShader;
        ThrowIfFailed(D3DCompile(shaderCode.data(), shaderCode.size(),
            nullptr, nullptr, nullptr,
            "CSEdgefriend", "cs_5_0", 0, 0, &computeShader, nullptr));
        //std::cout << shaderCode;

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());
        psoDesc.pRootSignature = m_rootSignature.Get();

        ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
        NAME_D3D12_OBJECT(m_pipelineState);
    }

    // Create the command list.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_computeCommandList)));
    NAME_D3D12_OBJECT(m_computeCommandList);

    CreateBuffers();

    ComPtr<ID3D12Resource> constantBufferCSUpload;

    // Create the compute shader's constant buffer.
    {
        const UINT bufferSize = sizeof(ConstantBufferCS);

        CD3DX12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapProps,
            D3D12_HEAP_FLAG_NONE,
            &resourceDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_constantBufferCS)));

        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC resourceDescUpload = CD3DX12_RESOURCE_DESC::Buffer(bufferSize);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescUpload,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&constantBufferCSUpload)));

        NAME_D3D12_OBJECT(m_constantBufferCS);

        ConstantBufferCS constantBufferCS = {};
        constantBufferCS.F = 1;
        constantBufferCS.V = 1;
        constantBufferCS.sharpnessFactor = 1.f;
        constantBufferCS.padding = 0;

        D3D12_SUBRESOURCE_DATA computeCBData = {};
        computeCBData.pData = reinterpret_cast<UINT8*>(&constantBufferCS);
        computeCBData.RowPitch = bufferSize;
        computeCBData.SlicePitch = computeCBData.RowPitch;

        UpdateSubresources<1>(m_computeCommandList.Get(), m_constantBufferCS.Get(), constantBufferCSUpload.Get(), 0, 0, 1, &computeCBData);
        // Define the transition barrier
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_constantBufferCS.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);

        // Invoke the command list's ResourceBarrier function
        m_computeCommandList->ResourceBarrier(1, &barrier);
        //m_computeCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(m_constantBufferCS.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
    }

    // Close the command list and execute it to begin the initial GPU setup.
    ThrowIfFailed(m_computeCommandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_computeCommandList.Get() };
    m_computeCommandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);
}

void EdgefriendDX12::CreateBuffers() {
    std::vector<XMFLOAT3> posIn;
    posIn.resize(posInCount);
    const UINT posInSize = posInCount * sizeof(XMFLOAT3);

    // load data

    D3D12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(posInSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(posInSize);

    for (UINT index = 0; index < ThreadCount; index++) {
        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&m_positionBufferIn[index])));

        ThrowIfFailed(m_device->CreateCommittedResource(
            &uploadHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &uploadBufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&m_positionBufferInUpload[index])));

        NAME_D3D12_OBJECT_INDEXED(m_positionBufferIn, index);

        D3D12_SUBRESOURCE_DATA posInData = {};
        posInData.pData = reinterpret_cast<UINT8*>(&posIn[0]);
        posInData.RowPitch = posInSize;
        posInData.SlicePitch = posInData.RowPitch;

        UpdateSubresources<1>(m_computeCommandList.Get(), m_positionBufferIn[index].Get(), m_positionBufferInUpload[index].Get(), 0, 0, 1, &posInData);
        
        D3D12_RESOURCE_BARRIER barrier = CD3DX12_RESOURCE_BARRIER::Transition(m_positionBufferIn[index].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_computeCommandList->ResourceBarrier(1, &barrier);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = posInCount;
        srvDesc.Buffer.StructureByteStride = sizeof(XMFLOAT3);
        srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandlePosIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvPosIn + index, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_positionBufferIn[index].Get(), &srvDesc, srvHandlePosIn);

        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.FirstElement = 0;
        uavDesc.Buffer.NumElements = posInCount;
        // maybe posOut?
        uavDesc.Buffer.StructureByteStride = sizeof(XMFLOAT3);
        uavDesc.Buffer.CounterOffsetInBytes = 0;
        uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandlePosOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavPosOut + index, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_positionBufferOut[index].Get(), nullptr, &uavDesc, uavHandlePosOut);
    }

}
