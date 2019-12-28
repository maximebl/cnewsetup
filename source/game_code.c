/*#define WIN32_LEAN_AND_MEAN*/
/*#include "Windows.h"*/
/*#include "cnewsetup.h"*/
/*#include <assert.h>*/
/*#include "d3d12.h"*/
/*#include <dxgiformat.h>*/
/*#include <dxgi1_4.h>*/

/*#pragma clang diagnostic push*/
/*#pragma clang diagnostic ignored "-Weverything"*/
/*#include "cimgui/cimgui.h"*/
/*#pragma clang diagnostic pop*/

/*#include "cimgui/cimgui.h"*/

#include "imgui_impl_dx12.c"
#include "imgui_impl_win32.c"
#include "common.h"
#include <processthreadsapi.h>
#include <realtimeapiset.h>

#define DX12_ENABLE_DEBUG_LAYER
#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#include "WinPixEventRuntime/cwinpix.h"
#endif

#pragma comment(lib, "cwinpix")
#pragma comment(lib, "cimgui")
#pragma comment(lib, "d3d12")
#pragma comment(lib, "dxgi")
#pragma comment(lib, "dxguid")
#pragma comment(lib, "d3dcompiler")
#pragma comment(lib, "user32")

struct FrameContext {
	ID3D12CommandAllocator* CommandAllocator;
	UINT64 FenceValue;
};

#define IDT_TIMER1 1
#define NUM_FRAMES_IN_FLIGHT 3
#define NUM_BACK_BUFFERS 3
static HWND* g_hwnd;
static struct FrameContext g_frameContext[NUM_FRAMES_IN_FLIGHT];
static UINT g_frameIndex = 0;
static ID3D12Device* g_pd3dDevice = NULL;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = NULL;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = NULL;
static ID3D12CommandQueue* g_pd3dCommandQueue = NULL;
static ID3D12GraphicsCommandList* g_pd3dCommandList = NULL;
static ID3D12Fence* g_fence = NULL;
static HANDLE g_fenceEvent = NULL;
static UINT64 g_fenceLastSignaledValue = 0;
static IDXGISwapChain3* g_pSwapChain = NULL;
static HANDLE g_hSwapChainWaitableObject = NULL;  // Signals when the DXGI Adapter finished presenting a new frame
static ID3D12Resource* g_mainRenderTargetResource[NUM_BACK_BUFFERS];
static D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS];

static UINT64* timestamp_buffer = NULL;
static ID3D12Resource* rb_buffer;
static ID3D12QueryHeap* query_heap;
static double frame_time = 0.0;
static UINT total_timer_count = 6;
static UINT ui_timer_count = 6;
UINT stats_counter = 0;
void frame_time_statistics(void);

static bool is_vsync = true;

//Benchmarks
double g_cpu_frequency = 0;
double g_gpu_frequency = 0;
measurement delta_time;
#define BUFFERED_FRAME_STATS 200
double delta_times[BUFFERED_FRAME_STATS];
double delta_time_avg = 0;

// required to fix a bug in the directx12 c language bindings
typedef void(__stdcall* fixed_GetCPUDescriptorHandleForHeapStart)(
    ID3D12DescriptorHeap* This,
    D3D12_CPU_DESCRIPTOR_HANDLE* pOut);
typedef void(__stdcall* fixed_GetGPUDescriptorHandleForHeapStart)(
    ID3D12DescriptorHeap* This,
    D3D12_GPU_DESCRIPTOR_HANDLE* pOut);

BOOL APIENTRY DllMain(DWORD ul_reason_for_call);
__declspec(dllexport) _Bool CreateDeviceD3D();
__declspec(dllexport) _Bool update_and_render(void);
__declspec(dllexport) void resize(HWND hWnd, int width, int height);
__declspec(dllexport) void CleanupDeviceD3D(void);
__declspec(dllexport) void ResizeSwapChain(HWND hWnd, int width, int height);
__declspec(dllexport) void CleanupRenderTarget(void);
__declspec(dllexport) void CreateRenderTarget(void);
__declspec(dllexport) void cleanup(void);
__declspec(dllexport) void wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
__declspec(dllexport) bool initialize(HWND* hwnd);

