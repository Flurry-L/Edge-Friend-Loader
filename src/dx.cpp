#include "dx.h"
#include "DXSampleHelper.h"
#include <glm/gtx/hash.hpp>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

void EdgefriendDX12::LoadObj() {
    auto model = rapidobj::ParseFile(file);
    if (model.error) {
        std::cerr << "Error: OBJ file" << file << "could not be loaded.";
        return;
    }

    if (model.shapes.size() == 0) {
        std::cerr << "Error: OBJ file" << file << "does not contain a mesh.";
        return;
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
    LoadPipeline();
    LoadAssets();
    ReadBack();
    WriteObj();
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
            "CSEdgefriend", "cs_5_1", 0, 0, &computeShader, nullptr));
        //std::cout << shaderCode;

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.CS = CD3DX12_SHADER_BYTECODE(computeShader.Get());
        psoDesc.pRootSignature = m_rootSignature.Get();

        ThrowIfFailed(m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState)));
        NAME_D3D12_OBJECT(m_pipelineState);
    }

    ComPtr<ID3D12Resource> constantBufferCSUpload;
    D3D12_SUBRESOURCE_DATA computeCBData = {};
    {
        
        const UINT constantBufferSize = (sizeof(ConstantBufferCS) + 255) & ~255; // Align to 256 bytes

        CD3DX12_HEAP_PROPERTIES constantBufferHeapProps(D3D12_HEAP_TYPE_DEFAULT);
        CD3DX12_RESOURCE_DESC constantBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &constantBufferHeapProps,
            D3D12_HEAP_FLAG_NONE,
            &constantBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_constantBufferCS)));

        CD3DX12_HEAP_PROPERTIES heapPropsUpload(D3D12_HEAP_TYPE_UPLOAD);
        CD3DX12_RESOURCE_DESC resourceDescUpload = CD3DX12_RESOURCE_DESC::Buffer(constantBufferSize);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &heapPropsUpload,
            D3D12_HEAP_FLAG_NONE,
            &resourceDescUpload,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&constantBufferCSUpload)));

        NAME_D3D12_OBJECT(m_constantBufferCS);
        NAME_D3D12_OBJECT(constantBufferCSUpload);

        ConstantBufferCS constantBufferCS = {};
        constantBufferCS.F = orig_geometry.friendsAndSharpnesses.size();
        constantBufferCS.V = orig_geometry.positions.size();
        constantBufferCS.sharpnessFactor = 1;

        
        computeCBData.pData = reinterpret_cast<UINT8*>(&constantBufferCS);
        computeCBData.RowPitch = constantBufferSize;
        computeCBData.SlicePitch = computeCBData.RowPitch;
    }

    // Create the command list.
    ThrowIfFailed(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_commandAllocator.Get(), m_pipelineState.Get(), IID_PPV_ARGS(&m_computeCommandList)));
    m_computeCommandList->SetComputeRootSignature(m_rootSignature.Get());

    ID3D12DescriptorHeap* ppHeaps[] = { m_srvUavHeap.Get() };
    m_computeCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    CD3DX12_GPU_DESCRIPTOR_HANDLE srvHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), SrvPosIn, m_srvUavDescriptorSize);
    CD3DX12_GPU_DESCRIPTOR_HANDLE uavHandle(m_srvUavHeap->GetGPUDescriptorHandleForHeapStart(), UavPosOut, m_srvUavDescriptorSize);

    m_computeCommandList->SetComputeRootConstantBufferView(ComputeRootCBV, m_constantBufferCS->GetGPUVirtualAddress());
    m_computeCommandList->SetComputeRootDescriptorTable(ComputeRootSRVTable, srvHandle);
    m_computeCommandList->SetComputeRootDescriptorTable(ComputeRootUAVTable, uavHandle);
    NAME_D3D12_OBJECT(m_computeCommandList);

    CreateBuffers();

    

    // Create the compute shader's constant buffer.
    {
        

        UpdateSubresources<1>(m_computeCommandList.Get(), m_constantBufferCS.Get(), constantBufferCSUpload.Get(), 0, 0, 1, &computeCBData);
    }

    m_computeCommandList->Dispatch(orig_geometry.positions.size(), 1, 1);

    // Close the command list and execute it to begin the initial GPU setup.
    ThrowIfFailed(m_computeCommandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_computeCommandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    // Create synchronization objects and wait until assets have been uploaded to the GPU.
    {
        ThrowIfFailed(m_device->CreateFence(m_renderContextFenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_renderContextFence)));
        m_renderContextFenceValue++;

        m_renderContextFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (m_renderContextFenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }

        WaitForRenderContext();
    }
}

