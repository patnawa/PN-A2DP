/* Exercise the production service helpers against a fault-injected Windows API.
 * No real services are opened, stopped or deleted. SPDX-License-Identifier: MIT */
#include "pndrv.h"
#include "pnsecurepath.h"
#include <stdio.h>

namespace fake {
struct State {
    DWORD openError = 0, controlError = 0, queryError = 0, deleteError = 0;
    DWORD processOpenError = 0, waitResult = WAIT_OBJECT_0, clock = 0;
    DWORD state = SERVICE_RUNNING;
    DWORD settleAt = MAXDWORD;
    bool stuck = false;
    bool stoppingDuringControl = false;
    int controls = 0, deletes = 0, processOpens = 0, processWaits = 0, creates = 0;
} state;
BOOL Fail(DWORD error) { SetLastError(error); return FALSE; }
SC_HANDLE WINAPI OpenSCManager(LPCWSTR, LPCWSTR, DWORD) { return (SC_HANDLE)1; }
SC_HANDLE WINAPI OpenService(SC_HANDLE, LPCWSTR, DWORD) {
    if (state.openError) { Fail(state.openError); return nullptr; }
    return (SC_HANDLE)2;
}
BOOL WINAPI CloseServiceHandle(SC_HANDLE) { return TRUE; }
SC_HANDLE WINAPI CreateService(SC_HANDLE, LPCWSTR, LPCWSTR, DWORD, DWORD, DWORD, DWORD,
    LPCWSTR, LPCWSTR, LPDWORD, LPCWSTR, LPCWSTR, LPCWSTR) { ++state.creates; return (SC_HANDLE)3; }
BOOL WINAPI ChangeServiceConfig2(SC_HANDLE, DWORD, LPVOID) { return TRUE; }
BOOL WINAPI ControlService(SC_HANDLE, DWORD, LPSERVICE_STATUS) {
    ++state.controls;
    if (state.stoppingDuringControl) state.state = SERVICE_STOP_PENDING;
    if (state.state == SERVICE_STOP_PENDING) return Fail(ERROR_SERVICE_CANNOT_ACCEPT_CTRL);
    if (state.controlError) return Fail(state.controlError);
    if (!state.stuck) state.state = SERVICE_STOPPED;
    return TRUE;
}
BOOL WINAPI QueryServiceStatus(SC_HANDLE, LPSERVICE_STATUS status) {
    if (state.queryError) return Fail(state.queryError);
    status->dwCurrentState = state.state; return TRUE;
}
BOOL WINAPI QueryServiceStatusEx(SC_HANDLE, SC_STATUS_TYPE, LPBYTE data, DWORD, LPDWORD) {
    if (state.queryError) return Fail(state.queryError);
    auto status = (SERVICE_STATUS_PROCESS *)data;
    status->dwCurrentState = state.state; status->dwProcessId = 123;
    return TRUE;
}
BOOL WINAPI DeleteService(SC_HANDLE) {
    ++state.deletes;
    return state.deleteError ? Fail(state.deleteError) : TRUE;
}
HANDLE WINAPI OpenProcess(DWORD, BOOL, DWORD) {
    ++state.processOpens;
    if (state.processOpenError) { Fail(state.processOpenError); return nullptr; }
    return (HANDLE)3;
}
DWORD WINAPI WaitForSingleObject(HANDLE, DWORD) {
    ++state.processWaits;
    if (state.waitResult == WAIT_FAILED) SetLastError(ERROR_INVALID_HANDLE);
    return state.waitResult;
}
BOOL WINAPI CloseHandle(HANDLE) { return TRUE; }
DWORD WINAPI GetTickCount() { return state.clock; }
void WINAPI Sleep(DWORD ms) {
    state.clock += ms;
    if (state.clock >= state.settleAt) state.state = SERVICE_STOPPED;
}
}

// Compile the real implementation with only these OS boundaries substituted.
// Keep the substitutions in this test translation unit, out of shipped code.
#define OpenSCManagerW fake::OpenSCManager
#define OpenServiceW fake::OpenService
#define CreateServiceW fake::CreateService
#define ChangeServiceConfig2W fake::ChangeServiceConfig2
#define CloseServiceHandle fake::CloseServiceHandle
#define ControlService fake::ControlService
#define QueryServiceStatus fake::QueryServiceStatus
#define QueryServiceStatusEx fake::QueryServiceStatusEx
#define DeleteService fake::DeleteService
#define OpenProcess fake::OpenProcess
#define WaitForSingleObject fake::WaitForSingleObject
#define CloseHandle fake::CloseHandle
#define GetTickCount fake::GetTickCount
#define Sleep fake::Sleep
#include "../pndrv.cpp"

