#include "imgui_impl_dx12.c"
#include "imgui_impl_win32.c"
#include "common.h"
#include "math.h"

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

void failed_assert(const char* file, int line, const char* statement);

#define ASSERT(b) \
	if (!(b)) failed_assert(__FILE__, __LINE__, #b)

void failed_assert(const char* file, int line, const char* statement)
{
	static bool debug = true;

	if (debug)
	{
		wchar_t str[1024];
		wchar_t message[1024];
		wchar_t wfile[1024];
		mbstowcs(message, statement, 1024);
		mbstowcs(wfile, file, 1024);
		wsprintfW(str, L"Failed: (%s)\n\nFile: %s\nLine: %d\n\n", message, wfile, line);

		if (IsDebuggerPresent())
		{
			wcscat(str, L"Debug?");
			int res = MessageBoxW(NULL, str, L"Assert failed", MB_YESNOCANCEL | MB_ICONERROR);
			if (res == IDYES)
			{
				__debugbreak();
			}
			else if (res == IDCANCEL)
			{
				debug = false;
			}
		}
		else
		{
			wcscat(str, L"Display more asserts?");
			if (MessageBoxW(NULL, str, L"Assert failed", MB_YESNO | MB_ICONERROR | MB_DEFBUTTON2) != IDYES)
			{
				debug = false;
			}
		}
	}
}

struct FrameContext {
	ID3D12CommandAllocator* CommandAllocator;
	UINT64 FenceValue;
};

#define IDT_TIMER1 1
#define NUM_FRAMES_IN_FLIGHT 3
#define NUM_BACK_BUFFERS 3
static HWND* g_hwnd;
static UINT64 hwnd_width;
static UINT hwnd_height;
static struct FrameContext g_frameContext[NUM_FRAMES_IN_FLIGHT];
static UINT g_frameIndex = 0;
static ID3D12Device* g_pd3dDevice = NULL;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = NULL;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = NULL;
static ID3D12DescriptorHeap* dsv_heap = NULL;
static ID3D12CommandQueue* g_pd3dCommandQueue = NULL;
static ID3D12GraphicsCommandList* g_pd3dCommandList = NULL;
static ID3D12Fence* g_fence = NULL;
static ID3D12PipelineState* g_pso = NULL; 
static ID3D12RootSignature* g_rootsig = NULL;

static DXGI_FORMAT dsv_format = DXGI_FORMAT_D24_UNORM_S8_UINT;
static ID3D12Resource* dsv_resource;
static HANDLE g_fenceEvent = NULL;
static UINT64 g_fenceLastSignaledValue = 0;
static IDXGISwapChain3* g_pSwapChain = NULL;
static HANDLE g_hSwapChainWaitableObject = NULL;  // Signals when the DXGI Adapter finished presenting a new frame
static ID3D12Resource* g_mainRenderTargetResource[NUM_BACK_BUFFERS];
static D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS];

ID3DBlob* vs_blob = NULL;
ID3DBlob* ps_blob = NULL;

static UINT64* timestamp_buffer = NULL;
static ID3D12Resource* rb_buffer;
static ID3D12QueryHeap* query_heap;
static double frame_time = 0.0;
static UINT total_timer_count = 6;
static UINT ui_timer_count = 6;
UINT stats_counter = 0;
void frame_time_statistics(void);
void print_cache_info(CACHE_RELATIONSHIP cache);
void print_mask(KAFFINITY mask);
void get_processor_cache_info();
#define core_masks_count 2
#define cache_masks_count 8
int get_core_mask(KAFFINITY mask);
int* get_cache_masks(KAFFINITY mask);
void associate_caches_with_core();
char* get_cache_type_string(PROCESSOR_CACHE_TYPE type);

struct cache_info
{
	CACHE_RELATIONSHIP cache_rel;
	unsigned int masks[4];
};
struct cache_info cache_infos[26];

struct core_info
{
	unsigned int core_id;
	unsigned int mask;
	struct cache_info cache_infos[4];
};
struct core_info core_infos[8];

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
__declspec(dllexport) void create_dsv(UINT64 width, UINT height);
__declspec(dllexport) D3D12_CPU_DESCRIPTOR_HANDLE get_dsv_cpuhandle(void);
__declspec(dllexport) void cleanup(void);
__declspec(dllexport) void wndproc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
__declspec(dllexport) bool initialize(HWND* hwnd);

void WaitForLastSubmittedFrame(void);
struct FrameContext* WaitForNextFrameResources(void);
void create_query_objects(void);
void cpu_wait(UINT64 fence_value);

ID3D12Resource* create_committed_resource(const D3D12_HEAP_PROPERTIES* heap_props,
					  const D3D12_RESOURCE_DESC* resource_desc,
					  const D3D12_HEAP_FLAGS flags,
					  const D3D12_RESOURCE_STATES state,
					  const D3D12_CLEAR_VALUE* clear_value);

ID3D12PipelineState* create_aos_pso(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pso_desc);

// cube
void create_aos_cube(ID3D12GraphicsCommandList* cmd_list);

// defaults
#define set_default(val, def) (((val) == 0) ? (def) : (val))
D3D12_HEAP_PROPERTIES default_heap_props(const D3D12_HEAP_PROPERTIES* heap_props);
D3D12_RESOURCE_DESC default_resource_desc(D3D12_RESOURCE_DESC* resource_desc);
D3D12_GRAPHICS_PIPELINE_STATE_DESC default_pso_desc();
D3D12_BLEND_DESC default_blenddesc(D3D12_BLEND_DESC* blend_desc);
D3D12_RASTERIZER_DESC default_rasterizer_desc(D3D12_RASTERIZER_DESC* rasterizer_desc);
D3D12_DEPTH_STENCIL_DESC default_depthstencil_desc(D3D12_DEPTH_STENCIL_DESC* depthstencil_desc);

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