void WaitForLastSubmittedFrame(void);
struct FrameContext* WaitForNextFrameResources(void);
void create_query_objects(void);
void cpu_wait(UINT64 fence_value);

BOOL APIENTRY DllMain(DWORD ul_reason_for_call)
{
	switch (ul_reason_for_call) {
		case DLL_PROCESS_ATTACH:
			break;

		case DLL_THREAD_ATTACH:
			break;

		case DLL_THREAD_DETACH:
			break;

		case DLL_PROCESS_DETACH:
			break;
	}
	return TRUE;
}

__declspec(dllexport) bool initialize(HWND* hwnd)
{
	g_hwnd = hwnd;
	if (!CreateDeviceD3D()) {
		CleanupDeviceD3D();
		return true;
	}
	igCreateContext(0);
	ImGuiIO* io = igGetIO();
	(void)io;
	igStyleColorsDark(0);
	ImGui_ImplWin32_Init(*hwnd);

	fixed_GetCPUDescriptorHandleForHeapStart fixed_gchfhs =
	    (fixed_GetCPUDescriptorHandleForHeapStart)
		g_pd3dRtvDescHeap->lpVtbl->GetCPUDescriptorHandleForHeapStart;
	fixed_GetGPUDescriptorHandleForHeapStart fixed_gghfhs =
	    (fixed_GetGPUDescriptorHandleForHeapStart)
		g_pd3dSrvDescHeap->lpVtbl->GetGPUDescriptorHandleForHeapStart;

	D3D12_CPU_DESCRIPTOR_HANDLE rtvhandle;
	D3D12_GPU_DESCRIPTOR_HANDLE srvhandle;
	fixed_gchfhs(g_pd3dSrvDescHeap, &rtvhandle);
	fixed_gghfhs(g_pd3dSrvDescHeap, &srvhandle);

	ImGui_ImplDX12_Init(g_pd3dDevice,
			    NUM_FRAMES_IN_FLIGHT,
			    DXGI_FORMAT_R8G8B8A8_UNORM,
			    g_pd3dSrvDescHeap,
			    rtvhandle,
			    srvhandle);

	LARGE_INTEGER tmp_gpu_frequency;
	g_pd3dCommandQueue->lpVtbl->GetTimestampFrequency(g_pd3dCommandQueue, &tmp_gpu_frequency);
	g_gpu_frequency = (double)tmp_gpu_frequency.QuadPart;

	LARGE_INTEGER tmp_cpu_frequency;
	QueryPerformanceFrequency(&tmp_cpu_frequency);
	g_cpu_frequency = (double)tmp_cpu_frequency.QuadPart;
	delta_time = measurement_default;

	SetTimer(*hwnd, IDT_TIMER1, 5000 ,NULL);
	return true;
}

__declspec(dllexport) void cleanup(void)
{
	KillTimer(*g_hwnd, IDT_TIMER1);
	cpu_wait(g_fenceLastSignaledValue);
	ImGui_ImplDX12_Shutdown();
	ImGui_ImplWin32_Shutdown();
	igDestroyContext(0);
	CleanupDeviceD3D();
}

void frame_time_statistics(void){
	double sum = 0;
	for(int i = 0; i < stats_counter; ++i ) {
		sum += delta_times[i];
	}
	delta_time_avg = sum / stats_counter;
	memset(&delta_times, 0, stats_counter);
}

__declspec(dllexport) void wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);

	switch (msg) {
		case WM_TIMER:
			switch(wParam)
			{
				case IDT_TIMER1:
					frame_time_statistics();
					return 0;
			}
			break;
	}
}

