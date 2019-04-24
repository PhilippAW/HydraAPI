#include "RenderDriverDXRExperimental.h"
#include "LiteMath.h"

using namespace HydraLiteMath;

#include "RTX/d3dx12.h"
#include <DirectXMath.h>
#include <iostream>
#include "RTX/Externals/GLM/glm/gtc/matrix_transform.hpp"

extern HWND mainWindowHWND;
using namespace glm;
static dxc::DxcDllSupport gDxcDllHelper;

///////////////////////////////////////////////////////////////////////////////////////////////////
//                            Tutorial stuff
///////////////////////////////////////////////////////////////////////////////////////////////////
IDXGISwapChain3Ptr createDxgiSwapChain(IDXGIFactory4Ptr pFactory, HWND hwnd, uint32_t width, uint32_t height, DXGI_FORMAT format, ID3D12CommandQueuePtr pCommandQueue)
{
  DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
  swapChainDesc.BufferCount = kDefaultSwapChainBuffers;
  swapChainDesc.Width = width;
  swapChainDesc.Height = height;
  swapChainDesc.Format = format;
  swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swapChainDesc.SampleDesc.Count = 1;

  // CreateSwapChainForHwnd() doesn't accept IDXGISwapChain3 (Why MS? Why?)
  MAKE_SMART_COM_PTR(IDXGISwapChain1);
  IDXGISwapChain1Ptr pSwapChain;

  HRESULT hr = pFactory->CreateSwapChainForHwnd(pCommandQueue, hwnd, &swapChainDesc, nullptr, nullptr, &pSwapChain);
  if (FAILED(hr))
  {
    d3dTraceHR("Failed to create the swap-chain", hr);
    return false;
  }

  IDXGISwapChain3Ptr pSwapChain3;
  d3d_call(pSwapChain->QueryInterface(IID_PPV_ARGS(&pSwapChain3)));
  return pSwapChain3;
}

ID3D12Device5Ptr createDevice(IDXGIFactory4Ptr pDxgiFactory)
{
  // Find the HW adapter
  IDXGIAdapter1Ptr pAdapter;

  for (uint32_t i = 0; DXGI_ERROR_NOT_FOUND != pDxgiFactory->EnumAdapters1(i, &pAdapter); i++)
  {
    DXGI_ADAPTER_DESC1 desc;
    pAdapter->GetDesc1(&desc);

    // Skip SW adapters
    if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
#ifdef _DEBUG
    ID3D12DebugPtr pDx12Debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDx12Debug))))
    {
      pDx12Debug->EnableDebugLayer();
    }
#endif
    // Create the device
    ID3D12Device5Ptr pDevice;
    d3d_call(D3D12CreateDevice(pAdapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&pDevice)));

    D3D12_FEATURE_DATA_D3D12_OPTIONS5 features5;
    HRESULT hr = pDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features5, sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS5));
    if (FAILED(hr) || features5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED)
    {
      msgBox("Raytracing is not supported on this device. Make sure your GPU supports DXR (such as Nvidia's Volta or Turing RTX) and you're on the latest drivers. The DXR fallback layer is not supported.");
      exit(1);
    }
    return pDevice;
  }
  return nullptr;
}

ID3D12CommandQueuePtr createCommandQueue(ID3D12Device5Ptr pDevice)
{
  ID3D12CommandQueuePtr pQueue;
  D3D12_COMMAND_QUEUE_DESC cqDesc = {};
  cqDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  cqDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  d3d_call(pDevice->CreateCommandQueue(&cqDesc, IID_PPV_ARGS(&pQueue)));
  return pQueue;
}

ID3D12DescriptorHeapPtr createDescriptorHeap(ID3D12Device5Ptr pDevice, uint32_t count, D3D12_DESCRIPTOR_HEAP_TYPE type, bool shaderVisible)
{
  D3D12_DESCRIPTOR_HEAP_DESC desc = {};
  desc.NumDescriptors = count;
  desc.Type = type;
  desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

  ID3D12DescriptorHeapPtr pHeap;
  d3d_call(pDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&pHeap)));
  return pHeap;
}

D3D12_CPU_DESCRIPTOR_HANDLE createRTV(ID3D12Device5Ptr pDevice, ID3D12ResourcePtr pResource, ID3D12DescriptorHeapPtr pHeap, uint32_t& usedHeapEntries, DXGI_FORMAT format)
{
  D3D12_RENDER_TARGET_VIEW_DESC desc = {};
  desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  desc.Format = format;
  desc.Texture2D.MipSlice = 0;
  D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = pHeap->GetCPUDescriptorHandleForHeapStart();
  rtvHandle.ptr += usedHeapEntries * pDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  usedHeapEntries++;
  pDevice->CreateRenderTargetView(pResource, &desc, rtvHandle);
  return rtvHandle;
}

void resourceBarrier(ID3D12GraphicsCommandList4Ptr pCmdList, ID3D12ResourcePtr pResource, D3D12_RESOURCE_STATES stateBefore, D3D12_RESOURCE_STATES stateAfter)
{
  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = pResource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = stateBefore;
  barrier.Transition.StateAfter = stateAfter;
  pCmdList->ResourceBarrier(1, &barrier);
}

uint64_t submitCommandList(ID3D12GraphicsCommandList4Ptr pCmdList, ID3D12CommandQueuePtr pCmdQueue, ID3D12FencePtr pFence, uint64_t fenceValue)
{
  pCmdList->Close();
  ID3D12CommandList* pGraphicsList = pCmdList.GetInterfacePtr();
  pCmdQueue->ExecuteCommandLists(1, &pGraphicsList);
  fenceValue++;
  pCmdQueue->Signal(pFence, fenceValue);
  return fenceValue;
}

void RD_DXR_Experimental::initDXR(HWND winHandle, uint32_t winWidth, uint32_t winHeight)
{
  mHwnd = winHandle;
  mSwapChainSize = uvec2(winWidth, winHeight);

  // Initialize the debug layer for debug builds
#ifdef _DEBUG
  ID3D12DebugPtr pDebug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pDebug))))
  {
    pDebug->EnableDebugLayer();
  }
#endif
  // Create the DXGI factory
  IDXGIFactory4Ptr pDxgiFactory;
  d3d_call(CreateDXGIFactory1(IID_PPV_ARGS(&pDxgiFactory)));
  mpDevice = createDevice(pDxgiFactory);
  mpCmdQueue = createCommandQueue(mpDevice);
  mpSwapChain = createDxgiSwapChain(pDxgiFactory, mHwnd, winWidth, winHeight, DXGI_FORMAT_R8G8B8A8_UNORM, mpCmdQueue);

  // Create a RTV descriptor heap
  mRtvHeap.pHeap = createDescriptorHeap(mpDevice, kRtvHeapSize, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, false);

  // Create the per-frame objects
  for (uint32_t i = 0; i < arraysize(mFrameObjects); i++)
  {
    d3d_call(mpDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&mFrameObjects[i].pCmdAllocator)));
    d3d_call(mpSwapChain->GetBuffer(i, IID_PPV_ARGS(&mFrameObjects[i].pSwapChainBuffer)));
    mFrameObjects[i].rtvHandle = createRTV(mpDevice, mFrameObjects[i].pSwapChainBuffer, mRtvHeap.pHeap, mRtvHeap.usedEntries, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
  }

  // Create the command-list
  d3d_call(mpDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, mFrameObjects[0].pCmdAllocator, nullptr, IID_PPV_ARGS(&mpCmdList)));

  // Create a fence and the event
  d3d_call(mpDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&mpFence)));
  mFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
}

uint32_t RD_DXR_Experimental::beginFrame()
{
  // Bind the descriptor heaps
  ID3D12DescriptorHeap* heaps[] = { mpSrvUavHeap };
  mpCmdList->SetDescriptorHeaps(arraysize(heaps), heaps);
  return mpSwapChain->GetCurrentBackBufferIndex();
}

