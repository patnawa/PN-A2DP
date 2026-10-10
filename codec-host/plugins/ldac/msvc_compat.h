/* msvc_compat.h - force-included when compiling libldac with MSVC (GCC attribute syntax). */
#pragma once
#ifdef _MSC_VER
#define __attribute__(x)
#endif