static int failures = 0, checks = 0;
#define CHECK(condition, text) do { ++checks; if (!(condition)) { ++failures; printf("FAIL: %s\n", text); } } while (0)
int main()
{
    fake::state = {};
    CHECK(pn::CreateHostService(pn::ModulePath()) == ERROR_ACCESS_DENIED && fake::state.creates == 0,
          "user-owned executable cannot be registered as LocalSystem service");
    fake::state = {};
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_SUCCESS && fake::state.deletes == 1, "stopped service is deleted");
    fake::state = {}; fake::state.controlError = ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_SERVICE_CANNOT_ACCEPT_CTRL && !fake::state.deletes, "stop refusal keeps service registration");
    fake::state = {}; fake::state.stuck = true;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_SERVICE_REQUEST_TIMEOUT && !fake::state.deletes, "stop timeout keeps service registration");
    fake::state = {}; fake::state.queryError = ERROR_ACCESS_DENIED;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_ACCESS_DENIED && !fake::state.deletes, "unknown stopped state cannot permit deletion");
    fake::state = {}; fake::state.state = SERVICE_STOPPED; fake::state.controlError = ERROR_SERVICE_NOT_ACTIVE;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_SUCCESS && fake::state.deletes == 1, "already stopped service can be deleted");
    fake::state = {}; fake::state.deleteError = ERROR_ACCESS_DENIED;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_ACCESS_DENIED, "delete failure propagated");
    fake::state = {}; fake::state.openError = ERROR_SERVICE_DOES_NOT_EXIST;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_SUCCESS && !fake::state.controls, "absent service needs no removal");
    fake::state = {}; fake::state.queryError = ERROR_ACCESS_DENIED;
    CHECK(pn::StopServiceAndWaitForExit(L"fixture", 100) == ERROR_ACCESS_DENIED && !fake::state.controls, "failed initial status cannot report successful shutdown");
    fake::state = {}; fake::state.processOpenError = ERROR_ACCESS_DENIED;
    CHECK(pn::StopServiceAndWaitForExit(L"fixture", 100) == ERROR_ACCESS_DENIED && !fake::state.controls, "inaccessible process cannot report successful shutdown");
    fake::state = {}; fake::state.waitResult = WAIT_FAILED;
    CHECK(pn::StopServiceAndWaitForExit(L"fixture", 100) == ERROR_INVALID_HANDLE, "process wait failure propagated");
    fake::state = {}; fake::state.waitResult = WAIT_TIMEOUT;
    CHECK(pn::StopServiceAndWaitForExit(L"fixture", 100) == ERROR_SERVICE_REQUEST_TIMEOUT, "process exit timeout propagated");
    fake::state = {};
    CHECK(pn::StopServiceAndWaitForExit(L"fixture", 100) == ERROR_SUCCESS && fake::state.processWaits == 1, "normal shutdown waits for process release");
    fake::state = {}; fake::state.state = SERVICE_STOPPED;
    CHECK(pn::StopServiceAndWaitForExit(L"fixture", 100) == ERROR_SUCCESS && !fake::state.processOpens, "stopped service PID is not valid and must not be opened");
    fake::state = {}; fake::state.state = SERVICE_STOP_PENDING; fake::state.settleAt = 200;
    DWORD stopResult = pn::StopServiceByName(L"fixture", 1000);
    CHECK(stopResult == ERROR_SUCCESS && !fake::state.controls && fake::state.clock >= 200, "Stop waits for an already-stopping service without sending STOP twice");
    printf("  pending Stop: result=%lu controls=%d elapsed=%lu\n", stopResult, fake::state.controls, fake::state.clock);
    fake::state = {}; fake::state.state = SERVICE_STOP_PENDING; fake::state.settleAt = 200;
    DWORD exitResult = pn::StopServiceAndWaitForExit(L"fixture", 1000);
    CHECK(exitResult == ERROR_SUCCESS && !fake::state.controls && !fake::state.processOpens && fake::state.clock >= 200, "Upgrade waits for pending stop without opening an unconfirmed PID");
    printf("  pending upgrade: result=%lu controls=%d processOpens=%d elapsed=%lu\n", exitResult, fake::state.controls, fake::state.processOpens, fake::state.clock);
    fake::state = {}; fake::state.state = SERVICE_STOP_PENDING; fake::state.settleAt = 200; fake::state.processOpenError = ERROR_ACCESS_DENIED;
    CHECK(pn::StopServiceAndWaitForExit(L"fixture", 1000) == ERROR_SUCCESS && !fake::state.processOpens, "Pending service must not depend on access to a possibly stale PID");
    fake::state = {}; fake::state.state = SERVICE_STOP_PENDING; fake::state.settleAt = 200;
    CHECK(pn::StopServiceByName(L"fixture", 10000) == ERROR_SUCCESS && fake::state.clock >= 200, "Longer timeout still waits for the pending transition");
    fake::state = {}; fake::state.state = SERVICE_STOP_PENDING; fake::state.settleAt = 200;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_SUCCESS && fake::state.deletes == 1 && !fake::state.controls && fake::state.clock >= 200, "Uninstall waits for pending stop before deleting service registration");
    fake::state = {}; fake::state.stoppingDuringControl = true; fake::state.settleAt = 200;
    CHECK(pn::StopServiceByName(L"fixture", 1000) == ERROR_SUCCESS && fake::state.controls == 1 && fake::state.clock >= 200, "Concurrent stop between query and request is observed and awaited");
    fake::state = {}; fake::state.state = SERVICE_STOP_PENDING;
    CHECK(pn::DeleteServiceByName(L"fixture") == ERROR_SERVICE_REQUEST_TIMEOUT && !fake::state.deletes && !fake::state.controls, "Stuck pending stop cannot permit deletion");
    printf("Service failure handling: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
