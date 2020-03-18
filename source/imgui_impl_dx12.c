// dear imgui: Renderer for DirectX12
// This needs to be used along with a Platform Binding (e.g. Win32)

// Implemented features:
//  [X] Renderer: User texture binding. Use 'D3D12_GPU_DESCRIPTOR_HANDLE' as ImTextureID. Read the FAQ about ImTextureID!
//  [X] Renderer: Support for large meshes (64k+ vertices) with 16-bits indices.
// Issues:
//  [ ] 64-bit only for now! (Because sizeof(ImTextureId) == sizeof(void*)). See github.com/ocornut/imgui/pull/301

// You can copy and use unmodified imgui_impl_* files in your project. See main.cpp for an example of using this.
// If you are new to dear imgui, read examples/README.txt and read the documentation at the top of imgui.cpp.
// https://github.com/ocornut/imgui

// CHANGELOG
// (minor and older changes stripped away, please see git history for details)
//  2019-10-18: DirectX12: *BREAKING CHANGE* Added extra ID3D12DescriptorHeap parameter to ImGui_ImplDX12_Init() function.
//  2019-05-29: DirectX12: Added support for large mesh (64K+ vertices), enable ImGuiBackendFlags_RendererHasVtxOffset flag.
//  2019-04-30: DirectX12: Added support for special ImDrawCallback_ResetRenderState callback to reset render state.
//  2019-03-29: Misc: Various minor tidying up.
//  2018-12-03: Misc: Added #pragma comment statement to automatically link with d3dcompiler.lib when using D3DCompile().
//  2018-11-30: Misc: Setting up io.BackendRendererName so it can be displayed in the About Window.
//  2018-06-12: DirectX12: Moved the ID3D12GraphicsCommandList* parameter from NewFrame() to RenderDrawData().
//  2018-06-08: Misc: Extracted imgui_impl_dx12.cpp/.h away from the old combined DX12+Win32 example.
//  2018-06-08: DirectX12: Use draw_data->DisplayPos and draw_data->DisplaySize to setup projection matrix and clipping rectangle (to ease support for future multi-viewport).
//  2018-02-22: Merged into master with all Win32 code synchronized to other examples.

#include "cimgui.h"

#include <d3d12.h>
#pragma clang diagnostic ignored "-Weverything"

#define ImDrawCallback_ResetRenderState (ImDrawCallback)(-1)

// DirectX data
// Use if you want to reset your rendering device without losing ImGui state.
static void     ImGui_ImplDX12_InvalidateDeviceObjects(void);
static bool     ImGui_ImplDX12_CreateDeviceObjects(void);
static ID3D12Device*                g_pd3dDevice ;
static ID3D10Blob*                  g_pVertexShaderBlob ;
static ID3D10Blob*                  g_pPixelShaderBlob ;
static ID3D12RootSignature*         g_pRootSignature ;
static ID3D12PipelineState*         g_pPipelineState ;
static DXGI_FORMAT                  g_RTVFormat = DXGI_FORMAT_UNKNOWN;
static ID3D12Resource*              g_pFontTextureResource ;
static D3D12_CPU_DESCRIPTOR_HANDLE  g_hFontSrvCpuDescHandle;
static D3D12_GPU_DESCRIPTOR_HANDLE  g_hFontSrvGpuDescHandle;

typedef struct FrameResources

{
	ID3D12Resource*     IndexBuffer;
	ID3D12Resource*     VertexBuffer;
	int                 IndexBufferSize;
	int                 VertexBufferSize;
} FrameResources;
static FrameResources*  g_pFrameResources ;
static UINT             g_numFramesInFlight ;
static UINT             g_frameIndex;

typedef struct VERTEX_CONSTANT_BUFFER
{
	float   mvp[4][4];
} VERTEX_CONSTANT_BUFFER;