__declspec(dllexport) bool CreateDeviceD3D()
{
	DXGI_SWAP_CHAIN_DESC1 sd;
	{
		ZeroMemory(&sd, sizeof(sd));
		sd.BufferCount = NUM_BACK_BUFFERS;
		sd.Width = 0;
		sd.Height = 0;
		sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.SampleDesc.Count = 1;
		sd.SampleDesc.Quality = 0;
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
		sd.Scaling = DXGI_SCALING_STRETCH;
		sd.Stereo = FALSE;
	}

#ifdef DX12_ENABLE_DEBUG_LAYER
	ID3D12Debug* pdx12Debug = NULL;
	if (SUCCEEDED(D3D12GetDebugInterface(&IID_ID3D12Debug, (void**)&pdx12Debug))) {
		pdx12Debug->lpVtbl->EnableDebugLayer(pdx12Debug);
		pdx12Debug->lpVtbl->Release(pdx12Debug);
	}
#endif
	D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
	if (D3D12CreateDevice(NULL, featureLevel, &IID_ID3D12Device, (void**)&g_pd3dDevice) != S_OK)
		return false;

	g_pd3dDevice->lpVtbl->SetName(g_pd3dDevice,L"main_device");

	{
		D3D12_DESCRIPTOR_HEAP_DESC desc;
		desc.NodeMask = 1;
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		desc.NumDescriptors = NUM_BACK_BUFFERS;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

		if (g_pd3dDevice->lpVtbl->CreateDescriptorHeap(g_pd3dDevice,
							       &desc,
							       &IID_ID3D12DescriptorHeap,
							       (void**)&g_pd3dRtvDescHeap) != S_OK)
			return false;

		g_pd3dRtvDescHeap->lpVtbl->SetName(g_pd3dRtvDescHeap, L"main_rtv_desc_heap");
		SIZE_T rtvDescriptorSize = g_pd3dDevice->lpVtbl->GetDescriptorHandleIncrementSize( g_pd3dDevice, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

		fixed_GetCPUDescriptorHandleForHeapStart fixed_gchfhs =
		    (fixed_GetCPUDescriptorHandleForHeapStart)
			g_pd3dRtvDescHeap->lpVtbl->GetCPUDescriptorHandleForHeapStart;
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
		fixed_gchfhs(g_pd3dRtvDescHeap, &rtvHandle);

		for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
			g_mainRenderTargetDescriptor[i] = rtvHandle;
			rtvHandle.ptr += rtvDescriptorSize;
		}
	}

	{
		D3D12_DESCRIPTOR_HEAP_DESC desc;
		desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		desc.NumDescriptors = 1;
		desc.NodeMask = 1;
		desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

		g_pd3dDevice->lpVtbl->CreateDescriptorHeap(g_pd3dDevice,
							   &desc,
							   &IID_ID3D12DescriptorHeap,
							   (void**)&g_pd3dSrvDescHeap);
		g_pd3dSrvDescHeap->lpVtbl->SetName(g_pd3dSrvDescHeap, L"main_srv_desc_heap");
	}

	{
		D3D12_COMMAND_QUEUE_DESC desc;
		desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		desc.NodeMask = 1;
		if (g_pd3dDevice->lpVtbl->CreateCommandQueue(g_pd3dDevice,
							     &desc,
							     &IID_ID3D12CommandQueue,
							     (void**)&g_pd3dCommandQueue) != S_OK)
			return false;
		g_pd3dCommandQueue->lpVtbl->SetName(g_pd3dCommandQueue, L"main_cmd_queue");
	}

	for (UINT i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
		if (g_pd3dDevice->lpVtbl->CreateCommandAllocator(
			g_pd3dDevice,
			D3D12_COMMAND_LIST_TYPE_DIRECT,
			&IID_ID3D12CommandAllocator,
			(void**)&g_frameContext[i].CommandAllocator) != S_OK)
			return false;

	if (g_pd3dDevice->lpVtbl->CreateCommandList(g_pd3dDevice,
						    0,
						    D3D12_COMMAND_LIST_TYPE_DIRECT,
						    g_frameContext[0].CommandAllocator,
						    NULL,
						    &IID_ID3D12CommandList,
						    (void**)&g_pd3dCommandList) != S_OK ||
	    g_pd3dCommandList->lpVtbl->Close(g_pd3dCommandList) != S_OK)
		return false;

	g_pd3dCommandList->lpVtbl->SetName(g_pd3dCommandList, L"main_cmd_list");

	if (g_pd3dDevice->lpVtbl->CreateFence(g_pd3dDevice,
					      0,
					      D3D12_FENCE_FLAG_NONE,
					      &IID_ID3D12Fence,
					      (void**)&g_fence) != S_OK)
		return false;

	g_fence->lpVtbl->SetName(g_fence, L"main fence");

	g_fenceEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (g_fenceEvent == NULL) return false;

	{
		IDXGIFactory4* dxgiFactory = NULL;
		IDXGISwapChain1* swapChain1 = NULL;
		if (CreateDXGIFactory1(&IID_IDXGIFactory4, (void**)&dxgiFactory) != S_OK ||
		    dxgiFactory->lpVtbl->CreateSwapChainForHwnd(dxgiFactory,
								(IUnknown*)g_pd3dCommandQueue,
								*g_hwnd,
								&sd,
								NULL,
								NULL,
								&swapChain1) != S_OK ||
		    swapChain1->lpVtbl->QueryInterface(swapChain1,
						       &IID_IDXGISwapChain1,
						       (void**)&g_pSwapChain) != S_OK)
			return false;
		swapChain1->lpVtbl->Release(swapChain1);
		dxgiFactory->lpVtbl->Release(dxgiFactory);
		g_pSwapChain->lpVtbl->SetMaximumFrameLatency(g_pSwapChain, NUM_BACK_BUFFERS);

		g_hSwapChainWaitableObject = g_pSwapChain->lpVtbl->GetFrameLatencyWaitableObject(g_pSwapChain);
	}

	CreateRenderTarget();
	create_query_objects();
	return true;
}

__declspec(dllexport) void CreateRenderTarget()
{
	for (UINT i = 0; i < NUM_BACK_BUFFERS; i++) {
		ID3D12Resource* pBackBuffer = NULL;
		g_pSwapChain->lpVtbl->GetBuffer(g_pSwapChain,
						i,
						&IID_ID3D12Resource,
						(void**)&pBackBuffer);
		g_pd3dDevice->lpVtbl->CreateRenderTargetView(g_pd3dDevice,
							     pBackBuffer,
							     NULL,
							     g_mainRenderTargetDescriptor[i]);
		g_mainRenderTargetResource[i] = pBackBuffer;
		g_mainRenderTargetResource[i]->lpVtbl->SetName(g_mainRenderTargetResource[i], L"rtv_");
	}
}

__declspec(dllexport) void CleanupRenderTarget()
{
	WaitForLastSubmittedFrame();

	for (UINT i = 0; i < NUM_BACK_BUFFERS; i++)
		if (g_mainRenderTargetResource[i]) {
			g_mainRenderTargetResource[i]->lpVtbl->Release(
			    g_mainRenderTargetResource[i]);
			g_mainRenderTargetResource[i] = NULL;
		}
}

void cpu_wait(UINT64 fence_value)
{
	if (fence_value == 0) return;  // No fence was signaled
	if (g_fence->lpVtbl->GetCompletedValue(g_fence) >= fence_value) return; // We're already exactly at that fence value, or past that fence value

	g_fence->lpVtbl->SetEventOnCompletion(g_fence, fence_value, g_fenceEvent);
	WaitForSingleObject(g_fenceEvent, INFINITE);
	/*cPIXNotifyWakeFromFenceSignal(g_fenceEvent);  // The event was successfully signaled, so notify PIX*/
}

void WaitForLastSubmittedFrame()
{
	struct FrameContext* frameCtxt = &g_frameContext[g_frameIndex % NUM_FRAMES_IN_FLIGHT];

	UINT64 fenceValue = frameCtxt->FenceValue;
	if (fenceValue == 0) return;  // No fence was signaled

	frameCtxt->FenceValue = 0;
	if (g_fence->lpVtbl->GetCompletedValue(g_fence) >= fenceValue) return;

	g_fence->lpVtbl->SetEventOnCompletion(g_fence, fenceValue, g_fenceEvent);

	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(0, 0, 0), "WaitForLastSubmittedFrame");*/
	DWORD wait_result = WaitForSingleObject(g_fenceEvent, INFINITE);
	/*cPIXEndEvent_gpu(g_pd3dCommandQueue);*/

	/*switch (wait_result)*/
	/*{*/
	/*case WAIT_OBJECT_0:*/
	/*cPIXNotifyWakeFromFenceSignal( g_fenceEvent);  // The event was successfully signaled, so notify PIX*/
			    /*break;*/
			    /*}*/
}

