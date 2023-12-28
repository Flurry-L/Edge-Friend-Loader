#include "dx.h"

#include <stdexcept>

// Define the ThrowIfFailed macro.
#ifndef ThrowIfFailed
#define ThrowIfFailed(x)                                              \
{                                                                     \
    HRESULT hr__ = (x);                                               \
    std::wstring wfn = AnsiToWString(__FILE__);                       \
    if(FAILED(hr__)) { throw std::runtime_error(                      \
        FormatString("%s(%d) : HRESULT of 0x%08X",                    \
            wfn.c_str(), __LINE__, static_cast<UINT>(hr__)));         \
    }                                                                 \
}
#endif

// Utility function to convert ANSI string to wide string
std::wstring AnsiToWString(const std::string& str) {
    std::wstring wide_str(str.begin(), str.end());
    return wide_str;
}

// Simple string formatting function
template<typename... Args>
std::string FormatString(const char* format, Args... args) {
    size_t size = snprintf(nullptr, 0, format, args...) + 1; // Extra space for '\0'
    std::unique_ptr<char[]> buf(new char[size]);
    snprintf(buf.get(), size, format, args...);
    return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

using Microsoft::WRL::ComPtr;

void EdgefriendDX12::Init() {
    CreateDevice();
    CreateRootSignature();
    CreateComputePipelineStateObject(); 
    CreateComputeCommands();
    CreateDescriptorHeap();
    UploadDataAndCreateView();
    Compute();
}

void EdgefriendDX12::CreateDevice() {
    ComPtr<IDXGIFactory4> dxgiFactory;
    CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory));

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
}

void EdgefriendDX12::CreateRootSignature()
{
    // Define the root parameters for the root signature.
    CD3DX12_ROOT_PARAMETER parameters[9];

    // Define a single constant buffer view (b0)
    parameters[0].InitAsConstantBufferView(0);

    // Define SRVs (t0 - t3)
    parameters[1].InitAsShaderResourceView(0); // positionBufferIn: register(t0)
    parameters[2].InitAsShaderResourceView(1); // indexBufferIn: register(t1)
    parameters[3].InitAsShaderResourceView(2); // friendAndSharpnessBufferIn: register(t2)
    parameters[4].InitAsShaderResourceView(3); // valenceStartInfoBufferIn: register(t3)

    // Define UAVs (u0 - u3)
    parameters[5].InitAsUnorderedAccessView(0); // positionBufferOut: register(u0)
    parameters[6].InitAsUnorderedAccessView(1); // indexBufferOut: register(u1)
    parameters[7].InitAsUnorderedAccessView(2); // friendAndSharpnessBufferOut: register(u2)
    parameters[8].InitAsUnorderedAccessView(3); // valenceStartInfoBufferOut: register(u3)

    // Define the root signature description with the parameters and flags.
    CD3DX12_ROOT_SIGNATURE_DESC descRootSignature;
    descRootSignature.Init(_countof(parameters), parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE);

    // Serialize and create the root signature.
    ComPtr<ID3DBlob> rootBlob;
    ComPtr<ID3DBlob> errorBlob;
    HRESULT hr = D3D12SerializeRootSignature(&descRootSignature,
        D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errorBlob);

    // Check for errors during serialization.
    if (FAILED(hr))
    {
        // Handle error, possibly outputting the error message from errorBlob
        return;
    }

    // Create the root signature.
    hr = m_device->CreateRootSignature(0,
        rootBlob->GetBufferPointer(),
        rootBlob->GetBufferSize(), IID_PPV_ARGS(&m_rootSignature));

    // Check for errors during root signature creation.
    if (FAILED(hr))
    {
        // Handle error
        return;
    }
}