static void ImGui_ImplDX12_SetupRenderState(ImDrawData* draw_data, ID3D12GraphicsCommandList* ctx, FrameResources* fr)
{
	// Setup orthographic projection matrix into our constant buffer
	// Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right).
	VERTEX_CONSTANT_BUFFER vertex_constant_buffer;
	{
		float L = draw_data->DisplayPos.x;
		float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
		float T = draw_data->DisplayPos.y;
		float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
		float mvp[4][4] =
		{
			{ 2.0f/(R-L),   0.0f,           0.0f,       0.0f },
			{ 0.0f,         2.0f/(T-B),     0.0f,       0.0f },
			{ 0.0f,         0.0f,           0.5f,       0.0f },
			{ (R+L)/(L-R),  (T+B)/(B-T),    0.5f,       1.0f },
		};
		memcpy(&vertex_constant_buffer.mvp, mvp, sizeof(mvp));
	}

	// Setup viewport
	D3D12_VIEWPORT vp;
	memset(&vp, 0, sizeof(D3D12_VIEWPORT));
	vp.Width = draw_data->DisplaySize.x;
	vp.Height = draw_data->DisplaySize.y;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	vp.TopLeftX = vp.TopLeftY = 0.0f;
	ctx->lpVtbl->RSSetViewports(ctx, 1, &vp);

	// Bind shader and vertex buffers
	unsigned int stride = sizeof(ImDrawVert);
	unsigned int offset = 0;
	D3D12_VERTEX_BUFFER_VIEW vbv;
	memset(&vbv, 0, sizeof(D3D12_VERTEX_BUFFER_VIEW));
	vbv.BufferLocation = fr->VertexBuffer->lpVtbl->GetGPUVirtualAddress(fr->VertexBuffer) + offset;
	vbv.SizeInBytes = (UINT)fr->VertexBufferSize * stride;
	vbv.StrideInBytes = stride;
	ctx->lpVtbl->IASetVertexBuffers( ctx,0, 1, &vbv);
	D3D12_INDEX_BUFFER_VIEW ibv;
	memset(&ibv, 0, sizeof(D3D12_INDEX_BUFFER_VIEW));
	ibv.BufferLocation = fr->IndexBuffer->lpVtbl->GetGPUVirtualAddress(fr->IndexBuffer);
	ibv.SizeInBytes = (UINT)fr->IndexBufferSize * sizeof(ImDrawIdx);
	ibv.Format = sizeof(ImDrawIdx) == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
	ctx->lpVtbl-> IASetIndexBuffer(ctx,&ibv);
	ctx->lpVtbl->IASetPrimitiveTopology(ctx,D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ctx->lpVtbl->SetPipelineState(ctx,g_pPipelineState);
	ctx->lpVtbl->SetGraphicsRootSignature(ctx,g_pRootSignature);
	ctx->lpVtbl->SetGraphicsRoot32BitConstants(ctx,0, 16, &vertex_constant_buffer, 0);

	// Setup blend factor
	const float blend_factor[4] = { 0.f, 0.f, 0.f, 0.f };
	ctx->lpVtbl->OMSetBlendFactor(ctx,blend_factor);
}

// Render function
// (this used to be set in io.RenderDrawListsFn and called by ImGui::Render(), but you can now call this directly from your main loop)
static void ImGui_ImplDX12_RenderDrawData(ImDrawData* draw_data, ID3D12GraphicsCommandList* ctx)
{
	// Avoid rendering when minimized
	if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
		return;

	// FIXME: I'm assuming that this only gets called once per frame!
	// If not, we can't just re-allocate the IB or VB, we'll have to do a proper allocator.
	g_frameIndex = g_frameIndex + 1;
	FrameResources* fr = &g_pFrameResources[g_frameIndex % g_numFramesInFlight];

	// Create and grow vertex/index buffers if needed
	if (fr->VertexBuffer == NULL || fr->VertexBufferSize < draw_data->TotalVtxCount)
	{
		if(fr->VertexBuffer)
		{
			fr->VertexBuffer->lpVtbl-> Release(fr->VertexBuffer);
			fr->VertexBuffer = NULL;
		}
		fr->VertexBufferSize = draw_data->TotalVtxCount + 5000;
		D3D12_HEAP_PROPERTIES props;
		memset(&props, 0, sizeof(D3D12_HEAP_PROPERTIES));
		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		D3D12_RESOURCE_DESC desc;
		memset(&desc, 0, sizeof(D3D12_RESOURCE_DESC));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = (UINT64)fr->VertexBufferSize * sizeof(ImDrawVert);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (g_pd3dDevice->lpVtbl-> CreateCommittedResource(g_pd3dDevice,&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,(void**)&fr->VertexBuffer) < 0)
			return;
	}
	if (fr->IndexBuffer == NULL || fr->IndexBufferSize < draw_data->TotalIdxCount)
	{
		if(fr->IndexBuffer)
		{
			fr->IndexBuffer->lpVtbl-> Release(fr->IndexBuffer);
			fr->IndexBuffer = NULL;
		}
		fr->IndexBufferSize = draw_data->TotalIdxCount + 10000;
		D3D12_HEAP_PROPERTIES props;
		memset(&props, 0, sizeof(D3D12_HEAP_PROPERTIES));
		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
		D3D12_RESOURCE_DESC desc;
		memset(&desc, 0, sizeof(D3D12_RESOURCE_DESC));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = (UINT64)fr->IndexBufferSize * sizeof(ImDrawIdx);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;
		if (g_pd3dDevice->lpVtbl-> CreateCommittedResource(g_pd3dDevice,&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource,(void**)&fr->IndexBuffer) < 0)
			return;
	}

	// Upload vertex/index data into a single contiguous GPU buffer
	void* vtx_resource, *idx_resource;
	D3D12_RANGE range;
	memset(&range, 0, sizeof(D3D12_RANGE));
	if (fr->VertexBuffer->lpVtbl->Map(fr->VertexBuffer,0, &range, &vtx_resource) != S_OK)
		return;
	if (fr->IndexBuffer->lpVtbl->Map(fr->IndexBuffer,0, &range, &idx_resource) != S_OK)
		return;
	ImDrawVert* vtx_dst = (ImDrawVert*)vtx_resource;
	ImDrawIdx* idx_dst = (ImDrawIdx*)idx_resource;
	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		memcpy(vtx_dst, cmd_list->VtxBuffer.Data, (UINT64)cmd_list->VtxBuffer.Size * sizeof(ImDrawVert));
		memcpy(idx_dst, cmd_list->IdxBuffer.Data, (UINT64)cmd_list->IdxBuffer.Size * sizeof(ImDrawIdx));
		vtx_dst += cmd_list->VtxBuffer.Size;
		idx_dst += cmd_list->IdxBuffer.Size;
	}
	fr->VertexBuffer->lpVtbl->Unmap(fr->VertexBuffer,0, &range);
	fr->IndexBuffer->lpVtbl->Unmap(fr->IndexBuffer,0, &range);

	// Setup desired DX state
	ImGui_ImplDX12_SetupRenderState(draw_data, ctx, fr);

	// Render command lists
	// (Because we merged all buffers into a single one, we maintain our own offset into them)
	int global_vtx_offset = 0;
	int global_idx_offset = 0;
	ImVec2 clip_off = draw_data->DisplayPos;
	for (int n = 0; n < draw_data->CmdListsCount; n++)
	{
		const ImDrawList* cmd_list = draw_data->CmdLists[n];
		for (int cmd_i = 0; cmd_i < cmd_list->CmdBuffer.Size; cmd_i++)
		{
			const ImDrawCmd* pcmd = &cmd_list->CmdBuffer.Data[cmd_i];
			if (pcmd->UserCallback != NULL)
			{
				// User callback, registered via ImDrawList::AddCallback()
				// (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
				if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
					ImGui_ImplDX12_SetupRenderState(draw_data, ctx, fr);
				else
					pcmd->UserCallback(cmd_list, pcmd);
			}
			else
			{
				// Apply Scissor, Bind texture, Draw
				const D3D12_RECT r = { (LONG)(pcmd->ClipRect.x - clip_off.x), (LONG)(pcmd->ClipRect.y - clip_off.y), (LONG)(pcmd->ClipRect.z - clip_off.x), (LONG)(pcmd->ClipRect.w - clip_off.y) };
				ctx->lpVtbl-> SetGraphicsRootDescriptorTable(ctx,1, *(const D3D12_GPU_DESCRIPTOR_HANDLE*)&pcmd->TextureId);
				ctx->lpVtbl->RSSetScissorRects(ctx,1, &r);
				ctx->lpVtbl->DrawIndexedInstanced(ctx,pcmd->ElemCount, 1, pcmd->IdxOffset + (UINT)global_idx_offset,(INT)pcmd->VtxOffset + global_vtx_offset, 0);
			}
		}
		global_idx_offset += cmd_list->IdxBuffer.Size;
		global_vtx_offset += cmd_list->VtxBuffer.Size;
	}
}

