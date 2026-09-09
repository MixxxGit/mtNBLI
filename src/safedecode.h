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
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>

//-------------------------------------------------------------------------------------------------- signal guard
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

// sigsetjmp() has to sit in the frame that siglongjmp() returns to, so the guard is a macro
// pair and not a function :
//
//      FAULT_TRY                       FAULT_TRY
//          ... decode ...       or         ... decode ...
//      FAULT_CATCH                     FAULT_END
//          ... error handling ...
//      FAULT_END
//
#define  FAULT_TRY    if (sigsetjmp (FaultGuard::jmpBuf(), 1) == 0) { FaultGuard::arm();
#define  FAULT_CATCH  FaultGuard::disarm(); } else { FaultGuard::disarm();
#define  FAULT_END    }

__thread sigjmp_buf            FaultGuard::jmp_buf_;
__thread volatile sig_atomic_t FaultGuard::armed_ = 0;
struct sigaction FaultGuard::old_segv_;
struct sigaction FaultGuard::old_bus_;

//-------------------------------------------------------------------------------------------------- guarded input buffer
struct GuardedBuffer {
    uint8_t *p;          // start of the file data (2-byte aligned by construction)
    size_t   len;        // file size
    size_t   map_len;    // total mapped size (including the guard page)
};

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
        uint8_t *p = new uint8_t [len + (1<<16)];
        FILE *fp = fopen (p_fname, "rb");
        if (!fp) { delete[] p; return false; }
        size_t got = fread (p, 1, len, fp);
        fclose (fp);
        memset (p + got, 0, len + (1<<16) - got);
        gb.p = p; gb.len = len; gb.map_len = 0;
        return true;
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

#endif // __SAFE_DECODE_H__