void EdgefriendDX12::CreateComputePipelineStateObject() {
    std::ifstream shaderFile("C:/Users/17480/Desktop/dx12/hlsl/edgefriend.hlsl");
    if (!shaderFile.is_open()) {
        std::cerr << "Failed to open hlsl." << std::endl;
        return;
    }
    std::stringstream shaderStream;
    shaderStream << shaderFile.rdbuf();
    std::string shaderCode = shaderStream.str();
    std::cout << shaderCode;

    ComPtr<ID3DBlob> computeShader;
    D3DCompile(shaderCode.data(), shaderCode.size(),
        nullptr, nullptr, nullptr, 
        "CSEdgefriend", "cs_5_0", 0, 0, &computeShader, nullptr);
    //std::cout << shaderCode;

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.CS.BytecodeLength = computeShader->GetBufferSize();
    psoDesc.CS.pShaderBytecode = computeShader->GetBufferPointer();
    psoDesc.pRootSignature = m_rootSignature.Get();

    m_device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&m_pipelineState));
}

void EdgefriendDX12::CreateComputeCommands() {
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;

    HRESULT hr = m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_computeCommandQueue));
    if (FAILED(hr)) {
        std::cerr << "Failed to create command queue. HRESULT: " << std::hex << hr << std::endl;
    }

    hr = m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&m_commandAllocator));
    if (FAILED(hr)) {
        std::cerr << "Failed to create command allocator. HRESULT: " << std::hex << hr << std::endl;
    }

    hr = m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_commandAllocator.Get(), nullptr, IID_PPV_ARGS(&m_computeCommandList));
    if (FAILED(hr)) {
        std::cerr << "Failed to create command list. HRESULT: " << std::hex << hr << std::endl;
    }

    // Close the command list now. It will be reset before recording commands.
    m_computeCommandList->Close();
}

// 创建并更新常量缓冲区的函数
// 创建并更新常量缓冲区的函数
void EdgefriendDX12::CreateCBV(D3D12_CPU_DESCRIPTOR_HANDLE handle, OldSizeConstants& constants)
{
    D3D12_HEAP_PROPERTIES heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    D3D12_RESOURCE_DESC resourceDesc = CD3DX12_RESOURCE_DESC::Buffer((sizeof(OldSizeConstants) + 255) & ~255); // Align to 256-byte boundaries
    ThrowIfFailed(m_device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&m_constantBuffer)));

    // 更新常量缓冲区
    OldSizeConstants* mappedConstants;
    CD3DX12_RANGE readRange(0, 0); // 不打算从CPU读取
    ThrowIfFailed(m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedConstants)));
    *mappedConstants = constants;
    m_constantBuffer->Unmap(0, nullptr);

    // 创建CBV
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
    cbvDesc.BufferLocation = m_constantBuffer->GetGPUVirtualAddress();
    cbvDesc.SizeInBytes = (sizeof(OldSizeConstants) + 255) & ~255; // Size must be 256-byte aligned

    // 在描述符堆中创建CBV
    m_device->CreateConstantBufferView(&cbvDesc, handle);

}

void EdgefriendDX12::CreateDescriptorHeap() {
    D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc = {};
    descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descHeapDesc.NumDescriptors = 9;
    descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descHeapDesc.NodeMask = 0;

    HRESULT hr = m_device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&m_descHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to CreateDescriptorHeap." << std::endl;
        return;
    }
    
}

// Helper function to create a buffer resource
void CreateBufferResource(ID3D12Device* device,
    UINT64 byteSize,
    D3D12_RESOURCE_FLAGS flags,
    D3D12_HEAP_TYPE heapType,
    ID3D12Resource** ppResource,
    D3D12_RESOURCE_STATES initialState) {
    auto heapProperties = CD3DX12_HEAP_PROPERTIES(heapType);
    auto resourceDesc = CD3DX12_RESOURCE_DESC::Buffer(byteSize, flags);

    ThrowIfFailed(device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &resourceDesc,
        initialState,
        nullptr,
        IID_PPV_ARGS(ppResource)));
}

// Helper function to create a SRV for a StructuredBuffer
void CreateStructuredBufferSRV(ID3D12Device* device,
    ID3D12Resource* resource,
    UINT numElements,
    UINT elementSize,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Format = DXGI_FORMAT_UNKNOWN;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = numElements;
    srvDesc.Buffer.StructureByteStride = elementSize;
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    device->CreateShaderResourceView(resource, &srvDesc, handle);
}

