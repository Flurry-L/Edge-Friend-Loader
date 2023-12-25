#include "dx.h"

using Microsoft::WRL::ComPtr;

void EdgefriendDX12::Init() {
    CreateDevice();
    CreateRootSignature();
    CreateComputePipelineStateObject(); 
    CreateComputeCommands();
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
    ComPtr<ID3D12DescriptorHeap> descHeap;
    D3D12_DESCRIPTOR_HEAP_DESC descHeapDesc = {};
    descHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    descHeapDesc.NumDescriptors = 9;
    descHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    descHeapDesc.NodeMask = 0;

    HRESULT hr = m_device->CreateDescriptorHeap(&descHeapDesc, IID_PPV_ARGS(&descHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to CreateDescriptorHeap." << std::endl;
        return;
    }
   /* UINT cbvSrvUavDescriptorSize = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    D3D12_CPU_DESCRIPTOR_HANDLE heapHandle = descHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle = heapHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = { cbvHandle.ptr + cbvSrvUavDescriptorSize };
    D3D12_CPU_DESCRIPTOR_HANDLE uavHandle = { srvHandle.ptr + (cbvSrvUavDescriptorSize * 4) };*/

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
