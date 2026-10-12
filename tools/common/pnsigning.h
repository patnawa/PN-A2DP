/* Driver package verification. SPDX-License-Identifier: MIT */
#pragma once
#include <windows.h>
#include <string>

namespace pn {
struct PackageTrust {
    DWORD error = ERROR_INVALID_DATA;
    bool trusted = false;
    bool microsoftDriver = false;
    bool attestation = false;
    std::wstring signer;
    std::wstring detail;
};

// Verifies the catalog signature and the actual SHA-256 member digest. No
// certificate import, catalog registration, boot-policy change or device access.
PackageTrust VerifyCatalogMember(const std::wstring &catalog, const std::wstring &member);
PackageTrust VerifyDriverPackage(const std::wstring &folder);
}