void print_cache_info(CACHE_RELATIONSHIP cache)
{
	printf("%s: cache.Associativity=%d\n", __func__, cache.Associativity);
	printf("%s: cache.CacheSize=%d\n", __func__, cache.CacheSize);
	printf("%s: cache.Level=%d\n", __func__, cache.Level);
	printf("%s: cache.LineSize=%d\n", __func__, cache.LineSize);
	printf("%s: cache.Type=%d\n", __func__, cache.Type);
	printf("\n");
}

void print_mask(KAFFINITY mask)
{
	printf(" [");
	for (int i = 0; i < sizeof(mask) * 8; i++) {
		KAFFINITY shifted_i = (KAFFINITY)1 << i;
		KAFFINITY masked = mask & shifted_i;
		if (masked) {
			printf(" %d", i);
		}
	}
	printf(" ]");
}

int get_core_mask(KAFFINITY mask)
{
	static int masks[core_masks_count] = {0};
	int size = 0;
	int result = 0;
	for (int i = 0; i < sizeof(mask) * 8; i++) {
		KAFFINITY shifted_i = (KAFFINITY)1 << i;
		KAFFINITY masked = mask & shifted_i;

		if (masked && size < core_masks_count) {
			masks[size] = i;
			size++;
		}
	}
	result = masks[0] + masks[1];

	return result;
}

int* get_cache_masks(KAFFINITY mask)
{
	int masks[cache_masks_count];
	memset(masks, 0,cache_masks_count * sizeof(int) );
	static int result[4];
	memset(result, 0, 4 * sizeof(int) );

	int size = 0;
	for (int i = 0; i < sizeof(mask) * 8; i++) {
		KAFFINITY shifted_i = (KAFFINITY)1 << i;
		KAFFINITY masked = mask & shifted_i;

		if (masked && size < cache_masks_count) {
			masks[size] = i;
			size++;
		}
	}

	int index = 0;
	for(int i = 0; i < _countof(masks); i+=2)
	{
		result[index++] = masks[i] + masks[i+1];
	}
	return result;
}

void associate_caches_with_core()
{
	for(int i = 0; i < _countof(core_infos);++i)
	{
		int core_cache_info_index = 0;
		for(int j = 0; j < _countof(cache_infos); ++j)
		{
			for(int k = 0; k < _countof(cache_infos[j].masks); ++k)
			{
				if(core_infos[i].mask == cache_infos[j].masks[k] )
				{
					core_infos[i].cache_infos[core_cache_info_index++] = cache_infos[j];
				}
			}
		}
	}
}

char* get_cache_type_string(PROCESSOR_CACHE_TYPE type)
{
	switch(type)
	{
		case CacheUnified:
			return "Unified cache";
		case CacheInstruction:
			return "Instruction cache";
		case CacheData:
			return "Data cache";
		case CacheTrace:
			return "Trace cache";
	}
}

void get_processor_cache_info()
{
	PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = NULL;
	char *buffer = NULL;
	char *p;
	DWORD len = 0;

	GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &len);

	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
		return -1;

	buffer = malloc(len);

	GetLogicalProcessorInformationEx(RelationAll, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buffer, &len);

	int core_index = 0;
	int cache_index = 0;

	for (p = buffer; p < buffer + len; p += info->Size) {
		info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)p;

		switch(info->Relationship)
		{
			case RelationProcessorCore:
				struct core_info core_info;
				/*core_info.mask = get_core_mask(info->Processor.GroupMask[0].Mask);*/
				int mask = get_core_mask(info->Processor.GroupMask[0].Mask);
				memcpy(&core_info.mask, &mask, sizeof(int));
				core_info.core_id = core_index;
				core_infos[core_index] = core_info;
				core_index++;

				print_mask(info->Processor.GroupMask[0].Mask);
				break;

			case RelationCache:
				print_cache_info(info->Cache);
				/*print_mask(info->Cache.GroupMask.Mask);*/
				struct cache_info cache_info;

				cache_info.cache_rel = info->Cache;
				memcpy(cache_info.masks,
				       get_cache_masks (info->Cache.GroupMask.Mask),
				       cache_masks_count * sizeof(int));

				cache_infos[cache_index] = cache_info;

				cache_index++;
				break;
		}
	}

}

__declspec(dllexport) bool initialize(HWND* hwnd)
{
	get_processor_cache_info();
	associate_caches_with_core();


	g_hwnd = hwnd;
	RECT rect;
	if (GetClientRect(*g_hwnd, &rect)) {
		hwnd_width = rect.right - rect.left;
		hwnd_height = rect.bottom - rect.top;
	}


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
		g_pd3dDevice->lpVtbl->CreateDescriptorHeap(
		    g_pd3dDevice,
		    &(D3D12_DESCRIPTOR_HEAP_DESC){.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV,
						  .NumDescriptors = 1,
						  .NodeMask = 1,
						  .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE},
		    &IID_ID3D12DescriptorHeap,
		    (void**)&dsv_heap);
		dsv_heap->lpVtbl->SetName(dsv_heap, L"main_dsv_desc_heap");
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
	g_pSwapChain->lpVtbl->GetDesc1(g_pSwapChain, &sd);
	create_dsv(sd.Width,sd.Height);
	create_query_objects();
	return true;
}

