// A BITS client for checking the qmgr shim: downloads a URL with AddFileWithRanges
// (or AddFile with no ranges) through whatever BITS the prefix has, and reports
// the time. Usage: bitsclient URL LOCAL [offset length]...
#define COBJMACROS
#include <initguid.h>
#include <windows.h>
#include <bits.h>
#include <bits1_5.h>
#include <bits2_0.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int wmain(int argc, wchar_t **argv)
{
    IBackgroundCopyManager *mgr;
    IBackgroundCopyJob *job;
    IBackgroundCopyJob3 *job3;
    GUID id;
    HRESULT hr;
    DWORD start = GetTickCount();
    BG_JOB_STATE state;
    BG_JOB_PROGRESS progress;

    if (argc < 3) return 2;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    hr = CoCreateInstance(&CLSID_BackgroundCopyManager, NULL, CLSCTX_LOCAL_SERVER, &IID_IBackgroundCopyManager, (void **)&mgr);
    if (FAILED(hr)) { printf("CoCreateInstance: %08lx\n", hr); return 1; }
    hr = IBackgroundCopyManager_CreateJob(mgr, L"test", BG_JOB_TYPE_DOWNLOAD, &id, &job);
    if (FAILED(hr)) { printf("CreateJob: %08lx\n", hr); return 1; }
    if (argc > 3) {
        BG_FILE_RANGE ranges[16];
        int n = 0, i;
        for (i = 3; i + 1 < argc && n < 16; i += 2, ++n) {
            ranges[n].InitialOffset = _wcstoui64(argv[i], NULL, 10);
            ranges[n].Length = _wcstoui64(argv[i + 1], NULL, 10);
        }
        hr = IBackgroundCopyJob_QueryInterface(job, &IID_IBackgroundCopyJob3, (void **)&job3);
        if (FAILED(hr)) { printf("QI job3: %08lx\n", hr); return 1; }
        hr = IBackgroundCopyJob3_AddFileWithRanges(job3, argv[1], argv[2], n, ranges);
    } else {
        hr = IBackgroundCopyJob_AddFile(job, argv[1], argv[2]);
    }
    if (FAILED(hr)) { printf("AddFile: %08lx\n", hr); return 1; }
    hr = IBackgroundCopyJob_Resume(job);
    if (FAILED(hr)) { printf("Resume: %08lx\n", hr); return 1; }
    for (;;) {
        IBackgroundCopyJob_GetState(job, &state);
        IBackgroundCopyJob_GetProgress(job, &progress);
        if (state == BG_JOB_STATE_TRANSFERRED || state == BG_JOB_STATE_ERROR) break;
        Sleep(200);
    }
    if (state == BG_JOB_STATE_ERROR) {
        IBackgroundCopyError *err; BG_ERROR_CONTEXT ctx; HRESULT code = 0;
        if (SUCCEEDED(IBackgroundCopyJob_GetError(job, &err))) { IBackgroundCopyError_GetError(err, &ctx, &code); }
        printf("job error %08lx\n", code);
        return 1;
    }
    hr = IBackgroundCopyJob_Complete(job);
    printf("done: %llu bytes in %lu ms, Complete=%08lx\n", (unsigned long long)progress.BytesTransferred, GetTickCount() - start, hr);
    return 0;
}