static void ImGui_ImplDX12_CreateFontsTexture()
{
	// Build texture atlas
	ImGuiIO* io = igGetIO();
	unsigned char* pixels;
	int width, height;
	ImFontAtlas_GetTexDataAsRGBA32(io->Fonts,&pixels, &width, &height, NULL);

	// Upload texture to graphics system
	{
		D3D12_HEAP_PROPERTIES props;
		memset(&props, 0, sizeof(D3D12_HEAP_PROPERTIES));
		props.Type = D3D12_HEAP_TYPE_DEFAULT;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC desc;
		ZeroMemory(&desc, sizeof(desc));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Alignment = 0;
		desc.Width = (UINT64)width;
		desc.Height =(UINT)height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		 desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		ID3D12Resource* pTexture = NULL;
		g_pd3dDevice->lpVtbl->CreateCommittedResource(g_pd3dDevice,&props, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource,(void**)&pTexture);

		pTexture->lpVtbl->SetName(pTexture, L"imgui_fonts_default_buffer");

		UINT uploadPitch = ((UINT)width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
		UINT uploadSize = (UINT)height * uploadPitch;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Alignment = 0;
		desc.Width = uploadSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		ID3D12Resource* uploadBuffer = NULL;
		HRESULT hr = g_pd3dDevice->lpVtbl->CreateCommittedResource(g_pd3dDevice,&props, D3D12_HEAP_FLAG_NONE, &desc,
				D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void**)&uploadBuffer);
		uploadBuffer->lpVtbl->SetName(uploadBuffer, L"imgui_fonts_upload_buffer");

		void* mapped = NULL;
		D3D12_RANGE range = { 0, uploadSize };
		hr = uploadBuffer->lpVtbl->Map(uploadBuffer,0, &range, &mapped);
		for (int y = 0; y < height; y++)
			memcpy((void*) ((uintptr_t) mapped + (UINT)y * uploadPitch), pixels + y * width * 4, (UINT)width * 4);
		uploadBuffer->lpVtbl->Unmap(uploadBuffer,0, &range);

		D3D12_TEXTURE_COPY_LOCATION srcLocation ;
		srcLocation.pResource = uploadBuffer;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint.Offset=0;
		srcLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srcLocation.PlacedFootprint.Footprint.Width = (UINT)width;
		srcLocation.PlacedFootprint.Footprint.Height =(UINT)height;
		srcLocation.PlacedFootprint.Footprint.Depth = 1;
		srcLocation.PlacedFootprint.Footprint.RowPitch = uploadPitch;

		D3D12_TEXTURE_COPY_LOCATION dstLocation ;
		dstLocation.pResource = pTexture;
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = 0;

		D3D12_RESOURCE_BARRIER barrier ;
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource   = pTexture;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		ID3D12Fence* fence = NULL;
		hr = g_pd3dDevice->lpVtbl->CreateFence(g_pd3dDevice,0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence,(void**)&fence);
		fence->lpVtbl->SetName(fence, L"imgui_fence");

		HANDLE event = CreateEvent(0, 0, 0, 0);

		D3D12_COMMAND_QUEUE_DESC queueDesc ;
		queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		queueDesc.Type     = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Flags    = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.NodeMask = 1;

		ID3D12CommandQueue* cmdQueue = NULL;
		hr = g_pd3dDevice->lpVtbl->CreateCommandQueue(g_pd3dDevice,&queueDesc, &IID_ID3D12CommandQueue,(void**)&cmdQueue);
		cmdQueue->lpVtbl->SetName(cmdQueue, L"imgui_cmd_queue");

		ID3D12CommandAllocator* cmdAlloc = NULL;
		hr = g_pd3dDevice->lpVtbl->CreateCommandAllocator(g_pd3dDevice,D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator,(void**)&cmdAlloc);
		cmdAlloc->lpVtbl->SetName(cmdAlloc, L"imgui_cmd_alloc");

		ID3D12GraphicsCommandList* cmdList = NULL;
		hr = g_pd3dDevice->lpVtbl->CreateCommandList(g_pd3dDevice,0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, NULL,&IID_ID3D12CommandList, (void**)&cmdList);
		cmdList->lpVtbl->SetName(cmdList, L"imgui_cmd_list");

		cmdList->lpVtbl->CopyTextureRegion(cmdList,&dstLocation, 0, 0, 0, &srcLocation, NULL);
		cmdList->lpVtbl->ResourceBarrier(cmdList,1, &barrier);

		hr = cmdList->lpVtbl->Close(cmdList);

		cmdQueue->lpVtbl->ExecuteCommandLists(cmdQueue,1, (ID3D12CommandList* const*) &cmdList);
		hr = cmdQueue->lpVtbl->Signal(cmdQueue,fence, 1);

		fence->lpVtbl->SetEventOnCompletion(fence,1, event);
		WaitForSingleObject(event, INFINITE);

		cmdList->lpVtbl->Release(cmdList);
		cmdAlloc->lpVtbl->Release(cmdAlloc);
		cmdQueue->lpVtbl->Release(cmdQueue);
		CloseHandle(event);
		fence->lpVtbl->Release(fence);
		uploadBuffer->lpVtbl->Release(uploadBuffer);

		// Create texture view
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
		ZeroMemory(&srvDesc, sizeof(srvDesc));
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		g_pd3dDevice->lpVtbl->CreateShaderResourceView(g_pd3dDevice,pTexture, &srvDesc, g_hFontSrvCpuDescHandle);
		if(g_pFontTextureResource)
		{
			g_pFontTextureResource->lpVtbl-> Release(g_pFontTextureResource);
			g_pFontTextureResource = NULL;
		}
		g_pFontTextureResource = pTexture;
	}

	// Store our identifier
	io->Fonts->TexID = (ImTextureID)g_hFontSrvGpuDescHandle.ptr;
}

