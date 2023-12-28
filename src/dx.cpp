#include "dx.h"
#include "DXSampleHelper.h"

#include <stdexcept>

// Define the ThrowIfFailed macro.
//#ifndef ThrowIfFailed
//#define ThrowIfFailed(x)                                              \
//{                                                                     \
//    HRESULT hr__ = (x);                                               \
//    std::wstring wfn = AnsiToWString(__FILE__);                       \
//    if(FAILED(hr__)) { throw std::runtime_error(                      \
//        FormatString("%s(%d) : HRESULT of 0x%08X",                    \
//            wfn.c_str(), __LINE__, static_cast<UINT>(hr__)));         \
//    }                                                                 \
//}
//#endif
//
//// Utility function to convert ANSI string to wide string
//std::wstring AnsiToWString(const std::string& str) {
//    std::wstring wide_str(str.begin(), str.end());
//    return wide_str;
//}
//
//// Simple string formatting function
//template<typename... Args>
//std::string FormatString(const char* format, Args... args) {
//    size_t size = snprintf(nullptr, 0, format, args...) + 1; // Extra space for '\0'
//    std::unique_ptr<char[]> buf(new char[size]);
//    snprintf(buf.get(), size, format, args...);
//    return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
//}

using Microsoft::WRL::ComPtr;

void EdgefriendDX12::Init() {
    LoadPipeline();
    LoadAssets();
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

}