struct FrameContext* WaitForNextFrameResources()
{
	// Make the CPU wait until the adapter finished presenting the new frame.(g_hSwapChainWaitableObject)
	// Make the CPU wait until the GPU has reached the fence value for the next frame. (g_fenceEvent)
	UINT nextFrameIndex = g_frameIndex + 1;
	g_frameIndex = nextFrameIndex;

	HANDLE waitableObjects[] = {g_hSwapChainWaitableObject, NULL};
	DWORD numWaitableObjects = 1;

	struct FrameContext* frameCtxt = &g_frameContext[nextFrameIndex % NUM_FRAMES_IN_FLIGHT];
	UINT64 fenceValue = frameCtxt->FenceValue;
	if (fenceValue != 0)  // means no fence was signaled
	{
		frameCtxt->FenceValue = 0;
		g_fence->lpVtbl->SetEventOnCompletion(g_fence, fenceValue, g_fenceEvent);
		waitableObjects[1] = g_fenceEvent;
		numWaitableObjects = 2;
	}

	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(100, 100, 100), "WaitForNextFrameResources");*/
	DWORD wait_result = WaitForMultipleObjects(numWaitableObjects, waitableObjects, TRUE, INFINITE);
	/*cPIXEndEvent_gpu(g_pd3dCommandQueue);*/
	/*switch (wait_result)*/
	/*{*/
	/*case WAIT_OBJECT_0:*/
	/*cPIXNotifyWakeFromFenceSignal( g_fenceEvent);  // The event was successfully signaled, so notify PIX*/
			    /*break;*/
	/*}*/
	return frameCtxt;
}

