//==================================================================================================
//  mt_fopen : fopen() for UTF-8 file names  (see ioutf8.h)
//==================================================================================================
#include <stdio.h>
#include <stdlib.h>
#include "ioutf8.h"

#ifdef _WIN32
#  include <windows.h>
#  ifdef near
#    undef near
#  endif
#  ifdef far
#    undef far
#  endif
#endif

FILE *mt_fopen (const char *p_filename, const char *p_mode)
{
#ifdef _WIN32
    int n = MultiByteToWideChar (CP_UTF8, 0, p_filename, -1, NULL, 0);
    int m = MultiByteToWideChar (CP_UTF8, 0, p_mode,     -1, NULL, 0);
    if (n > 0 && m > 0) {
        wchar_t *wname = (wchar_t*) malloc (sizeof(wchar_t) * (size_t) n);
        wchar_t *wmode = (wchar_t*) malloc (sizeof(wchar_t) * (size_t) m);
        if (wname && wmode) {
            MultiByteToWideChar (CP_UTF8, 0, p_filename, -1, wname, n);
            MultiByteToWideChar (CP_UTF8, 0, p_mode,     -1, wmode, m);
            FILE *fp = _wfopen (wname, wmode);
            free (wname);
            free (wmode);
            return fp;
        }
        free (wname);
        free (wmode);
    }
    return NULL;
#else
    return fopen (p_filename, p_mode);
#endif
}