// Helper function to create a SRV for a ByteAddressBuffer
void CreateByteAddressBufferSRV(ID3D12Device* device,
    ID3D12Resource* resource,
    UINT64 bufferSize,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srvDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Buffer.NumElements = static_cast<UINT>(bufferSize / 4); // Size in bytes / 4
    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    device->CreateShaderResourceView(resource, &srvDesc, handle);
}

// Helper function to create a UAV for a StructuredBuffer
void CreateStructuredBufferUAV(ID3D12Device* device,
    ID3D12Resource* resource,
    UINT numElements,
    UINT elementSize,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.NumElements = numElements;
    uavDesc.Buffer.StructureByteStride = elementSize;
    uavDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured buffers always use DXGI_FORMAT_UNKNOWN.

    device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
}

// Helper function to create a UAV for a ByteAddressBuffer
void CreateByteAddressBufferUAV(ID3D12Device* device,
    ID3D12Resource* resource,
    UINT64 bufferSize,
    D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = (UINT)(bufferSize / 4); // ByteAddressBuffer elements are 4 bytes each.
    uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW; // ByteAddressBuffer requires the RAW flag.
    uavDesc.Format = DXGI_FORMAT_R32_TYPELESS; // Format must be DXGI_FORMAT_R32_TYPELESS for RAW views.

    device->CreateUnorderedAccessView(resource, nullptr, &uavDesc, handle);
}

void EdgefriendDX12::UploadDataAndCreateView() {
    UINT cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE heapHandle = m_descHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle = heapHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = { cbvHandle.ptr + cbvSrvUavDescriptorSize };
    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = { srvHandle.ptr + (cbvSrvUavDescriptorSize * 4) };

    OldSizeConstants constants;
    CreateCBV(cbvHandle, constants);
    CreateSRV(srvHandle, 1, 1, 1, 1);
    CreateUAV(uavHandle, 1, 1, 1, 1);

}