void create_query_objects(void)
{
	SUCCEEDED(g_pd3dDevice->lpVtbl->CreateQueryHeap(
	    g_pd3dDevice,
	    &(D3D12_QUERY_HEAP_DESC){.Count = total_timer_count,
				     .NodeMask = 1,
				     .Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP},
	    &IID_ID3D12QueryHeap,
	    (void**)&query_heap));

	query_heap->lpVtbl->SetName(query_heap, L"timestamp_query_heap");

	SUCCEEDED( g_pd3dDevice->lpVtbl->CreateCommittedResource(
	    g_pd3dDevice,
	    &(D3D12_HEAP_PROPERTIES){.CreationNodeMask = 1,
				     .Type = D3D12_HEAP_TYPE_READBACK,
				     .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN,
				     .MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN,
				     .VisibleNodeMask = 1},
	    D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
	    &(D3D12_RESOURCE_DESC){.Alignment = 0,
				   .DepthOrArraySize = 1,
				   .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
				   .Flags = D3D12_RESOURCE_FLAG_NONE,
				   .Format = DXGI_FORMAT_UNKNOWN,
				   .Height = 1,
				   .MipLevels = 1,
				   .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
				   .Width = sizeof(UINT64) * total_timer_count,
				   .SampleDesc.Count = 1,
				   .SampleDesc.Quality = 0},
	    D3D12_RESOURCE_STATE_COPY_DEST,
	    NULL,
	    &IID_ID3D12Resource,
	    (void**)&rb_buffer));
	rb_buffer->lpVtbl->SetName(rb_buffer, L"queries_rb_buffer");
}

__declspec(dllexport) void ResizeSwapChain(HWND hWnd, int width, int height)
{
	DXGI_SWAP_CHAIN_DESC1 sd;
	g_pSwapChain->lpVtbl->GetDesc1(g_pSwapChain, &sd);
	sd.Width = (UINT)width;
	sd.Height = (UINT)height;

	IDXGIFactory4* dxgiFactory = NULL;
	g_pSwapChain->lpVtbl->GetParent(g_pSwapChain, &IID_IDXGIFactory4, (void**)&dxgiFactory);

	g_pSwapChain->lpVtbl->Release(g_pSwapChain);
	CloseHandle(g_hSwapChainWaitableObject);

	IDXGISwapChain1* swapChain1 = NULL;

	dxgiFactory->lpVtbl->CreateSwapChainForHwnd(dxgiFactory,
						    (IUnknown*)g_pd3dCommandQueue,
						    hWnd,
						    &sd,
						    NULL,
						    NULL,
						    &swapChain1);
	swapChain1->lpVtbl->QueryInterface(swapChain1, &IID_IDXGISwapChain1, (void**)&g_pSwapChain);
	swapChain1->lpVtbl->Release(swapChain1);
	dxgiFactory->lpVtbl->Release(dxgiFactory);

	g_pSwapChain->lpVtbl->SetMaximumFrameLatency(g_pSwapChain, NUM_BACK_BUFFERS);

	g_hSwapChainWaitableObject =
	    g_pSwapChain->lpVtbl->GetFrameLatencyWaitableObject(g_pSwapChain);
	assert(g_hSwapChainWaitableObject != NULL);
}