void RD_DXR_Experimental::endFrame(uint32_t rtvIndex)
{
  resourceBarrier(mpCmdList, mFrameObjects[rtvIndex].pSwapChainBuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  mFenceValue = submitCommandList(mpCmdList, mpCmdQueue, mpFence, mFenceValue);
  mpSwapChain->Present(0, 0);

  // Prepare the command list for the next frame
  uint32_t bufferIndex = mpSwapChain->GetCurrentBackBufferIndex();

  // Make sure we have the new back-buffer is ready
  if (mFenceValue > kDefaultSwapChainBuffers)
  {
    mpFence->SetEventOnCompletion(mFenceValue - kDefaultSwapChainBuffers + 1, mFenceEvent);
    WaitForSingleObject(mFenceEvent, INFINITE);
  }

  mFrameObjects[bufferIndex].pCmdAllocator->Reset();
  mpCmdList->Reset(mFrameObjects[bufferIndex].pCmdAllocator, nullptr);
}

//////////////////////////////////////////////////////////////////////////
// Tutorial 03 code
//////////////////////////////////////////////////////////////////////////
static const D3D12_HEAP_PROPERTIES kUploadHeapProps =
{
    D3D12_HEAP_TYPE_UPLOAD,
    D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    D3D12_MEMORY_POOL_UNKNOWN,
    0,
    0,
};

static const D3D12_HEAP_PROPERTIES kDefaultHeapProps =
{
    D3D12_HEAP_TYPE_DEFAULT,
    D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
    D3D12_MEMORY_POOL_UNKNOWN,
    0,
    0
};

ID3D12ResourcePtr createBuffer(ID3D12Device5Ptr pDevice, uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES initState, const D3D12_HEAP_PROPERTIES& heapProps)
{
  D3D12_RESOURCE_DESC bufDesc = {};
  bufDesc.Alignment = 0;
  bufDesc.DepthOrArraySize = 1;
  bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bufDesc.Flags = flags;
  bufDesc.Format = DXGI_FORMAT_UNKNOWN;
  bufDesc.Height = 1;
  bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  bufDesc.MipLevels = 1;
  bufDesc.SampleDesc.Count = 1;
  bufDesc.SampleDesc.Quality = 0;
  bufDesc.Width = size;

  ID3D12ResourcePtr pBuffer;
  d3d_call(pDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc, initState, nullptr, IID_PPV_ARGS(&pBuffer)));
  return pBuffer;
}


template<typename T>
ID3D12ResourcePtr createVB(ID3D12Device5Ptr pDevice, vector<T> mesh)
{
  size_t bufferSize = mesh.size() * sizeof(mesh[0]);

  // For simplicity, we create the vertex buffer on the upload heap, but that's not required
  ID3D12ResourcePtr pBuffer = createBuffer(pDevice, bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);
  uint8_t* pData;
  pBuffer->Map(0, nullptr, (void**)&pData);
  memcpy(pData, mesh.data(), bufferSize);
  pBuffer->Unmap(0, nullptr);
  return pBuffer;
}

ID3D12ResourcePtr createTriangleVB(ID3D12Device5Ptr pDevice)
{
  vector<vec3> vertices =
  {
      vec3(0,          1,  0),
      vec3(0.866f,  -0.5f, 0),
      vec3(-0.866f, -0.5f, 0),
  };

  return createVB(pDevice, vertices);
}

ID3D12ResourcePtr createPlaneVB(ID3D12Device5Ptr pDevice)
{
  vector<vec3> vertices =
  {
      vec3(-100, -1,  -2),
      vec3(100, -1,  100),
      vec3(-100, -1,  100),

      vec3(-100, -1,  -2),
      vec3(100, -1,  -2),
      vec3(100, -1,  100),
  };

  return createVB(pDevice, vertices);
}

AccelerationStructureBuffers createBottomLevelAS(ID3D12Device5Ptr pDevice, ID3D12GraphicsCommandList4Ptr pCmdList, pair<ID3D12ResourcePtr, ID3D12ResourcePtr> pVB[], const pair<uint32_t, uint32_t> vertexCount[], uint32_t geometryCount)
{
  std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geomDesc;
  geomDesc.resize(geometryCount);

  for (uint32_t i = 0; i < geometryCount; i++)
  {
    geomDesc[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geomDesc[i].Triangles.VertexBuffer.StartAddress = get<0>(pVB[i])->GetGPUVirtualAddress();
    geomDesc[i].Triangles.VertexBuffer.StrideInBytes = sizeof(Vertex);
    geomDesc[i].Triangles.IndexBuffer = get<1>(pVB[i])->GetGPUVirtualAddress();
    geomDesc[i].Triangles.IndexCount = get<1>(vertexCount[i]);
    geomDesc[i].Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    geomDesc[i].Triangles.VertexCount = get<0>(vertexCount[i]);
    geomDesc[i].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geomDesc[i].Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
  }

  // Get the size requirements for the scratch and AS buffers
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
  inputs.NumDescs = geometryCount;
  inputs.pGeometryDescs = geomDesc.data();
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
  pDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

  // Create the buffers. They need to support UAV, and since we are going to immediately use them, we create them with an unordered-access state
  AccelerationStructureBuffers buffers;
  buffers.pScratch = createBuffer(pDevice, info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, kDefaultHeapProps);
  buffers.pResult = createBuffer(pDevice, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, kDefaultHeapProps);

  // Create the bottom-level AS
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
  asDesc.Inputs = inputs;
  asDesc.DestAccelerationStructureData = buffers.pResult->GetGPUVirtualAddress();
  asDesc.ScratchAccelerationStructureData = buffers.pScratch->GetGPUVirtualAddress();

  pCmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

  // We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
  D3D12_RESOURCE_BARRIER uavBarrier = {};
  uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = buffers.pResult;
  pCmdList->ResourceBarrier(1, &uavBarrier);

  return buffers;
}

AccelerationStructureBuffers createSingleGeomBottomLevelAS(ID3D12Device5Ptr pDevice, ID3D12GraphicsCommandList4Ptr pCmdList, pair<ID3D12ResourcePtr, ID3D12ResourcePtr> pVB, const pair<uint32_t, uint32_t> vertexCount) {
  pair<ID3D12ResourcePtr, ID3D12ResourcePtr> b[] = { pVB };
  return createBottomLevelAS(pDevice, pCmdList, b, &vertexCount, 1);
}

AccelerationStructureBuffers createTopLevelAS(ID3D12Device5Ptr pDevice, ID3D12GraphicsCommandList4Ptr pCmdList, vector<ID3D12ResourcePtr> &pBottomLevelAS, vector<Instance> instances, uint64_t& tlasSize)
{
  // First, get the size of the TLAS buffers and create them
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {};
  inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
  inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_NONE;
  inputs.NumDescs = instances.size();
  inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;

  D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
  pDevice->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);

  // Create the buffers
  AccelerationStructureBuffers buffers;
  buffers.pScratch = createBuffer(pDevice, info.ScratchDataSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, kDefaultHeapProps);
  buffers.pResult = createBuffer(pDevice, info.ResultDataMaxSizeInBytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, kDefaultHeapProps);
  tlasSize = info.ResultDataMaxSizeInBytes;

  // The instance desc should be inside a buffer, create and map the buffer
  buffers.pInstanceDesc = createBuffer(pDevice, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instances.size(), D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);
  D3D12_RAYTRACING_INSTANCE_DESC* instanceDescs;
  buffers.pInstanceDesc->Map(0, nullptr, (void**)&instanceDescs);
  ZeroMemory(instanceDescs, sizeof(D3D12_RAYTRACING_INSTANCE_DESC) * instances.size());

  uint32_t i = 0;
  for (auto inst : instances)
  {
    instanceDescs[i].InstanceID = i; // This value will be exposed to the shader via InstanceID()
    instanceDescs[i].InstanceContributionToHitGroupIndex = inst.meshid * 2 + 2;  // The indices are relative to to the start of the hit-table entries specified in Raytrace(), so we need 4 and 6
    instanceDescs[i].Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE;
    mat4 m = transpose(inst.tr); // GLM is column major, the INSTANCE_DESC is row major
    memcpy(instanceDescs[i].Transform, &m, sizeof(instanceDescs[i].Transform));
    instanceDescs[i].AccelerationStructure = pBottomLevelAS[inst.meshid]->GetGPUVirtualAddress();
    instanceDescs[i].InstanceMask = 0xFF;
    i++;
  }

  // Unmap
  buffers.pInstanceDesc->Unmap(0, nullptr);

  // Create the TLAS
  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc = {};
  asDesc.Inputs = inputs;
  asDesc.Inputs.InstanceDescs = buffers.pInstanceDesc->GetGPUVirtualAddress();
  asDesc.DestAccelerationStructureData = buffers.pResult->GetGPUVirtualAddress();
  asDesc.ScratchAccelerationStructureData = buffers.pScratch->GetGPUVirtualAddress();

  pCmdList->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);

  // We need to insert a UAV barrier before using the acceleration structures in a raytracing operation
  D3D12_RESOURCE_BARRIER uavBarrier = {};
  uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  uavBarrier.UAV.pResource = buffers.pResult;
  pCmdList->ResourceBarrier(1, &uavBarrier);

  return buffers;
}
size_t RD_DXR_Experimental::addMesh(pair<vector<Vertex>, vector<uint32_t>> mesh) {
  /*
  vector<vec3> vertices =
  {
      vec3(0,          1,  0),
      vec3(0.866f,  -0.5f, 0),
      vec3(-0.866f, -0.5f, 0)
  };


  vector<vec3> verticesp =
  {
      vec3(-100, -1,  -2),
      vec3(100, -1,  100),
      vec3(-100, -1,  100),

      vec3(-100, -1,  -2),
      vec3(100, -1,  -2),
      vec3(100, -1,  100)
  };
  
  vector<vector<vec3>> meshList = {
    verticesp,
    vertices
  };
  */
  
  meshList.push_back(mesh);
  return meshList.size() - 1;
}

