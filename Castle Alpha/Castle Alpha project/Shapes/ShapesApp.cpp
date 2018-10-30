//***************************************************************************************
// ShapesApp.cpp by Macro Orders (C) 2015 All Rights Reserved.
//
// Hold down '1' key to view scene in wireframe mode.
//***************************************************************************************

#include "../../Common/d3dApp.h"
#include "../../Common/MathHelper.h"
#include "../../Common/UploadBuffer.h"
#include "../../Common/GeometryGenerator.h"
#include "FrameResource.h"

#define deg2rad(x)(x * 3.14159265358979323846 / 180)

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace DirectX::PackedVector;

const int gNumFrameResources = 3;

// Lightweight structure stores parameters to draw a shape.  This will
// vary from app-to-app.
struct RenderItem
{
	RenderItem() = default;

    // World matrix of the shape that describes the object's local space
    // relative to the world space, which defines the position, orientation,
    // and scale of the object in the world.
    XMFLOAT4X4 World = MathHelper::Identity4x4();

	// Dirty flag indicating the object data has changed and we need to update the constant buffer.
	// Because we have an object cbuffer for each FrameResource, we have to apply the
	// update to each FrameResource.  Thus, when we modify obect data we should set 
	// NumFramesDirty = gNumFrameResources so that each frame resource gets the update.
	int NumFramesDirty = gNumFrameResources;

	// Index into GPU constant buffer corresponding to the ObjectCB for this render item.
	UINT ObjCBIndex = -1;

	MeshGeometry* Geo = nullptr;

    // Primitive topology.
    D3D12_PRIMITIVE_TOPOLOGY PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;

    // DrawIndexedInstanced parameters.
    UINT IndexCount = 0;
    UINT StartIndexLocation = 0;
    int BaseVertexLocation = 0;
};

class ShapesApp : public D3DApp
{
public:
    ShapesApp(HINSTANCE hInstance);
    ShapesApp(const ShapesApp& rhs) = delete;
    ShapesApp& operator=(const ShapesApp& rhs) = delete;
    ~ShapesApp();

    virtual bool Initialize()override;

private:
    virtual void OnResize()override;
    virtual void Update(const GameTimer& gt)override;
    virtual void Draw(const GameTimer& gt)override;

    virtual void OnMouseDown(WPARAM btnState, int x, int y)override;
    virtual void OnMouseUp(WPARAM btnState, int x, int y)override;
    virtual void OnMouseMove(WPARAM btnState, int x, int y)override;

    void OnKeyboardInput(const GameTimer& gt);
	void UpdateCamera(const GameTimer& gt);
	void UpdateObjectCBs(const GameTimer& gt);
	void UpdateMainPassCB(const GameTimer& gt);

    void BuildDescriptorHeaps();
    void BuildConstantBufferViews();
    void BuildRootSignature();
    void BuildShadersAndInputLayout();
    void BuildShapeGeometry();
    void BuildPSOs();
    void BuildFrameResources();
    void BuildRenderItems();
    void DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems);
 
private:

    std::vector<std::unique_ptr<FrameResource>> mFrameResources;
    FrameResource* mCurrFrameResource = nullptr;
    int mCurrFrameResourceIndex = 0;

    ComPtr<ID3D12RootSignature> mRootSignature = nullptr;
    ComPtr<ID3D12DescriptorHeap> mCbvHeap = nullptr;

	ComPtr<ID3D12DescriptorHeap> mSrvDescriptorHeap = nullptr;

	std::unordered_map<std::string, std::unique_ptr<MeshGeometry>> mGeometries;
	std::unordered_map<std::string, ComPtr<ID3DBlob>> mShaders;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> mPSOs;

    std::vector<D3D12_INPUT_ELEMENT_DESC> mInputLayout;

	// List of all the render items.
	std::vector<std::unique_ptr<RenderItem>> mAllRitems;

	// Render items divided by PSO.
	std::vector<RenderItem*> mOpaqueRitems;

    PassConstants mMainPassCB;

    UINT mPassCbvOffset = 0;

    bool mIsWireframe = false;

	XMFLOAT3 mEyePos = { 0.0f, 0.0f, 0.0f };
	XMFLOAT4X4 mView = MathHelper::Identity4x4();
	XMFLOAT4X4 mProj = MathHelper::Identity4x4();

    float mTheta = 1.5f*XM_PI;
    float mPhi = 0.2f*XM_PI;
    float mRadius = 15.0f;

    POINT mLastMousePos;
};

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE prevInstance,
    PSTR cmdLine, int showCmd)
{
    // Enable run-time memory check for debug builds.
#if defined(DEBUG) | defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    try
    {
        ShapesApp theApp(hInstance);
        if(!theApp.Initialize())
            return 0;

        return theApp.Run();
    }
    catch(DxException& e)
    {
        MessageBox(nullptr, e.ToString().c_str(), L"HR Failed", MB_OK);
        return 0;
    }
}

ShapesApp::ShapesApp(HINSTANCE hInstance)
    : D3DApp(hInstance)
{
}

ShapesApp::~ShapesApp()
{
    if(md3dDevice != nullptr)
        FlushCommandQueue();
}

bool ShapesApp::Initialize()
{
    if(!D3DApp::Initialize())
        return false;

    // Reset the command list to prep for initialization commands.
    ThrowIfFailed(mCommandList->Reset(mDirectCmdListAlloc.Get(), nullptr));

    BuildRootSignature();
    BuildShadersAndInputLayout();
    BuildShapeGeometry();
    BuildRenderItems();
    BuildFrameResources();
    BuildDescriptorHeaps();
    BuildConstantBufferViews();
    BuildPSOs();

    // Execute the initialization commands.
    ThrowIfFailed(mCommandList->Close());
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Wait until initialization is complete.
    FlushCommandQueue();

    return true;
}
 
void ShapesApp::OnResize()
{
    D3DApp::OnResize();

    // The window resized, so update the aspect ratio and recompute the projection matrix.
    XMMATRIX P = XMMatrixPerspectiveFovLH(0.25f*MathHelper::Pi, AspectRatio(), 1.0f, 1000.0f);
    XMStoreFloat4x4(&mProj, P);
}

