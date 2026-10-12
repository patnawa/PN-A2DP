/* Tests the production descriptor/path policy without changing any ACL or service. */
#include "pnsecurepath.h"
#include <stdio.h>
static unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static bool Descriptor(const wchar_t *sddl, bool contents = true) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr)) return false;
    bool result = pn::securepath::ProtectedDescriptor(sd, contents); LocalFree(sd); return result;
}
int wmain() {
    CHECK(Descriptor(L"O:BAG:BAD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)"));
    CHECK(Descriptor(L"O:SYG:SYD:")); // empty ACL grants no ordinary-user write
    CHECK(!Descriptor(L"O:BAG:BAD:NO_ACCESS_CONTROL"));
    CHECK(!Descriptor(L"O:BUG:BUD:(A;;FA;;;SY)(A;;FA;;;BA)")); // owner can change DACL
    CHECK(!Descriptor(L"O:BAG:BAD:(A;;GW;;;BU)"));
    CHECK(!Descriptor(L"O:BAG:BAD:(A;;WD;;;AU)"));
    CHECK(!Descriptor(L"O:BAG:BAD:(A;;WO;;;AU)"));
    CHECK(!Descriptor(L"O:BAG:BAD:(A;;SD;;;BU)"));
    CHECK(!Descriptor(L"O:BAG:BAD:(A;;0x40;;;BU)", false)); // FILE_DELETE_CHILD on ancestor
    CHECK(!Descriptor(L"O:BAG:BAD:(A;;0x2;;;BU)")); // create/replace file in service directory
    CHECK(Descriptor(L"O:BAG:BAD:(A;;0x2;;;BU)", false)); // unrelated ancestor creation is harmless
    CHECK(Descriptor(L"O:BAG:BAD:(A;OICIIO;GA;;;CO)(A;;FA;;;BA)"));
    CHECK(!Descriptor(L"O:BAG:BAD:(D;;GW;;;BU)(A;;GA;;;BU)")); // conservative, not ACE-order dependent
    for (auto path : { L"C:\\", L"relative\\app.exe", L"\\\\server\\share\\app.exe", L"\\\\?\\C:\\app.exe",
        L"C:\\safe\\..\\app.exe", L"C:\\safe.\\app.exe", L"C:\\safe \\app.exe", L"C:\\app.exe:stream" }) {
        std::wstring canonical;
        CHECK(pn::securepath::Canonical(path, canonical) == ERROR_ACCESS_DENIED);
    }
    wchar_t self[MAX_PATH]{}; GetModuleFileNameW(nullptr, self, MAX_PATH);
    CHECK(pn::ValidateProtectedService(self) != ERROR_SUCCESS); // build in user's workspace
    wchar_t programFiles[MAX_PATH]{}; GetEnvironmentVariableW(L"ProgramFiles", programFiles, MAX_PATH);
    std::wstring installed = std::wstring(programFiles) + L"\\PN-A2DP\\pna2dp-codechost.exe";
    if (GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DWORD error = pn::ValidateProtectedService(installed);
        printf("Installed protected path: %lu\n", error); CHECK(error == ERROR_SUCCESS);
    } else printf("SKIP installed-path integration (PN-A2DP absent)\n");
    CHECK(pn::ValidateProtectedDirectory(std::wstring(programFiles) + L"\\PN-A2DP-absent-test\\child", true) == ERROR_SUCCESS);
    printf("Protected paths: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