void RD_DXR_Experimental::addInstance(Instance inst) {
  instancesList.push_back(inst);
}

void RD_DXR_Experimental::initGeometry() {
  /*
  vector<Instance> instances = {
  {mat4(), 0, 0},
  {translate(mat4(), vec3(-2, 0, 0)), 1, 4},
  {translate(mat4(), vec3(2, 0, 0)), 1, 6}
  };
  for (auto inst : instances) {
    addInstance(inst);
  }*/

  for (auto m : meshList) {
    mpVertexBuffer.push_back(make_pair(createVB(mpDevice, get<0>(m)), createVB(mpDevice, get<1>(m))));
  }
  
  int i = 0;
  for (auto m : meshList) {
    bottomLevelBuffers.push_back(createSingleGeomBottomLevelAS(mpDevice, mpCmdList, mpVertexBuffer[i], make_pair(get<0>(m).size(),get<1>(m).size())));
    mpBottomLevelAS.push_back(bottomLevelBuffers[i].pResult);
    i++;
  }
}

void RD_DXR_Experimental::createAccelerationStructures()
{
  // Create the TLAS
  AccelerationStructureBuffers topLevelBuffers = createTopLevelAS(mpDevice, mpCmdList, mpBottomLevelAS, instancesList, mTlasSize);

  // The tutorial doesn't have any resource lifetime management, so we flush and sync here. This is not required by the DXR spec - you can submit the list whenever you like as long as you take care of the resources lifetime.
  mFenceValue = submitCommandList(mpCmdList, mpCmdQueue, mpFence, mFenceValue);
  mpFence->SetEventOnCompletion(mFenceValue, mFenceEvent);
  WaitForSingleObject(mFenceEvent, INFINITE);
  uint32_t bufferIndex = mpSwapChain->GetCurrentBackBufferIndex();
  mpCmdList->Reset(mFrameObjects[0].pCmdAllocator, nullptr);

  // Store the AS buffers. The rest of the buffers will be released once we exit the function
  mpTopLevelAS = topLevelBuffers.pResult;
}

//////////////////////////////////////////////////////////////////////////
// Tutorial 04 code
//////////////////////////////////////////////////////////////////////////
ID3DBlobPtr compileLibrary(const WCHAR* filename, const WCHAR* targetString)
{
  // Initialize the helper
  d3d_call(gDxcDllHelper.Initialize());
  IDxcCompiler* pCompiler;
  IDxcLibrary* pLibrary;
  d3d_call(gDxcDllHelper.CreateInstance(CLSID_DxcCompiler, &pCompiler));
  d3d_call(gDxcDllHelper.CreateInstance(CLSID_DxcLibrary, &pLibrary));

  // Open and read the file
  std::ifstream shaderFile(filename);
  if (shaderFile.good() == false)
  {
    msgBox("Can't open file " + wstring_2_string(std::wstring(filename)));
    return nullptr;
  }
  std::stringstream strStream;
  strStream << shaderFile.rdbuf();
  std::string shader = strStream.str();

  // Create blob from the string
  IDxcBlobEncodingPtr pTextBlob;
  d3d_call(pLibrary->CreateBlobWithEncodingFromPinned((LPBYTE)shader.c_str(), (uint32_t)shader.size(), 0, &pTextBlob));

  // Compile
  IDxcOperationResult* pResult;
  d3d_call(pCompiler->Compile(pTextBlob, filename, L"", targetString, nullptr, 0, nullptr, 0, nullptr, &pResult));

  // Verify the result
  HRESULT resultCode;
  d3d_call(pResult->GetStatus(&resultCode));
  if (FAILED(resultCode))
  {
    IDxcBlobEncodingPtr pError;
    d3d_call(pResult->GetErrorBuffer(&pError));
    std::string log = convertBlobToString(pError.GetInterfacePtr());
    msgBox("Compiler error:\n" + log);
    return nullptr;
  }

  MAKE_SMART_COM_PTR(IDxcBlob);
  IDxcBlobPtr pBlob;
  d3d_call(pResult->GetResult(&pBlob));
  return pBlob;
}

ID3D12RootSignaturePtr createRootSignature(ID3D12Device5Ptr pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
{
  ID3DBlobPtr pSigBlob;
  ID3DBlobPtr pErrorBlob;
  HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &pSigBlob, &pErrorBlob);
  if (FAILED(hr))
  {
    std::string msg = convertBlobToString(pErrorBlob.GetInterfacePtr());
    msgBox(msg);
    return nullptr;
  }
  ID3D12RootSignaturePtr pRootSig;
  d3d_call(pDevice->CreateRootSignature(0, pSigBlob->GetBufferPointer(), pSigBlob->GetBufferSize(), IID_PPV_ARGS(&pRootSig)));
  return pRootSig;
}

struct RootSignatureDesc
{
  D3D12_ROOT_SIGNATURE_DESC desc = {};
  std::vector<D3D12_DESCRIPTOR_RANGE> range;
  std::vector<D3D12_ROOT_PARAMETER> rootParams;
};

RootSignatureDesc createRayGenRootDesc()
{
  // Create the root-signature
  RootSignatureDesc desc;
  desc.range.resize(2);
  // gOutput
  desc.range[0].BaseShaderRegister = 0;
  desc.range[0].NumDescriptors = 1;
  desc.range[0].RegisterSpace = 0;
  desc.range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  desc.range[0].OffsetInDescriptorsFromTableStart = 0;

  // gRtScene
  desc.range[1].BaseShaderRegister = 0;
  desc.range[1].NumDescriptors = 1;
  desc.range[1].RegisterSpace = 0;
  desc.range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  desc.range[1].OffsetInDescriptorsFromTableStart = 1;

  desc.rootParams.resize(2);
  desc.rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  desc.rootParams[0].DescriptorTable.NumDescriptorRanges = 2;
  desc.rootParams[0].DescriptorTable.pDescriptorRanges = desc.range.data();

  desc.rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  desc.rootParams[1].Descriptor.RegisterSpace = 1;
  desc.rootParams[1].Descriptor.ShaderRegister = 0;

  // Create the desc
  desc.desc.NumParameters = desc.rootParams.size();
  desc.desc.pParameters = desc.rootParams.data();
  desc.desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

  return desc;
}