void ShapesApp::Update(const GameTimer& gt)
{
    OnKeyboardInput(gt);
	UpdateCamera(gt);

    // Cycle through the circular frame resource array.
    mCurrFrameResourceIndex = (mCurrFrameResourceIndex + 1) % gNumFrameResources;
    mCurrFrameResource = mFrameResources[mCurrFrameResourceIndex].get();

    // Has the GPU finished processing the commands of the current frame resource?
    // If not, wait until the GPU has completed commands up to this fence point.
    if(mCurrFrameResource->Fence != 0 && mFence->GetCompletedValue() < mCurrFrameResource->Fence)
    {
        HANDLE eventHandle = CreateEventEx(nullptr, false, false, EVENT_ALL_ACCESS);
        ThrowIfFailed(mFence->SetEventOnCompletion(mCurrFrameResource->Fence, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

	UpdateObjectCBs(gt);
	UpdateMainPassCB(gt);
}

void ShapesApp::Draw(const GameTimer& gt)
{
    auto cmdListAlloc = mCurrFrameResource->CmdListAlloc;

    // Reuse the memory associated with command recording.
    // We can only reset when the associated command lists have finished execution on the GPU.
    ThrowIfFailed(cmdListAlloc->Reset());

    // A command list can be reset after it has been added to the command queue via ExecuteCommandList.
    // Reusing the command list reuses memory.
    if(mIsWireframe)
    {
        ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque_wireframe"].Get()));
    }
    else
    {
        ThrowIfFailed(mCommandList->Reset(cmdListAlloc.Get(), mPSOs["opaque"].Get()));
    }

    mCommandList->RSSetViewports(1, &mScreenViewport);
    mCommandList->RSSetScissorRects(1, &mScissorRect);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));

    // Clear the back buffer and depth buffer.
    mCommandList->ClearRenderTargetView(CurrentBackBufferView(), Colors::White, 0, nullptr);
    mCommandList->ClearDepthStencilView(DepthStencilView(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    // Specify the buffers we are going to render to.
    mCommandList->OMSetRenderTargets(1, &CurrentBackBufferView(), true, &DepthStencilView());

    ID3D12DescriptorHeap* descriptorHeaps[] = { mCbvHeap.Get() };
    mCommandList->SetDescriptorHeaps(_countof(descriptorHeaps), descriptorHeaps);

	mCommandList->SetGraphicsRootSignature(mRootSignature.Get());

    int passCbvIndex = mPassCbvOffset + mCurrFrameResourceIndex;
    auto passCbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
    passCbvHandle.Offset(passCbvIndex, mCbvSrvUavDescriptorSize);
    mCommandList->SetGraphicsRootDescriptorTable(1, passCbvHandle);

    DrawRenderItems(mCommandList.Get(), mOpaqueRitems);

    // Indicate a state transition on the resource usage.
	mCommandList->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(CurrentBackBuffer(),
		D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    // Done recording commands.
    ThrowIfFailed(mCommandList->Close());

    // Add the command list to the queue for execution.
    ID3D12CommandList* cmdsLists[] = { mCommandList.Get() };
    mCommandQueue->ExecuteCommandLists(_countof(cmdsLists), cmdsLists);

    // Swap the back and front buffers
    ThrowIfFailed(mSwapChain->Present(0, 0));
	mCurrBackBuffer = (mCurrBackBuffer + 1) % SwapChainBufferCount;

    // Advance the fence value to mark commands up to this fence point.
    mCurrFrameResource->Fence = ++mCurrentFence;
    
    // Add an instruction to the command queue to set a new fence point. 
    // Because we are on the GPU timeline, the new fence point won't be 
    // set until the GPU finishes processing all the commands prior to this Signal().
    mCommandQueue->Signal(mFence.Get(), mCurrentFence);
}

void ShapesApp::OnMouseDown(WPARAM btnState, int x, int y)
{
    mLastMousePos.x = x;
    mLastMousePos.y = y;

    SetCapture(mhMainWnd);
}

void ShapesApp::OnMouseUp(WPARAM btnState, int x, int y)
{
    ReleaseCapture();
}

void ShapesApp::OnMouseMove(WPARAM btnState, int x, int y)
{
    if((btnState & MK_LBUTTON) != 0)
    {
        // Make each pixel correspond to a quarter of a degree.
        float dx = XMConvertToRadians(0.25f*static_cast<float>(x - mLastMousePos.x));
        float dy = XMConvertToRadians(0.25f*static_cast<float>(y - mLastMousePos.y));

        // Update angles based on input to orbit camera around box.
        mTheta += dx;
        mPhi += dy;

        // Restrict the angle mPhi.
        mPhi = MathHelper::Clamp(mPhi, 0.1f, MathHelper::Pi - 0.1f);
    }
    else if((btnState & MK_RBUTTON) != 0)
    {
        // Make each pixel correspond to 0.2 unit in the scene.
        float dx = 0.05f*static_cast<float>(x - mLastMousePos.x);
        float dy = 0.05f*static_cast<float>(y - mLastMousePos.y);

        // Update the camera radius based on input.
        mRadius += dx - dy;

        // Restrict the radius.
        mRadius = MathHelper::Clamp(mRadius, 5.0f, 150.0f);
    }

    mLastMousePos.x = x;
    mLastMousePos.y = y;
}
 
void ShapesApp::OnKeyboardInput(const GameTimer& gt)
{
    if(GetAsyncKeyState('1') & 0x8000)
        mIsWireframe = false;
    else
        mIsWireframe = true;
}
 
void ShapesApp::UpdateCamera(const GameTimer& gt)
{
	// Convert Spherical to Cartesian coordinates.
	mEyePos.x = mRadius*sinf(mPhi)*cosf(mTheta);
	mEyePos.z = mRadius*sinf(mPhi)*sinf(mTheta);
	mEyePos.y = mRadius*cosf(mPhi);

	// Build the view matrix.
	XMVECTOR pos = XMVectorSet(mEyePos.x, mEyePos.y, mEyePos.z, 1.0f);
	XMVECTOR target = XMVectorZero();
	XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

	XMMATRIX view = XMMatrixLookAtLH(pos, target, up);
	XMStoreFloat4x4(&mView, view);
}

void ShapesApp::UpdateObjectCBs(const GameTimer& gt)
{
	auto currObjectCB = mCurrFrameResource->ObjectCB.get();
	for(auto& e : mAllRitems)
	{
		// Only update the cbuffer data if the constants have changed.  
		// This needs to be tracked per frame resource.
		if(e->NumFramesDirty > 0)
		{
			XMMATRIX world = XMLoadFloat4x4(&e->World);

			ObjectConstants objConstants;
			XMStoreFloat4x4(&objConstants.World, XMMatrixTranspose(world));

			currObjectCB->CopyData(e->ObjCBIndex, objConstants);

			// Next FrameResource need to be updated too.
			e->NumFramesDirty--;
		}
	}
}

void ShapesApp::UpdateMainPassCB(const GameTimer& gt)
{
	XMMATRIX view = XMLoadFloat4x4(&mView);
	XMMATRIX proj = XMLoadFloat4x4(&mProj);

	XMMATRIX viewProj = XMMatrixMultiply(view, proj);
	XMMATRIX invView = XMMatrixInverse(&XMMatrixDeterminant(view), view);
	XMMATRIX invProj = XMMatrixInverse(&XMMatrixDeterminant(proj), proj);
	XMMATRIX invViewProj = XMMatrixInverse(&XMMatrixDeterminant(viewProj), viewProj);

	XMStoreFloat4x4(&mMainPassCB.View, XMMatrixTranspose(view));
	XMStoreFloat4x4(&mMainPassCB.InvView, XMMatrixTranspose(invView));
	XMStoreFloat4x4(&mMainPassCB.Proj, XMMatrixTranspose(proj));
	XMStoreFloat4x4(&mMainPassCB.InvProj, XMMatrixTranspose(invProj));
	XMStoreFloat4x4(&mMainPassCB.ViewProj, XMMatrixTranspose(viewProj));
	XMStoreFloat4x4(&mMainPassCB.InvViewProj, XMMatrixTranspose(invViewProj));
	mMainPassCB.EyePosW = mEyePos;
	mMainPassCB.RenderTargetSize = XMFLOAT2((float)mClientWidth, (float)mClientHeight);
	mMainPassCB.InvRenderTargetSize = XMFLOAT2(1.0f / mClientWidth, 1.0f / mClientHeight);
	mMainPassCB.NearZ = 1.0f;
	mMainPassCB.FarZ = 1000.0f;
	mMainPassCB.TotalTime = gt.TotalTime();
	mMainPassCB.DeltaTime = gt.DeltaTime();

	auto currPassCB = mCurrFrameResource->PassCB.get();
	currPassCB->CopyData(0, mMainPassCB);
}

void ShapesApp::BuildDescriptorHeaps()
{
    UINT objCount = (UINT)mOpaqueRitems.size();

    // Need a CBV descriptor for each object for each frame resource,
    // +1 for the perPass CBV for each frame resource.
    UINT numDescriptors = (objCount+1) * gNumFrameResources;

    // Save an offset to the start of the pass CBVs.  These are the last 3 descriptors.
    mPassCbvOffset = objCount * gNumFrameResources;

    D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc;
    cbvHeapDesc.NumDescriptors = numDescriptors;
    cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbvHeapDesc.NodeMask = 0;
    ThrowIfFailed(md3dDevice->CreateDescriptorHeap(&cbvHeapDesc,
        IID_PPV_ARGS(&mCbvHeap)));
}

void ShapesApp::BuildConstantBufferViews()
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));

    UINT objCount = (UINT)mOpaqueRitems.size();

    // Need a CBV descriptor for each object for each frame resource.
    for(int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto objectCB = mFrameResources[frameIndex]->ObjectCB->Resource();
        for(UINT i = 0; i < objCount; ++i)
        {
            D3D12_GPU_VIRTUAL_ADDRESS cbAddress = objectCB->GetGPUVirtualAddress();

            // Offset to the ith object constant buffer in the buffer.
            cbAddress += i*objCBByteSize;

            // Offset to the object cbv in the descriptor heap.
            int heapIndex = frameIndex*objCount + i;
            auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
            handle.Offset(heapIndex, mCbvSrvUavDescriptorSize);

            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
            cbvDesc.BufferLocation = cbAddress;
            cbvDesc.SizeInBytes = objCBByteSize;

            md3dDevice->CreateConstantBufferView(&cbvDesc, handle);
        }
    }

    UINT passCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(PassConstants));

    // Last three descriptors are the pass CBVs for each frame resource.
    for(int frameIndex = 0; frameIndex < gNumFrameResources; ++frameIndex)
    {
        auto passCB = mFrameResources[frameIndex]->PassCB->Resource();
        D3D12_GPU_VIRTUAL_ADDRESS cbAddress = passCB->GetGPUVirtualAddress();

        // Offset to the pass cbv in the descriptor heap.
        int heapIndex = mPassCbvOffset + frameIndex;
        auto handle = CD3DX12_CPU_DESCRIPTOR_HANDLE(mCbvHeap->GetCPUDescriptorHandleForHeapStart());
        handle.Offset(heapIndex, mCbvSrvUavDescriptorSize);

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc;
        cbvDesc.BufferLocation = cbAddress;
        cbvDesc.SizeInBytes = passCBByteSize;
        
        md3dDevice->CreateConstantBufferView(&cbvDesc, handle);
    }
}