void EdgefriendDX12::WaitForRenderContext() {
    // Add a signal command to the queue.
    ThrowIfFailed(m_commandQueue->Signal(m_renderContextFence.Get(), m_renderContextFenceValue));

    // Instruct the fence to set the event object when the signal command completes.
    ThrowIfFailed(m_renderContextFence->SetEventOnCompletion(m_renderContextFenceValue, m_renderContextFenceEvent));
    m_renderContextFenceValue++;

    // Wait until the signal command has been processed.
    WaitForSingleObject(m_renderContextFenceEvent, INFINITE);
}



void EdgefriendDX12::CreateBuffers() {

    UINT posInSize = orig_geometry.positions.size() * sizeof(glm::vec3);
    UINT indexInSize = orig_geometry.indices.size() * sizeof(int);
    UINT sharpnessInSize = orig_geometry.friendsAndSharpnesses.size() * sizeof(glm::uvec4);
    UINT valenceInSize = orig_geometry.valenceStartInfos.size() * sizeof(int);

    UINT totalSize = (posInSize + indexInSize + sharpnessInSize + valenceInSize + 255) & ~255;


    D3D12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    D3D12_HEAP_PROPERTIES uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    //D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_RESOURCE_DESC uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize); 
    D3D12_RESOURCE_DESC posInBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((posInSize + 255) & ~255);
    D3D12_RESOURCE_DESC indexInBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((indexInSize + 255) & ~255);
    D3D12_RESOURCE_DESC sharpnessInBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((sharpnessInSize + 255) & ~255);
    D3D12_RESOURCE_DESC valenceInBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((valenceInSize + 255) & ~255);

    // CreateCommittedResource
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
            &posInBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_positionBufferIn)
        ));
        NAME_D3D12_OBJECT(m_positionBufferIn);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &indexInBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_indexBufferIn)
        ));
        NAME_D3D12_OBJECT(m_indexBufferIn);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &sharpnessInBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_friendAndSharpnessBufferIn)
        ));
        NAME_D3D12_OBJECT(m_friendAndSharpnessBufferIn);

        ThrowIfFailed(m_device->CreateCommittedResource(
            &defaultHeapProperties,
            D3D12_HEAP_FLAG_NONE,
            &valenceInBufferDesc,
            D3D12_RESOURCE_STATE_COMMON,
            nullptr,
            IID_PPV_ARGS(&m_valenceStartInfoBufferIn)
        ));
        NAME_D3D12_OBJECT(m_valenceStartInfoBufferIn);
    }
    

    UINT8* pUploadHeapData;
    ThrowIfFailed(m_uploadHeap->Map(0, nullptr, reinterpret_cast<void**>(&pUploadHeapData)));

    memcpy(pUploadHeapData, orig_geometry.positions.data(), posInSize);
    UINT indexInOffset = posInSize;
    memcpy(pUploadHeapData + indexInOffset, orig_geometry.indices.data(), indexInSize);
    UINT sharpnessInOffset = indexInOffset + indexInSize;
    memcpy(pUploadHeapData + sharpnessInOffset, orig_geometry.friendsAndSharpnesses.data(), sharpnessInSize);
    UINT valenceInOffset = sharpnessInOffset + sharpnessInSize;
    memcpy(pUploadHeapData + valenceInOffset, orig_geometry.valenceStartInfos.data(), valenceInSize);

    m_uploadHeap->Unmap(0, nullptr);

    // 拷贝到实际的 gpu 资源
    {
        m_computeCommandList->CopyBufferRegion(m_positionBufferIn.Get(), 0, m_uploadHeap.Get(), 0, posInSize);
        m_computeCommandList->CopyBufferRegion(m_indexBufferIn.Get(), 0, m_uploadHeap.Get(), indexInOffset, indexInSize);
        m_computeCommandList->CopyBufferRegion(m_friendAndSharpnessBufferIn.Get(), 0, m_uploadHeap.Get(), sharpnessInOffset, sharpnessInSize);
        m_computeCommandList->CopyBufferRegion(m_valenceStartInfoBufferIn.Get(), 0, m_uploadHeap.Get(), valenceInOffset, valenceInSize);
    }

    // create views
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_posIn = {};
        srvDesc_posIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_posIn.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc_posIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_posIn.Buffer.FirstElement = 0;
        srvDesc_posIn.Buffer.NumElements = orig_geometry.positions.size();
        srvDesc_posIn.Buffer.StructureByteStride = sizeof(glm::vec3);
        srvDesc_posIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandlePosIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvPosIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_positionBufferIn.Get(), &srvDesc_posIn, srvHandlePosIn);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_indexIn = {};
        srvDesc_indexIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_indexIn.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc_indexIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_indexIn.Buffer.FirstElement = 0;
        srvDesc_indexIn.Buffer.NumElements = orig_geometry.indices.size();
        srvDesc_indexIn.Buffer.StructureByteStride = 0;
        srvDesc_indexIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandleIndexIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvIndexIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_indexBufferIn.Get(), &srvDesc_indexIn, srvHandleIndexIn);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_sharpnessIn = {};
        srvDesc_sharpnessIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_sharpnessIn.Format = DXGI_FORMAT_R32_TYPELESS;
        srvDesc_sharpnessIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_sharpnessIn.Buffer.FirstElement = 0;
        srvDesc_sharpnessIn.Buffer.NumElements = orig_geometry.friendsAndSharpnesses.size() * sizeof(glm::uvec4) / 4;
        srvDesc_sharpnessIn.Buffer.StructureByteStride = 0;
        srvDesc_sharpnessIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandleSharpnessIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvFriendIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_friendAndSharpnessBufferIn.Get(), &srvDesc_sharpnessIn, srvHandleSharpnessIn);

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc_valenceIn = {};
        srvDesc_valenceIn.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc_valenceIn.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc_valenceIn.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srvDesc_valenceIn.Buffer.FirstElement = 0;
        srvDesc_valenceIn.Buffer.NumElements = orig_geometry.valenceStartInfos.size();
        srvDesc_valenceIn.Buffer.StructureByteStride = sizeof(int);
        srvDesc_valenceIn.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE srvHandleValenceIn(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), SrvValenceIn, m_srvUavDescriptorSize);
        m_device->CreateShaderResourceView(m_valenceStartInfoBufferIn.Get(), &srvDesc_valenceIn, srvHandleValenceIn);

        int oV = orig_geometry.positions.size();
        new_geometry.positions.resize(oV + 3 * orig_geometry.valenceStartInfos.size());
        new_geometry.indices.resize(orig_geometry.indices.size() * 4);
        new_geometry.friendsAndSharpnesses.resize(orig_geometry.indices.size());
        new_geometry.valenceStartInfos.resize(new_geometry.positions.size());


        UINT posOutSize = new_geometry.positions.size() * sizeof(glm::vec3);
        UINT indexOutSize = new_geometry.indices.size() * sizeof(int);
        UINT sharpnessOutSize = new_geometry.friendsAndSharpnesses.size() * sizeof(glm::uvec4);
        UINT valenceOutSize = new_geometry.valenceStartInfos.size() * sizeof(int);

        {
            D3D12_HEAP_PROPERTIES defaultHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
            D3D12_RESOURCE_DESC posOutBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((posOutSize + 255) & ~255);
            posOutBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_RESOURCE_DESC indexOutBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((indexOutSize + 255) & ~255);
            indexOutBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_RESOURCE_DESC sharpnessOutBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((sharpnessOutSize + 255) & ~255);
            sharpnessOutBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_RESOURCE_DESC valenceOutBufferDesc = CD3DX12_RESOURCE_DESC::Buffer((valenceOutSize + 255) & ~255);
            valenceOutBufferDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ThrowIfFailed(m_device->CreateCommittedResource(
                &defaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &posOutBufferDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&m_positionBufferOut)));
            NAME_D3D12_OBJECT(m_positionBufferOut);
            ThrowIfFailed(m_device->CreateCommittedResource(
                &defaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &indexOutBufferDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&m_indexBufferOut)));
            NAME_D3D12_OBJECT(m_indexBufferOut);
            ThrowIfFailed(m_device->CreateCommittedResource(
                &defaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &sharpnessOutBufferDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&m_friendAndSharpnessBufferOut)));
            NAME_D3D12_OBJECT(m_friendAndSharpnessBufferOut);
            ThrowIfFailed(m_device->CreateCommittedResource(
                &defaultHeapProperties,
                D3D12_HEAP_FLAG_NONE,
                &valenceOutBufferDesc,
                D3D12_RESOURCE_STATE_COMMON,
                nullptr,
                IID_PPV_ARGS(&m_valenceStartInfoBufferOut)));
            NAME_D3D12_OBJECT(m_valenceStartInfoBufferOut);
        }


        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_posOut = {};
        uavDesc_posOut.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc_posOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_posOut.Buffer.FirstElement = 0;
        uavDesc_posOut.Buffer.NumElements = new_geometry.positions.size();
        uavDesc_posOut.Buffer.StructureByteStride = sizeof(glm::vec3);
        uavDesc_posOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_posOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandlePosOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavPosOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_positionBufferOut.Get(), nullptr, &uavDesc_posOut, uavHandlePosOut);

        UINT indexOutOffset = posOutSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_indexOut = {};
        uavDesc_indexOut.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc_indexOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_indexOut.Buffer.FirstElement = 0;
        uavDesc_indexOut.Buffer.NumElements = new_geometry.indices.size();
        uavDesc_indexOut.Buffer.StructureByteStride = 0;
        uavDesc_indexOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_indexOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandleIndexOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavIndexOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_indexBufferOut.Get(), nullptr, &uavDesc_indexOut, uavHandleIndexOut);

        UINT sharpnessOutOffset = indexOutOffset + indexOutSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_sharpnessOut = {};
        uavDesc_sharpnessOut.Format = DXGI_FORMAT_R32_TYPELESS;
        uavDesc_sharpnessOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_sharpnessOut.Buffer.FirstElement = 0;
        uavDesc_sharpnessOut.Buffer.NumElements = new_geometry.friendsAndSharpnesses.size() * sizeof(glm::uvec4) / 4;
        uavDesc_sharpnessOut.Buffer.StructureByteStride = 0;
        uavDesc_sharpnessOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_sharpnessOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandleSharpnessOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavFriendOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_friendAndSharpnessBufferOut.Get(), nullptr, &uavDesc_sharpnessOut, uavHandleSharpnessOut);

        UINT valenceOutOffset = sharpnessOutOffset + sharpnessOutSize;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc_valenceOut = {};
        uavDesc_valenceOut.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc_valenceOut.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uavDesc_valenceOut.Buffer.FirstElement = 0;
        uavDesc_valenceOut.Buffer.NumElements = new_geometry.valenceStartInfos.size();
        uavDesc_valenceOut.Buffer.StructureByteStride = sizeof(int);
        uavDesc_valenceOut.Buffer.CounterOffsetInBytes = 0;
        uavDesc_valenceOut.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
        CD3DX12_CPU_DESCRIPTOR_HANDLE uavHandleValenceOut(m_srvUavHeap->GetCPUDescriptorHandleForHeapStart(), UavValenceOut, m_srvUavDescriptorSize);
        m_device->CreateUnorderedAccessView(m_valenceStartInfoBufferOut.Get(), nullptr, &uavDesc_valenceOut, uavHandleValenceOut);
        printf("posSize: %d, indexOffset: %d, indexSize: %d, sharpnessOffset: %d, sharpnessSize: %d, valenceOffset: %d, valenceSize: %d\n", posOutSize, indexOutOffset, indexOutSize, sharpnessOutOffset, sharpnessOutSize, valenceOutOffset, valenceOutSize);
    }

}

