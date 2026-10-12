/* Real catalog verification smoke/regression test. SPDX-License-Identifier: MIT */
#include "../pnsigning.h"
#include <stdio.h>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 4) { fwprintf(stderr, L"Usage: signing_test <Microsoft catalog> <member INF> <member SYS>\n"); return 2; }
    for (int i = 2; i < 4; ++i) {
        auto result = pn::VerifyCatalogMember(argv[1], argv[i]);
        wprintf(L"%s: trusted=%d Microsoft=%d attestation=%d error=0x%08lX signer=%s\n",
            argv[i], result.trusted, result.microsoftDriver, result.attestation, result.error, result.signer.c_str());
        if (!result.trusted || !result.microsoftDriver) return 1;
    }
    auto absent = pn::VerifyCatalogMember(argv[1], L"Z:\\pna2dp-signature-test-does-not-exist.inf");
    if (absent.trusted || absent.microsoftDriver || !absent.error) return 1;
    puts("PASSED");
    return 0;
}