void ShapesApp::BuildRootSignature()
{
    CD3DX12_DESCRIPTOR_RANGE cbvTable0;
    cbvTable0.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0);

    CD3DX12_DESCRIPTOR_RANGE cbvTable1;
    cbvTable1.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1);

	// Root parameter can be a table, root descriptor or root constants.
	CD3DX12_ROOT_PARAMETER slotRootParameter[2];

	// Create root CBVs.
    slotRootParameter[0].InitAsDescriptorTable(1, &cbvTable0);
    slotRootParameter[1].InitAsDescriptorTable(1, &cbvTable1);

	// A root signature is an array of root parameters.
	CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc(2, slotRootParameter, 0, nullptr, 
        D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

	// create a root signature with a single slot which points to a descriptor range consisting of a single constant buffer
	ComPtr<ID3DBlob> serializedRootSig = nullptr;
	ComPtr<ID3DBlob> errorBlob = nullptr;
	HRESULT hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
		serializedRootSig.GetAddressOf(), errorBlob.GetAddressOf());

	if(errorBlob != nullptr)
	{
		::OutputDebugStringA((char*)errorBlob->GetBufferPointer());
	}
	ThrowIfFailed(hr);

	ThrowIfFailed(md3dDevice->CreateRootSignature(
		0,
		serializedRootSig->GetBufferPointer(),
		serializedRootSig->GetBufferSize(),
		IID_PPV_ARGS(mRootSignature.GetAddressOf())));
}

