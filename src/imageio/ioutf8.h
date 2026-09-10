//==================================================================================================
//  mt_fopen : fopen() for UTF-8 file names
//
//  On Windows the C runtime's fopen() converts the name to the *ANSI* code page, so a path with
//  Cyrillic / Chinese / accented characters silently fails to open -- while CreateFileW() (which
//  mtnbli uses for the compressed streams) has no such problem.  mt_fopen() is fopen() on every
//  other platform and _wfopen() with a UTF-8 -> UTF-16 conversion on Windows.
//==================================================================================================
#ifndef   __MT_FOPEN_H__
#define   __MT_FOPEN_H__

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

FILE *mt_fopen (const char *p_filename, const char *p_mode);

#ifdef __cplusplus
}
#endif

#endif // __MT_FOPEN_H__
