__declspec(dllexport) void cPIXBeginEvent(UINT64 color, const char* label);
__declspec(dllexport) void cPIXEndEvent();
__declspec(dllexport) UINT cPIX_COLOR(BYTE r, BYTE g, BYTE b); 
__declspec(dllexport) void cPIXNotifyWakeFromFenceSignal(HANDLE event);
__declspec(dllexport) void cPIXBeginEvent_gpu(ID3D12CommandQueue* commandList, UINT64 color, const char* label);
__declspec(dllexport) void cPIXEndEvent_gpu(ID3D12CommandQueue* commandList);