void ShapesApp::BuildShadersAndInputLayout()
{
	mShaders["standardVS"] = d3dUtil::CompileShader(L"Shaders\\color.hlsl", nullptr, "VS", "vs_5_1");
	mShaders["opaquePS"] = d3dUtil::CompileShader(L"Shaders\\color.hlsl", nullptr, "PS", "ps_5_1");
	
    mInputLayout =
    {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
}

void ShapesApp::BuildShapeGeometry()////////////////////////////////////////////////////////////////////////////
{
    GeometryGenerator geoGen;
	GeometryGenerator::MeshData box = geoGen.CreateBox(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData grid = geoGen.CreateGrid(20.0f, 30.0f, 60, 40);
	GeometryGenerator::MeshData sphere = geoGen.CreateSphere(0.5f, 20, 20);
	GeometryGenerator::MeshData cylinder = geoGen.CreateCylinder(0.5f, 0.5f, 1.0f, 20, 20);
	GeometryGenerator::MeshData hexagon = geoGen.CreateHexagon(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData tetrahedron = geoGen.CreateTetrahedron(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData pyramid = geoGen.CreatePyramid(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData diamond = geoGen.CreateDiamond(3.0f, 10.0f, 3.0f, 3);
	GeometryGenerator::MeshData cone = geoGen.CreateCone(0.5f, 1.0f, 20, 20);
	GeometryGenerator::MeshData wedge = geoGen.CreateWedge(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData quad = geoGen.CreateQuad(0.0f, 0.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData bar = geoGen.CreateBar(1.0f, 1.0f, 1.0f, 3);
	GeometryGenerator::MeshData boxthree = box;
	GeometryGenerator::MeshData boxfour = box;
	GeometryGenerator::MeshData boxfive = box;
	GeometryGenerator::MeshData boxsix = box;
	GeometryGenerator::MeshData cylinder2 = cylinder;
	GeometryGenerator::MeshData cylinder3 = cylinder;
	GeometryGenerator::MeshData cylinder4 = cylinder;
	GeometryGenerator::MeshData cylinder5 = cylinder;
	GeometryGenerator::MeshData wedge2 = wedge;
	GeometryGenerator::MeshData wedge3 = wedge;
	GeometryGenerator::MeshData wedge4 = wedge;
	GeometryGenerator::MeshData cone2 = cone;
	GeometryGenerator::MeshData cone3 = cone;
	GeometryGenerator::MeshData cone4 = cone;
	GeometryGenerator::MeshData cone5 = cone;
	GeometryGenerator::MeshData geosphere = geoGen.CreateGeosphere(0.5, 3);

	//
	// We are concatenating all the geometry into one big vertex/index buffer.  So
	// define the regions in the buffer each submesh covers.
	//

	// Cache the vertex offsets to each object in the concatenated vertex buffer.///////////////////////////////
	UINT boxVertexOffset = 0;
	UINT gridVertexOffset = (UINT)box.Vertices.size();
	UINT sphereVertexOffset = gridVertexOffset + (UINT)grid.Vertices.size();
	UINT cylinderVertexOffset = sphereVertexOffset + (UINT)sphere.Vertices.size();
	UINT hexagonVertexOffset = cylinderVertexOffset + (UINT)cylinder.Vertices.size();
	UINT tetrahedronVertexOffset = hexagonVertexOffset + (UINT)hexagon.Vertices.size();
	UINT pyramidVertexOffset = tetrahedronVertexOffset + (UINT)tetrahedron.Vertices.size();
	UINT diamondVertexOffset = pyramidVertexOffset + (UINT)pyramid.Vertices.size();	
	UINT coneVertexOffset = diamondVertexOffset + (UINT)diamond.Vertices.size();
	UINT wedgeVertexOffset = coneVertexOffset + (UINT)cone.Vertices.size();
	UINT quadVertexOffset = wedgeVertexOffset + (UINT)wedge.Vertices.size();
	UINT barVertexOffset = quadVertexOffset + (UINT)quad.Vertices.size();
	UINT boxthreeVertexOffset = barVertexOffset + (UINT)bar.Vertices.size();
	UINT boxfourVertexOffset = boxthreeVertexOffset + (UINT)boxthree.Vertices.size();
	UINT boxfiveVertexOffset = boxfourVertexOffset + (UINT)boxfour.Vertices.size();
	UINT boxsixVertexOffset = boxfiveVertexOffset + (UINT)boxfive.Vertices.size();
	UINT cylinder2VertexOffset = boxsixVertexOffset + (UINT)box.Vertices.size();
	UINT cylinder3VertexOffset = cylinder2VertexOffset + (UINT)cylinder.Vertices.size();
	UINT cylinder4VertexOffset = cylinder3VertexOffset + (UINT)cylinder.Vertices.size();
	UINT cylinder5VertexOffset = cylinder4VertexOffset + (UINT)cylinder.Vertices.size();
	UINT wedge2VertexOffset = cylinder5VertexOffset + (UINT)cylinder.Vertices.size();
	UINT wedge3VertexOffset = wedge2VertexOffset + (UINT)wedge.Vertices.size();
	UINT wedge4VertexOffset = wedge3VertexOffset + (UINT)wedge.Vertices.size();
	UINT cone2VertexOffset = wedge4VertexOffset + (UINT)wedge.Vertices.size();
	UINT cone3VertexOffset = cone2VertexOffset + (UINT)cone.Vertices.size();
	UINT cone4VertexOffset = cone3VertexOffset + (UINT)cone.Vertices.size();
	UINT cone5VertexOffset = cone4VertexOffset + (UINT)cone.Vertices.size();
	UINT geosphereVertexOffset = cone5VertexOffset + (UINT)cone.Vertices.size();

	// Cache the starting index for each object in the concatenated index buffer./////////////////////////////////
	UINT boxIndexOffset = 0;
	UINT gridIndexOffset = (UINT)box.Indices32.size();
	UINT sphereIndexOffset = gridIndexOffset + (UINT)grid.Indices32.size();
	UINT cylinderIndexOffset = sphereIndexOffset + (UINT)sphere.Indices32.size();
	UINT hexagonIndexOffset = cylinderIndexOffset + (UINT)cylinder.Indices32.size();
	UINT tetrahedronIndexOffset = hexagonIndexOffset + (UINT)hexagon.Indices32.size();
	UINT pyramidIndexOffset = tetrahedronIndexOffset + (UINT)tetrahedron.Indices32.size();
	UINT diamondIndexOffset = pyramidIndexOffset + (UINT)pyramid.Indices32.size();
	UINT coneIndexOffset = diamondIndexOffset + (UINT)diamond.Indices32.size();
	UINT wedgeIndexOffset = coneIndexOffset + (UINT)cone.Indices32.size();
	UINT quadIndexOffset = wedgeIndexOffset + (UINT)wedge.Indices32.size();
	UINT barIndexOffset = quadIndexOffset + (UINT)quad.Indices32.size();
	UINT boxthreeIndexOffset = barIndexOffset + (UINT)bar.Indices32.size();
	UINT boxfourIndexOffset = boxthreeIndexOffset + (UINT)boxthree.Indices32.size();
	UINT boxfiveIndexOffset = boxfourIndexOffset + (UINT)boxfour.Indices32.size();
	UINT boxsixIndexOffset = boxfiveIndexOffset + (UINT)boxfive.Indices32.size();
	UINT cylinder2IndexOffset = boxsixIndexOffset + (UINT)box.Indices32.size();
	UINT cylinder3IndexOffset = cylinder2IndexOffset + (UINT)cylinder.Indices32.size();
	UINT cylinder4IndexOffset = cylinder3IndexOffset + (UINT)cylinder.Indices32.size();
	UINT cylinder5IndexOffset = cylinder4IndexOffset + (UINT)cylinder.Indices32.size();
	UINT wedge2IndexOffset = cylinder5IndexOffset + (UINT)cylinder.Indices32.size();
	UINT wedge3IndexOffset = wedge2IndexOffset + (UINT)wedge.Indices32.size();
	UINT wedge4IndexOffset = wedge3IndexOffset + (UINT)wedge.Indices32.size();
	UINT cone2IndexOffset = wedge4IndexOffset + (UINT)wedge.Indices32.size();
	UINT cone3IndexOffset = cone2IndexOffset + (UINT)cone.Indices32.size();
	UINT cone4IndexOffset = cone3IndexOffset + (UINT)cone.Indices32.size();
	UINT cone5IndexOffset = cone4IndexOffset + (UINT)cone.Indices32.size();
	UINT geosphereIndexOffset = cone5IndexOffset + (UINT)cone.Indices32.size();

    // Define the SubmeshGeometry that cover different 
    // regions of the vertex/index buffers.

	////////////////////////////////////////////////////////////////////////////////submesh
	SubmeshGeometry boxSubmesh;
	boxSubmesh.IndexCount = (UINT)box.Indices32.size();
	boxSubmesh.StartIndexLocation = boxIndexOffset;
	boxSubmesh.BaseVertexLocation = boxVertexOffset;

	SubmeshGeometry gridSubmesh;
	gridSubmesh.IndexCount = (UINT)grid.Indices32.size();
	gridSubmesh.StartIndexLocation = gridIndexOffset;
	gridSubmesh.BaseVertexLocation = gridVertexOffset;

	SubmeshGeometry sphereSubmesh;
	sphereSubmesh.IndexCount = (UINT)sphere.Indices32.size();
	sphereSubmesh.StartIndexLocation = sphereIndexOffset;
	sphereSubmesh.BaseVertexLocation = sphereVertexOffset;

	SubmeshGeometry cylinderSubmesh;
	cylinderSubmesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinderSubmesh.StartIndexLocation = cylinderIndexOffset;
	cylinderSubmesh.BaseVertexLocation = cylinderVertexOffset;

	SubmeshGeometry hexagonSubmesh;
	hexagonSubmesh.IndexCount = (UINT)hexagon.Indices32.size();
	hexagonSubmesh.StartIndexLocation = hexagonIndexOffset;
	hexagonSubmesh.BaseVertexLocation = hexagonVertexOffset;

	SubmeshGeometry tetrahedronSubmesh;
	tetrahedronSubmesh.IndexCount = (UINT)tetrahedron.Indices32.size();
	tetrahedronSubmesh.StartIndexLocation = tetrahedronIndexOffset;
	tetrahedronSubmesh.BaseVertexLocation = tetrahedronVertexOffset;

	SubmeshGeometry pyramidSubmesh;
	pyramidSubmesh.IndexCount = (UINT)pyramid.Indices32.size();
	pyramidSubmesh.StartIndexLocation = pyramidIndexOffset;
	pyramidSubmesh.BaseVertexLocation = pyramidVertexOffset;

	SubmeshGeometry diamondSubmesh;
	diamondSubmesh.IndexCount = (UINT)diamond.Indices32.size();
	diamondSubmesh.StartIndexLocation = diamondIndexOffset;
	diamondSubmesh.BaseVertexLocation = diamondVertexOffset;

	SubmeshGeometry coneSubmesh;
	coneSubmesh.IndexCount = (UINT)cone.Indices32.size();
	coneSubmesh.StartIndexLocation = coneIndexOffset;
	coneSubmesh.BaseVertexLocation = coneVertexOffset;

	SubmeshGeometry wedgeSubmesh;
	wedgeSubmesh.IndexCount = (UINT)wedge.Indices32.size();
	wedgeSubmesh.StartIndexLocation = wedgeIndexOffset;
	wedgeSubmesh.BaseVertexLocation = wedgeVertexOffset;
	
	SubmeshGeometry quadSubmesh;
	quadSubmesh.IndexCount = (UINT)quad.Indices32.size();
	quadSubmesh.StartIndexLocation = quadIndexOffset;
	quadSubmesh.BaseVertexLocation = quadVertexOffset;

	SubmeshGeometry barSubmesh;
	barSubmesh.IndexCount = (UINT)bar.Indices32.size();
	barSubmesh.StartIndexLocation = barIndexOffset;
	barSubmesh.BaseVertexLocation = barVertexOffset;

	SubmeshGeometry boxthreeSubmesh;
	boxthreeSubmesh.IndexCount = (UINT)boxthree.Indices32.size();
	boxthreeSubmesh.StartIndexLocation = boxthreeIndexOffset;
	boxthreeSubmesh.BaseVertexLocation = boxthreeVertexOffset;

	SubmeshGeometry boxfourSubmesh;
	boxfourSubmesh.IndexCount = (UINT)boxfour.Indices32.size();
	boxfourSubmesh.StartIndexLocation = boxfourIndexOffset;
	boxfourSubmesh.BaseVertexLocation = boxfourVertexOffset;

	SubmeshGeometry boxfiveSubmesh;
	boxfiveSubmesh.IndexCount = (UINT)boxfive.Indices32.size();
	boxfiveSubmesh.StartIndexLocation = boxfiveIndexOffset;
	boxfiveSubmesh.BaseVertexLocation = boxfiveVertexOffset;

	SubmeshGeometry boxsixSubmesh;
	boxsixSubmesh.IndexCount = (UINT)boxsix.Indices32.size();
	boxsixSubmesh.StartIndexLocation = boxsixIndexOffset;
	boxsixSubmesh.BaseVertexLocation = boxsixVertexOffset;

	SubmeshGeometry cylinder2Submesh;
	cylinder2Submesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinder2Submesh.StartIndexLocation = cylinder2IndexOffset;
	cylinder2Submesh.BaseVertexLocation = cylinder2VertexOffset;

	SubmeshGeometry cylinder3Submesh;
	cylinder3Submesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinder3Submesh.StartIndexLocation = cylinder3IndexOffset;
	cylinder3Submesh.BaseVertexLocation = cylinder3VertexOffset;

	SubmeshGeometry cylinder4Submesh;
	cylinder4Submesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinder4Submesh.StartIndexLocation = cylinder4IndexOffset;
	cylinder4Submesh.BaseVertexLocation = cylinder4VertexOffset;

	SubmeshGeometry cylinder5Submesh;
	cylinder5Submesh.IndexCount = (UINT)cylinder.Indices32.size();
	cylinder5Submesh.StartIndexLocation = cylinder5IndexOffset;
	cylinder5Submesh.BaseVertexLocation = cylinder5VertexOffset;

	SubmeshGeometry wedge2Submesh;
	wedge2Submesh.IndexCount = (UINT)wedge.Indices32.size();
	wedge2Submesh.StartIndexLocation = wedge2IndexOffset;
	wedge2Submesh.BaseVertexLocation = wedge2VertexOffset;

	SubmeshGeometry wedge3Submesh;
	wedge3Submesh.IndexCount = (UINT)wedge.Indices32.size();
	wedge3Submesh.StartIndexLocation = wedge3IndexOffset;
	wedge3Submesh.BaseVertexLocation = wedge3VertexOffset;

	SubmeshGeometry wedge4Submesh;
	wedge4Submesh.IndexCount = (UINT)wedge.Indices32.size();
	wedge4Submesh.StartIndexLocation = wedge4IndexOffset;
	wedge4Submesh.BaseVertexLocation = wedge4VertexOffset;

	SubmeshGeometry cone2Submesh;
	cone2Submesh.IndexCount = (UINT)cone.Indices32.size();
	cone2Submesh.StartIndexLocation = cone2IndexOffset;
	cone2Submesh.BaseVertexLocation = cone2VertexOffset;

	SubmeshGeometry cone3Submesh;
	cone3Submesh.IndexCount = (UINT)cone.Indices32.size();
	cone3Submesh.StartIndexLocation = cone3IndexOffset;
	cone3Submesh.BaseVertexLocation = cone3VertexOffset;

	SubmeshGeometry cone4Submesh;
	cone4Submesh.IndexCount = (UINT)cone.Indices32.size();
	cone4Submesh.StartIndexLocation = cone4IndexOffset;
	cone4Submesh.BaseVertexLocation = cone4VertexOffset;

	SubmeshGeometry cone5Submesh;
	cone5Submesh.IndexCount = (UINT)cone.Indices32.size();
	cone5Submesh.StartIndexLocation = cone5IndexOffset;
	cone5Submesh.BaseVertexLocation = cone5VertexOffset;

	SubmeshGeometry geosphereSubmesh;
	geosphereSubmesh.IndexCount = (UINT)geosphere.Indices32.size();
	geosphereSubmesh.StartIndexLocation = geosphereIndexOffset;
	geosphereSubmesh.BaseVertexLocation = geosphereVertexOffset;
	//
	// Extract the vertex elements we are interested in and pack the
	// vertices of all the meshes into one vertex buffer.
	//
	/////////////////////////////////////////////////////////////total vertex count
	auto totalVertexCount =
		box.Vertices.size() +
		grid.Vertices.size() +
		sphere.Vertices.size() +
		cylinder.Vertices.size() * 5 +
		hexagon.Vertices.size() +
		tetrahedron.Vertices.size() +
		pyramid.Vertices.size() +
		diamond.Vertices.size() +
		cone.Vertices.size() * 5 +
		wedge.Vertices.size() * 4 +
		quad.Vertices.size() +
		bar.Vertices.size() +
		boxthree.Vertices.size() +
		boxfour.Vertices.size() +
		boxfive.Vertices.size() +
		boxsix.Vertices.size() +
		geosphere.Vertices.size();

	std::vector<Vertex> vertices(totalVertexCount);

	////////////////////////////////////////////////////////////////////////vertices loop
	UINT k = 0;
	for(size_t i = 0; i < box.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = box.Vertices[i].Position;
        vertices[k].Color = XMFLOAT4(DirectX::Colors::Black);
	}

	for(size_t i = 0; i < grid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = grid.Vertices[i].Position;
        vertices[k].Color = XMFLOAT4(DirectX::Colors::RosyBrown);
	}

	for(size_t i = 0; i < sphere.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = sphere.Vertices[i].Position;
        vertices[k].Color = XMFLOAT4(DirectX::Colors::Crimson);
	}

	for(size_t i = 0; i < cylinder.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Gold);
	}

	for (size_t i = 0; i < hexagon.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = hexagon.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Aqua);
	}

	for (size_t i = 0; i < tetrahedron.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = tetrahedron.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Gray);
	}

	for (size_t i = 0; i < pyramid.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = pyramid.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Pink);
	}

	for (size_t i = 0; i < diamond.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = diamond.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Magenta);
	}

	for (size_t i = 0; i < cone.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cone.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Green);
	}

	for (size_t i = 0; i < wedge.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = wedge.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Red);
	}

	for (size_t i = 0; i < quad.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = quad.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Silver);
	}

	for (size_t i = 0; i < bar.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = bar.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Black);
	}

	for (size_t i = 0; i < boxthree.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = boxthree.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Black);
	}

	for (size_t i = 0; i < boxfour.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = boxfour.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Black);
	}

	for (size_t i = 0; i < boxfive.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = boxfive.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Black);
	}

	for (size_t i = 0; i < boxsix.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = boxsix.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Black);
	}

	for (size_t i = 0; i < cylinder2.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder2.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Gold);
	}

	for (size_t i = 0; i < cylinder3.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder3.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Gold);
	}

	for (size_t i = 0; i < cylinder4.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder4.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Gold);
	}

	for (size_t i = 0; i < cylinder5.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cylinder5.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Gold);
	}

	for (size_t i = 0; i < wedge2.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = wedge2.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Red);
	}

	for (size_t i = 0; i < wedge3.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = wedge3.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Red);
	}

	for (size_t i = 0; i < wedge4.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = wedge4.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Red);
	}

	for (size_t i = 0; i < cone2.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cone2.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Green);
	}

	for (size_t i = 0; i < cone3.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cone3.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Green);
	}

	for (size_t i = 0; i < cone4.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cone4.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Green);
	}

	for (size_t i = 0; i < cone5.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = cone5.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Green);
	}

	for (size_t i = 0; i < geosphere.Vertices.size(); ++i, ++k)
	{
		vertices[k].Pos = geosphere.Vertices[i].Position;
		vertices[k].Color = XMFLOAT4(DirectX::Colors::Crimson);
	}

	/////////////////////////////////////////////////////////////////////insert indices
	std::vector<std::uint16_t> indices;
	indices.insert(indices.end(), std::begin(box.GetIndices16()), std::end(box.GetIndices16()));
	indices.insert(indices.end(), std::begin(grid.GetIndices16()), std::end(grid.GetIndices16()));
	indices.insert(indices.end(), std::begin(sphere.GetIndices16()), std::end(sphere.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder.GetIndices16()), std::end(cylinder.GetIndices16()));
	indices.insert(indices.end(), std::begin(hexagon.GetIndices16()), std::end(hexagon.GetIndices16()));
	indices.insert(indices.end(), std::begin(tetrahedron.GetIndices16()), std::end(tetrahedron.GetIndices16()));
	indices.insert(indices.end(), std::begin(pyramid.GetIndices16()), std::end(pyramid.GetIndices16()));
	indices.insert(indices.end(), std::begin(diamond.GetIndices16()), std::end(diamond.GetIndices16()));
	indices.insert(indices.end(), std::begin(cone.GetIndices16()), std::end(cone.GetIndices16()));
	indices.insert(indices.end(), std::begin(wedge.GetIndices16()), std::end(wedge.GetIndices16()));
	indices.insert(indices.end(), std::begin(quad.GetIndices16()), std::end(quad.GetIndices16()));
	indices.insert(indices.end(), std::begin(bar.GetIndices16()), std::end(bar.GetIndices16()));
	indices.insert(indices.end(), std::begin(boxthree.GetIndices16()), std::end(boxthree.GetIndices16()));
	indices.insert(indices.end(), std::begin(boxfour.GetIndices16()), std::end(boxfour.GetIndices16()));
	indices.insert(indices.end(), std::begin(boxfive.GetIndices16()), std::end(boxfive.GetIndices16()));
	indices.insert(indices.end(), std::begin(boxsix.GetIndices16()), std::end(boxsix.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder2.GetIndices16()), std::end(cylinder2.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder3.GetIndices16()), std::end(cylinder3.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder4.GetIndices16()), std::end(cylinder4.GetIndices16()));
	indices.insert(indices.end(), std::begin(cylinder5.GetIndices16()), std::end(cylinder5.GetIndices16()));
	indices.insert(indices.end(), std::begin(wedge2.GetIndices16()), std::end(wedge2.GetIndices16()));
	indices.insert(indices.end(), std::begin(wedge3.GetIndices16()), std::end(wedge3.GetIndices16()));
	indices.insert(indices.end(), std::begin(wedge4.GetIndices16()), std::end(wedge4.GetIndices16()));
	indices.insert(indices.end(), std::begin(cone2.GetIndices16()), std::end(cone2.GetIndices16()));
	indices.insert(indices.end(), std::begin(cone3.GetIndices16()), std::end(cone3.GetIndices16()));
	indices.insert(indices.end(), std::begin(cone4.GetIndices16()), std::end(cone4.GetIndices16()));
	indices.insert(indices.end(), std::begin(cone5.GetIndices16()), std::end(cone5.GetIndices16()));
	indices.insert(indices.end(), std::begin(geosphere.GetIndices16()), std::end(geosphere.GetIndices16()));

    const UINT vbByteSize = (UINT)vertices.size() * sizeof(Vertex);
    const UINT ibByteSize = (UINT)indices.size()  * sizeof(std::uint16_t);

	auto geo = std::make_unique<MeshGeometry>();
	geo->Name = "shapeGeo";

	ThrowIfFailed(D3DCreateBlob(vbByteSize, &geo->VertexBufferCPU));
	CopyMemory(geo->VertexBufferCPU->GetBufferPointer(), vertices.data(), vbByteSize);

	ThrowIfFailed(D3DCreateBlob(ibByteSize, &geo->IndexBufferCPU));
	CopyMemory(geo->IndexBufferCPU->GetBufferPointer(), indices.data(), ibByteSize);

	geo->VertexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), vertices.data(), vbByteSize, geo->VertexBufferUploader);

	geo->IndexBufferGPU = d3dUtil::CreateDefaultBuffer(md3dDevice.Get(),
		mCommandList.Get(), indices.data(), ibByteSize, geo->IndexBufferUploader);

	geo->VertexByteStride = sizeof(Vertex);
	geo->VertexBufferByteSize = vbByteSize;
	geo->IndexFormat = DXGI_FORMAT_R16_UINT;
	geo->IndexBufferByteSize = ibByteSize;

	/////////////////////////////////////////////////////////////draw 
	geo->DrawArgs["box"] = boxSubmesh;
	geo->DrawArgs["grid"] = gridSubmesh;
	geo->DrawArgs["sphere"] = sphereSubmesh;
	geo->DrawArgs["cylinder"] = cylinderSubmesh;
	geo->DrawArgs["hexagon"] = hexagonSubmesh;
	geo->DrawArgs["tetrahedron"] = tetrahedronSubmesh;
	geo->DrawArgs["pyramid"] = pyramidSubmesh;
	geo->DrawArgs["diamond"] = diamondSubmesh;
	geo->DrawArgs["cone"] = coneSubmesh;
	geo->DrawArgs["wedge"] = wedgeSubmesh;
	geo->DrawArgs["quad"] = quadSubmesh;
	geo->DrawArgs["bar"] = barSubmesh;
	geo->DrawArgs["boxthree"] = boxthreeSubmesh;
	geo->DrawArgs["boxfour"] = boxfourSubmesh;
	geo->DrawArgs["boxfive"] = boxfiveSubmesh;
	geo->DrawArgs["boxsix"] = boxsixSubmesh;
	geo->DrawArgs["cylinder2"] = cylinder2Submesh;
	geo->DrawArgs["cylinder3"] = cylinder3Submesh;
	geo->DrawArgs["cylinder4"] = cylinder4Submesh;
	geo->DrawArgs["cylinder5"] = cylinder5Submesh;
	geo->DrawArgs["wedge2"] = wedge2Submesh;
	geo->DrawArgs["wedge3"] = wedge3Submesh;
	geo->DrawArgs["wedge4"] = wedge4Submesh;
	geo->DrawArgs["cone2"] = cone2Submesh;
	geo->DrawArgs["cone3"] = cone3Submesh;
	geo->DrawArgs["cone4"] = cone4Submesh;
	geo->DrawArgs["cone5"] = cone5Submesh;
	geo->DrawArgs["geosphere"] = geosphereSubmesh;

	mGeometries[geo->Name] = std::move(geo);
}

