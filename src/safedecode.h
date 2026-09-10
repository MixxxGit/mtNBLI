//==================================================================================================
//  "safe decode" helpers
//
//  The NBLI / fNBLI bit streams are self describing adaptive streams : a single flipped byte
//  can make the decoder compute a wild offset or run past the end of the buffer.  The upstream
//  implementations do not validate anything (they simply crash), and while we added bounds
//  checks to every place we could find, a hand written codec can never be proven complete.
//
//  Therefore a decoder runs under two extra safety nets :
//
//   1. the input file is mapped read-only with a PROT_NONE guard page right behind it, so any
//      sequential over-read stops immediately instead of wandering into unrelated memory.
//
//   2. SIGSEGV / SIGBUS are caught while a decode is running.  A fault is turned into a normal
//      "decode failed" return value of the *faulting thread* (per thread sigsetjmp), so damaged
//      files are rejected instead of taking the whole batch down.
//==================================================================================================
#ifndef __SAFE_DECODE_H__
#define __SAFE_DECODE_H__

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <setjmp.h>

#ifdef _WIN32
//--------------------------------------------------------------------------- Windows : SEH
//  MinGW-w64 does not implement MSVC's __try/__except, so the access violation is caught by a
//  vectored exception handler which longjmps back into the guarded decode.  The jump target is
//  thread local, so a fault only unwinds the thread that caused it -- same contract as the
//  POSIX version below.
#  define  WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <string>
#  undef   near                 // see mtnbli.cpp : windef.h defines these 16-bit leftovers
#  undef   far

class FaultGuard {
public:
    static __thread jmp_buf                 jmp_buf_;
    static __thread volatile long           armed_;
    static void *                           hHandler_;

    static LONG CALLBACK handler (PEXCEPTION_POINTERS p_ep) {
        DWORD c = p_ep->ExceptionRecord->ExceptionCode;
        if (armed_ && (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_IN_PAGE_ERROR ||
                       c == EXCEPTION_ARRAY_BOUNDS_EXCEEDED || c == EXCEPTION_DATATYPE_MISALIGNMENT)) {
            armed_ = 0;
            longjmp (jmp_buf_, 1);         // -> the FAULT_TRY of the *faulting* thread
        }
        return EXCEPTION_CONTINUE_SEARCH;  // not ours -> let Windows deal with it
    }

public:
    static void install () {
        if (!hHandler_) hHandler_ = AddVectoredExceptionHandler (1, handler);
    }

    static jmp_buf &jmpBuf ()  { return jmp_buf_; }
    static void     arm    ()  { armed_ = 1; }
    static void     disarm ()  { armed_ = 0; }
};

__thread jmp_buf         FaultGuard::jmp_buf_;
__thread volatile long   FaultGuard::armed_ = 0;
void *                   FaultGuard::hHandler_ = NULL;

#  define  FAULT_TRY    if (setjmp (FaultGuard::jmpBuf()) == 0) { FaultGuard::arm();
#  define  FAULT_CATCH  FaultGuard::disarm(); } else { FaultGuard::disarm();
#  define  FAULT_END    }

#else
//--------------------------------------------------------------------------- POSIX : signals
#  include <signal.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>

class FaultGuard {
public:
    static __thread sigjmp_buf          jmp_buf_;
    static __thread volatile sig_atomic_t armed_;
    static struct sigaction old_segv_, old_bus_;