void EdgefriendDX12::CreateSRV(D3D12_CPU_DESCRIPTOR_HANDLE handle, int numPositionElements, int numIndexElements, int numFriendAndSharpnessElements, int numValenceStartInfoElements) {
    // Assume that m_device is ID3D12Device* and descriptor handles are initialized correctly

    UINT cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Assuming m_device is ID3D12Device* and we have descriptor handles for each UAV
    D3D12_CPU_DESCRIPTOR_HANDLE positionBufferHandle = handle; // Initialize with actual handle
    D3D12_CPU_DESCRIPTOR_HANDLE indexBufferHandle = { handle.ptr + cbvSrvUavDescriptorSize };    // Initialize with actual handle
    D3D12_CPU_DESCRIPTOR_HANDLE friendAndSharpnessBufferHandle = { handle.ptr + 2 * cbvSrvUavDescriptorSize }; // Initialize with actual handle
    D3D12_CPU_DESCRIPTOR_HANDLE valenceStartInfoBufferHandle = { handle.ptr + 3 * cbvSrvUavDescriptorSize };   // Initialize with actual handle

    // Sizes of the resources
    UINT64 positionBufferSize = sizeof(float3) * numPositionElements;
    UINT64 indexBufferSize = sizeof(UINT) * numIndexElements; // Assuming indexes are stored as UINT
    UINT64 friendAndSharpnessBufferSize = sizeof(UINT) * numFriendAndSharpnessElements;
    UINT64 valenceStartInfoBufferSize = sizeof(int) * numValenceStartInfoElements;

    CreateBufferResource(m_device, positionBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, &m_positionBufferIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CreateBufferResource(m_device, indexBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, &m_indexBufferIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CreateBufferResource(m_device, friendAndSharpnessBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, &m_friendAndSharpnessBufferIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    CreateBufferResource(m_device, valenceStartInfoBufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_HEAP_TYPE_DEFAULT, &m_valenceStartInfoBufferIn, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    // Create SRVs for each buffer
    CreateStructuredBufferSRV(m_device, m_positionBufferIn.Get(), numPositionElements, sizeof(float3), positionBufferHandle);
    CreateByteAddressBufferSRV(m_device, m_indexBufferIn.Get(), indexBufferSize, indexBufferHandle);
    CreateByteAddressBufferSRV(m_device, m_friendAndSharpnessBufferIn.Get(), friendAndSharpnessBufferSize, friendAndSharpnessBufferHandle);
    CreateStructuredBufferSRV(m_device, m_valenceStartInfoBufferIn.Get(), numValenceStartInfoElements, sizeof(int), valenceStartInfoBufferHandle);

    // Bind the SRVs to the pipeline

    // Assume that m_commandList is ID3D12GraphicsCommandList* and you have a root signature that matches this setup
    m_computeCommandList->SetGraphicsRootShaderResourceView(1, m_positionBufferIn->GetGPUVirtualAddress());
    m_computeCommandList->SetGraphicsRootShaderResourceView(2, m_indexBufferIn->GetGPUVirtualAddress());
    m_computeCommandList->SetGraphicsRootShaderResourceView(3, m_friendAndSharpnessBufferIn->GetGPUVirtualAddress());
    m_computeCommandList->SetGraphicsRootShaderResourceView(4, m_valenceStartInfoBufferIn->GetGPUVirtualAddress());

    // Now your buffers are set up and bound to the pipeline. You can now use them in your shaders.
}

void EdgefriendDX12::CreateUAV(D3D12_CPU_DESCRIPTOR_HANDLE handle, int numPositionElements, int numIndexElements, int numFriendAndSharpnessElements, int numValenceStartInfoElements) {
    UINT cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    // Assuming m_device is ID3D12Device* and we have descriptor handles for each UAV
    D3D12_CPU_DESCRIPTOR_HANDLE positionBufferHandle = handle; // Initialize with actual handle
    D3D12_CPU_DESCRIPTOR_HANDLE indexBufferHandle = { handle.ptr + cbvSrvUavDescriptorSize };    // Initialize with actual handle
    D3D12_CPU_DESCRIPTOR_HANDLE friendAndSharpnessBufferHandle = { handle.ptr + 2 * cbvSrvUavDescriptorSize }; // Initialize with actual handle
    D3D12_CPU_DESCRIPTOR_HANDLE valenceStartInfoBufferHandle = { handle.ptr + 3 * cbvSrvUavDescriptorSize };   // Initialize with actual handle

    // Size of the resources
    UINT64 positionBufferSize = sizeof(float3) * numPositionElements; // numPositionElements should be defined
    UINT64 indexBufferSize = sizeof(UINT) * numIndexElements;         // numIndexElements should be defined
    UINT64 friendAndSharpnessBufferSize = sizeof(UINT) * numFriendAndSharpnessElements; // numFriendAndSharpnessElements should be defined
    UINT64 valenceStartInfoBufferSize = sizeof(int) * numValenceStartInfoElements;      // numValenceStartInfoElements should be defined

    // Create buffer resources
    CreateBufferResource(m_device, positionBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT, &m_positionBufferOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CreateBufferResource(m_device, indexBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT, &m_indexBufferOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CreateBufferResource(m_device, friendAndSharpnessBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT, &m_friendAndSharpnessBufferOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CreateBufferResource(m_device, valenceStartInfoBufferSize, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_HEAP_TYPE_DEFAULT, &m_valenceStartInfoBufferOut, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Create UAVs for each buffer
    CreateStructuredBufferUAV(m_device, m_positionBufferOut.Get(), numPositionElements, sizeof(float3), positionBufferHandle);
    CreateByteAddressBufferUAV(m_device, m_indexBufferOut.Get(), indexBufferSize, indexBufferHandle);
    CreateByteAddressBufferUAV(m_device, m_friendAndSharpnessBufferOut.Get(), friendAndSharpnessBufferSize, friendAndSharpnessBufferHandle);
    CreateStructuredBufferUAV(m_device, m_valenceStartInfoBufferOut.Get(), numValenceStartInfoElements, sizeof(int), valenceStartInfoBufferHandle);

    // Now you have created UAVs for the structured and byte address buffers
    // You would bind these to the pipeline as needed before dispatching your compute shader or drawing with your pipeline
}

//void EdgefriendDX12::CreateCBV(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
//    // constantBuffer
//    CBuffer cbufferdata = {};
//    auto bufferSize = sizeof(CBuffer);
//    ThrowIfFailed(m_device->CreateCommittedResource(
//        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), D3D12_HEAP_FLAG_NONE,
//        &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
//        D3D12_RESOURCE_STATE_GENERIC_READ,
//        nullptr,
//        IID_PPV_ARGS(&m_constantBuffer))
//    );
//    UINT8* pDataBegin = nullptr;
//    CD3DX12_RANGE readRange(0, 0);
//    ThrowIfFailed(m_constantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&pDataBegin)));
//    memcpy(pDataBegin, &cbufferdata, sizeof(CBuffer));
//    m_constantBuffer->Unmap(0, nullptr);
//
//
//    // Step 2: Create the constant buffer view (CBV)
//    D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
//    cbvDesc.BufferLocation = m_constantBuffer->GetGPUVirtualAddress();
//    // Ensure the constant buffer size is 256-byte aligned
//    cbvDesc.SizeInBytes = (sizeof(CBuffer) + (D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1)) & ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);
//    m_device->CreateConstantBufferView(&cbvDesc, handle);
//
//    /*m_computeCommandList->SetDescriptorHeaps(1, cbvHeap.GetAddressOf());
//    m_computeCommandList->SetGraphicsRootDescriptorTable(0, cbvHeap->GetGPUDescriptorHandleForHeapStart());*/
//}
//
//void EdgefriendDX12::CreateSRVpos(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
//    UINT8* pDataBegin;
//    // positionBufferIn
//    std::vector<float3> positionDataIn = {};
//    // Create a committed resource for the input structured buffer
//    positionDataIn.push_back({ 1.f, 2.f, 3.f });
//    UINT64 positionBufferSize = sizeof(float3) * positionDataIn.size(); // Calculate the needed size
//    ThrowIfFailed(m_device->CreateCommittedResource(
//        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // Upload heap
//        D3D12_HEAP_FLAG_NONE,
//        &CD3DX12_RESOURCE_DESC::Buffer(positionBufferSize),
//        D3D12_RESOURCE_STATE_GENERIC_READ,
//        nullptr,
//        IID_PPV_ARGS(&m_positionBufferIn))
//    );
//    CD3DX12_RANGE readRange_1(0, 0); // We do not intend to read from this resource on the CPU.
//    ThrowIfFailed(m_positionBufferIn->Map(0, &readRange_1, reinterpret_cast<void**>(&pDataBegin)));
//    memcpy(pDataBegin, positionDataIn.data(), positionBufferSize);
//    m_positionBufferIn->Unmap(0, nullptr);
//
//    // Step 1: Create a descriptor heap for SRV
//    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
//    srvHeapDesc.NumDescriptors = 1; // Increase this if you have more descriptors.
//    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
//    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
//    ComPtr<ID3D12DescriptorHeap> srvHeap;
//    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)));
//
//    // Step 2: Create the shader resource view (SRV) for the structured buffer
//    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
//    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
//    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured buffer SRVs should have the format set to DXGI_FORMAT_UNKNOWN.
//    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
//    srvDesc.Buffer.NumElements = static_cast<UINT>(positionDataIn.size());
//    srvDesc.Buffer.StructureByteStride = sizeof(float3); // The size of each element in the buffer.
//    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
//    m_device->CreateShaderResourceView(m_positionBufferIn.Get(), &srvDesc, handle);
//
//    // Step 3: Bind the SRV to the pipeline (usually done as part of your rendering or compute loop)
//    /*m_computeCommandList->SetDescriptorHeaps(1, srvHeap.GetAddressOf());
//    m_computeCommandList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());*/
//}
//
//void EdgefriendDX12::CreateSRVindex(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
//    UINT8* pDataBegin;
//    // indexBufferIn
//    std::vector<char> indexDataIn = {};
//    indexDataIn.push_back(0);
//    // Create a committed resource for the input structured buffer
//    
//    UINT64 indexBufferSize = sizeof(char) * indexDataIn.size(); // Calculate the needed size
//    ThrowIfFailed(m_device->CreateCommittedResource(
//        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // Upload heap
//        D3D12_HEAP_FLAG_NONE,
//        &CD3DX12_RESOURCE_DESC::Buffer(indexBufferSize),
//        D3D12_RESOURCE_STATE_GENERIC_READ,
//        nullptr,
//        IID_PPV_ARGS(&m_indexBufferIn))
//    );
//    CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
//    ThrowIfFailed(m_indexBufferIn->Map(0, &readRange, reinterpret_cast<void**>(&pDataBegin)));
//    memcpy(pDataBegin, indexDataIn.data(), indexBufferSize);
//    m_indexBufferIn->Unmap(0, nullptr);
//
//    // Step 1: Create a descriptor heap for SRV
//    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
//    srvHeapDesc.NumDescriptors = 1; // Increase this if you have more descriptors.
//    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
//    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
//    ComPtr<ID3D12DescriptorHeap> srvHeap;
//    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)));
//
//    // Step 2: Create the shader resource view (SRV) for the structured buffer
//    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
//    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
//    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured buffer SRVs should have the format set to DXGI_FORMAT_UNKNOWN.
//    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
//    srvDesc.Buffer.NumElements = static_cast<UINT>(indexDataIn.size());
//    srvDesc.Buffer.StructureByteStride = sizeof(char); // The size of each element in the buffer.
//    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
//    m_device->CreateShaderResourceView(m_indexBufferIn.Get(), &srvDesc, handle);
//
//    // Step 3: Bind the SRV to the pipeline (usually done as part of your rendering or compute loop)
//    /*m_computeCommandList->SetDescriptorHeaps(1, srvHeap.GetAddressOf());
//    m_computeCommandList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());*/
//}
//
//void EdgefriendDX12::CreateSRVfriend(D3D12_CPU_DESCRIPTOR_HANDLE handle){
//    UINT8* pDataBegin;
//    // friendAndSharpnessBufferIn
//    std::vector<char> friendAndSharpnessBufferIn = {};
//    // Create a committed resource for the input structured buffer
//    friendAndSharpnessBufferIn.push_back(0);
//    UINT64 friendAndSharpnessBufferSize = sizeof(char) * friendAndSharpnessBufferIn.size(); // Calculate the needed size
//    ThrowIfFailed(m_device->CreateCommittedResource(
//        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // Upload heap
//        D3D12_HEAP_FLAG_NONE,
//        &CD3DX12_RESOURCE_DESC::Buffer(friendAndSharpnessBufferSize),
//        D3D12_RESOURCE_STATE_GENERIC_READ,
//        nullptr,
//        IID_PPV_ARGS(&m_friendAndSharpnessBufferIn))
//    );
//    CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
//    ThrowIfFailed(m_friendAndSharpnessBufferIn->Map(0, &readRange, reinterpret_cast<void**>(&pDataBegin)));
//    memcpy(pDataBegin, friendAndSharpnessBufferIn.data(), friendAndSharpnessBufferSize);
//    m_friendAndSharpnessBufferIn->Unmap(0, nullptr);
//
//    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
//    srvHeapDesc.NumDescriptors = 1; // Increase this if you have more descriptors.
//    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
//    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
//    ComPtr<ID3D12DescriptorHeap> srvHeap;
//    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)));
//
//    // Step 2: Create the shader resource view (SRV) for the structured buffer
//    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
//    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
//    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured buffer SRVs should have the format set to DXGI_FORMAT_UNKNOWN.
//    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
//    srvDesc.Buffer.NumElements = static_cast<UINT>(friendAndSharpnessBufferIn.size());
//    srvDesc.Buffer.StructureByteStride = sizeof(char); // The size of each element in the buffer.
//    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
//    m_device->CreateShaderResourceView(m_friendAndSharpnessBufferIn.Get(), &srvDesc, handle);
//
//    // Step 3: Bind the SRV to the pipeline (usually done as part of your rendering or compute loop)
//    /*m_computeCommandList->SetDescriptorHeaps(1, srvHeap.GetAddressOf());
//    m_computeCommandList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());*/
//}
//
//void EdgefriendDX12::CreateSRVvalence(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
//    UINT8* pDataBegin;
//    // valenceStartInfoBufferIn
//    std::vector<int> valenceStartInfoBufferIn = {};
//    // Create a committed resource for the input structured buffer
//    valenceStartInfoBufferIn.push_back(0);
//    UINT64 valenceStartInfoBufferSize = sizeof(int) * valenceStartInfoBufferIn.size(); // Calculate the needed size
//    ThrowIfFailed(m_device->CreateCommittedResource(
//        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD), // Upload heap
//        D3D12_HEAP_FLAG_NONE,
//        &CD3DX12_RESOURCE_DESC::Buffer(valenceStartInfoBufferSize),
//        D3D12_RESOURCE_STATE_GENERIC_READ,
//        nullptr,
//        IID_PPV_ARGS(&m_valenceStartInfoBufferIn))
//    );
//    CD3DX12_RANGE readRange(0, 0); // We do not intend to read from this resource on the CPU.
//    ThrowIfFailed(m_valenceStartInfoBufferIn->Map(0, &readRange, reinterpret_cast<void**>(&pDataBegin)));
//    memcpy(pDataBegin, valenceStartInfoBufferIn.data(), valenceStartInfoBufferSize);
//    m_valenceStartInfoBufferIn->Unmap(0, nullptr);
//
//    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
//    srvHeapDesc.NumDescriptors = 1; // Increase this if you have more descriptors.
//    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
//    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
//    ComPtr<ID3D12DescriptorHeap> srvHeap;
//    ThrowIfFailed(m_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&srvHeap)));
//
//    // Step 2: Create the shader resource view (SRV) for the structured buffer
//    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
//    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
//    srvDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured buffer SRVs should have the format set to DXGI_FORMAT_UNKNOWN.
//    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
//    srvDesc.Buffer.NumElements = static_cast<UINT>(valenceStartInfoBufferIn.size());
//    srvDesc.Buffer.StructureByteStride = sizeof(int); // The size of each element in the buffer.
//    srvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
//    m_device->CreateShaderResourceView(m_valenceStartInfoBufferIn.Get(), &srvDesc, handle);
//
//    // Step 3: Bind the SRV to the pipeline (usually done as part of your rendering or compute loop)
//    /*m_computeCommandList->SetDescriptorHeaps(1, srvHeap.GetAddressOf());
//    m_computeCommandList->SetGraphicsRootDescriptorTable(1, srvHeap->GetGPUDescriptorHandleForHeapStart());*/
//}

void EdgefriendDX12::Compute() {
    UINT cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto gpu_cbv_handle = m_descHeap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv_handle = { gpu_cbv_handle.ptr + cbvSrvUavDescriptorSize };
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv_handle1 = { gpu_cbv_handle.ptr + cbvSrvUavDescriptorSize * 2 };
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv_handle2 = { gpu_cbv_handle.ptr + cbvSrvUavDescriptorSize * 3 };
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_srv_handle3 = { gpu_cbv_handle.ptr + cbvSrvUavDescriptorSize * 4 };
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_uav_handle = { gpu_cbv_handle.ptr + cbvSrvUavDescriptorSize * 5 };
    m_computeCommandList->SetComputeRootDescriptorTable(0, gpu_cbv_handle);
    m_computeCommandList->SetComputeRootDescriptorTable(1, gpu_srv_handle);
    m_computeCommandList->SetComputeRootDescriptorTable(2, gpu_srv_handle1);
    m_computeCommandList->SetComputeRootDescriptorTable(3, gpu_srv_handle2);
    m_computeCommandList->SetComputeRootDescriptorTable(4, gpu_srv_handle3);             

    //m_computeCommandList->SetComputeRootSignature(m_rootSignature);
}