void ShapesApp::BuildPSOs()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaquePsoDesc;

	//
	// PSO for opaque objects.
	//
    ZeroMemory(&opaquePsoDesc, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	opaquePsoDesc.InputLayout = { mInputLayout.data(), (UINT)mInputLayout.size() };
	opaquePsoDesc.pRootSignature = mRootSignature.Get();
	opaquePsoDesc.VS = 
	{ 
		reinterpret_cast<BYTE*>(mShaders["standardVS"]->GetBufferPointer()), 
		mShaders["standardVS"]->GetBufferSize()
	};
	opaquePsoDesc.PS = 
	{ 
		reinterpret_cast<BYTE*>(mShaders["opaquePS"]->GetBufferPointer()),
		mShaders["opaquePS"]->GetBufferSize()
	};
	opaquePsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	opaquePsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	opaquePsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	opaquePsoDesc.SampleMask = UINT_MAX;
	opaquePsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	opaquePsoDesc.NumRenderTargets = 1;
	opaquePsoDesc.RTVFormats[0] = mBackBufferFormat;
	opaquePsoDesc.SampleDesc.Count = m4xMsaaState ? 4 : 1;
	opaquePsoDesc.SampleDesc.Quality = m4xMsaaState ? (m4xMsaaQuality - 1) : 0;
	opaquePsoDesc.DSVFormat = mDepthStencilFormat;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaquePsoDesc, IID_PPV_ARGS(&mPSOs["opaque"])));


    //
    // PSO for opaque wireframe objects.
    //

    D3D12_GRAPHICS_PIPELINE_STATE_DESC opaqueWireframePsoDesc = opaquePsoDesc;
    opaqueWireframePsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
    ThrowIfFailed(md3dDevice->CreateGraphicsPipelineState(&opaqueWireframePsoDesc, IID_PPV_ARGS(&mPSOs["opaque_wireframe"])));
}