void EdgefriendDX12::ReadBack() {
    ThrowIfFailed(m_commandAllocator->Reset());
    ThrowIfFailed(m_computeCommandList->Reset(m_commandAllocator.Get(), nullptr));

    D3D12_HEAP_PROPERTIES readbackHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);

    UINT posSize = new_geometry.positions.size() * sizeof(glm::vec3);
    UINT indexOffset = posSize;
    UINT indexSize = new_geometry.indices.size() * sizeof(int);
    UINT sharpnessOffset = indexOffset + indexSize;
    UINT sharpnessSize = new_geometry.friendsAndSharpnesses.size() * sizeof(glm::uvec4);
    UINT valenceOffset = sharpnessOffset + sharpnessSize;
    UINT valenceSize = new_geometry.valenceStartInfos.size() * sizeof(int);

    printf("posSize: %d, indexOffset: %d, indexSize: %d, sharpnessOffset: %d, sharpnessSize: %d, valenceOffset: %d, valenceSize: %d\n", posSize, indexOffset, indexSize, sharpnessOffset, sharpnessSize, valenceOffset, valenceSize);

    UINT totalSize = (posSize + indexSize + sharpnessSize + valenceSize + 255) & ~255;

    D3D12_RESOURCE_DESC readbackBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalSize);

    ThrowIfFailed(m_device->CreateCommittedResource(
        &readbackHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &readbackBufferDesc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&m_readbackHeap)));
    NAME_D3D12_OBJECT(m_readbackHeap);

    // 拷贝资源到 readbackHeap
    {
        m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), 0, m_positionBufferOut.Get(), 0, posSize);
        m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), indexOffset, m_indexBufferOut.Get(), 0, indexSize);
        m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), sharpnessOffset, m_friendAndSharpnessBufferOut.Get(), 0, sharpnessSize);
        m_computeCommandList->CopyBufferRegion(m_readbackHeap.Get(), valenceOffset, m_valenceStartInfoBufferOut.Get(), 0, valenceSize);
    }

    ThrowIfFailed(m_computeCommandList->Close());
    ID3D12CommandList* ppCommandLists[] = { m_computeCommandList.Get() };
    m_commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    WaitForRenderContext();


    // 把 readbackHeap 里的内容拷贝到 pReadbackHeapData
    void* pReadbackHeapData;
    ThrowIfFailed(m_readbackHeap->Map(0, nullptr, reinterpret_cast<void**>(&pReadbackHeapData)));
    memcpy(new_geometry.positions.data(), pReadbackHeapData, posSize);
    memcpy(new_geometry.indices.data(), reinterpret_cast<UINT8*>(pReadbackHeapData) + indexOffset, indexSize);
    memcpy(new_geometry.friendsAndSharpnesses.data(), reinterpret_cast<UINT8*>(pReadbackHeapData) + sharpnessOffset, sharpnessSize);
    memcpy(new_geometry.valenceStartInfos.data(), reinterpret_cast<UINT8*>(pReadbackHeapData) + valenceOffset, valenceSize);
    m_readbackHeap->Unmap(0, nullptr);
}

void EdgefriendDX12::WriteObj() {
    std::ofstream obj("output_1iter.obj");
    for (const auto& position : new_geometry.positions) {
        obj << "v " << position.x << ' ' << position.y << ' ' << position.z << '\n';
    }
    for (int i = 0; i < new_geometry.friendsAndSharpnesses.size(); ++i) {
        obj << 'f';
        for (int j = 0; j < 4; ++j) {
            obj << ' ' << new_geometry.indices[4 * i + j] + 1;
        }
        obj << '\n';
    }
    obj.close();
}