__declspec(dllexport) void CleanupDeviceD3D()
{
	CleanupRenderTarget();

	csafe_release(g_pSwapChain);
	if (g_hSwapChainWaitableObject != NULL) CloseHandle(g_hSwapChainWaitableObject);

	for (UINT i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
		if (g_frameContext[i].CommandAllocator) {
			g_frameContext[i].CommandAllocator->lpVtbl->Release(
			    g_frameContext[i].CommandAllocator);
			g_frameContext[i].CommandAllocator = NULL;
		}

	csafe_release(g_pd3dCommandQueue);
	csafe_release(g_pd3dCommandList);
	csafe_release(g_pd3dRtvDescHeap);
	csafe_release(g_pd3dSrvDescHeap);
	csafe_release(g_fence);
	if (g_fenceEvent) {
		CloseHandle(g_fenceEvent);
		g_fenceEvent = NULL;
	}
	csafe_release(rb_buffer);
	csafe_release(query_heap);

	/*csafe_release(g_pd3dDevice);*/
	/*ULONG refcount = g_pd3dDevice->lpVtbl->Release(g_pd3dDevice);*/

#ifdef DX12_ENABLE_DEBUG_LAYER
	IDXGIDebug1* pDebug = NULL;
	if (SUCCEEDED(DXGIGetDebugInterface1(0, &IID_IDXGIDebug1, (void**)&pDebug))) {
		pDebug->lpVtbl->ReportLiveObjects(pDebug, DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_DETAIL);
		pDebug->lpVtbl->Release(pDebug);
	}
#endif
}

__declspec(dllexport) bool update_and_render()
{
	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(250, 0, 0), "update_and_render");*/

	ImGui_ImplDX12_NewFrame();
	ImGui_ImplWin32_NewFrame();
	igNewFrame();

	bool show_demo_window = true;
	bool show_another_window = false;
	ImVec4 clear_color;
	clear_color.x = 1.0f;
	clear_color.y = 0.0f;
	clear_color.z = 1.0f;
	clear_color.w = 0.0f;

	if (show_demo_window) igShowDemoWindow(&show_demo_window);

	{
		ImGuiContext* imguictx = igGetCurrentContext();
		igSetCurrentContext(imguictx);

		igCheckbox("Demo Window", &show_demo_window);
		igCheckbox("VSync", &is_vsync);
		igColorEdit3("clear color", (float*)&clear_color, 0);

		igText("imgui gpu time %.4f ms/frame",
		       frame_time);

		igText("elapsed time %.4f ms/frame",
		       delta_time.elapsed_ms);

		igText("average delta %.4f ms/frame",
		       delta_time_avg);

		igText("Application average %.4f ms/frame (%.1f FPS)",
		       (double)(1000.0f / igGetIO()->Framerate),
		       (double)igGetIO()->Framerate);
	}

	struct FrameContext* frameCtxt = WaitForNextFrameResources();

	UINT backBufferIdx = g_pSwapChain->lpVtbl->GetCurrentBackBufferIndex(g_pSwapChain);
	frameCtxt->CommandAllocator->lpVtbl->Reset(frameCtxt->CommandAllocator);
	g_pd3dCommandList->lpVtbl->Reset(g_pd3dCommandList, frameCtxt->CommandAllocator, NULL);

	D3D12_RESOURCE_BARRIER barrier;
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

	g_pd3dCommandList->lpVtbl->ResourceBarrier(g_pd3dCommandList, 1, &barrier);
	g_pd3dCommandList->lpVtbl->ClearRenderTargetView(
	    g_pd3dCommandList,
	    g_mainRenderTargetDescriptor[backBufferIdx],
	    (float*)&clear_color,
	    0,
	    NULL);
	g_pd3dCommandList->lpVtbl->OMSetRenderTargets(g_pd3dCommandList,
						      1,
						      &g_mainRenderTargetDescriptor[backBufferIdx],
						      FALSE,
						      NULL);
	g_pd3dCommandList->lpVtbl->SetDescriptorHeaps(g_pd3dCommandList, 1, &g_pd3dSrvDescHeap);

	UINT buffer_start = backBufferIdx * 2;
	UINT buffer_end = (backBufferIdx * 2 + 1);

	g_pd3dCommandList->lpVtbl->EndQuery(g_pd3dCommandList,
					    query_heap,
					    D3D12_QUERY_TYPE_TIMESTAMP,
					    buffer_start);

	/*cPIXEndEvent_gpu(g_pd3dCommandQueue);*/
	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(0, 0, 255), "igRender");*/

	igRender();  // render ui

	/*cPIXEndEvent_gpu(g_pd3dCommandQueue);*/
	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(255, 0, 255), "RenderDrawData");*/
	ImGui_ImplDX12_RenderDrawData(igGetDrawData(), g_pd3dCommandList);
	/*cPIXEndEvent_gpu(g_pd3dCommandQueue);*/

	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
	g_pd3dCommandList->lpVtbl->ResourceBarrier(g_pd3dCommandList, 1, &barrier);

	g_pd3dCommandList->lpVtbl->EndQuery(g_pd3dCommandList,
					    query_heap,
					    D3D12_QUERY_TYPE_TIMESTAMP,
					    buffer_end);

	g_pd3dCommandList->lpVtbl->ResolveQueryData(g_pd3dCommandList,
						    query_heap,
						    D3D12_QUERY_TYPE_TIMESTAMP,
						    0,
						    ui_timer_count,
						    rb_buffer,
						    0);

	g_pd3dCommandList->lpVtbl->Close(g_pd3dCommandList);
	g_pd3dCommandQueue->lpVtbl->ExecuteCommandLists(
	    g_pd3dCommandQueue,
	    1,
	    (ID3D12CommandList* const*)&g_pd3dCommandList);

	HRESULT hr4 = rb_buffer->lpVtbl->Map(rb_buffer,
					     0,
					     &(D3D12_RANGE){.Begin = buffer_start * sizeof(UINT64),
							    .End = buffer_end * sizeof(UINT64)},
					     (void**)&timestamp_buffer);

	UINT64 time_delta = timestamp_buffer[buffer_end] - timestamp_buffer[buffer_start];
	frame_time = ((double)time_delta / g_gpu_frequency) * 1000.0;

	rb_buffer->lpVtbl->Unmap(rb_buffer, 0, &(D3D12_RANGE){.Begin = 0, .End = 0});
	timestamp_buffer = NULL;

	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(255, 255, 0), "Present");*/

	UINT sync_interval = is_vsync ? 1 : 0;
	UINT present_flags = is_vsync ? 0 : DXGI_PRESENT_ALLOW_TEARING;
	g_pSwapChain->lpVtbl->Present(g_pSwapChain, sync_interval, present_flags);

	UINT64 fenceValue = g_fenceLastSignaledValue + 1;
	g_pd3dCommandQueue->lpVtbl->Signal(g_pd3dCommandQueue, g_fence, fenceValue);
	g_fenceLastSignaledValue = fenceValue;
	frameCtxt->FenceValue = fenceValue;

	// Gather statistics
	DXGI_FRAME_STATISTICS frame_stats;
	g_pSwapChain->lpVtbl->GetFrameStatistics(g_pSwapChain, &frame_stats);

	LARGE_INTEGER current_time;
	QueryPerformanceCounter(&current_time);
	delta_time.end_time = (double)current_time.QuadPart;

	delta_time.elapsed_ms = (delta_time.end_time - delta_time.start_time);
	delta_time.elapsed_ms /= g_cpu_frequency;
	delta_time.elapsed_ms *= millisecond;

	delta_time.start_time = delta_time.end_time;

	delta_times[stats_counter++] = delta_time.elapsed_ms;
	if(stats_counter == BUFFERED_FRAME_STATS) stats_counter = 0;

	/*cPIXEndEvent_gpu(g_pd3dCommandQueue);*/
	return true;
}

__declspec(dllexport) void resize(HWND hWnd, int width, int height)
{
	ImGui_ImplDX12_InvalidateDeviceObjects();
	CleanupRenderTarget();
	ResizeSwapChain(hWnd, width, height);

	CreateRenderTarget();
	ImGui_ImplDX12_CreateDeviceObjects();
}