RootSignatureDesc createTriangleHitRootDesc()
{
  RootSignatureDesc desc;
  desc.range.resize(2);
  desc.range[0].BaseShaderRegister = 0;
  desc.range[0].NumDescriptors = 1;
  desc.range[0].RegisterSpace = 1;
  desc.range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  desc.range[0].OffsetInDescriptorsFromTableStart = 0;
  
  desc.range[1].BaseShaderRegister = 0;
  desc.range[1].NumDescriptors = 1;
  desc.range[1].RegisterSpace = 2;
  desc.range[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  desc.range[1].OffsetInDescriptorsFromTableStart = 1;
  
  desc.rootParams.resize(2);
  desc.rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  desc.rootParams[0].DescriptorTable.NumDescriptorRanges = desc.range.size();
  desc.rootParams[0].DescriptorTable.pDescriptorRanges = desc.range.data();

  desc.rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  desc.rootParams[1].Descriptor.RegisterSpace = 0;
  desc.rootParams[1].Descriptor.ShaderRegister = 0;
  
  
  desc.desc.NumParameters = desc.rootParams.size();
  desc.desc.pParameters = desc.rootParams.data();
  desc.desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

  return desc;
}

RootSignatureDesc createPlaneHitRootDesc()
{
  RootSignatureDesc desc;
  desc.range.resize(1);
  desc.range[0].BaseShaderRegister = 0;
  desc.range[0].NumDescriptors = 1;
  desc.range[0].RegisterSpace = 0;
  desc.range[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  desc.range[0].OffsetInDescriptorsFromTableStart = 0;

  desc.rootParams.resize(1);
  desc.rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  desc.rootParams[0].DescriptorTable.NumDescriptorRanges = 1;
  desc.rootParams[0].DescriptorTable.pDescriptorRanges = desc.range.data();

  desc.desc.NumParameters = 1;
  desc.desc.pParameters = desc.rootParams.data();
  desc.desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;

  return desc;
}

struct DxilLibrary
{
  DxilLibrary(ID3DBlobPtr pBlob, const WCHAR* entryPoint[], uint32_t entryPointCount) : pShaderBlob(pBlob)
  {
    stateSubobject.Type = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
    stateSubobject.pDesc = &dxilLibDesc;

    dxilLibDesc = {};
    exportDesc.resize(entryPointCount);
    exportName.resize(entryPointCount);
    if (pBlob)
    {
      dxilLibDesc.DXILLibrary.pShaderBytecode = pBlob->GetBufferPointer();
      dxilLibDesc.DXILLibrary.BytecodeLength = pBlob->GetBufferSize();
      dxilLibDesc.NumExports = entryPointCount;
      dxilLibDesc.pExports = exportDesc.data();

      for (uint32_t i = 0; i < entryPointCount; i++)
      {
        exportName[i] = entryPoint[i];
        exportDesc[i].Name = exportName[i].c_str();
        exportDesc[i].Flags = D3D12_EXPORT_FLAG_NONE;
        exportDesc[i].ExportToRename = nullptr;
      }
    }
  };

  DxilLibrary() : DxilLibrary(nullptr, nullptr, 0) {}

  D3D12_DXIL_LIBRARY_DESC dxilLibDesc = {};
  D3D12_STATE_SUBOBJECT stateSubobject{};
  ID3DBlobPtr pShaderBlob;
  std::vector<D3D12_EXPORT_DESC> exportDesc;
  std::vector<std::wstring> exportName;
};

static const WCHAR* kRayGenShader = L"rayGen";
static const WCHAR* kMissShader = L"miss";
static const WCHAR* kTriangleChs = L"triangleChs";
static const WCHAR* kPlaneChs = L"planeChs";
static const WCHAR* kTriHitGroup = L"TriHitGroup";
static const WCHAR* kPlaneHitGroup = L"PlaneHitGroup";
static const WCHAR* kShadowChs = L"shadowChs";
static const WCHAR* kShadowMiss = L"shadowMiss";
static const WCHAR* kShadowHitGroup = L"ShadowHitGroup";

DxilLibrary createDxilLibrary()
{
  // Compile the shader
  ID3DBlobPtr pDxilLib = compileLibrary(L"shaders/13-Shaders.hlsl", L"lib_6_3");
  const WCHAR* entryPoints[] = { kRayGenShader, kMissShader, kPlaneChs, kTriangleChs, kShadowMiss, kShadowChs };
  return DxilLibrary(pDxilLib, entryPoints, arraysize(entryPoints));
}

struct HitProgram
{
  HitProgram(LPCWSTR ahsExport, LPCWSTR chsExport, const std::wstring& name) : exportName(name)
  {
    desc = {};
    desc.AnyHitShaderImport = ahsExport;
    desc.ClosestHitShaderImport = chsExport;
    desc.HitGroupExport = exportName.c_str();

    subObject.Type = D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP;
    subObject.pDesc = &desc;
  }

  std::wstring exportName;
  D3D12_HIT_GROUP_DESC desc;
  D3D12_STATE_SUBOBJECT subObject;
};

struct ExportAssociation
{
  ExportAssociation(const WCHAR* exportNames[], uint32_t exportCount, const D3D12_STATE_SUBOBJECT* pSubobjectToAssociate)
  {
    association.NumExports = exportCount;
    association.pExports = exportNames;
    association.pSubobjectToAssociate = pSubobjectToAssociate;

    subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION;
    subobject.pDesc = &association;
  }

  D3D12_STATE_SUBOBJECT subobject = {};
  D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION association = {};
};

struct LocalRootSignature
{
  LocalRootSignature(ID3D12Device5Ptr pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
  {
    pRootSig = createRootSignature(pDevice, desc);
    pInterface = pRootSig.GetInterfacePtr();
    subobject.pDesc = &pInterface;
    subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_LOCAL_ROOT_SIGNATURE;
  }
  ID3D12RootSignaturePtr pRootSig;
  ID3D12RootSignature* pInterface = nullptr;
  D3D12_STATE_SUBOBJECT subobject = {};
};

struct GlobalRootSignature
{
  GlobalRootSignature(ID3D12Device5Ptr pDevice, const D3D12_ROOT_SIGNATURE_DESC& desc)
  {
    pRootSig = createRootSignature(pDevice, desc);
    pInterface = pRootSig.GetInterfacePtr();
    subobject.pDesc = &pInterface;
    subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
  }
  ID3D12RootSignaturePtr pRootSig;
  ID3D12RootSignature* pInterface = nullptr;
  D3D12_STATE_SUBOBJECT subobject = {};
};

struct ShaderConfig
{
  ShaderConfig(uint32_t maxAttributeSizeInBytes, uint32_t maxPayloadSizeInBytes)
  {
    shaderConfig.MaxAttributeSizeInBytes = maxAttributeSizeInBytes;
    shaderConfig.MaxPayloadSizeInBytes = maxPayloadSizeInBytes;

    subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
    subobject.pDesc = &shaderConfig;
  }

  D3D12_RAYTRACING_SHADER_CONFIG shaderConfig = {};
  D3D12_STATE_SUBOBJECT subobject = {};
};

struct PipelineConfig
{
  PipelineConfig(uint32_t maxTraceRecursionDepth)
  {
    config.MaxTraceRecursionDepth = maxTraceRecursionDepth;

    subobject.Type = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
    subobject.pDesc = &config;
  }

  D3D12_RAYTRACING_PIPELINE_CONFIG config = {};
  D3D12_STATE_SUBOBJECT subobject = {};
};

void RD_DXR_Experimental::createRtPipelineState()
{
  // Need 16 subobjects:
  //  1 for DXIL library    
  //  3 for the hit-groups (triangle hit group, plane hit-group, shadow-hit group)
  //  2 for RayGen root-signature (root-signature and the subobject association)
  //  2 for triangle hit-program root-signature (root-signature and the subobject association)
  //  2 for the plane-hit root-signature (root-signature and the subobject association)
  //  2 for shadow-program and miss root-signature (root-signature and the subobject association)
  //  2 for shader config (shared between all programs. 1 for the config, 1 for association)
  //  1 for pipeline config
  //  1 for the global root signature
  std::array<D3D12_STATE_SUBOBJECT, 16> subobjects;
  uint32_t index = 0;

  // Create the DXIL library
  DxilLibrary dxilLib = createDxilLibrary();
  subobjects[index++] = dxilLib.stateSubobject; // 0 Library

  // Create the triangle HitProgram
  HitProgram triHitProgram(nullptr, kTriangleChs, kTriHitGroup);
  subobjects[index++] = triHitProgram.subObject; // 1 Triangle Hit Group

  // Create the plane HitProgram
  HitProgram planeHitProgram(nullptr, kPlaneChs, kPlaneHitGroup);
  subobjects[index++] = planeHitProgram.subObject; // 2 Plant Hit Group

  // Create the shadow-ray hit group
  HitProgram shadowHitProgram(nullptr, kShadowChs, kShadowHitGroup);
  subobjects[index++] = shadowHitProgram.subObject; // 3 Shadow Hit Group

  // Create the ray-gen root-signature and association
  LocalRootSignature rgsRootSignature(mpDevice, createRayGenRootDesc().desc);
  subobjects[index] = rgsRootSignature.subobject; // 4 Ray Gen Root Sig

  uint32_t rgsRootIndex = index++; // 4
  ExportAssociation rgsRootAssociation(&kRayGenShader, 1, &(subobjects[rgsRootIndex]));
  subobjects[index++] = rgsRootAssociation.subobject; // 5 Associate Root Sig to RGS

  // Create the tri hit root-signature and association
  LocalRootSignature triHitRootSignature(mpDevice, createTriangleHitRootDesc().desc);
  subobjects[index] = triHitRootSignature.subobject; // 6 Triangle Hit Root Sig

  uint32_t triHitRootIndex = index++; // 6
  ExportAssociation triHitRootAssociation(&kTriangleChs, 1, &(subobjects[triHitRootIndex]));
  subobjects[index++] = triHitRootAssociation.subobject; // 7 Associate Triangle Root Sig to Triangle Hit Group

  // Create the plane hit root-signature and association
  LocalRootSignature planeHitRootSignature(mpDevice, createPlaneHitRootDesc().desc);
  subobjects[index] = planeHitRootSignature.subobject; // 8 Plane Hit Root Sig

  uint32_t planeHitRootIndex = index++; // 8
  ExportAssociation planeHitRootAssociation(&kPlaneHitGroup, 1, &(subobjects[planeHitRootIndex]));
  subobjects[index++] = planeHitRootAssociation.subobject; // 9 Associate Plane Hit Root Sig to Plane Hit Group

  // Create the empty root-signature and associate it with the primary miss-shader and the shadow programs
  D3D12_ROOT_SIGNATURE_DESC emptyDesc = {};
  emptyDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE;
  LocalRootSignature emptyRootSignature(mpDevice, emptyDesc);
  subobjects[index] = emptyRootSignature.subobject; // 10 Empty Root Sig for Plane Hit Group and Miss

  uint32_t emptyRootIndex = index++; // 10
  const WCHAR* emptyRootExport[] = { kMissShader, kShadowChs, kShadowMiss };
  ExportAssociation emptyRootAssociation(emptyRootExport, arraysize(emptyRootExport), &(subobjects[emptyRootIndex]));
  subobjects[index++] = emptyRootAssociation.subobject; // 11 Associate empty root sig to Plane Hit Group and Miss shader

  // Bind the payload size to all programs
  ShaderConfig primaryShaderConfig(sizeof(float) * 2, sizeof(float) * 3);
  subobjects[index] = primaryShaderConfig.subobject; // 12

  uint32_t primaryShaderConfigIndex = index++;
  const WCHAR* primaryShaderExports[] = { kRayGenShader, kMissShader, kTriangleChs, kPlaneChs, kShadowMiss, kShadowChs };
  ExportAssociation primaryConfigAssociation(primaryShaderExports, arraysize(primaryShaderExports), &(subobjects[primaryShaderConfigIndex]));
  subobjects[index++] = primaryConfigAssociation.subobject; // 13 Associate shader config to all programs

  // Create the pipeline config
  PipelineConfig config(2); // maxRecursionDepth - 1 TraceRay() from the ray-gen, 1 TraceRay() from the primary hit-shader
  subobjects[index++] = config.subobject; // 14

  // Create the global root signature and store the empty signature
  GlobalRootSignature root(mpDevice, {});
  mpEmptyRootSig = root.pRootSig;
  subobjects[index++] = root.subobject; // 15

  // Create the state
  D3D12_STATE_OBJECT_DESC desc;
  desc.NumSubobjects = index; // 16
  desc.pSubobjects = subobjects.data();
  desc.Type = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;

  d3d_call(mpDevice->CreateStateObject(&desc, IID_PPV_ARGS(&mpPipelineState)));
}

//////////////////////////////////////////////////////////////////////////
// Tutorial 05
//////////////////////////////////////////////////////////////////////////


vector<ShaderTableEntry> RD_DXR_Experimental::GetShaderTableEntryStructure() {
  uint64_t heapStart = mpSrvUavHeap->GetGPUDescriptorHandleForHeapStart().ptr;

  vector<ShaderTableEntry> entries = {
    {kRayGenShader, vector<D3D12_GPU_VIRTUAL_ADDRESS>{heapStart, mpCameraConstantBuffer[0]->GetGPUVirtualAddress()}},
    {kMissShader, vector<D3D12_GPU_VIRTUAL_ADDRESS>{}},
    {kShadowMiss, vector<D3D12_GPU_VIRTUAL_ADDRESS>{}},
  };

  for (int i = 0; i < mpVertexBuffer.size(); i++) {
    entries.push_back({ kTriHitGroup, vector<D3D12_GPU_VIRTUAL_ADDRESS>{ heapStart + mpDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) * 2 * (i + 1), mpConstantBuffer[0]->GetGPUVirtualAddress()} });
    entries.push_back({ kShadowHitGroup, vector<D3D12_GPU_VIRTUAL_ADDRESS>{} });
  }

  return entries;
}

void RD_DXR_Experimental::updateShaderTable() {
  /** The shader-table layout is as follows:
      Entry 0 - Ray-gen program
      Entry 1 - Miss program for the primary ray
      Entry 2 - Miss program for the shadow ray
      Entries 3,4 - Hit programs for triangle 0 (primary followed by shadow)
      Entries 5,6 - Hit programs for the plane (primary followed by shadow)
      Entries 7,8 - Hit programs for triangle 1 (primary followed by shadow)
      Entries 9,10 - Hit programs for triangle 2 (primary followed by shadow)
      All entries in the shader-table must have the same size, so we will choose it base on the largest required entry.
      The triangle primary-ray hit program requires the largest entry - sizeof(program identifier) + 8 bytes for the constant-buffer root descriptor.
      The entry size must be aligned up to D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT
  */
  // Entry 0 - ray-gen program ID and descriptor data

  vector<ShaderTableEntry> entries = GetShaderTableEntryStructure();

  auto mel = max_element(entries.begin(), entries.end(), [](const ShaderTableEntry& s1, const ShaderTableEntry& s2) {
    return s1.addr.size() < s2.addr.size();
  });

  mShaderTableEntrySize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
  mShaderTableEntrySize += 8 * mel->addr.size();
  mShaderTableEntrySize = align_to(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT, mShaderTableEntrySize);
  uint32_t shaderTableSize = mShaderTableEntrySize * entries.size();

  // Map the buffer
  uint8_t* pData;
  d3d_call(mpShaderTable->Map(0, nullptr, (void**)&pData));

  MAKE_SMART_COM_PTR(ID3D12StateObjectProperties);
  ID3D12StateObjectPropertiesPtr pRtsoProps;
  mpPipelineState->QueryInterface(IID_PPV_ARGS(&pRtsoProps));

  int ei = 0;

  for (auto &e : entries) {
    uint8_t *entryPtr = pData + ei * mShaderTableEntrySize;
    memcpy(entryPtr, pRtsoProps->GetShaderIdentifier(e.shader_name), D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES);

    int i = 0;

    for (auto addr : e.addr) {
      uint8_t* currPtr = entryPtr + D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES + i * 8;

      assert(((uint64_t)(currPtr) % 8) == 0); // Root descriptor must be stored at an 8-byte aligned address
      *(D3D12_GPU_VIRTUAL_ADDRESS*)(currPtr) = addr;
      i++;
    }
    ei++;
  }

  // Unmap
  mpShaderTable->Unmap(0, nullptr);
}

void RD_DXR_Experimental::createShaderTable()
{
  /** The shader-table layout is as follows:
      Entry 0 - Ray-gen program
      Entry 1 - Miss program for the primary ray
      Entry 2 - Miss program for the shadow ray
      Entries 3,4 - Hit programs for triangle 0 (primary followed by shadow)
      Entries 5,6 - Hit programs for the plane (primary followed by shadow)
      Entries 7,8 - Hit programs for triangle 1 (primary followed by shadow)
      Entries 9,10 - Hit programs for triangle 2 (primary followed by shadow)
      All entries in the shader-table must have the same size, so we will choose it base on the largest required entry.
      The triangle primary-ray hit program requires the largest entry - sizeof(program identifier) + 8 bytes for the constant-buffer root descriptor.
      The entry size must be aligned up to D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT
  */
  
  vector<ShaderTableEntry> entries = GetShaderTableEntryStructure();

  auto mel = max_element(entries.begin(), entries.end(), [](const ShaderTableEntry& s1, const ShaderTableEntry& s2) {
    return s1.addr.size() < s2.addr.size();
  });

  mShaderTableEntrySize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
  mShaderTableEntrySize += 8 * mel->addr.size();
  mShaderTableEntrySize = align_to(D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT, mShaderTableEntrySize);
  uint32_t shaderTableSize = mShaderTableEntrySize * entries.size();


  // For simplicity, we create the shader-table on the upload heap. You can also create it on the default heap
  mpShaderTable = createBuffer(mpDevice, shaderTableSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);
}

//////////////////////////////////////////////////////////////////////////
// Tutorial 06
//////////////////////////////////////////////////////////////////////////
void RD_DXR_Experimental::createShaderResources()
{
  // Create the output resource. The dimensions and format should match the swap-chain
  D3D12_RESOURCE_DESC resDesc = {};
  resDesc.DepthOrArraySize = 1;
  resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  resDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // The backbuffer is actually DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, but sRGB formats can't be used with UAVs. We will convert to sRGB ourselves in the shader
  resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  resDesc.Height = mSwapChainSize.y;
  resDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  resDesc.MipLevels = 1;
  resDesc.SampleDesc.Count = 1;
  resDesc.Width = mSwapChainSize.x;
  d3d_call(mpDevice->CreateCommittedResource(&kDefaultHeapProps, D3D12_HEAP_FLAG_NONE, &resDesc, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&mpOutputResource))); // Starting as copy-source to simplify onFrameRender()

  // Create an SRV/UAV descriptor heap. Need 2 entries - 1 SRV for the scene and 1 UAV for the output
  mpSrvUavHeap = createDescriptorHeap(mpDevice, 2 + 2 * mpVertexBuffer.size(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, true);

  // Create the UAV. Based on the root signature we created it should be the first entry
  D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
  uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  mpDevice->CreateUnorderedAccessView(mpOutputResource, nullptr, &uavDesc, mpSrvUavHeap->GetCPUDescriptorHandleForHeapStart());

  // Create the TLAS SRV right after the UAV. Note that we are using a different SRV desc here
  D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
  srvDesc.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
  srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srvDesc.RaytracingAccelerationStructure.Location = mpTopLevelAS->GetGPUVirtualAddress();
  D3D12_CPU_DESCRIPTOR_HANDLE srvHandle = mpSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
  srvHandle.ptr += mpDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  mpDevice->CreateShaderResourceView(nullptr, &srvDesc, srvHandle);

  int i = 0;
  for (auto v : mpVertexBuffer) {
    {
      srvHandle.ptr +=
        mpDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2 = {};
      srvDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc2.Format = DXGI_FORMAT_UNKNOWN;
      srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      srvDesc2.Buffer.FirstElement = 0;
      srvDesc2.Buffer.NumElements = static_cast<uint>(meshList[i].first.size());
      srvDesc2.Buffer.StructureByteStride = sizeof(meshList[i].first[0]);
      srvDesc2.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
      // Write the per-instance properties buffer view in the heap
      mpDevice->CreateShaderResourceView(get<0>(mpVertexBuffer[i]), &srvDesc2, srvHandle);
    }
    {
      srvHandle.ptr +=
        mpDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc2 = {};
      srvDesc2.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      srvDesc2.Format = DXGI_FORMAT_UNKNOWN;
      srvDesc2.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
      srvDesc2.Buffer.FirstElement = 0;
      srvDesc2.Buffer.NumElements = static_cast<uint>(meshList[i].second.size());
      srvDesc2.Buffer.StructureByteStride = sizeof(meshList[i].second[0]);
      srvDesc2.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
      // Write the per-instance properties buffer view in the heap
      mpDevice->CreateShaderResourceView(get<1>(mpVertexBuffer[i]), &srvDesc2, srvHandle);
    }
    i++;
  }
}

//////////////////////////////////////////////////////////////////////////
// Tutorial 10
//////////////////////////////////////////////////////////////////////////
void RD_DXR_Experimental::createConstantBuffers()
{
  // The shader declares each CB with 3 float3. However, due to HLSL packing rules, we create the CB with vec4 (each float3 needs to start on a 16-byte boundary)
  vec4 bufferData[] = {
    // Instance 0
    vec4(1.0f, 0.0f, 0.0f, 1.0f),
    vec4(1.0f, 1.0f, 0.0f, 1.0f),
    vec4(1.0f, 0.0f, 1.0f, 1.0f),

    // Instance 1
    vec4(0.0f, 1.0f, 0.0f, 1.0f),
    vec4(0.0f, 1.0f, 1.0f, 1.0f),
    vec4(1.0f, 1.0f, 0.0f, 1.0f),

    // Instance 2
    vec4(0.0f, 0.0f, 1.0f, 1.0f),
    vec4(1.0f, 0.0f, 1.0f, 1.0f),
    vec4(0.0f, 1.0f, 1.0f, 1.0f),
  };

  for (uint32_t i = 0; i < 3; i++)
  {
    const uint32_t bufferSize = sizeof(vec4) * 3;
    mpConstantBuffer[i] = createBuffer(mpDevice, bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);
    uint8_t* pData;
    d3d_call(mpConstantBuffer[i]->Map(0, nullptr, (void**)&pData));
    memcpy(pData, &bufferData[i * 3], sizeof(bufferData));
    mpConstantBuffer[i]->Unmap(0, nullptr);
  }
}

void RD_DXR_Experimental::createCameraConstantBuffers() {
  mat4 bufferData[] = {
    mat4(),
    mat4()
  };

  for (uint32_t i = 0; i < 1; i++)
  {
    const uint32_t bufferSize = sizeof(bufferData) * 2;
    mpCameraConstantBuffer[i] = createBuffer(mpDevice, bufferSize, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ, kUploadHeapProps);
    uint8_t* pData;
    d3d_call(mpCameraConstantBuffer[i]->Map(0, nullptr, (void**)&pData));
    memcpy(pData, &bufferData[i], sizeof(bufferData));
    mpCameraConstantBuffer[i]->Unmap(0, nullptr);
  }
}

void RD_DXR_Experimental::updateCameraConstantBuffers(mat4 proj, mat4 view) {
  mat4 bufferData[] = {
    proj,
    view
  };

  for (uint32_t i = 0; i < 1; i++)
  {
    uint8_t* pData;
    d3d_call(mpCameraConstantBuffer[i]->Map(0, nullptr, (void**)&pData));
    memcpy(pData, &bufferData[i], sizeof(bufferData));
    mpCameraConstantBuffer[i]->Unmap(0, nullptr);
  }
}

//////////////////////////////////////////////////////////////////////////
// The render itself
//////////////////////////////////////////////////////////////////////////

RD_DXR_Experimental::RD_DXR_Experimental() {

}

void RD_DXR_Experimental::ClearAll()
{
  printf("ClearAll\n\n");

  // Wait for the command queue to finish execution
  mFenceValue++;
  mpCmdQueue->Signal(mpFence, mFenceValue);
  mpFence->SetEventOnCompletion(mFenceValue, mFenceEvent);
  WaitForSingleObject(mFenceEvent, INFINITE);
}

HRDriverAllocInfo RD_DXR_Experimental::AllocAll(HRDriverAllocInfo a_info)
{
  printf("AllocAll\n\n");
  RECT r;
  GetClientRect(mainWindowHWND, &r);
  int g_width = r.right - r.left;
  int g_height = r.bottom - r.top;

  initDXR(mainWindowHWND, g_width, g_height);        // Tutorial 02
  vector<vec3> vertices =
  {
      vec3(0,          1,  0),
      vec3(0.866f,  -0.5f, 0),
      vec3(-0.866f, -0.5f, 0)
  };


  vector<vec3> verticesp =
  {
      vec3(-100, -1,  -2),
      vec3(100, -1,  100),
      vec3(-100, -1,  100),

      vec3(-100, -1,  -2),
      vec3(100, -1,  -2),
      vec3(100, -1,  100)
  };
  
  vector<vector<vec3>> meshList = {
    verticesp,
    vertices
  };
  for (auto m : meshList) {
    //addMesh(m);
  }

  vector<Instance> instances = {
    {mat4(), 0, 0},
    {translate(mat4(), vec3(-2, 0, 0)), 1, 4},
    {translate(mat4(), vec3(2, 0, 0)), 1, 6}
  };

  for (auto m : instances) {
  //  addInstance(m);
  }

  createConstantBuffers();                        // Tutorial 10. Yes, we need to do it before creating the shader-table
  createCameraConstantBuffers();


  return a_info;
}

HRDriverInfo RD_DXR_Experimental::Info()
{
  HRDriverInfo info; // very simple render driver implementation, does not support any other/advanced stuff

  info.supportHDRFrameBuffer = false;
  info.supportHDRTextures = false;
  info.supportMultiMaterialInstance = false;

  info.supportImageLoadFromInternalFormat = false;
  info.supportImageLoadFromExternalFormat = false;
  info.supportMeshLoadFromInternalFormat = false;
  info.supportLighting = false;

  info.memTotal = int64_t(8) * int64_t(1024 * 1024 * 1024);

  return info;
}

#pragma warning(disable:4996) // for wcscpy to be ok

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// typedef void (APIENTRYP PFNGLGENERATEMIPMAPPROC)(GLenum target);
// PFNGLGENERATEMIPMAPPROC glad_glGenerateMipmap;
//
//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

bool RD_DXR_Experimental::UpdateImage(int32_t a_texId, int32_t w, int32_t h, int32_t bpp, const void* a_data, pugi::xml_node a_texNode)
{
  if (a_data == nullptr)
    return false;


  printf("UpdateImage\n");

  return true;
}


bool RD_DXR_Experimental::UpdateMaterial(int32_t a_matId, pugi::xml_node a_materialNode)
{

  printf("UpdateMaterial\n");

  return true;
}

bool RD_DXR_Experimental::UpdateLight(int32_t a_lightIdId, pugi::xml_node a_lightNode)
{
  printf("UpdateLight\n");
  return true;
}


bool RD_DXR_Experimental::UpdateCamera(pugi::xml_node a_camNode)
{
  if (a_camNode == nullptr)
    return true;

  //this->camUseMatrices = false;
  

  if (std::wstring(a_camNode.attribute(L"type").as_string()) == L"two_matrices")
  {
    const wchar_t* m1 = a_camNode.child(L"mWorldView").text().as_string();
    const wchar_t* m2 = a_camNode.child(L"mProj").text().as_string();

    mat4 mWorldView;
    mat4 mProj;
    
    std::wstringstream str1(m1), str2(m2);
    for (int i = 0; i < 4; i++)
    for (int j = 0; j < 4; j++)
    {
      str1 >> mWorldView[i][j];
      str2 >> mProj[i][j];
    }

    /*this->camWorldViewMartrixTransposed = transpose(float4x4(mWorldView));
    this->camProjMatrixTransposed = transpose(float4x4(mProj));
    this->camUseMatrices = true;
*/
    return true;
  }

  const wchar_t* camPosStr = a_camNode.child(L"position").text().as_string();
  const wchar_t* camLAtStr = a_camNode.child(L"look_at").text().as_string();
  const wchar_t* camUpStr = a_camNode.child(L"up").text().as_string();
  //const wchar_t* testStr   = a_camNode.child(L"test").text().as_string();

  if (!a_camNode.child(L"fov").text().empty())
    camFov = a_camNode.child(L"fov").text().as_float();

  if (!a_camNode.child(L"nearClipPlane").text().empty())
    camNearPlane = a_camNode.child(L"nearClipPlane").text().as_float();

  if (!a_camNode.child(L"farClipPlane").text().empty())
    camFarPlane = a_camNode.child(L"farClipPlane").text().as_float();

  if (std::wstring(camPosStr) != L"")
  {
    std::wstringstream input(camPosStr);
    input >> camPos[0] >> camPos[1] >> camPos[2];
  }

  if (std::wstring(camLAtStr) != L"")
  {
    std::wstringstream input(camLAtStr);
    input >> camLookAt[0] >> camLookAt[1] >> camLookAt[2];
  }

  if (std::wstring(camUpStr) != L"")
  {
    std::wstringstream input(camUpStr);
    input >> camUp[0] >> camUp[1] >> camUp[2];
  }

  const float aspect = float(m_width) / float(m_height);
  mat4 projMatrixInv = glm::perspectiveFov(camFov, (float)m_width, (float)m_height, camNearPlane, camFarPlane);
  projMatrixInv = inverse(projMatrixInv);
  vec3 eye(camPos[0], camPos[1], camPos[2]);
  vec3 center(camLookAt[0], camLookAt[1], camLookAt[2]);
  vec3 up(camUp[0], camUp[1], camUp[2]);

  mat4 lookAtMatrix = lookAt(eye, center, up); // get inverse lookAt matrix
  lookAtMatrix = inverse(lookAtMatrix);

  updateCameraConstantBuffers(projMatrixInv, lookAtMatrix);

  printf("UpdateCamera\n");

  return true;
}

bool RD_DXR_Experimental::UpdateSettings(pugi::xml_node a_settingsNode)
{
  printf("UpdateSettings\n");
  if (a_settingsNode.child(L"width") != nullptr)
    m_width = a_settingsNode.child(L"width").text().as_int();

  if (a_settingsNode.child(L"height") != nullptr)
    m_height = a_settingsNode.child(L"height").text().as_int();

  if (m_width < 0 || m_height < 0)
  {
    if (m_pInfoCallBack != nullptr)
      m_pInfoCallBack(L"bad input resolution", L"RD_DXR_Experimental::UpdateSettings", HR_SEVERITY_ERROR);
    return false;
  }

  return true;
}


bool RD_DXR_Experimental::UpdateMesh(int32_t a_meshId, pugi::xml_node a_meshNode, const HRMeshDriverInput& a_input, const HRBatchInfo* a_batchList, int32_t a_listSize)
{
  printf("UpdateMesh\n");

  if (a_input.triNum == 0) // don't support loading mesh from file 'a_fileName'
  {

    return true;
  }

  for (int32_t batchId = 0; batchId < a_listSize; batchId++)
  {
    HRBatchInfo batch = a_batchList[batchId];
    const int drawElementsNum = batch.triEnd - batch.triBegin;

    vector<glm::vec3> currMesh;

    for (int triid = batch.triBegin; triid < batch.triBegin + drawElementsNum; triid++)
    {
      const int vInds[] = {
        a_input.indices[triid * 3 + 0],
        a_input.indices[triid * 3 + 1],
        a_input.indices[triid * 3 + 2]
      };
      
      for (auto vInd : vInds) {
        currMesh.push_back(glm::vec3(a_input.pos4f[vInd * 4 + 0], a_input.pos4f[vInd * 4 + 1], a_input.pos4f[vInd * 4 + 2]));
      }
    }
    
    size_t ind_num = a_input.triNum * 3;
    vector<uint32_t> indices(a_input.indices, a_input.indices + ind_num);
  
    size_t vertex_num = a_input.vertNum;
    vector<Vertex> vertex(vertex_num);

    const float *read_ptr = a_input.pos4f;
    const float *read_ptr_normal = a_input.norm4f;
    
    for (auto &v : vertex) {
      memcpy(&v.pos, read_ptr, sizeof(v.pos));
      memcpy(&v.normal, read_ptr_normal, sizeof(v.normal));
      
      read_ptr += 4;
      read_ptr_normal += 4;
    }

    meshIdToReal[a_meshId].push_back(addMesh(make_pair(vertex, indices)));
    
    MeshAttrib<glm::vec3> c;
    c.data = currMesh;
    sceneCoord.push_back(c);
  }
  return true;
}


void RD_DXR_Experimental::BeginScene(pugi::xml_node a_sceneNode)
{
  sceneCoord.resize(0);

  printf("BeginScene\n");
}

void RD_DXR_Experimental::EndScene()
{
  printf("EndScene\n");

  std::vector<glm::vec3> vertices =
  {
      glm::vec3(0.5, 0.5,  0) + glm::vec3(0,          1,  0),
      glm::vec3(0.5, 0.5,  0) + glm::vec3(0.866f,  -0.5f, 0),
      glm::vec3(0.5, 0.5,  0) + glm::vec3(-0.866f, -0.5f, 0),
      glm::vec3(0.5, 1.5,  0) + glm::vec3(-0.866f, -0.5f, 0),
  };

  std::vector<glm::vec3> vertices2 =
  {
      glm::vec3(0,          1,  0),
      glm::vec3(0.866f,  -0.5f, 0),
      glm::vec3(-0.866f, -0.5f, 0),
  };

  // @BUG Fix leaking and infinite handle creation
  
  vector<vector<glm::vec3>> meshes;
  /*
  for (auto m : sceneCoord) {
    meshes.push_back(m.data);
  }
  */

  meshes.push_back(vertices);
  meshes.push_back(vertices2);

  static bool init = true;

  if (init) {
    init = false;
    initGeometry();
    createAccelerationStructures();                 // Tutorial 03
    createRtPipelineState();                        // Tutorial 04
    createShaderResources();                        // Tutorial 06
    createShaderTable();
  }

  updateShaderTable();                            // Tutorial 05
}

void RD_DXR_Experimental::Draw()
{
  uint32_t rtvIndex = beginFrame();

  // Let's raytrace
  resourceBarrier(mpCmdList, mpOutputResource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  D3D12_DISPATCH_RAYS_DESC raytraceDesc = {};
  raytraceDesc.Width = mSwapChainSize.x;
  raytraceDesc.Height = mSwapChainSize.y;
  raytraceDesc.Depth = 1;

  // RayGen is the first entry in the shader-table
  raytraceDesc.RayGenerationShaderRecord.StartAddress = mpShaderTable->GetGPUVirtualAddress() + 0 * mShaderTableEntrySize;
  raytraceDesc.RayGenerationShaderRecord.SizeInBytes = mShaderTableEntrySize;

  // Miss is the second entry in the shader-table
  size_t missOffset = 1 * mShaderTableEntrySize;
  raytraceDesc.MissShaderTable.StartAddress = mpShaderTable->GetGPUVirtualAddress() + missOffset;
  raytraceDesc.MissShaderTable.StrideInBytes = mShaderTableEntrySize;
  raytraceDesc.MissShaderTable.SizeInBytes = mShaderTableEntrySize * 2;   // 2 miss-entries

  // Hit is the fourth entry in the shader-table
  size_t hitOffset = 3 * mShaderTableEntrySize;
  raytraceDesc.HitGroupTable.StartAddress = mpShaderTable->GetGPUVirtualAddress() + hitOffset;
  raytraceDesc.HitGroupTable.StrideInBytes = mShaderTableEntrySize;
  raytraceDesc.HitGroupTable.SizeInBytes = mShaderTableEntrySize * 6;    // 8 hit-entries

  // Bind the empty root signature
  mpCmdList->SetComputeRootSignature(mpEmptyRootSig);

  // Dispatch
  mpCmdList->SetPipelineState1(mpPipelineState.GetInterfacePtr());
  mpCmdList->DispatchRays(&raytraceDesc);

  // Copy the results to the back-buffer
  resourceBarrier(mpCmdList, mpOutputResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  resourceBarrier(mpCmdList, mFrameObjects[rtvIndex].pSwapChainBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
  mpCmdList->CopyResource(mFrameObjects[rtvIndex].pSwapChainBuffer, mpOutputResource);

  endFrame(rtvIndex);

  printf("Draw\n");
}


void RD_DXR_Experimental::InstanceMeshes(int32_t a_mesh_id, const float* a_matrices, int32_t a_instNum, const int* a_lightInstId, const int* a_remapId, const int* a_realInstId)
{
  printf("InstanceMeshes\n");

  for (int n = 0; n < a_instNum; n++) {
    for (auto mid : meshIdToReal[a_mesh_id]) {
      Instance inst;
      inst.hitGroup = 0;
      inst.meshid = mid;

      for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
          inst.tr[i][j] = a_matrices[i + 4 * j + 16 * n];
        }
      }

      addInstance(inst);
    }
  }
}


void RD_DXR_Experimental::InstanceLights(int32_t a_light_id, const float* a_matrix, pugi::xml_node* a_custAttrArray, int32_t a_instNum, int32_t a_lightGroupId)
{
  printf("InstanceLights\n");
}

HRRenderUpdateInfo RD_DXR_Experimental::HaveUpdateNow(int a_maxRaysPerPixel)
{
  //glFlush();
  HRRenderUpdateInfo res;
  res.finalUpdate = true;
  res.haveUpdateFB = true;
  res.progress = 100.0f;
  return res;
}


void RD_DXR_Experimental::GetFrameBufferHDR(int32_t w, int32_t h, float*   a_out, const wchar_t* a_layerName)
{
  printf("GetFrameBufferHDR\n");
}

void RD_DXR_Experimental::GetFrameBufferLDR(int32_t w, int32_t h, int32_t* a_out)
{
  printf("GetFrameBufferLDR\n");
}


IHRRenderDriver* CreateDXRExperimental_RenderDriver()
{
  return new RD_DXR_Experimental;
}