void ShapesApp::BuildFrameResources()
{
    for(int i = 0; i < gNumFrameResources; ++i)
    {
        mFrameResources.push_back(std::make_unique<FrameResource>(md3dDevice.Get(),
            1, (UINT)mAllRitems.size()));
    }
}

void ShapesApp::BuildRenderItems()/////////////////////////////////////////////////////////////////render
{
	int j = 0;
	/*
	


	auto boxthreeRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxthreeRitem->World, XMMatrixScaling(5.0f, 2.0f, 0.5f)*XMMatrixRotationY(deg2rad(45))*XMMatrixTranslation(-3.0f, 0.5f, -8.0f));
	boxthreeRitem->ObjCBIndex = j;
	boxthreeRitem->Geo = mGeometries["shapeGeo"].get();
	boxthreeRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxthreeRitem->IndexCount = boxthreeRitem->Geo->DrawArgs["boxthree"].IndexCount;
	boxthreeRitem->StartIndexLocation = boxthreeRitem->Geo->DrawArgs["boxthree"].StartIndexLocation;
	boxthreeRitem->BaseVertexLocation = boxthreeRitem->Geo->DrawArgs["boxthree"].BaseVertexLocation;
	mAllRitems.push_back(std::move(boxthreeRitem));
	j++;

	auto boxfourRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxfourRitem->World, XMMatrixScaling(5.0f, 2.0f, 0.50f)*XMMatrixRotationY(deg2rad(-45))*XMMatrixTranslation(3.0f, 0.5f, -8.0f));
	boxfourRitem->ObjCBIndex = j;
	boxfourRitem->Geo = mGeometries["shapeGeo"].get();
	boxfourRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxfourRitem->IndexCount = boxfourRitem->Geo->DrawArgs["boxfour"].IndexCount;
	boxfourRitem->StartIndexLocation = boxfourRitem->Geo->DrawArgs["boxfour"].StartIndexLocation;
	boxfourRitem->BaseVertexLocation = boxfourRitem->Geo->DrawArgs["boxfour"].BaseVertexLocation;
	mAllRitems.push_back(std::move(boxfourRitem));
	j++;

	auto boxfiveRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxfiveRitem->World, XMMatrixScaling(5.0f, 2.0f, 0.5f)*XMMatrixRotationY(deg2rad(90))*XMMatrixTranslation(6.0f, 0.5f, -1.0f));
	boxfiveRitem->ObjCBIndex = j;
	boxfiveRitem->Geo = mGeometries["shapeGeo"].get();
	boxfiveRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxfiveRitem->IndexCount = boxfiveRitem->Geo->DrawArgs["boxfive"].IndexCount;
	boxfiveRitem->StartIndexLocation = boxfiveRitem->Geo->DrawArgs["boxfive"].StartIndexLocation;
	boxfiveRitem->BaseVertexLocation = boxfiveRitem->Geo->DrawArgs["boxfive"].BaseVertexLocation;
	mAllRitems.push_back(std::move(boxfiveRitem));
	j++;

	auto boxsixRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&boxsixRitem->World, XMMatrixScaling(5.0f, 2.0f, 0.50f)*XMMatrixRotationY(deg2rad(45))*XMMatrixTranslation(3.0f, 0.5f, 6.0f));
	boxsixRitem->ObjCBIndex = j;
	boxsixRitem->Geo = mGeometries["shapeGeo"].get();
	boxsixRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	boxsixRitem->IndexCount = boxsixRitem->Geo->DrawArgs["boxsix"].IndexCount;
	boxsixRitem->StartIndexLocation = boxsixRitem->Geo->DrawArgs["boxsix"].StartIndexLocation;
	boxsixRitem->BaseVertexLocation = boxsixRitem->Geo->DrawArgs["boxsix"].BaseVertexLocation;
	mAllRitems.push_back(std::move(boxsixRitem));
	j++;

    auto gridRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&gridRitem->World, XMMatrixScaling(0.05f, 0.0f, 0.05f)*XMMatrixRotationX(deg2rad(-90))*XMMatrixTranslation(0.0f, 0.5f, -10.25f));
	gridRitem->ObjCBIndex = j;
	gridRitem->Geo = mGeometries["shapeGeo"].get();
	gridRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    gridRitem->IndexCount = gridRitem->Geo->DrawArgs["grid"].IndexCount;
    gridRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["grid"].StartIndexLocation;
    gridRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["grid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(gridRitem));
	j++;

	auto hexRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&hexRitem->World, XMMatrixRotationY(deg2rad(0)));
	hexRitem->ObjCBIndex = j;
	hexRitem->Geo = mGeometries["shapeGeo"].get();
	hexRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	hexRitem->IndexCount = hexRitem->Geo->DrawArgs["hexagon"].IndexCount;
	hexRitem->StartIndexLocation = hexRitem->Geo->DrawArgs["hexagon"].StartIndexLocation;
	hexRitem->BaseVertexLocation = hexRitem->Geo->DrawArgs["hexagon"].BaseVertexLocation;
	mAllRitems.push_back(std::move(hexRitem));
	j++;
	

	auto tetraRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&tetraRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f)*XMMatrixRotationX(0.0f)*XMMatrixTranslation(0.0f, 2.0f, -10.0f));
	tetraRitem->ObjCBIndex = j;
	tetraRitem->Geo = mGeometries["shapeGeo"].get();
	tetraRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	tetraRitem->IndexCount = tetraRitem->Geo->DrawArgs["tetrahedron"].IndexCount;
	tetraRitem->StartIndexLocation = tetraRitem->Geo->DrawArgs["tetrahedron"].StartIndexLocation;
	tetraRitem->BaseVertexLocation = tetraRitem->Geo->DrawArgs["tetrahedron"].BaseVertexLocation;
	mAllRitems.push_back(std::move(tetraRitem));
	j++;

	auto sphereRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&sphereRitem->World, XMMatrixScaling(1.0f, 1.0f, 1.0f)*XMMatrixTranslation(0.0f, 3.5f, -10.5f));
	sphereRitem->ObjCBIndex = j;
	sphereRitem->Geo = mGeometries["shapeGeo"].get();
	sphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	sphereRitem->IndexCount = sphereRitem->Geo->DrawArgs["sphere"].IndexCount;
	sphereRitem->StartIndexLocation = sphereRitem->Geo->DrawArgs["sphere"].StartIndexLocation;
	sphereRitem->BaseVertexLocation = sphereRitem->Geo->DrawArgs["sphere"].BaseVertexLocation;
	mAllRitems.push_back(std::move(sphereRitem));
	j++;

	auto pyramidRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&pyramidRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f)*XMMatrixRotationX(0.0f)*XMMatrixTranslation(0.0f, 0.0f, 0.0f));
	pyramidRitem->ObjCBIndex = j;
	pyramidRitem->Geo = mGeometries["shapeGeo"].get();
	pyramidRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	pyramidRitem->IndexCount = pyramidRitem->Geo->DrawArgs["pyramid"].IndexCount;
	pyramidRitem->StartIndexLocation = pyramidRitem->Geo->DrawArgs["pyramid"].StartIndexLocation;
	pyramidRitem->BaseVertexLocation = pyramidRitem->Geo->DrawArgs["pyramid"].BaseVertexLocation;
	mAllRitems.push_back(std::move(pyramidRitem));
	j++;

	auto diamondRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&diamondRitem->World, XMMatrixScaling(2.0f, 2.0f, 2.0f)*XMMatrixRotationX(0.0f)*XMMatrixTranslation(0.0f, 1.0f, -3.0f));
	diamondRitem->ObjCBIndex = j;
	diamondRitem->Geo = mGeometries["shapeGeo"].get();
	diamondRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	diamondRitem->IndexCount = diamondRitem->Geo->DrawArgs["diamond"].IndexCount;
	diamondRitem->StartIndexLocation = diamondRitem->Geo->DrawArgs["diamond"].StartIndexLocation;
	diamondRitem->BaseVertexLocation = diamondRitem->Geo->DrawArgs["diamond"].BaseVertexLocation;
	mAllRitems.push_back(std::move(diamondRitem));
	j++;
	
	auto coneRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&coneRitem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(6.0f, 3.0f, -5.0f));
	coneRitem->ObjCBIndex = j;
	coneRitem->Geo = mGeometries["shapeGeo"].get();
	coneRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	coneRitem->IndexCount = coneRitem->Geo->DrawArgs["cone"].IndexCount;
	coneRitem->StartIndexLocation = coneRitem->Geo->DrawArgs["cone"].StartIndexLocation;
	coneRitem->BaseVertexLocation = coneRitem->Geo->DrawArgs["cone"].BaseVertexLocation;
	mAllRitems.push_back(std::move(coneRitem));
	j++;

	auto cone2Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cone2Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(6.0f, 3.0f, 3.0f));
	cone2Ritem->ObjCBIndex = j;
	cone2Ritem->Geo = mGeometries["shapeGeo"].get();
	cone2Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cone2Ritem->IndexCount = cone2Ritem->Geo->DrawArgs["cone2"].IndexCount;
	cone2Ritem->StartIndexLocation = cone2Ritem->Geo->DrawArgs["cone2"].StartIndexLocation;
	cone2Ritem->BaseVertexLocation = cone2Ritem->Geo->DrawArgs["cone2"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cone2Ritem));
	j++;

	auto cone3Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cone3Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(0.0f, 3.0f, 9.0f));
	cone3Ritem->ObjCBIndex = j;
	cone3Ritem->Geo = mGeometries["shapeGeo"].get();
	cone3Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cone3Ritem->IndexCount = cone3Ritem->Geo->DrawArgs["cone3"].IndexCount;
	cone3Ritem->StartIndexLocation = cone3Ritem->Geo->DrawArgs["cone3"].StartIndexLocation;
	cone3Ritem->BaseVertexLocation = cone3Ritem->Geo->DrawArgs["cone3"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cone3Ritem));
	j++;

	auto cone4Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cone4Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(-6.0f, 3.0f, -5.0f));
	cone4Ritem->ObjCBIndex = j;
	cone4Ritem->Geo = mGeometries["shapeGeo"].get();
	cone4Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cone4Ritem->IndexCount = cone4Ritem->Geo->DrawArgs["cone4"].IndexCount;
	cone4Ritem->StartIndexLocation = cone4Ritem->Geo->DrawArgs["cone4"].StartIndexLocation;
	cone4Ritem->BaseVertexLocation = cone4Ritem->Geo->DrawArgs["cone4"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cone4Ritem));
	j++;

	auto cone5Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cone5Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(-6.0f, 3.0f, 3.0f));
	cone5Ritem->ObjCBIndex = j;
	cone5Ritem->Geo = mGeometries["shapeGeo"].get();
	cone5Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cone5Ritem->IndexCount = cone5Ritem->Geo->DrawArgs["cone5"].IndexCount;
	cone5Ritem->StartIndexLocation = cone5Ritem->Geo->DrawArgs["cone5"].StartIndexLocation;
	cone5Ritem->BaseVertexLocation = cone5Ritem->Geo->DrawArgs["cone5"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cone5Ritem));
	j++;
	
	auto cylinderRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cylinderRitem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(6.0f, 1.0f, -5.0f));
	cylinderRitem->ObjCBIndex = j;
	cylinderRitem->Geo = mGeometries["shapeGeo"].get();
	cylinderRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinderRitem->IndexCount = cylinderRitem->Geo->DrawArgs["cylinder5"].IndexCount;
	cylinderRitem->StartIndexLocation = cylinderRitem->Geo->DrawArgs["cylinder5"].StartIndexLocation;
	cylinderRitem->BaseVertexLocation = cylinderRitem->Geo->DrawArgs["cylinder5"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinderRitem));
	j++;

	auto cylinder2Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cylinder2Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(6.0f, 1.0f, 3.0f));
	cylinder2Ritem->ObjCBIndex = j;
	cylinder2Ritem->Geo = mGeometries["shapeGeo"].get();
	cylinder2Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinder2Ritem->IndexCount = cylinder2Ritem->Geo->DrawArgs["cylinder2"].IndexCount;
	cylinder2Ritem->StartIndexLocation = cylinder2Ritem->Geo->DrawArgs["cylinder2"].StartIndexLocation;
	cylinder2Ritem->BaseVertexLocation = cylinder2Ritem->Geo->DrawArgs["cylinder2"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinder2Ritem));
	j++;

	auto cylinder3Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cylinder3Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(0.0f, 1.0f, 9.0f));
	cylinder3Ritem->ObjCBIndex = j;
	cylinder3Ritem->Geo = mGeometries["shapeGeo"].get();
	cylinder3Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinder3Ritem->IndexCount = cylinder3Ritem->Geo->DrawArgs["cylinder3"].IndexCount;
	cylinder3Ritem->StartIndexLocation = cylinder3Ritem->Geo->DrawArgs["cylinder3"].StartIndexLocation;
	cylinder3Ritem->BaseVertexLocation = cylinder3Ritem->Geo->DrawArgs["cylinder3"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinder3Ritem));
	j++;

	auto cylinder4Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cylinder4Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(-6.0f, 1.0f, 3.0f));
	cylinder4Ritem->ObjCBIndex = j;
	cylinder4Ritem->Geo = mGeometries["shapeGeo"].get();
	cylinder4Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinder4Ritem->IndexCount = cylinder4Ritem->Geo->DrawArgs["cylinder4"].IndexCount;
	cylinder4Ritem->StartIndexLocation = cylinder4Ritem->Geo->DrawArgs["cylinder4"].StartIndexLocation;
	cylinder4Ritem->BaseVertexLocation = cylinder4Ritem->Geo->DrawArgs["cylinder4"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinder4Ritem));
	j++;

	auto cylinder5Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&cylinder5Ritem->World, XMMatrixScaling(1.0f, 2.0f, 1.0f)*XMMatrixTranslation(-6.0f, 1.0f, -5.0f));
	cylinder5Ritem->ObjCBIndex = j;
	cylinder5Ritem->Geo = mGeometries["shapeGeo"].get();
	cylinder5Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	cylinder5Ritem->IndexCount = cylinder5Ritem->Geo->DrawArgs["cylinder5"].IndexCount;
	cylinder5Ritem->StartIndexLocation = cylinder5Ritem->Geo->DrawArgs["cylinder5"].StartIndexLocation;
	cylinder5Ritem->BaseVertexLocation = cylinder5Ritem->Geo->DrawArgs["cylinder5"].BaseVertexLocation;
	mAllRitems.push_back(std::move(cylinder5Ritem));
	j++;

	auto wedgeRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&wedgeRitem->World, XMMatrixScaling(3.0f, 2.0f, 1.0f)*XMMatrixRotationY(deg2rad(225))*XMMatrixTranslation(-1.5f, 1.0f, -9.5f));
	wedgeRitem->ObjCBIndex = j;
	wedgeRitem->Geo = mGeometries["shapeGeo"].get();
	wedgeRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wedgeRitem->IndexCount = wedgeRitem->Geo->DrawArgs["wedge"].IndexCount;
	wedgeRitem->StartIndexLocation = wedgeRitem->Geo->DrawArgs["wedge"].StartIndexLocation;
	wedgeRitem->BaseVertexLocation = wedgeRitem->Geo->DrawArgs["wedge"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wedgeRitem));
	j++;

	auto wedge2Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&wedge2Ritem->World, XMMatrixScaling(3.0f, 2.0f, 1.0f)*XMMatrixRotationY(deg2rad(45))*XMMatrixTranslation(-4.5f, 1.0f, -6.5f));
	wedge2Ritem->ObjCBIndex = j;
	wedge2Ritem->Geo = mGeometries["shapeGeo"].get();
	wedge2Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wedge2Ritem->IndexCount = wedge2Ritem->Geo->DrawArgs["wedge2"].IndexCount;
	wedge2Ritem->StartIndexLocation = wedge2Ritem->Geo->DrawArgs["wedge2"].StartIndexLocation;
	wedge2Ritem->BaseVertexLocation = wedge2Ritem->Geo->DrawArgs["wedge2"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wedge2Ritem));
	j++;

	auto wedge3Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&wedge3Ritem->World, XMMatrixScaling(3.0f, 2.0f, 1.0f)*XMMatrixRotationY(deg2rad(135))*XMMatrixTranslation(4.5f, 1.0f, -6.5f));
	wedge3Ritem->ObjCBIndex = j;
	wedge3Ritem->Geo = mGeometries["shapeGeo"].get();
	wedge3Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wedge3Ritem->IndexCount = wedge3Ritem->Geo->DrawArgs["wedge3"].IndexCount;
	wedge3Ritem->StartIndexLocation = wedge3Ritem->Geo->DrawArgs["wedge3"].StartIndexLocation;
	wedge3Ritem->BaseVertexLocation = wedge3Ritem->Geo->DrawArgs["wedge3"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wedge3Ritem));
	j++;

	auto wedge4Ritem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&wedge4Ritem->World, XMMatrixScaling(3.0f, 2.0f, 1.0f)*XMMatrixRotationY(deg2rad(315))*XMMatrixTranslation(1.5f, 1.0f, -9.5f));
	wedge4Ritem->ObjCBIndex = j;
	wedge4Ritem->Geo = mGeometries["shapeGeo"].get();
	wedge4Ritem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	wedge4Ritem->IndexCount = wedge4Ritem->Geo->DrawArgs["wedge4"].IndexCount;
	wedge4Ritem->StartIndexLocation = wedge4Ritem->Geo->DrawArgs["wedge4"].StartIndexLocation;
	wedge4Ritem->BaseVertexLocation = wedge4Ritem->Geo->DrawArgs["wedge4"].BaseVertexLocation;
	mAllRitems.push_back(std::move(wedge4Ritem));
	j++;

	auto geosphereRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&geosphereRitem->World, XMMatrixScaling(3.0f, 3.0f, 3.0f)*XMMatrixTranslation(-10.0f, 1.0f, 9.0f));
	geosphereRitem->ObjCBIndex = j;
	geosphereRitem->Geo = mGeometries["shapeGeo"].get();
	geosphereRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	geosphereRitem->IndexCount = geosphereRitem->Geo->DrawArgs["geosphere"].IndexCount;
	geosphereRitem->StartIndexLocation = geosphereRitem->Geo->DrawArgs["geosphere"].StartIndexLocation;
	geosphereRitem->BaseVertexLocation = geosphereRitem->Geo->DrawArgs["geosphere"].BaseVertexLocation;
	mAllRitems.push_back(std::move(geosphereRitem));
	j++;

/*	

	auto quadRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&quadRitem->World, XMMatrixScaling(1.0f, 0.0f, 1.0f)*XMMatrixTranslation(0.0f, 0.5f, -2.0f));
	quadRitem->ObjCBIndex = j;
	quadRitem->Geo = mGeometries["shapeGeo"].get();
	quadRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	quadRitem->IndexCount = gridRitem->Geo->DrawArgs["quad"].IndexCount;
	quadRitem->StartIndexLocation = gridRitem->Geo->DrawArgs["quad"].StartIndexLocation;
	quadRitem->BaseVertexLocation = gridRitem->Geo->DrawArgs["quad"].BaseVertexLocation;
	mAllRitems.push_back(std::move(quadRitem));
	j++;
	*/

for (int i = 1; i < 11; i++)
{
	auto barItem = std::make_unique<RenderItem>();
	auto boxItem = std::make_unique<RenderItem>();

	for (int q = 0; q < 2; q++)
	{
		for (int k = 0; k < 5; k++)
		{

			auto boxRitem = std::make_unique<RenderItem>();
			XMStoreFloat4x4(&boxRitem->World, XMMatrixScaling(1.0f, 0.1f, 0.5f)*XMMatrixTranslation(q - 0.5f, i + 0.1f, -1.0f + k * 0.5f));
			boxRitem->ObjCBIndex = j;
			boxRitem->Geo = mGeometries["shapeGeo"].get();
			boxRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
			boxRitem->IndexCount = boxRitem->Geo->DrawArgs["box"].IndexCount;
			boxRitem->StartIndexLocation = boxRitem->Geo->DrawArgs["box"].StartIndexLocation;
			boxRitem->BaseVertexLocation = boxRitem->Geo->DrawArgs["box"].BaseVertexLocation;
			mAllRitems.push_back(std::move(boxRitem));
			j++;

		}
	}

	auto barRitem = std::make_unique<RenderItem>();
	XMStoreFloat4x4(&barRitem->World, XMMatrixScaling(2.0f, 0.1f, 2.5f)*XMMatrixTranslation(0.0f, i, 0.0f));
	barRitem->ObjCBIndex = j;
	barRitem->Geo = mGeometries["shapeGeo"].get();
	barRitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	barRitem->IndexCount = barRitem->Geo->DrawArgs["bar"].IndexCount;
	barRitem->StartIndexLocation = barRitem->Geo->DrawArgs["bar"].StartIndexLocation;
	barRitem->BaseVertexLocation = barRitem->Geo->DrawArgs["bar"].BaseVertexLocation;
	mAllRitems.push_back(std::move(barRitem));
	j++;
}

UINT objCBIndex = j;
	for(int c = 0; c < 10; ++c)
	{
		auto topDiamitem = std::make_unique<RenderItem>();
		auto botDiamitem = std::make_unique<RenderItem>();

		XMMATRIX topDiamWorld = XMMatrixTranslation(0.0f + (2*c), 0.225f, 0.0f);
		XMMATRIX botDiamWorld = XMMatrixRotationZ(deg2rad(180))*XMMatrixTranslation(0.0f + (2*c), -0.375f, 0.0f);

		XMStoreFloat4x4(&topDiamitem->World, botDiamWorld);
		topDiamitem->ObjCBIndex = objCBIndex++;
		topDiamitem->Geo = mGeometries["shapeGeo"].get();
		topDiamitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		topDiamitem->IndexCount = topDiamitem->Geo->DrawArgs["hexagon"].IndexCount;
		topDiamitem->StartIndexLocation = topDiamitem->Geo->DrawArgs["hexagon"].StartIndexLocation;
		topDiamitem->BaseVertexLocation = topDiamitem->Geo->DrawArgs["hexagon"].BaseVertexLocation;

		XMStoreFloat4x4(&botDiamitem->World, topDiamWorld);
		botDiamitem->ObjCBIndex = objCBIndex++;
		botDiamitem->Geo = mGeometries["shapeGeo"].get();
		botDiamitem->PrimitiveType = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
		botDiamitem->IndexCount = botDiamitem->Geo->DrawArgs["hexagon"].IndexCount;
		botDiamitem->StartIndexLocation = botDiamitem->Geo->DrawArgs["hexagon"].StartIndexLocation;
		botDiamitem->BaseVertexLocation = botDiamitem->Geo->DrawArgs["hexagon"].BaseVertexLocation;

		mAllRitems.push_back(std::move(topDiamitem));
		mAllRitems.push_back(std::move(botDiamitem));
	}

	
	
	
	// All the render items are opaque.
	for(auto& e : mAllRitems)
		mOpaqueRitems.push_back(e.get());
}

