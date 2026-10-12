/* Driver package verification. SPDX-License-Identifier: MIT */
#include "pnsigning.h"
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <mscat.h>
#include <vector>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace pn {
static bool HasUsage(PCCERT_CONTEXT cert, const char *oid)
{
    DWORD size = 0;
    if (!CertGetEnhancedKeyUsage(cert, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, NULL, &size)) return false;
    std::vector<BYTE> storage(size);
    auto usage = reinterpret_cast<PCERT_ENHKEY_USAGE>(storage.data());
    if (!CertGetEnhancedKeyUsage(cert, CERT_FIND_EXT_ONLY_ENHKEY_USAGE_FLAG, usage, &size)) return false;
    for (DWORD i = 0; i < usage->cUsageIdentifier; ++i)
        if (!strcmp(usage->rgpszUsageIdentifier[i], oid)) return true;
    return false;
}

PackageTrust VerifyCatalogMember(const std::wstring &catalog, const std::wstring &member)
{
    PackageTrust result;
    HANDLE file = CreateFileW(member.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { result.error = GetLastError(); result.detail = L"Missing or unreadable package file: " + member; return result; }
    HCATADMIN admin = NULL;
    GUID driverAction = DRIVER_ACTION_VERIFY;
    if (!CryptCATAdminAcquireContext2(&admin, &driverAction, L"SHA256", NULL, 0)) {
        result.error = GetLastError(); CloseHandle(file); result.detail = L"Windows could not initialize catalog verification."; return result;
    }
    DWORD size = 0;
    bool ok = CryptCATAdminCalcHashFromFileHandle2(admin, file, &size, NULL, 0) != FALSE;
    std::vector<BYTE> hash(size);
    if (ok) ok = CryptCATAdminCalcHashFromFileHandle2(admin, file, &size, hash.data(), 0) != FALSE;
    if (!ok) {
        result.error = GetLastError(); CryptCATAdminReleaseContext(admin, 0); CloseHandle(file);
        result.detail = L"Windows could not hash the package file: " + member; return result;
    }
    std::wstring tag;
    const wchar_t digits[] = L"0123456789ABCDEF";
    for (BYTE byte : hash) { tag += digits[byte >> 4]; tag += digits[byte & 15]; }
    WINTRUST_CATALOG_INFO cat = {sizeof(cat)};
    cat.pcwszCatalogFilePath = catalog.c_str(); cat.pcwszMemberTag = tag.c_str();
    cat.pcwszMemberFilePath = member.c_str(); cat.hMemberFile = file;
    cat.pbCalculatedFileHash = hash.data(); cat.cbCalculatedFileHash = size; cat.hCatAdmin = admin;
    WINTRUST_DATA trust = {sizeof(trust)};
    trust.dwUIChoice = WTD_UI_NONE; trust.dwUnionChoice = WTD_CHOICE_CATALOG; trust.pCatalog = &cat;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    result.error = static_cast<DWORD>(WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust));
    result.trusted = result.error == ERROR_SUCCESS;
    if (result.trusted) {
        auto provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        if (signer && signer->csCertChain && signer->pasCertChain && signer->pChainContext) {
            auto cert = signer->pasCertChain[0].pCert;
            wchar_t name[512] = {};
            CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, _countof(name));
            result.signer = name;
            CERT_CHAIN_POLICY_PARA params = {sizeof(params)};
            // Use Windows' root-key policy, not a display-name/issuer substring or
            // a certificate pinned to one generation of Microsoft's signing CAs.
            params.dwFlags = MICROSOFT_ROOT_CERT_CHAIN_POLICY_DISABLE_FLIGHT_ROOT_FLAG;
            CERT_CHAIN_POLICY_STATUS status = {sizeof(status)};
            bool microsoftRoot = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_MICROSOFT_ROOT,
                signer->pChainContext, &params, &status) && status.dwError == ERROR_SUCCESS;
            result.attestation = HasUsage(cert, szOID_ATTEST_WHQL_CRYPTO);
            result.microsoftDriver = microsoftRoot && (result.attestation || HasUsage(cert, szOID_WHQL_CRYPTO) || HasUsage(cert, szOID_NT5_CRYPTO));
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(reinterpret_cast<HWND>(INVALID_HANDLE_VALUE), &action, &trust);
    CryptCATAdminReleaseContext(admin, 0); CloseHandle(file);
    result.detail = result.trusted ? (result.microsoftDriver ? L"Microsoft driver signature and catalog membership verified." :
        L"Valid catalog signature, but it is not a Microsoft release driver signature.") :
        L"Catalog signature or file membership verification failed: " + member;
    return result;
}

PackageTrust VerifyDriverPackage(const std::wstring &folder)
{
    const std::wstring prefix = folder + L"\\";
    auto inf = VerifyCatalogMember(prefix + L"pna2dp.cat", prefix + L"pna2dp.inf");
    if (!inf.trusted) return inf;
    auto sys = VerifyCatalogMember(prefix + L"pna2dp.cat", prefix + L"pna2dp.sys");
    if (!sys.trusted) return sys;
    sys.microsoftDriver = sys.microsoftDriver && inf.microsoftDriver;
    return sys;
}
}