    static void handler (int sig) {
        if (armed_) {
            armed_ = 0;
            siglongjmp (jmp_buf_, 1);      // -> the FAULT_TRY of the *faulting* thread
        }
        // the fault did not happen inside a guarded decode -> die like a normal program
        struct sigaction dfl;
        memset (&dfl, 0, sizeof(dfl));
        dfl.sa_handler = SIG_DFL;
        sigaction (sig, &dfl, NULL);
        raise (sig);
    }

public:
    static void install () {
        struct sigaction sa;
        memset (&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        sigemptyset (&sa.sa_mask);
        sigaction (SIGSEGV, &sa, &old_segv_);
        sigaction (SIGBUS , &sa, &old_bus_ );
    }

    static sigjmp_buf &jmpBuf ()  { return jmp_buf_; }
    static void        arm    ()  { armed_ = 1; }
    static void        disarm ()  { armed_ = 0; }
};

__thread sigjmp_buf            FaultGuard::jmp_buf_;
__thread volatile sig_atomic_t FaultGuard::armed_ = 0;
struct sigaction FaultGuard::old_segv_;
struct sigaction FaultGuard::old_bus_;

#  define  FAULT_TRY    if (sigsetjmp (FaultGuard::jmpBuf(), 1) == 0) { FaultGuard::arm();
#  define  FAULT_CATCH  FaultGuard::disarm(); } else { FaultGuard::disarm();
#  define  FAULT_END    }

#endif

// NOTE: the guard is a macro triad and not a function, because the (sig)setjmp() has to sit in
// the very frame that (sig)longjmp() returns to :
//
//      FAULT_TRY                       FAULT_TRY
//          ... decode ...       or         ... decode ...
//      FAULT_CATCH                     FAULT_END
//          ... error handling ...
//      FAULT_END
//
//-------------------------------------------------------------------------------------------------- guarded input buffer
struct GuardedBuffer {
    uint8_t *p;          // start of the file data (2-byte aligned by construction)
    size_t   len;        // file size
    size_t   map_len;    // total mapped size (including the guard page)
};

// fallback used when no guard page can be set up : a plain buffer with 64 kB of zero padding
inline static bool loadFilePadded (const char *p_fname, size_t len, GuardedBuffer &gb) {
    uint8_t *p = new uint8_t [len + (1<<16)];
    FILE *fp = fopen (p_fname, "rb");
    if (!fp) { delete[] p; return false; }
    size_t got = fread (p, 1, len, fp);
    fclose (fp);
    memset (p + got, 0, len + (1<<16) - got);
    gb.p = p; gb.len = len; gb.map_len = 0;
    return true;
}

#ifdef _WIN32

// Windows: reserve one extra page behind the data and simply never commit it.  Reserved pages
// fault on any access, which gives exactly the same stop-here behaviour as a PROT_NONE page.
inline static bool loadFileGuarded (const char *p_fname, GuardedBuffer &gb) {
    gb.p = NULL; gb.len = 0; gb.map_len = 0;

    // UTF-8 -> UTF-16 so that non-ASCII file names work
    int nw = MultiByteToWideChar (CP_UTF8, 0, p_fname, -1, NULL, 0);
    if (nw <= 0) return false;
    std::wstring w (nw, L'\0');
    MultiByteToWideChar (CP_UTF8, 0, p_fname, -1, &w[0], nw);
    while (!w.empty() && w.back() == L'\0') w.pop_back();

    HANDLE hf = CreateFileW (w.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER sz;
    if (!GetFileSizeEx (hf, &sz)) { CloseHandle (hf); return false; }
    size_t len = (size_t) sz.QuadPart;
    if (len < 16) { CloseHandle (hf); return false; }

    SYSTEM_INFO si; GetSystemInfo (&si);
    size_t page = si.dwPageSize;
    if (page < 4096) page = 4096;
    size_t data_pages = (len + page - 1) / page;
    size_t map_len    = (data_pages + 1) * page;      // + 1 guard page

    uint8_t *base = (uint8_t*) VirtualAlloc (NULL, map_len, MEM_RESERVE, PAGE_NOACCESS);
    uint8_t *data = NULL;
    if (base) data = (uint8_t*) VirtualAlloc (base, data_pages * page, MEM_COMMIT, PAGE_READWRITE);
    if (!data) {
        if (base) VirtualFree (base, 0, MEM_RELEASE);
        CloseHandle (hf);
        return loadFilePadded (p_fname, len, gb);
    }

    DWORD got = 0;
    BOOL  ok  = ReadFile (hf, data, (DWORD) len, &got, NULL);
    CloseHandle (hf);
    if (!ok || got != len) { VirtualFree (base, 0, MEM_RELEASE); return false; }

    gb.p = data; gb.len = len; gb.map_len = map_len;
    return true;
}

inline static void freeGuarded (GuardedBuffer &gb) {
    if (gb.p == NULL) return;
    if (gb.map_len) VirtualFree (gb.p, 0, MEM_RELEASE);
    else            delete[] gb.p;
    gb.p = NULL; gb.len = 0; gb.map_len = 0;
}

#else

inline static bool loadFileGuarded (const char *p_fname, GuardedBuffer &gb) {
    gb.p = NULL; gb.len = 0; gb.map_len = 0;

    int fd = open (p_fname, O_RDONLY);
    if (fd < 0) return false;

    struct stat st;
    if (fstat (fd, &st) != 0) { close (fd); return false; }
    size_t len = (size_t) st.st_size;
    if (len < 16) { close (fd); return false; }

    long page = sysconf (_SC_PAGESIZE);
    if (page < 4096) page = 4096;
    size_t data_pages = ((len + (size_t)page - 1) / (size_t)page);
    size_t map_len    = (data_pages + 1) * (size_t)page;          // + 1 guard page

    // MAP_PRIVATE : some decoders scribble into their input buffer, with MAP_PRIVATE those
    //               writes go to a private copy and never touch the file.
    void *m = mmap (NULL, map_len, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close (fd);
    if (m == MAP_FAILED) {
        // no mmap (or no guard page possible) -> fall back to a plain padded buffer
        return loadFilePadded (p_fname, len, gb);
    }

    // the page behind the data is inaccessible -> an over-read faults right there
    if (mprotect ((uint8_t*)m + data_pages*(size_t)page, (size_t)page, PROT_NONE) != 0) {
        munmap (m, map_len);
        return false;
    }

    gb.p = (uint8_t*) m; gb.len = len; gb.map_len = map_len;
    return true;
}

inline static void freeGuarded (GuardedBuffer &gb) {
    if (gb.p == NULL) return;
    if (gb.map_len) munmap (gb.p, gb.map_len);
    else            delete[] gb.p;
    gb.p = NULL; gb.len = 0; gb.map_len = 0;
}

#endif // !_WIN32

#endif // __SAFE_DECODE_H__