void ShapesApp::DrawRenderItems(ID3D12GraphicsCommandList* cmdList, const std::vector<RenderItem*>& ritems)
{
    UINT objCBByteSize = d3dUtil::CalcConstantBufferByteSize(sizeof(ObjectConstants));
 
	auto objectCB = mCurrFrameResource->ObjectCB->Resource();

    // For each render item...
    for(size_t i = 0; i < ritems.size(); ++i)
    {
        auto ri = ritems[i];

        cmdList->IASetVertexBuffers(0, 1, &ri->Geo->VertexBufferView());
        cmdList->IASetIndexBuffer(&ri->Geo->IndexBufferView());
        cmdList->IASetPrimitiveTopology(ri->PrimitiveType);

        // Offset to the CBV in the descriptor heap for this object and for this frame resource.
        UINT cbvIndex = mCurrFrameResourceIndex*(UINT)mOpaqueRitems.size() + ri->ObjCBIndex;
        auto cbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(mCbvHeap->GetGPUDescriptorHandleForHeapStart());
        cbvHandle.Offset(cbvIndex, mCbvSrvUavDescriptorSize);

        cmdList->SetGraphicsRootDescriptorTable(0, cbvHandle);

        cmdList->DrawIndexedInstanced(ri->IndexCount, 1, ri->StartIndexLocation, ri->BaseVertexLocation, 0);
    }
}