bool    ImGui_ImplDX12_CreateDeviceObjects()
{
	if (!g_pd3dDevice)
		return false;
	if (g_pPipelineState)
		ImGui_ImplDX12_InvalidateDeviceObjects();

	// Create the root signature
	{
		D3D12_DESCRIPTOR_RANGE descRange ;
		descRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		descRange.NumDescriptors = 1;
		descRange.BaseShaderRegister = 0;
		descRange.RegisterSpace = 0;
		descRange.OffsetInDescriptorsFromTableStart = 0;

		D3D12_ROOT_PARAMETER param[2] ;

		param[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		param[0].Constants.ShaderRegister = 0;
		param[0].Constants.RegisterSpace = 0;
		param[0].Constants.Num32BitValues = 16;
		param[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

		param[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		param[1].DescriptorTable.NumDescriptorRanges = 1;
		param[1].DescriptorTable.pDescriptorRanges = &descRange;
		param[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_STATIC_SAMPLER_DESC staticSampler ;
		staticSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		staticSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		staticSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		staticSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		staticSampler.MipLODBias = 0.f;
		staticSampler.MaxAnisotropy = 0;
		staticSampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		staticSampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
		staticSampler.MinLOD = 0.f;
		staticSampler.MaxLOD = 0.f;
		staticSampler.ShaderRegister = 0;
		staticSampler.RegisterSpace = 0;
		staticSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

		D3D12_ROOT_SIGNATURE_DESC desc ;
		desc.NumParameters = _countof(param);
		desc.pParameters = param;
		desc.NumStaticSamplers = 1;
		desc.pStaticSamplers = &staticSampler;
		desc.Flags =
			D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
			D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

		ID3DBlob* blob = NULL;
		if (D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, NULL) != S_OK)
			return false;


		g_pd3dDevice->lpVtbl->CreateRootSignature(g_pd3dDevice,0, blob->lpVtbl->GetBufferPointer(blob), blob->lpVtbl->GetBufferSize(blob), &IID_ID3D12RootSignature,(void**)&g_pRootSignature);
		g_pRootSignature->lpVtbl->SetName(g_pRootSignature, L"imgui_rootsig");
		blob->lpVtbl->Release(blob);
	}

	// By using D3DCompile() from <d3dcompiler.h> / d3dcompiler.lib, we introduce a dependency to a given version of d3dcompiler_XX.dll (see D3DCOMPILER_DLL_A)
	// If you would like to use this DX12 sample code but remove this dependency you can:
	//  1) compile once, save the compiled shader blobs into a file or source code and pass them to CreateVertexShader()/CreatePixelShader() [preferred solution]
	//  2) use code to detect any version of the DLL and grab a pointer to D3DCompile from the DLL.
	// See https://github.com/ocornut/imgui/pull/638 for sources and details.

	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc;
	memset(&psoDesc, 0, sizeof(D3D12_GRAPHICS_PIPELINE_STATE_DESC));
	psoDesc.NodeMask = 1;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.pRootSignature = g_pRootSignature;
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = g_RTVFormat;
	psoDesc.SampleDesc.Count = 1;
	psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

	// Create the vertex shader
	{
		static const char* vertexShader =
			"cbuffer vertexBuffer : register(b0) \
			{\
				float4x4 ProjectionMatrix; \
			};\
		struct VS_INPUT\
		{\
			float2 pos : POSITION;\
				float4 col : COLOR0;\
				float2 uv  : TEXCOORD0;\
		};\
		\
			struct PS_INPUT\
			{\
				float4 pos : SV_POSITION;\
					float4 col : COLOR0;\
					float2 uv  : TEXCOORD0;\
			};\
		\
			PS_INPUT main(VS_INPUT input)\
			{\
				PS_INPUT output;\
					output.pos = mul( ProjectionMatrix, float4(input.pos.xy, 0.f, 1.f));\
					output.col = input.col;\
					output.uv  = input.uv;\
					return output;\
			}";

		D3DCompile(vertexShader, strlen(vertexShader), NULL, NULL, NULL, "main", "vs_5_0", 0, 0, &g_pVertexShaderBlob, NULL);
		if (g_pVertexShaderBlob == NULL) // NB: Pass ID3D10Blob* pErrorBlob to D3DCompile() to get error showing in (const char*)pErrorBlob->GetBufferPointer(). Make sure to Release() the blob!
			return false;
		
		D3D12_SHADER_BYTECODE vs_bytecode;
		vs_bytecode.pShaderBytecode = g_pVertexShaderBlob->lpVtbl->GetBufferPointer(g_pVertexShaderBlob);
		vs_bytecode.BytecodeLength = g_pVertexShaderBlob->lpVtbl->GetBufferSize(g_pVertexShaderBlob);
		psoDesc.VS = vs_bytecode;

		// Create the input layout
		static D3D12_INPUT_ELEMENT_DESC local_layout[] = {
			{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, pos), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, (UINT)IM_OFFSETOF(ImDrawVert, uv),  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (UINT)IM_OFFSETOF(ImDrawVert, col), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};
		D3D12_INPUT_LAYOUT_DESC layout;
		layout.pInputElementDescs = local_layout;
		layout.NumElements = 3;
		psoDesc.InputLayout = layout;
	}

	// Create the pixel shader
	{
		static const char* pixelShader =
			"struct PS_INPUT\
			{\
				float4 pos : SV_POSITION;\
					float4 col : COLOR0;\
					float2 uv  : TEXCOORD0;\
			};\
		SamplerState sampler0 : register(s0);\
			Texture2D texture0 : register(t0);\
			\
			float4 main(PS_INPUT input) : SV_Target\
			{\
				float4 out_col = input.col * texture0.Sample(sampler0, input.uv); \
					return out_col; \
			}";

		D3DCompile(pixelShader, strlen(pixelShader), NULL, NULL, NULL, "main", "ps_5_0", 0, 0, &g_pPixelShaderBlob, NULL);
		if (g_pPixelShaderBlob == NULL)  // NB: Pass ID3D10Blob* pErrorBlob to D3DCompile() to get error showing in (const char*)pErrorBlob->GetBufferPointer(). Make sure to Release() the blob!
			return false;
		D3D12_SHADER_BYTECODE ps_bytecode;
		ps_bytecode.pShaderBytecode = g_pPixelShaderBlob->lpVtbl->GetBufferPointer(g_pPixelShaderBlob);
		ps_bytecode.BytecodeLength = g_pPixelShaderBlob->lpVtbl->GetBufferSize(g_pPixelShaderBlob);
		psoDesc.PS = ps_bytecode;
	}

	// Create the blending setup
	{
		D3D12_BLEND_DESC* desc = &psoDesc.BlendState;
		desc->AlphaToCoverageEnable = false;
		desc->RenderTarget[0].BlendEnable = true;
		desc->RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
		desc->RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		desc->RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
		desc->RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		desc->RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
		desc->RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
		desc->RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
	}

	// Create the rasterizer state
	{
		D3D12_RASTERIZER_DESC* desc = &psoDesc.RasterizerState;
		desc->FillMode = D3D12_FILL_MODE_SOLID;
		desc->CullMode = D3D12_CULL_MODE_NONE;
		desc->FrontCounterClockwise = FALSE;
		desc->DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		desc->DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		desc->SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		desc->DepthClipEnable = true;
		desc->MultisampleEnable = FALSE;
		desc->AntialiasedLineEnable = FALSE;
		desc->ForcedSampleCount = 0;
		desc->ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
	}

	// Create depth-stencil State
	{
		D3D12_DEPTH_STENCIL_DESC* desc = &psoDesc.DepthStencilState;
		desc->DepthEnable = false;
		desc->DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		desc->DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc->StencilEnable = false;
		desc->FrontFace.StencilFailOp = desc->FrontFace.StencilDepthFailOp = desc->FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		desc->FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		desc->BackFace = desc->FrontFace;
	}

	if (g_pd3dDevice->lpVtbl->CreateGraphicsPipelineState(g_pd3dDevice,&psoDesc, &IID_ID3D12PipelineState,(void**)&g_pPipelineState) != S_OK)
		return false;
	g_pPipelineState->lpVtbl->SetName(g_pPipelineState, L"imgui_pso");

	ImGui_ImplDX12_CreateFontsTexture();

	return true;
}

void    ImGui_ImplDX12_InvalidateDeviceObjects()
{
	if (!g_pd3dDevice)
		return;

	if(g_pVertexShaderBlob)
	{
		g_pVertexShaderBlob->lpVtbl->Release(g_pVertexShaderBlob);
		g_pVertexShaderBlob = NULL;
	}
	if(g_pPixelShaderBlob)
	{
		g_pPixelShaderBlob->lpVtbl->Release(g_pPixelShaderBlob);
		g_pPixelShaderBlob = NULL;
	}
	if(g_pRootSignature)
	{
		g_pRootSignature->lpVtbl->Release(g_pRootSignature);
		g_pRootSignature = NULL;
	}
	if(g_pPipelineState)
	{
		g_pPipelineState->lpVtbl->Release(g_pPipelineState);
		g_pPipelineState = NULL;
	}
	if(g_pFontTextureResource)
	{
		g_pFontTextureResource->lpVtbl->Release(g_pFontTextureResource);
		g_pFontTextureResource = NULL;
	}

	ImGuiIO* io = igGetIO();
	io->Fonts->TexID = NULL; // We copied g_pFontTextureView to io.Fonts->TexID so let's clear that as well.

	for (UINT i = 0; i < g_numFramesInFlight; i++)
	{
		FrameResources* fr = &g_pFrameResources[i];
		if(fr->IndexBuffer)
		{
			fr->IndexBuffer->lpVtbl->Release(fr->IndexBuffer);
			fr->IndexBuffer = NULL;
		}
		if(fr->VertexBuffer)
		{
			fr->VertexBuffer->lpVtbl->Release(fr->VertexBuffer);
			fr->VertexBuffer = NULL;
		}
	}
}

static bool ImGui_ImplDX12_Init(ID3D12Device* device, int num_frames_in_flight, DXGI_FORMAT rtv_format, ID3D12DescriptorHeap* cbv_srv_heap,
		D3D12_CPU_DESCRIPTOR_HANDLE font_srv_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE font_srv_gpu_desc_handle)
{
	// Setup back-end capabilities flags
	ImGuiIO* io = igGetIO();
	io->BackendRendererName = "imgui_impl_dx12";
	io->BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;  // We can honor the ImDrawCmd::VtxOffset field, allowing for large meshes.

	g_pd3dDevice = device;

	g_RTVFormat = rtv_format;
	g_hFontSrvCpuDescHandle = font_srv_cpu_desc_handle;
	g_hFontSrvGpuDescHandle = font_srv_gpu_desc_handle;
	g_pFrameResources = (FrameResources*)malloc(sizeof(FrameResources ) * (UINT64)num_frames_in_flight);
	g_numFramesInFlight = (UINT)num_frames_in_flight;
	g_frameIndex = UINT_MAX;

	// Create buffers with a default size (they will later be grown as needed)
	for (int i = 0; i < num_frames_in_flight; i++)
	{
		FrameResources* fr = &g_pFrameResources[i];
		fr->IndexBuffer = NULL;
		fr->VertexBuffer = NULL;
		fr->IndexBufferSize = 10000;
		fr->VertexBufferSize = 5000;
	}

	return true;
}

static void ImGui_ImplDX12_Shutdown()
{
	ImGui_ImplDX12_InvalidateDeviceObjects();
	free(g_pFrameResources);
	g_pFrameResources = NULL;
	g_pd3dDevice = NULL;
	g_hFontSrvCpuDescHandle.ptr = 0;
	g_hFontSrvGpuDescHandle.ptr = 0;
	g_numFramesInFlight = 0;
	g_frameIndex = UINT_MAX;
}

static void ImGui_ImplDX12_NewFrame()
{
	if (!g_pPipelineState) {
		ImGui_ImplDX12_CreateDeviceObjects();
	}
}
