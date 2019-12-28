#include "Windows.h"
#include "d3d12.h"
#include "pix3.h"

#pragma comment(lib, "WinPixEventRuntime")

extern "C"
{
	void cPIXBeginEvent(UINT64 color, const char* label)
	{
		PIXBeginEvent(color, label);
	}

	void cPIXEndEvent()
	{
		PIXEndEvent();
	}

	void cPIXBeginEvent_gpu(ID3D12CommandQueue* commandList, UINT64 color, const char* label)
	{
		PIXBeginEvent(commandList, color, label);
	}

	void cPIXEndEvent_gpu(ID3D12CommandQueue* commandList)
	{
		PIXEndEvent(commandList);
	}

	void cPIXNotifyWakeFromFenceSignal(HANDLE event)
	{
		PIXNotifyWakeFromFenceSignal(event);
	}

	UINT cPIX_COLOR(BYTE r, BYTE g, BYTE b) { return 0xff000000 | (r << 16) | (g << 8) | b; }
}