D3D12_CPU_DESCRIPTOR_HANDLE get_dsv_cpuhandle()
{
	D3D12_CPU_DESCRIPTOR_HANDLE dsvhandle;
	fixed_GetCPUDescriptorHandleForHeapStart fixed_get_dsv_cpuhandle = (fixed_GetCPUDescriptorHandleForHeapStart)dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart;
	fixed_get_dsv_cpuhandle(dsv_heap, &dsvhandle);
	return dsvhandle;
}

__declspec(dllexport) void create_dsv(UINT64 width, UINT height)
{

	D3D12_CLEAR_VALUE optimized_clear_value;
	optimized_clear_value.DepthStencil.Depth = 1.0f;
	optimized_clear_value.DepthStencil.Stencil = 0;
	optimized_clear_value.Format = dsv_format;

	dsv_resource = create_committed_resource(
	    &(D3D12_HEAP_PROPERTIES){.Type = D3D12_HEAP_TYPE_DEFAULT},
	    &(D3D12_RESOURCE_DESC){.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
				   .Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
				   .Format = dsv_format,
				   .Width = width,
				   .Height = height},
	    D3D12_HEAP_FLAG_NONE,
	    D3D12_RESOURCE_STATE_DEPTH_WRITE,
	    &optimized_clear_value);

	g_pd3dDevice->lpVtbl->CreateDepthStencilView(
	    g_pd3dDevice,
	    dsv_resource,
	    &(D3D12_DEPTH_STENCIL_VIEW_DESC){.Flags = D3D12_DSV_FLAG_NONE,
					     .Format = dsv_format,
					     .Texture2D.MipSlice = 0,
					     .ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D},
	    get_dsv_cpuhandle());
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

ID3D12Resource* create_committed_resource(const D3D12_HEAP_PROPERTIES* heap_props,
					  const D3D12_RESOURCE_DESC* resource_desc,
					  const D3D12_HEAP_FLAGS flags,
					  const D3D12_RESOURCE_STATES state,
					  const D3D12_CLEAR_VALUE* clear_value)
{
	ID3D12Resource* resource = NULL;
	D3D12_RESOURCE_DESC tmp_resource_desc = default_resource_desc(resource_desc);
	D3D12_HEAP_PROPERTIES tmp_heap_props = default_heap_props(heap_props);

	HRESULT hr = g_pd3dDevice->lpVtbl->CreateCommittedResource(g_pd3dDevice,
						      &tmp_heap_props,
						      flags,
						      &tmp_resource_desc,
						      state,
						      clear_value,
						      &IID_ID3D12Resource,
						      (void**)&resource);
	ASSERT(SUCCEEDED(hr));
	return resource;
}

ID3D12PipelineState* create_aos_pso(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pso_desc)
{
	ID3D12PipelineState* pso = NULL;
	D3D12_GRAPHICS_PIPELINE_STATE_DESC tmp_pso_desc = default_pso_desc(pso_desc);
	HRESULT hr = g_pd3dDevice->lpVtbl->CreateGraphicsPipelineState(g_pd3dDevice, &tmp_pso_desc, &IID_ID3D12PipelineState, (void**)&pso);
	ASSERT(SUCCEEDED(hr));
	return pso;
}

#define cube_vertices_count 3
#define cube_indices_count 36
#define no_offset 0
#define first_subresource 0
// TODO: try __m256?
struct position_color_simd
{
	__m128 position;
	__m128 color;
};

struct position_color
{
	float position[4];
	float color[4];
};

struct position_color cube_vertices[cube_vertices_count];

struct mesh
{
	ID3D12Resource* vertex_default_resource;
	ID3D12Resource* vertex_upload_resource;
	ID3D12Resource* indices_default_resource;
	ID3D12Resource* indices_upload_resource;
	D3D12_VERTEX_BUFFER_VIEW vbv;
	D3D12_INDEX_BUFFER_VIEW ibv;
};

struct mesh aos_cube;

void create_aos_cube(ID3D12GraphicsCommandList* cmd_list)
{

	// upload vertex buffer to the GPU
	/*struct position_color vertices[cube_vertices_count] = */
	/*{*/
		 /*{ .position ={ -1.0f , -1.f, -1.f, 1.f} ,.color = {0.0f , 0.0f, 0.0f, 1.f}} ,*/
		 /*{ .position ={ -1.f , 1.f , -1.f, 1.f} ,.color = {0.0f , 1.0f, 0.0f, 1.f}} ,*/
		 /*{ .position ={  1.f , 1.f , -1.f, 1.f} ,.color = {1.0f , 1.0f, 0.0f, 1.f}} ,*/
		 /*{ .position ={  1.f , -1.f, -1.f, 1.f} ,.color = {1.0f , 0.0f, 0.0f, 1.f}} ,*/
		 /*{ .position ={ -1.f , -1.f,  1.f, 1.f} ,.color = {0.0f , 0.0f, 1.0f, 1.f}} ,*/
		 /*{ .position ={ -1.f , 1.f ,  1.f, 1.f} ,.color = {0.0f , 1.0f, 1.0f, 1.f}} ,*/
		 /*{ .position ={  1.f , 1.f ,  1.f, 1.f} ,.color = {1.0f , 1.0f, 1.0f, 1.f}} ,*/
		 /*{ .position ={  1.f , -1.f,  1.f, 1.f} ,.color = {1.0f , 0.0f, 1.0f, 1.f}}*/
	/*};*/

	struct position_color vertices[cube_vertices_count] = 
	{
		 { .position ={ 0.0f , 0.25f, 0.0f, 1.0f} ,.color = {1.0f , 0.0f, 0.0f, 1.0f}} ,
		 { .position ={ 0.25f , -0.25f , 0.0f, 1.0f} ,.color = {0.0f , 1.0f, 0.0f, 1.0f}} ,
		 { .position ={  -0.25f , -0.25f , 0.0f, 1.0f} ,.color = {0.0f , 0.0f, 1.0f, 1.f}} ,
	};

	size_t cube_vertices_stride = sizeof(struct position_color);
	size_t vertex_buffer_byte_size = cube_vertices_stride * _countof(vertices);

	aos_cube.vertex_default_resource = create_committed_resource(&(D3D12_HEAP_PROPERTIES)
			                                           {
									.Type = D3D12_HEAP_TYPE_DEFAULT
							           },
							           &(D3D12_RESOURCE_DESC)
								   {
									.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								        .Width = vertex_buffer_byte_size,
									.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
								   },
                                                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
							           D3D12_RESOURCE_STATE_COPY_DEST,
								   NULL);
	aos_cube.vertex_default_resource->lpVtbl->SetName(aos_cube.vertex_default_resource, L"vertex_default_resource");

	aos_cube.vertex_upload_resource = create_committed_resource(&(D3D12_HEAP_PROPERTIES)
			                                           {
								        .Type = D3D12_HEAP_TYPE_UPLOAD
							           },
							           &(D3D12_RESOURCE_DESC)
								   {
									.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								        .Width = vertex_buffer_byte_size,
									.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
								   },
                                                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
							           D3D12_RESOURCE_STATE_GENERIC_READ,
								   NULL);
	aos_cube.vertex_upload_resource->lpVtbl->SetName(aos_cube.vertex_upload_resource, L"vertex_upload_resource");

	BYTE* mapped_vertex_data = NULL;
	aos_cube.vertex_upload_resource->lpVtbl->Map(aos_cube.vertex_upload_resource,
						 first_subresource,
						 &(D3D12_RANGE){.Begin = 0, .End = 0},
						 (void**)&mapped_vertex_data);

	memcpy((void*)mapped_vertex_data, 
	       (void*)vertices, 
	       vertex_buffer_byte_size);

	aos_cube.vertex_upload_resource->lpVtbl->Unmap(aos_cube.vertex_upload_resource,
						   first_subresource,
						   &(D3D12_RANGE){.Begin = 0, .End = 0});

	cmd_list->lpVtbl->CopyBufferRegion(cmd_list,
					   aos_cube.vertex_default_resource, no_offset,
					   aos_cube.vertex_upload_resource , no_offset,
					   vertex_buffer_byte_size);

	aos_cube.vbv.BufferLocation = aos_cube.vertex_default_resource->lpVtbl->GetGPUVirtualAddress(aos_cube.vertex_default_resource);
	aos_cube.vbv.SizeInBytes = vertex_buffer_byte_size;
	aos_cube.vbv.StrideInBytes = cube_vertices_stride;

	// index buffer
	unsigned short cube_indices[cube_indices_count] =	
	{
		0, 1, 2, 0, 2, 3,
		4, 6, 5, 4, 7, 6,
		4, 5, 1, 4, 1, 0,
		3, 2, 6, 3, 6, 7,
		1, 5, 6, 1, 6, 2,
		4, 0, 3, 4, 3, 7
	};

	size_t cube_indices_stride = sizeof(unsigned short);
	size_t index_buffer_byte_size = cube_indices_stride * _countof(cube_indices);

	aos_cube.indices_default_resource = create_committed_resource(&(D3D12_HEAP_PROPERTIES)
			                                           {
									.Type = D3D12_HEAP_TYPE_DEFAULT
							           },
							           &(D3D12_RESOURCE_DESC)
								   {
									.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								        .Width = index_buffer_byte_size,
									.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
								   },
                                                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
							           D3D12_RESOURCE_STATE_COPY_DEST,
								   NULL);
	aos_cube.indices_default_resource->lpVtbl->SetName(aos_cube.indices_default_resource, L"indices_default_resource");

	aos_cube.indices_upload_resource = create_committed_resource(&(D3D12_HEAP_PROPERTIES)
			                                           {
								        .Type = D3D12_HEAP_TYPE_UPLOAD
							           },
							           &(D3D12_RESOURCE_DESC)
								   {
									.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER,
								        .Width = index_buffer_byte_size,
									.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
								   },
                                                                   D3D12_DESCRIPTOR_HEAP_FLAG_NONE,
							           D3D12_RESOURCE_STATE_GENERIC_READ,
								   NULL);
	aos_cube.indices_upload_resource->lpVtbl->SetName(aos_cube.indices_upload_resource, L"indices_upload_resource");

	BYTE* mapped_index_data = NULL;
	aos_cube.indices_upload_resource->lpVtbl->Map(aos_cube.indices_upload_resource,
						 first_subresource,
						 &(D3D12_RANGE){.Begin = 0, .End = 0},
						 (void**)&mapped_index_data);

	memcpy((void*)mapped_index_data, (void*)cube_indices, sizeof(unsigned short) * cube_indices_count);

	aos_cube.indices_upload_resource->lpVtbl->Unmap(aos_cube.indices_upload_resource,
						   first_subresource,
						   &(D3D12_RANGE){.Begin = 0, .End = 0});

	cmd_list->lpVtbl->CopyBufferRegion(cmd_list,
					   aos_cube.indices_default_resource, no_offset,
					   aos_cube.indices_upload_resource , no_offset,
					   index_buffer_byte_size);


	aos_cube.ibv.BufferLocation = aos_cube.indices_default_resource->lpVtbl->GetGPUVirtualAddress(aos_cube.indices_default_resource);
	aos_cube.ibv.SizeInBytes = index_buffer_byte_size;
	aos_cube.ibv.Format = DXGI_FORMAT_R16_UINT;

	// resource transitions
	cmd_list->lpVtbl->ResourceBarrier(cmd_list, 1, &(D3D12_RESOURCE_BARRIER)
						   {
							.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, 
							.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE, 
							.Transition = 
							{
								.pResource = aos_cube.vertex_default_resource, 
								.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, 
								.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST, 
								.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ
							} 
						   });

	cmd_list->lpVtbl->ResourceBarrier(cmd_list, 1, &(D3D12_RESOURCE_BARRIER)
						   {
							.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, 
							.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE, 
							.Transition = 
							{
								.pResource = aos_cube.indices_default_resource, 
								.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, 
								.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST, 
								.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ
							} 
						   });

	// shaders compilation
	wchar_t* default_shader = L"C:\\Users\\maxim\\source\\repos\\cnewsetup\\source\\shaders\\default_shader.hlsl";

	HRESULT hr = NULL; 
	ID3DBlob* error_blob = NULL;

	hr = D3DCompileFromFile(default_shader,
				NULL,
				D3D_COMPILE_STANDARD_FILE_INCLUDE,
				"VS",
				"vs_5_1",
				D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG,
				0,
				(void**)&vs_blob,
				(void**)&error_blob);
	ASSERT(SUCCEEDED(hr));
	if(error_blob)
	{
		char* error_msg = (char*) error_blob->lpVtbl->GetBufferPointer(error_blob);
		OutputDebugString(error_msg);
	}

	hr = D3DCompileFromFile(default_shader,
				NULL,
				D3D_COMPILE_STANDARD_FILE_INCLUDE,
				"PS",
				"ps_5_1",
				D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG,
				0,
				(void**)&ps_blob,
				(void**)&error_blob);

	ASSERT(SUCCEEDED(hr));
	if(error_blob)
	{
		char* error_msg = (char*) error_blob->lpVtbl->GetBufferPointer(error_blob);
		OutputDebugString(error_msg);
	}


	ID3DBlob* rs_blob = NULL;

	D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_data = {};
	feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;
	HRESULT fs_gr = g_pd3dDevice->lpVtbl->CheckFeatureSupport(g_pd3dDevice, D3D12_FEATURE_ROOT_SIGNATURE, (void*)&feature_data, sizeof(feature_data) );

	hr = D3D12SerializeVersionedRootSignature(&(D3D12_VERSIONED_ROOT_SIGNATURE_DESC) {
				.Version = D3D_ROOT_SIGNATURE_VERSION_1_1, 
				.Desc_1_1 = (D3D12_ROOT_SIGNATURE_DESC1) {
					.NumParameters = 1,
					.pParameters = (D3D12_ROOT_PARAMETER1[1]){
						{
							.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV,
							.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX,
							.Descriptor = {
								.ShaderRegister = 0,
								.RegisterSpace = 0,
								.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE
							}
						}
					},
					.NumStaticSamplers = 0,
					.pStaticSamplers = NULL,
					.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT
				}
			}, 
			&rs_blob, &error_blob);

	ASSERT(SUCCEEDED(hr));
	if(error_blob)
	{
		char* error_msg = (char*) error_blob->lpVtbl->GetBufferPointer(error_blob);
		OutputDebugString(error_msg);
	}

	hr = g_pd3dDevice->lpVtbl->CreateRootSignature(g_pd3dDevice,
						  1,
						  rs_blob->lpVtbl->GetBufferPointer(rs_blob),
						  rs_blob->lpVtbl->GetBufferSize(rs_blob),
						  &IID_ID3D12RootSignature,
						  (void**)&g_rootsig);

	ASSERT(SUCCEEDED(hr));

	g_pso = create_aos_pso(&(D3D12_GRAPHICS_PIPELINE_STATE_DESC) {
				.pRootSignature = g_rootsig,
				.VS = {.pShaderBytecode = vs_blob->lpVtbl->GetBufferPointer(vs_blob), .BytecodeLength = vs_blob->lpVtbl->GetBufferSize(vs_blob)},
				.PS = {.pShaderBytecode = ps_blob->lpVtbl->GetBufferPointer(ps_blob), .BytecodeLength = ps_blob->lpVtbl->GetBufferSize(ps_blob)},
				.RasterizerState = {
					.FillMode = D3D12_FILL_MODE_SOLID, 
					.CullMode = D3D12_CULL_MODE_NONE
				},
				.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM,
				.InputLayout = {
					.NumElements = 2,
					.pInputElementDescs = (D3D12_INPUT_ELEMENT_DESC[2]) {
						{
							.SemanticName = "POSITION",
							.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
							.InputSlot = 0,
							.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
							.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
						},
						{
							.SemanticName = "COLOR",
							.Format = DXGI_FORMAT_R32G32B32A32_FLOAT,
							.InputSlot = 0,
							.AlignedByteOffset = D3D12_APPEND_ALIGNED_ELEMENT,
							.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA
						}
					}
				},
				.DSVFormat = dsv_format,
				.NumRenderTargets = NUM_BACK_BUFFERS,
				.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE
			});

}

D3D12_BLEND_DESC default_blenddesc(D3D12_BLEND_DESC* blend_desc)
{
	D3D12_BLEND_DESC default_blenddesc = *blend_desc;
	default_blenddesc.AlphaToCoverageEnable = set_default(default_blenddesc.AlphaToCoverageEnable, false);
	default_blenddesc.IndependentBlendEnable = set_default(default_blenddesc.IndependentBlendEnable, false);

	const D3D12_RENDER_TARGET_BLEND_DESC default_rendertarget_blenddesc =
	{
	    FALSE,FALSE,
	    D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
	    D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
	    D3D12_LOGIC_OP_NOOP,
	    D3D12_COLOR_WRITE_ENABLE_ALL,
	};

	for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
	    default_blenddesc.RenderTarget[ i ] = default_rendertarget_blenddesc;

	return default_blenddesc;
}

D3D12_RASTERIZER_DESC default_rasterizer_desc(D3D12_RASTERIZER_DESC* rasterizer_desc)
{
	D3D12_RASTERIZER_DESC default_rast_desc = *rasterizer_desc;
	default_rast_desc.AntialiasedLineEnable = set_default(default_rast_desc.AntialiasedLineEnable, false);
	default_rast_desc.MultisampleEnable = set_default(default_rast_desc.MultisampleEnable, false);
	default_rast_desc.DepthClipEnable = set_default(default_rast_desc.DepthClipEnable, true);
	default_rast_desc.FrontCounterClockwise = set_default(default_rast_desc.FrontCounterClockwise, false);
	default_rast_desc.ForcedSampleCount = set_default(default_rast_desc.ForcedSampleCount, 0);
	default_rast_desc.FillMode = set_default(default_rast_desc.FillMode, D3D12_FILL_MODE_SOLID);
	default_rast_desc.CullMode = set_default(default_rast_desc.CullMode, D3D12_CULL_MODE_BACK);
	default_rast_desc.DepthBias = set_default(default_rast_desc.DepthBias, D3D12_DEFAULT_DEPTH_BIAS);
	default_rast_desc.DepthBiasClamp = set_default(default_rast_desc.DepthBiasClamp, D3D12_DEFAULT_DEPTH_BIAS_CLAMP);
	default_rast_desc.SlopeScaledDepthBias = set_default(default_rast_desc.SlopeScaledDepthBias, D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS);
	default_rast_desc.ConservativeRaster = set_default(default_rast_desc.ConservativeRaster, D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF);
	return default_rast_desc;
}

D3D12_DEPTH_STENCIL_DESC default_depthstencil_desc(D3D12_DEPTH_STENCIL_DESC* depthstencil_desc)
{
	D3D12_DEPTH_STENCIL_DESC default_depthstencil_desc = *depthstencil_desc;
	default_depthstencil_desc.DepthEnable = set_default(default_depthstencil_desc.DepthEnable, false);
	default_depthstencil_desc.StencilEnable = set_default(default_depthstencil_desc.StencilEnable, false);
	default_depthstencil_desc.DepthWriteMask = set_default(default_depthstencil_desc.DepthWriteMask, D3D12_DEPTH_WRITE_MASK_ALL);
	default_depthstencil_desc.DepthFunc = set_default(default_depthstencil_desc.DepthFunc, D3D12_COMPARISON_FUNC_LESS);
	default_depthstencil_desc.StencilReadMask = set_default(default_depthstencil_desc.StencilReadMask, D3D12_DEFAULT_STENCIL_READ_MASK);
	default_depthstencil_desc.StencilWriteMask = set_default(default_depthstencil_desc.StencilWriteMask, D3D12_DEFAULT_STENCIL_WRITE_MASK);
	default_depthstencil_desc.FrontFace.StencilFailOp = set_default(default_depthstencil_desc.FrontFace.StencilFailOp, D3D12_STENCIL_OP_KEEP);
	default_depthstencil_desc.FrontFace.StencilDepthFailOp = set_default(default_depthstencil_desc.FrontFace.StencilDepthFailOp, D3D12_STENCIL_OP_KEEP);
	default_depthstencil_desc.FrontFace.StencilPassOp = set_default(default_depthstencil_desc.FrontFace.StencilPassOp, D3D12_STENCIL_OP_KEEP);
	default_depthstencil_desc.FrontFace.StencilFunc = set_default(default_depthstencil_desc.FrontFace.StencilFunc, D3D12_COMPARISON_FUNC_ALWAYS);
	default_depthstencil_desc.BackFace.StencilFailOp = set_default(default_depthstencil_desc.BackFace.StencilFailOp, D3D12_STENCIL_OP_KEEP);
	default_depthstencil_desc.BackFace.StencilDepthFailOp = set_default(default_depthstencil_desc.BackFace.StencilDepthFailOp, D3D12_STENCIL_OP_KEEP);
	default_depthstencil_desc.BackFace.StencilPassOp = set_default(default_depthstencil_desc.BackFace.StencilPassOp, D3D12_STENCIL_OP_KEEP);
	default_depthstencil_desc.BackFace.StencilFunc = set_default(default_depthstencil_desc.BackFace.StencilFunc, D3D12_COMPARISON_FUNC_ALWAYS);
	return default_depthstencil_desc;
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC default_pso_desc(D3D12_GRAPHICS_PIPELINE_STATE_DESC* pso_desc)
{
	D3D12_GRAPHICS_PIPELINE_STATE_DESC default_pso_desc = *pso_desc;
	default_pso_desc.BlendState = default_blenddesc(&default_pso_desc.BlendState);
	default_pso_desc.SampleMask = set_default(default_pso_desc.SampleMask, UINT_MAX);
	default_pso_desc.RasterizerState =  default_rasterizer_desc(&default_pso_desc.RasterizerState);
	default_pso_desc.DepthStencilState = default_depthstencil_desc(&default_pso_desc.DepthStencilState);
	default_pso_desc.IBStripCutValue = set_default(default_pso_desc.IBStripCutValue, D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED);
	default_pso_desc.PrimitiveTopologyType = set_default(default_pso_desc.PrimitiveTopologyType, D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) ;
	default_pso_desc.NumRenderTargets = set_default(default_pso_desc.NumRenderTargets, NUM_BACK_BUFFERS) ;
	default_pso_desc.RTVFormats[0] = set_default(default_pso_desc.RTVFormats[0], DXGI_FORMAT_UNKNOWN) ;
	default_pso_desc.DSVFormat = set_default(default_pso_desc.DSVFormat, DXGI_FORMAT_UNKNOWN) ;
	default_pso_desc.SampleDesc.Count = set_default(default_pso_desc.SampleDesc.Count, 1) ;
	default_pso_desc.SampleDesc.Quality = set_default(default_pso_desc.SampleDesc.Quality, 0) ;
	default_pso_desc.NodeMask = set_default(default_pso_desc.NodeMask, 1) ;
	default_pso_desc.CachedPSO.pCachedBlob = set_default(default_pso_desc.CachedPSO.pCachedBlob, NULL) ; 
	default_pso_desc.CachedPSO.CachedBlobSizeInBytes = set_default(default_pso_desc.CachedPSO.CachedBlobSizeInBytes, 0) ;
	default_pso_desc.Flags = set_default(default_pso_desc.Flags, D3D12_PIPELINE_STATE_FLAG_NONE) ;
	return default_pso_desc;
}

D3D12_RESOURCE_DESC default_resource_desc(D3D12_RESOURCE_DESC* resource_desc)
{
	D3D12_RESOURCE_DESC default_props = *resource_desc;
	default_props.Dimension = set_default(default_props.Dimension, 1);
	default_props.Alignment = set_default(default_props.Alignment, 0);
	default_props.Width = set_default(default_props.Width, 0);
	default_props.Height = set_default(default_props.Height, 1);
	default_props.DepthOrArraySize = set_default(default_props.DepthOrArraySize, 1);
	default_props.MipLevels = set_default(default_props.MipLevels, 1);
	default_props.Format = set_default(default_props.Format, DXGI_FORMAT_UNKNOWN);
	default_props.SampleDesc.Count = set_default(default_props.SampleDesc.Count, 1);
	default_props.SampleDesc.Quality = set_default(default_props.SampleDesc.Quality, 0);
	default_props.Layout = set_default(default_props.Layout, D3D12_TEXTURE_LAYOUT_UNKNOWN);
	default_props.Flags = set_default(default_props.Flags, D3D12_RESOURCE_FLAG_NONE);
	return default_props;
}

D3D12_HEAP_PROPERTIES default_heap_props(const D3D12_HEAP_PROPERTIES* heap_props)
{
	D3D12_HEAP_PROPERTIES default_props = *heap_props;
	default_props.Type = set_default(default_props.Type, D3D12_HEAP_TYPE_DEFAULT);
	default_props.CPUPageProperty = set_default(default_props.CPUPageProperty, D3D12_CPU_PAGE_PROPERTY_UNKNOWN);
	default_props.MemoryPoolPreference = set_default(default_props.MemoryPoolPreference, D3D12_MEMORY_POOL_UNKNOWN);
	default_props.CreationNodeMask = set_default(default_props.CreationNodeMask, 1);
	default_props.VisibleNodeMask = set_default(default_props.VisibleNodeMask, 1);
	return default_props;
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
	csafe_release(dsv_heap);
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
bool tester = true;
__declspec(dllexport) bool update_and_render()
{
	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(250, 0, 0), "update_and_render");*/

	/*ImGui_ImplDX12_NewFrame();*/
	/*ImGui_ImplWin32_NewFrame();*/
	/*igNewFrame();*/

	bool show_demo_window = true;
	bool show_another_window = false;
	ImVec4 clear_color;
	clear_color.x = 1.0f;
	clear_color.y = 0.0f;
	clear_color.z = 1.0f;
	clear_color.w = 0.0f;

	/*if (show_demo_window) igShowDemoWindow(&show_demo_window);*/

	/*{*/
		/*ImGuiContext* imguictx = igGetCurrentContext();*/
		/*igSetCurrentContext(imguictx);*/

		/*igCheckbox("Demo Window", &show_demo_window);*/
		/*igCheckbox("VSync", &is_vsync);*/
		/*igColorEdit3("clear color", (float*)&clear_color, 0);*/

		/*igText("imgui gpu time %.4f ms/frame",*/
		       /*frame_time);*/

		/*igText("elapsed time %.4f ms/frame",*/
		       /*delta_time.elapsed_ms);*/

		/*igText("average delta %.4f ms/frame",*/
		       /*delta_time_avg);*/

		/*igText("Application average %.4f ms/frame (%.1f FPS)",*/
		       /*(double)(1000.0f / igGetIO()->Framerate),*/
		       /*(double)igGetIO()->Framerate);*/

		/*if (igBeginTabBar("core_info_tabbar", 0))*/
		/*{*/
			/*for(int i = 0; i < _countof(core_infos); ++i)*/
			/*{*/
				/*char tabtitle[7] = "Core ";*/
				/*tabtitle[5] = '0' + core_infos[i].core_id;*/

				/*if (igBeginTabItem(tabtitle, false, 0))*/
				/*{*/
					/*igColumns(5, "core_info_columns",true);*/
					/*igSeparator();*/
					/*igText("Cache level"); igNextColumn();*/
					/*igText("Cache type"); igNextColumn();*/
					/*igText("Cache size"); igNextColumn();*/
					/*igText("Cache line size"); igNextColumn();*/
					/*igText("Cache associativity"); igNextColumn();*/
					/*igSeparator();*/

					/*for(int j = 0; j < _countof(core_infos[i].cache_infos); ++j)*/
					/*{*/
						/*igText("%d", core_infos[i].cache_infos[j].cache_rel.Level);igNextColumn();*/
						/*igText("%s", get_cache_type_string(core_infos[i].cache_infos[j].cache_rel.Type));igNextColumn();*/
						/*igText("%d", core_infos[i].cache_infos[j].cache_rel.CacheSize);igNextColumn();*/
						/*igText("%d", core_infos[i].cache_infos[j].cache_rel.LineSize);igNextColumn();*/
						/*igText("%d", core_infos[i].cache_infos[j].cache_rel.Associativity);igNextColumn();*/
					/*}*/
					/*igColumns(1,"",true);*/
					/*igSeparator();*/
					/*igEndTabItem();*/
				/*}*/
			/*}*/
			/*igEndTabBar();*/
		/*}*/
	/*}*/

	struct FrameContext* frameCtxt = WaitForNextFrameResources();

	UINT backBufferIdx = g_pSwapChain->lpVtbl->GetCurrentBackBufferIndex(g_pSwapChain);
	frameCtxt->CommandAllocator->lpVtbl->Reset(frameCtxt->CommandAllocator);
	g_pd3dCommandList->lpVtbl->Reset(g_pd3dCommandList, frameCtxt->CommandAllocator, NULL);

	if(tester)
	{
		create_aos_cube(g_pd3dCommandList);
		tester = false;
	}

	g_pd3dCommandList->lpVtbl->RSSetScissorRects(
	    g_pd3dCommandList,
	    1,
	    &(D3D12_RECT){.left = 0, .top = 0, .right = hwnd_width, .bottom = hwnd_height});

	g_pd3dCommandList->lpVtbl->RSSetViewports(g_pd3dCommandList,
						  1,
						  &(D3D12_VIEWPORT){.TopLeftX = 0,
								    .TopLeftY = 0,
								    .Width = hwnd_width,
								    .Height = hwnd_height,
								    .MaxDepth = 1.0f,
								    .MinDepth = 0.0f});

	//render aos cube
	g_pd3dCommandList->lpVtbl->IASetPrimitiveTopology(g_pd3dCommandList, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	g_pd3dCommandList->lpVtbl->SetGraphicsRootSignature(g_pd3dCommandList, g_rootsig);
	g_pd3dCommandList->lpVtbl->SetPipelineState(g_pd3dCommandList, g_pso);
	g_pd3dCommandList->lpVtbl->IASetVertexBuffers(g_pd3dCommandList, 0, 1, &aos_cube.vbv);
	g_pd3dCommandList->lpVtbl->IASetIndexBuffer(g_pd3dCommandList, &aos_cube.ibv);

	D3D12_RESOURCE_BARRIER barrier;
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
	barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

	g_pd3dCommandList->lpVtbl->ResourceBarrier(g_pd3dCommandList, 1, &barrier);
	g_pd3dCommandList->lpVtbl->ClearDepthStencilView(g_pd3dCommandList, get_dsv_cpuhandle(), D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.f, 0,0, NULL);
	g_pd3dCommandList->lpVtbl->ClearRenderTargetView(
	    g_pd3dCommandList,
	    g_mainRenderTargetDescriptor[backBufferIdx],
	    (float*)&clear_color,
	    0,
	    NULL);

	D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = get_dsv_cpuhandle();
	g_pd3dCommandList->lpVtbl->OMSetRenderTargets(g_pd3dCommandList,
						      1,
						      &g_mainRenderTargetDescriptor[backBufferIdx],
						      FALSE, 
						      &dsv_handle);
	g_pd3dCommandList->lpVtbl->SetDescriptorHeaps(g_pd3dCommandList, 1, &g_pd3dSrvDescHeap);
	g_pd3dCommandList->lpVtbl->DrawInstanced(g_pd3dCommandList, 3, 1, 0, 0);

	UINT buffer_start = backBufferIdx * 2;
	UINT buffer_end = (backBufferIdx * 2 + 1);

	g_pd3dCommandList->lpVtbl->EndQuery(g_pd3dCommandList,
					    query_heap,
					    D3D12_QUERY_TYPE_TIMESTAMP,
					    buffer_start);


	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(0, 0, 255), "igRender");*/

	/*igRender();  // render ui*/

	/*cPIXEndEvent_gpu(g_pd3dCommandQueue);*/
	/*cPIXBeginEvent_gpu(g_pd3dCommandQueue, cPIX_COLOR(255, 0, 255), "RenderDrawData");*/
	/*ImGui_ImplDX12_RenderDrawData(igGetDrawData(), g_pd3dCommandList);*/
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
	create_dsv(width,height);
	CreateRenderTarget();
	ImGui_ImplDX12_CreateDeviceObjects();
}
