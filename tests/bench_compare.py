#!/usr/bin/env python3
#===================================================================================================
#  bench_compare.py -- mtnbli against the upstream (author's) NBLI / fNBLI codecs
#
#  What it does, for every image of the input folder :
#
#      PNG --encode-->  .fnbli / .nbli / .tnbli  --decode-->  PNG
#
#  with five codecs (two of them the author's, three of them ours) :
#
#      fNBLI   (upstream)  fNBLI(.exe)                 -> .fnbli
#      NBLI    (upstream)  NBLI(.exe)                  -> .nbli
#      fNBLI   (mtnbli)    mtnbli(.exe) -M F           -> .fnbli
#      NBLI    (mtnbli)    mtnbli(.exe) -M N           -> .nbli
#      mtNBLI  (mtnbli)    mtnbli(.exe) -M MT          -> .tnbli   (tiled, all cores)
#
#  Everything is measured twice :
#
#      BATCH  -- the whole folder in one command line (that is how you use it for real)
#      SINGLE -- one command per file, times summed over the files (this is where the process
#                start up of every single invocation shows up)
#
#  and the report ends with an analytical summary that the script computes itself : the speed
#  ups against the author's codecs, the size the tiled container costs, how many cores the
#  parallel codec really managed to use, and whether every round trip was bit exact.
#
#  usage :
#      python3 bench_compare.py -c "C:\progz\NBLI" -i "C:\sources\yt-rnd-8K"
#      python3 bench_compare.py -c ../NBLI -i ./shots -m single -r 1
#      python3 bench_compare.py -c ../NBLI -i ./shots -o ./report --keep --t1 --nbli-opts "-g"
#
#  The binaries may be native Linux builds or .exe files -- on Linux an .exe is started through
#  wine, so the Windows binaries can be compared on this side too.
#
#  Nothing is written next to the originals : every intermediate file lives in a work directory
#  which is deleted at the end (--keep leaves it for inspection).  The report, a CSV with every
#  single measurement and a JSON with the aggregates are written to --out (default ./bench_report).
#
#  Three things this script is careful about :
#
#    * it tells you what it is doing.  Every step is "[stage 7/45] ..." with a progress bar for
#      the work inside it, so a run that takes half an hour never looks like it hung.
#    * it measures the CPU time of the child process on POSIX *and* on Windows.  os.times()
#      has no children_user/children_system on Windows and there is no `resource` module there,
#      so on Windows the child's times are read with GetProcessTimes() from a handle the script
#      owns itself (the handle subprocess gives us is closed the moment the child is reaped).
#    * it does not waste disk.  Every intermediate is deleted the moment it has been consumed,
#      and the "is it bit exact" check compares 128 bit BLAKE2b hashes of the raw pixels instead
#      of keeping the decoded images around for a byte by byte comparison.
#===================================================================================================
import sys, os, csv, json, time, struct, hashlib, platform, argparse, shutil
import subprocess, tempfile, threading

sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))

LINE  = '=' * 100
DLINE = '-' * 100

# work directories that must go away again, whatever happens (see the finally in __main__)
_WORK_DIRS = []

# psutil is optional : it is only a second opinion for the CPU time / RSS of the children
try:
    import psutil as _psutil
except Exception:
    _psutil = None

#===================================================================================================
#  a 128 bit hash of a file : BLAKE2b with a 16 byte digest
#
#  BLAKE2 lives in hashlib (nothing to install), it is C code so it runs at roughly 1 GB/s in
#  CPython, and digest_size = 16 is its native width for a 128 bit result.  Two different images
#  colliding by accident is a 2^-128 event -- every real difference is found, and we also keep
#  the byte count so a truncated or padded file cannot pass either.
#===================================================================================================
HASH_NAME = 'blake2b-128'

def file_hash128 (path, chunk = 1 << 22):
    """-> (hex digest, file size) ; 16 bytes of BLAKE2b, read in 4 MiB blocks"""
    h = hashlib.blake2b (digest_size = 16)
    n = 0
    with open (path, 'rb') as f:
        while True:
            b = f.read (chunk)
            if not b: break
            h.update (b); n += len (b)
    return h.hexdigest(), n

#===================================================================================================
#  small helpers
#===================================================================================================
class Log:
    """everything printed here also goes into report.txt -- except the progress bar"""
    def __init__ (self, path = None):
        self.f = open (path, 'w', encoding = 'utf-8') if path else None
        try:    self.tty   = sys.stdout.isatty()
        except Exception: self.tty = False
        try:    self.width = shutil.get_terminal_size ((100, 24)).columns
        except Exception: self.width = 100
        self.width = max (40, min (self.width, 120))
        self._last = ''; self._t = 0.0
    def __call__ (self, s = ''):
        print (s, flush = True)
        if self.f: self.f.write (s + '\n'); self.f.flush()
    def live (self, s):
        """the line that is overwritten by the next one : the progress bar"""
        s = s [:self.width - 1]
        if self.tty:
            sys.stdout.write ('\r' + s.ljust (self.width - 1)); sys.stdout.flush()
        else:
            #  redirected into a file : a '\r' would only make a mess and a line per tick would
            #  drown the report, so write at most one line a second
            now = time.time()
            if s != self._last and (now - self._t) >= 1.0:
                print (s, flush = True); self._last = s; self._t = now
    def live_end (self):
        if self.tty:
            sys.stdout.write ('\r' + ' ' * (self.width - 1) + '\r'); sys.stdout.flush()
        self._last = ''
    def close (self):
        if self.f: self.f.close(); self.f = None

def num (n, unit = ''):
    try:    return '{:,}'.format (int (n)).replace (',', ' ') + unit
    except: return 'n/a'

def pct (x, d = 1):  return 'n/a' if x is None else '%.*f%%' % (d, x)
def rel (x, d = 2):  return 'n/a' if x is None else '%.*fx' % (d, x)
def secs (x, d = 3): return 'n/a' if x is None else '%.*f' % (d, x)
def mbs (x):         return 'n/a' if x is None else '%.1f' % x

def cpu_pair (a, b):
    """a + b of two measurements that may be missing -> None instead of a wrong number"""
    if a is None or b is None: return None
    return a + b

def table (log, header, rows, aligns = None, indent = ' '):
    n = len (header); w = [len (h) for h in header]
    for r in rows:
        for i in range (min (n, len (r))): w[i] = max (w[i], len (r[i]))
    al = aligns or 'l' * n
    def line (cells, fill = ' '):
        out = []
        for i, c in enumerate (cells):
            out.append (c.rjust (w[i]) if (i < len (al) and al[i] == 'r') else c.ljust (w[i]))
        return (indent + fill.join (out)).rstrip()
    log (line (header)); log (line (['-' * x for x in w], '-'))
    for r in rows: log (line (r))

def sha256_of (path):
    h = hashlib.sha256()
    with open (path, 'rb') as f:
        for chunk in iter (lambda: f.read (1 << 20), b''): h.update (chunk)
    return h.hexdigest()

#===================================================================================================
#  stage counter + progress bar : a run over a folder of 8K frames takes minutes per phase,
#  and silence looks exactly like a hang
#===================================================================================================
SPIN = '|/-\\'

class Progress:
    def __init__ (self, log, total, enabled = True):
        self.log, self.total, self.on = log, max (1, total), enabled
        self.n = 0; self.title = ''; self.steps = 0; self.unit = ''; self.done = 0
        self.t0 = time.time(); self._i = 0
    def tag (self):
        return 'stage %d/%d' % (min (self.n, self.total), self.total)
    def skip (self, k = 1):
        """a stage will not happen (the codec failed) -- keep the numbering honest"""
        self.total = max (self.n, self.total - k)
    def begin (self, title, steps = 0, unit = ''):
        self.n += 1
        self.title, self.steps, self.unit, self.done = title, steps, unit, 0
        self.t0 = time.time(); self._i = 0
        self.log ('   [%s] %s' % (self.tag(), title))
        self.draw()
    def tick (self, k = None, note = ''):
        self.done = (self.done + 1) if k is None else k
        if note: self.title = note
        self.draw()
    def draw (self):
        if not self.on: return
        el = time.time() - self.t0
        if self.steps > 1:
            f   = min (1.0, self.done / float (self.steps))
            n   = 30; fill = int (round (n * f))
            bar = '#' * fill + '.' * (n - fill)
            line = '     [%s] %3d%%  %d/%d %s  %.1f s' % (bar, int (100 * f), self.done,
                                                          self.steps, self.unit, el)
        else:
            self._i += 1
            line = '     %s %s ... %.1f s' % (SPIN [self._i % 4], self.title, el)
        self.log.live (line)
    def end (self, extra = ''):
        if not self.on: return
        el = time.time() - self.t0
        self.log.live_end()
        if el >= 0.5:
            self.log ('     done in %.1f s%s' % (el, (' : ' + extra) if extra else ''))
    def pulse_start (self, delay = 0.2):
        """a heartbeat for a stage whose only step is one long command"""
        if not self.on: return
        self._stop = threading.Event()
        def loop (stop = self._stop):
            while not stop.is_set():
                self.draw(); time.sleep (delay)
        t = threading.Thread (target = loop); t.daemon = True; t.start()
        self._beat = t
    def pulse_stop (self):
        if getattr (self, '_stop', None) is not None:
            self._stop.set()
            if getattr (self, '_beat', None) is not None: self._beat.join (0.5)
            self._stop = self._beat = None

#===================================================================================================
#  the machine
#===================================================================================================
def cpu_name():
    if os.path.exists ('/proc/cpuinfo'):
        for line in open ('/proc/cpuinfo', encoding = 'utf-8', errors = 'replace'):
            if 'model name' in line: return line.split (':', 1)[1].strip()
    if sys.platform == 'win32':
        try:
            import winreg
            k = winreg.OpenKey (winreg.HKEY_LOCAL_MACHINE,
                                r'HARDWARE\DESCRIPTION\System\CentralProcessor\0')
            return winreg.QueryValueEx (k, 'ProcessorNameString')[0].strip()
        except Exception: pass
    return platform.processor() or platform.machine()

def has_avx2():
    """the author's fNBLI only takes its fast (16 lane) path when the CPU has AVX2"""
    try:
        return ' avx2 ' in open ('/proc/cpuinfo').read()
    except Exception: pass
    try:
        return 'avx2' in (platform.processor() or '').lower()
    except Exception: pass
    return False

def cpu_quota():
    """how many CPUs this process may really use -- nproc lies inside a container"""
    try:
        q, p = open ('/sys/fs/cgroup/cpu.max').read().split()
        if q != 'max': return float (q) / float (p)
    except Exception: pass
    try:
        q = int (open ('/sys/fs/cgroup/cpu/cpu.cfs_quota_us').read())
        p = int (open ('/sys/fs/cgroup/cpu/cpu.cfs_period_us').read())
        if q > 0: return float (q) / p
    except Exception: pass
    return None

#===================================================================================================
#  binaries
#===================================================================================================
def is_pe (path):
    try:    return open (path, 'rb').read (2) == b'MZ'
    except: return False

class Bin:
    def __init__ (self, path, wine):
        self.path, self.wine = path, wine
    def cmd (self, args):
        return (self.wine + [self.path] if self.wine else [self.path]) + list (args)
    @property
    def tag (self):
        return os.path.basename (self.path) + ('  [wine]' if self.wine else '')

def find_bin (directory, stem, override):
    if override:
        if not os.path.isfile (override):
            raise SystemExit ('*** %s : no such file' % override)
        return override
    if not os.path.isdir (directory):
        raise SystemExit ('*** %s : no such directory' % directory)
    want, hits = stem.lower(), []
    for name in sorted (os.listdir (directory)):
        base, ext = os.path.splitext (name)
        if base.lower() != want or ext.lower() not in ('', '.exe'): continue
        full = os.path.join (directory, name)
        if os.path.isfile (full): hits.append (full)
    if not hits: return None
    if sys.platform != 'win32':                       # prefer the native build over the .exe
        for h in hits:
            if not is_pe (h): return h
    return hits[0]

#===================================================================================================
#  CPU time and peak memory of a child process -- POSIX *and* Windows
#
#  POSIX   : os.times() accounts for every child that has been waited for, so the delta around a
#            run is exact, even when the child is wine with a tree of its own underneath.
#  Windows : os.times() simply has no children_user / children_system (always 0) and the
#            `resource` module does not exist, so the usual pair of answers is dead.  What works
#            is GetProcessTimes() -- but the handle has to be ours and it has to be taken while
#            the child is still alive, because subprocess closes its handle as soon as it reaps
#            the child and OpenProcess() on a dead pid fails.  We therefore duplicate the handle
#            right after the spawn (or open our own) and read it after the run.
#            FILETIME counts 100 ns units.
#===================================================================================================
def vmhwm_kb (pid):
    try:
        with open ('/proc/%d/status' % pid, encoding = 'utf-8', errors = 'replace') as f:
            for line in f:
                if line.startswith ('VmHWM:'): return int (line.split()[1])
    except Exception: pass
    return 0

def children_of (pid):
    kids = set()
    try:
        for tid in os.listdir ('/proc/%d/task' % pid):
            try:
                kids |= {int (x) for x in open ('/proc/%d/task/%s/children' % (pid, tid)).read().split()}
            except Exception: pass
    except Exception: pass
    return kids

_WIN_API = {}

def win_api():
    """ctypes bindings for the two calls we need, prepared once"""
    if os.name != 'nt': return None
    if 'api' in _WIN_API: return _WIN_API['api']
    api = None
    try:
        import ctypes
        k = ctypes.windll.kernel32
        #  a HANDLE is pointer sized -- without restype a 64 bit handle is truncated to 32 bit
        k.GetCurrentProcess.restype  = ctypes.c_void_p
        k.GetCurrentProcess.argtypes = []
        k.DuplicateHandle.restype    = ctypes.c_int32
        k.DuplicateHandle.argtypes   = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p,
                                        ctypes.POINTER (ctypes.c_void_p), ctypes.c_uint32,
                                        ctypes.c_int, ctypes.c_uint32]
        k.OpenProcess.restype        = ctypes.c_void_p
        k.OpenProcess.argtypes       = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
        api = (ctypes, k)
    except Exception:
        api = None
    _WIN_API['api'] = api
    return api

class WinHandle:
    """our own handle on the child, so its times stay readable after it has been reaped"""
    def __init__ (self, popen):
        self.h, self.api = None, win_api()
        if not self.api: return
        ct, k = self.api
        try:                                        # 1) duplicate what subprocess already has
            src = int (getattr (popen, '_handle', 0) or 0)
            if src:
                dup = ct.c_void_p()
                if k.DuplicateHandle (k.GetCurrentProcess(), ct.c_void_p (src),
                                      k.GetCurrentProcess(), ct.byref (dup),
                                      0, True, 2):       # 2 = DUPLICATE_SAME_ACCESS
                    self.h = dup
        except Exception:
            self.h = None
        if self.h is None:                          # 2) or open our own, while it is still alive
            try:
                h = k.OpenProcess (0x1000, False, int (popen.pid))   # PROCESS_QUERY_LIMITED_INFORMATION
                self.h = ct.c_void_p (h) if h else None
            except Exception:
                self.h = None
    def read (self):
        """-> (cpu seconds, peak working set in bytes) ; either may be None"""
        if not self.h: return (None, None)
        ct, k = self.api
        cpu = rss = None
        try:
            class FILETIME (ct.Structure):
                _fields_ = [('lo', ct.c_uint32), ('hi', ct.c_uint32)]
            cr, ex, kr, us = FILETIME(), FILETIME(), FILETIME(), FILETIME()
            k.GetProcessTimes.restype  = ct.c_int32
            k.GetProcessTimes.argtypes = [ct.c_void_p] + [ct.POINTER (FILETIME)] * 4
            if k.GetProcessTimes (self.h, ct.byref (cr), ct.byref (ex), ct.byref (kr), ct.byref (us)):
                units = ((kr.hi << 32) | kr.lo) + ((us.hi << 32) | us.lo)
                cpu = units * 1e-7                  # FILETIME is in 100 ns units
            class PMC (ct.Structure):
                _fields_ = [('cb', ct.c_uint32), ('PageFaultCount', ct.c_uint32),
                            ('PeakWorkingSetSize', ct.c_size_t), ('WorkingSetSize', ct.c_size_t),
                            ('QuotaPeakPagedPoolUsage', ct.c_size_t),
                            ('QuotaPagedPoolUsage', ct.c_size_t),
                            ('QuotaPeakNonPagedPoolUsage', ct.c_size_t),
                            ('QuotaNonPagedPoolUsage', ct.c_size_t),
                            ('PagefileUsage', ct.c_size_t), ('PeakPagefileUsage', ct.c_size_t),
                            ('PrivateUsage', ct.c_size_t)]
            pmc = PMC(); pmc.cb = ct.sizeof (PMC)
            libs = [k]
            try: libs.append (ct.windll.psapi)      # GetProcessMemoryInfo used to live there
            except Exception: pass
            for lib in libs:
                try:
                    fn = lib.GetProcessMemoryInfo
                    fn.restype  = ct.c_int32
                    fn.argtypes = [ct.c_void_p, ct.POINTER (PMC), ct.c_uint32]
                    if fn (self.h, ct.byref (pmc), ct.sizeof (PMC)):
                        rss = pmc.PeakWorkingSetSize
                        break
                except Exception: continue
        except Exception:
            pass
        return cpu, rss
    def close (self):
        try:
            if self.h: self.api[1].CloseHandle (self.h)
        except Exception: pass
        self.h = None

class RssWatch (threading.Thread):
    """peak RSS of the process tree, and -- where nothing else can get it -- its CPU time"""
    def __init__ (self, pid, want_cpu = False):
        threading.Thread.__init__ (self)
        self.pid = pid; self.peak = 0; self.cpu = None; self.stop = False
        self.want_cpu = want_cpu
        self.use_proc = os.path.isdir ('/proc')
        self.ps = None
        if (want_cpu or not self.use_proc) and _psutil is not None:
            try: self.ps = _psutil.Process (pid)
            except Exception: self.ps = None
        self.daemon = True
    def _tree (self):
        if self.ps is None: return []
        try: return [self.ps] + self.ps.children (recursive = True)
        except Exception: return [self.ps]
    def run (self):
        while not self.stop:
            if self.use_proc:
                seen, level = {self.pid}, [self.pid]
                for _ in range (3):
                    nxt = set()
                    for p in level: nxt |= children_of (p)
                    nxt -= seen; seen |= nxt; level = list (nxt)
                    if not level: break
                for p in seen:
                    v = vmhwm_kb (p) * 1024
                    if v > self.peak: self.peak = v
            elif self.ps is not None:
                tot = 0
                for p in self._tree():
                    try: tot += p.memory_info().rss
                    except Exception: pass
                if tot > self.peak: self.peak = tot
            if self.want_cpu and self.ps is not None:
                tot = 0.0
                for p in self._tree():
                    try:
                        t = p.cpu_times(); tot += (t.user + t.system)
                    except Exception: pass
                if self.cpu is None or tot > self.cpu: self.cpu = tot
            time.sleep (0.01)

class ProcMeter:
    """puts the platform specific answers together and returns (cpu seconds, peak MB)"""
    def __init__ (self, popen, times_before):
        self.t0    = times_before
        self.wh    = WinHandle (popen) if os.name == 'nt' else None
        want_cpu   = (os.name == 'nt') or (not os.path.isdir ('/proc'))
        self.watch = RssWatch (popen.pid, want_cpu = want_cpu)
        self.watch.start()
        self.how_cpu = self.how_rss = 'nothing'
    def finish (self, wall):
        self.watch.stop = True; self.watch.join (0.5)
        posix = None
        try:                                        # POSIX : os.times() accounts for every child
            after = os.times()
            posix = ((after.children_user + after.children_system)
                     - (self.t0.children_user + self.t0.children_system))
        except AttributeError:
            posix = None

        cpu, rss = None, (self.watch.peak or None)

        if self.wh is not None:                      # Windows : our own handle
            wcpu, wrss = self.wh.read()
            self.wh.close()
            cpu = wcpu if (wcpu and wcpu > 0) else None
            if wrss: rss = max (rss or 0, wrss)
            if cpu:  self.how_cpu = 'GetProcessTimes() on our own handle'
            if wrss: self.how_rss = 'GetProcessMemoryInfo() PeakWorkingSetSize'
        elif posix is not None and posix > 0:        # POSIX : os.times() knows every child
            cpu = posix
            self.how_cpu = 'os.times() children_user + children_system'

        if cpu is None and self.watch.cpu:           # psutil sampled while it was alive
            cpu = self.watch.cpu
            self.how_cpu = 'psutil, sampled while the codec was running'
        if cpu is not None and cpu <= 0:
            cpu = None if wall > 0.05 else 0.0
        if rss and self.how_rss == 'nothing':
            self.how_rss = ('/proc/<pid>/status VmHWM' if self.watch.use_proc
                            else 'psutil memory_info().rss')
        return cpu, (rss / 1048576.0 if rss else None)

def run_once (binobj, args, cwd, timeout):
    cmd = binobj.cmd (args)
    before = os.times(); t0 = time.perf_counter()
    try:
        p = subprocess.Popen (cmd, cwd = cwd, stdout = subprocess.PIPE, stderr = subprocess.PIPE)
    except OSError as e:
        return None, None, None, 127, '', 'cannot start %s : %s' % (cmd[0], e)
    met = ProcMeter (p, before)
    try:
        out, err = p.communicate (timeout = timeout); rc = p.returncode
    except subprocess.TimeoutExpired:
        p.kill(); out, err = p.communicate(); rc = -9; err = (err or b'') + b'\nTIMEOUT'
    wall = time.perf_counter() - t0
    cpu, rss = met.finish (wall)
    _LAST['cpu_from'] = met.how_cpu; _LAST['rss_from'] = met.how_rss
    return wall, cpu, rss, rc, out.decode ('utf-8', 'replace'), err.decode ('utf-8', 'replace')

#  where the last measurement got its numbers from -- printed by --diag
_LAST = {}

def measure (binobj, args, cwd, runs, timeout, tick = None):
    """run `runs` times and keep the fastest wall clock, with the cpu/rss of that run"""
    best = None
    for i in range (max (1, runs)):
        r = run_once (binobj, args, cwd, timeout)
        if tick: tick (i + 1)
        if r[0] is not None and (best is None or r[0] < best[0]): best = r
    if best is None: best = (None, None, None, 127, '', 'the command never ran')
    return dict (wall = best[0], cpu = best[1], rss = best[2], rc = best[3],
                 out = best[4], err = best[5])

def one_error_line (m):
    for line in reversed ((m['err'] or '').strip().splitlines()):
        if line.strip(): return line.strip()[:170]
    return 'rc=%d' % m['rc']

def startup_overhead (binobj):
    t = None
    for _ in range (5):
        r = run_once (binobj, ['--switch-that-does-not-exist'], None, 60)
        if r[0] is not None and (t is None or r[0] < t): t = r[0]
    return t

#===================================================================================================
#  images (only the header is read -- no pixel data is decoded here)
#===================================================================================================
IMG_EXT = ('.png', '.ppm', '.pgm', '.pnm')
NCH_OF_CTYPE = {0: 1, 2: 3, 3: 1, 4: 2, 6: 3}      # 6 = RGBA : the codecs drop the alpha

def image_info (path):
    """-> (w, h, nch) or None when the image cannot be used"""
    try:
        with open (path, 'rb') as f: head = f.read (64)
        if head[:8] == b'\x89PNG\r\n\x1a\n':
            w, h, depth, ctype, comp, filt, inter = struct.unpack ('>IIBBBBB', head[16:29])
            if depth != 8 or inter != 0: return None
            nch = NCH_OF_CTYPE.get (ctype)
            if nch is None or nch == 2: return None
            return w, h, nch
        if head[:2] in (b'P5', b'P6'):
            toks, i = [], 2
            while len (toks) < 3 and i < len (head):
                while i < len (head) and head[i:i+1].isspace(): i += 1
                j = i
                while j < len (head) and not head[j:j+1].isspace(): j += 1
                toks.append (head[i:j]); i = j
            return int (toks[0]), int (toks[1]), (3 if head[:2] == b'P6' else 1)
    except Exception: pass
    return None

def collect_images (directory, pattern, limit):
    if os.path.isfile (directory): return [os.path.abspath (directory)]
    if not os.path.isdir (directory):
        raise SystemExit ('*** %s : no such directory' % directory)
    names = sorted (n for n in os.listdir (directory) if n.lower().endswith (IMG_EXT))
    if pattern:
        import fnmatch
        names = [n for n in names if fnmatch.fnmatch (n.lower(), pattern.lower())]
    files = [os.path.join (directory, n) for n in names]
    return files[:limit] if limit else files

def chunk_by_bytes (items, size_of, limit):
    """split so that no chunk holds more than `limit` bytes of raw pixels (but >= 1 item)"""
    out, cur, cur_sz = [], [], 0
    for it in items:
        sz = size_of (it)
        if cur and cur_sz + sz > limit:
            out.append (cur); cur, cur_sz = [], 0
        cur.append (it); cur_sz += max (sz, 1)
    if cur: out.append (cur)
    return out

#===================================================================================================
#  the codecs under test
#===================================================================================================
class Codec:
    def __init__ (self, cid, label, family, binobj, enc_args, dec_args, ext, note = ''):
        self.id, self.label, self.family, self.bin = cid, label, family, binobj
        self.enc_args, self.dec_args, self.ext, self.note = enc_args, dec_args, ext, note
        self.enc = self.dec = None
        self.stream_bytes = 0
        self.n_ok = 0; self.n_img = 0
        self.tiles = None
        self.failed = None
        self.per_file = {}
    def mark_failed (self, why):
        self.failed = why
    @property
    def total (self):
        return None if (self.failed or not self.enc or not self.dec) else self.enc['wall'] + self.dec['wall']
    def cpu_total (self):
        return cpu_pair (self.enc['cpu'] if self.enc else None,
                         self.dec['cpu'] if self.dec else None)

def build_codecs (bins, args):
    mt  = ['-f'] + (['-x'] if args.crc else [])
    thr = [] if args.threads == 0 else ['-t', str (args.threads)]
    #  the author's tools always write an UNCOMPRESSED png (stored deflate blocks).  Decoding to
    #  a compressed png with our writer would be a comparison of png writers, not of codecs, so
    #  the default here is -pc 0.  Use --pc 6 for an end to end number instead.
    pc  = [] if args.pc is None else ['-pc', str (args.pc)]
    til = [] if args.tiles   == 0 else ['-T', str (args.tiles)]
    up  = ['-f'] + (['-x'] if args.crc else [])
    nbo = (args.nbli_opts or '').split()
    out = []
    if bins.get ('fnbli'):
        out.append (Codec ('up_fnbli', 'fNBLI  (upstream)', 'fnbli', bins['fnbli'],
                           up, list (up), 'fnbli', "the author's codec, single threaded"))
    if bins.get ('nbli'):
        out.append (Codec ('up_nbli', 'NBLI   (upstream)', 'nbli', bins['nbli'],
                           up + nbo, list (up), 'nbli', "the author's codec, single threaded"))
    if bins.get ('mtnbli'):
        out.append (Codec ('mt_fnbli', 'fNBLI  (mtnbli -M F)', 'fnbli', bins['mtnbli'],
                           mt + ['-M', 'F'] + thr, mt + thr + ['-d'] + pc, 'fnbli',
                           'same stream as upstream, our (SIMD free) build'))
        out.append (Codec ('mt_nbli', 'NBLI   (mtnbli -M N)', 'nbli', bins['mtnbli'],
                           mt + ['-M', 'N'] + thr + nbo, mt + thr + ['-d'] + pc, 'nbli',
                           'same stream as upstream, our build'))
        out.append (Codec ('mt_tnbli', 'mtNBLI (mtnbli -M MT)', 'fnbli', bins['mtnbli'],
                           mt + ['-M', 'MT'] + thr + til, mt + thr + ['-d'] + pc, 'tnbli',
                           'tiled container : K independent strips, one per core'))
        if args.t1:
            out.append (Codec ('mt_tnbli_1', 'mtNBLI (mtnbli -M MT -t 1)', 'fnbli', bins['mtnbli'],
                               mt + ['-M', 'MT', '-t', '1'] + til, mt + ['-t', '1', '-d'] + pc, 'tnbli',
                               'the same container with one thread : the scaling baseline'))
    return out

MAX_CMD_CHARS = 30000            # CreateProcess on Windows allows 32767
MAX_FILES_CMD = 900              # the upstream tools cap their file list at 999

def chunk_pairs (pairs):
    out, cur, cur_len = [], [], 0
    for p in pairs:
        add = len (p[0]) + len (p[1]) + 8
        if cur and (len (cur) >= MAX_FILES_CMD or cur_len + add > MAX_CMD_CHARS):
            out.append (cur); cur, cur_len = [], 0
        cur.append (p); cur_len += add
    if cur: out.append (cur)
    return out

def run_phase (log, codec, pairs, kind, work, runs, timeout, per_file, csv_rows, mode, raw_of,
               prog = None, title = None):
    """encode or decode a list of (src,dst) pairs; returns dict(wall,cpu,rss) or None on failure"""
    tot = dict (wall = 0.0, cpu = None, rss = 0.0)
    args  = codec.enc_args if kind == 'enc' else codec.dec_args
    groups = [[p] for p in pairs] if per_file else chunk_pairs (pairs)
    nruns  = max (1, runs)
    if prog:
        prog.begin (title or ('%s : %s %d file(s)' % (codec.label.strip(), kind, len (pairs))),
                    len (groups) * nruns, 'files' if per_file else 'commands')
        prog.pulse_start()
    for gi, g in enumerate (groups):
        argv = list (args)
        for src, dst in g: argv += [src, '-o', dst]
        m = measure (codec.bin, argv, work, runs, timeout,
                     tick = (lambda i, b = gi * nruns: prog.tick (b + i)) if prog else None)
        missing = [dst for _, dst in g if not os.path.exists (dst)]
        label = os.path.basename (g[0][0]) if per_file else '%d files' % len (g)
        csv_rows.append ([mode, codec.id, kind, label, runs,
                          '%.4f' % m['wall'] if m['wall'] is not None else '',
                          '%.4f' % m['cpu']  if m['cpu']  is not None else '',
                          '%.1f' % m['rss']  if m['rss']  is not None else '',
                          raw_of.get (g[0][0], ''),
                          (os.path.getsize (g[0][1]) if not missing and per_file else ''),
                          m['rc'], '' if (m['rc'] == 0 and not missing) else one_error_line (m)])
        if m['rc'] != 0 or missing:
            why = ('%d output file(s) missing' % len (missing)) if missing else one_error_line (m)
            codec.mark_failed ('%s failed (%s) : %s' % (kind, label, why))
            log ('   !! %-24s %s %s : %s' % (codec.label, kind, label, why))
            if prog: prog.pulse_stop(); prog.end ('FAILED')
            return None
        tot['wall'] += m['wall']
        tot['cpu']   = cpu_pair (tot['cpu'], m['cpu']) if tot['cpu'] is not None else m['cpu']
        tot['rss']   = max (tot['rss'], m['rss'] or 0)
        if per_file:
            key = os.path.splitext (os.path.basename (g[0][0]))[0]        # the image stem
            codec.per_file.setdefault (key, {})[kind] = m['wall']
    if prog: prog.pulse_stop(); prog.end ('codec time %.3f s' % tot['wall'])
    return tot

#===================================================================================================
#  main
#===================================================================================================
def main():
    ap = argparse.ArgumentParser (formatter_class = argparse.RawDescriptionHelpFormatter,
                                  description = __doc__)
    ap.add_argument ('-c', '--codecs', required = True, help = 'folder with fNBLI(.exe) NBLI(.exe) mtnbli(.exe)')
    ap.add_argument ('-i', '--images', required = True, help = 'folder with the source images (.png .ppm .pgm)')
    ap.add_argument ('-o', '--out',    default = '',    help = 'where report.txt / results.csv / results.json go (default ./bench_report)')
    ap.add_argument ('-m', '--mode',   default = 'both', choices = ['batch', 'single', 'both'])
    ap.add_argument ('-r', '--runs',   type = int, default = 3, help = 'repetitions in BATCH mode, best kept (default 3)')
    ap.add_argument ('--single-runs',  type = int, default = 1, help = 'repetitions per file in SINGLE mode (default 1)')
    ap.add_argument ('-t', '--threads',type = int, default = 0, help = '-t for mtnbli, 0 = let mtnbli decide (default)')
    ap.add_argument ('-T', '--tiles',  type = int, default = 0, help = '-T for -M MT, 0 = auto (default)')
    ap.add_argument ('--nbli-opts',    default = '', help = 'extra switches for the NBLI encoders, e.g. "-g" or "-a"')
    ap.add_argument ('--t1',           action = 'store_true', help = 'also run -M MT -t 1 as a single thread baseline')
    ap.add_argument ('--no-crc',       dest = 'crc', action = 'store_false', help = 'do not pass -x (CRC32 inside the stream)')
    ap.set_defaults (crc = True)
    ap.add_argument ('--limit',        type = int, default = 0, help = 'use only the first N images')
    ap.add_argument ('--filter',       default = '', help = 'only images matching this glob, e.g. "*.png"')
    ap.add_argument ('--compat',       type = int, default = 1, help = 'cross decode the first N images (default 1, 0 = off)')
    ap.add_argument ('--pc',          type = int, default = 0,
                      help = 'PNG level for the mtnbli decoder, 0 = stored like the author tools (default 0)')
    ap.add_argument ('--strict',       action = 'store_true', help = 'also compare the decoded pixels in pure Python (slow on big images)')
    ap.add_argument ('--timeout',      type = int, default = 3600, help = 'seconds per command (default 3600)')
    ap.add_argument ('--work',         default = '', help = 'work directory (default: a temporary one, deleted at the end)')
    ap.add_argument ('--keep',         action = 'store_true', help = 'do not delete the work directory')
    ap.add_argument ('--ref-chunk-mb', type = int, default = 512,
                     help = 'how many megabytes of raw pixels the reference is built in (default 512)')
    ap.add_argument ('--no-progress',  action = 'store_true', help = 'no stage counter / progress bar')
    ap.add_argument ('--diag',         action = 'store_true',
                     help = 'print where the CPU time and the peak memory of a child come from')
    ap.add_argument ('--wine',         default = os.environ.get ('WINE', 'wine'), help = 'wine binary (default: wine)')
    ap.add_argument ('--fnbli',  default = '', help = 'explicit path to the upstream fNBLI')
    ap.add_argument ('--nbli',   default = '', help = 'explicit path to the upstream NBLI')
    ap.add_argument ('--mtnbli', default = '', help = 'explicit path to mtnbli')
    args = ap.parse_args()
    t0_all = time.time()

    #------------------------------------------------------------------ the binaries
    bins = {}
    for key, stem, over in (('fnbli', 'fNBLI', args.fnbli), ('nbli', 'NBLI', args.nbli),
                            ('mtnbli', 'mtnbli', args.mtnbli)):
        p = find_bin (args.codecs, stem, over)
        if not p: continue
        bins[key] = Bin (p, [args.wine] if (sys.platform != 'win32' and is_pe (p) and args.wine) else [])
    if 'mtnbli' not in bins:
        raise SystemExit ('*** mtnbli(.exe) not found in %s  (use --mtnbli <path>)' % args.codecs)
    if not bins.get ('fnbli') and not bins.get ('nbli'):
        sys.stderr.write ('*** warning : neither fNBLI nor NBLI in %s -- only our own codecs are\n'
                          '    measured, and every comparison with the author\'s is skipped.\n'
                          % args.codecs)

    #------------------------------------------------------------------ the images
    files = collect_images (args.images, args.filter, args.limit)
    if not files:
        raise SystemExit ('*** no .png/.ppm/.pgm/.pnm in %s' % args.images)
    refs, skipped, raw_total, npix = [], [], 0, 0
    for f in files:
        info = image_info (f)
        if info is None:
            skipped.append (os.path.basename (f)); continue
        w, h, nch = info
        refs.append ((os.path.splitext (os.path.basename (f))[0], w, h, nch, w * h * nch))
        raw_total += w * h * nch; npix += w * h
    if not refs:
        raise SystemExit ('*** none of those images can be used ' \
                          '(need 8 bit, non interlaced, non palette PNG or a raw PNM)')
    stems = {r[0] for r in refs}
    files = [f for f in files if os.path.splitext (os.path.basename (f))[0] in stems]
    raw_of = {f: r[4] for f, r in zip (files, refs)}

    #------------------------------------------------------------------ folders
    out_dir = os.path.abspath (args.out or os.path.join (os.getcwd (), 'bench_report'))
    os.makedirs (out_dir, exist_ok = True)
    work = os.path.abspath (args.work) if args.work else tempfile.mkdtemp (prefix = 'mtnbli_bench_')
    os.makedirs (work, exist_ok = True)
    remove_work = (not args.keep) and (not args.work)
    if remove_work: _WORK_DIRS.append (work)

    log = Log (os.path.join (out_dir, 'report.txt'))
    csv_rows = []

    #------------------------------------------------------------------ how many stages will there be
    modes  = [args.mode] if args.mode != 'both' else ['batch', 'single']
    n_cod  = len (build_codecs (bins, args))
    total  = 3                                   # the reference : encode, decode, hash
    total += 4 * n_cod * len (modes)             # per codec : compress, decompress, re-decode, hash
    if 'single' in modes:              total += 1     # thread pool probe
    if args.compat and (bins.get ('fnbli') or bins.get ('nbli')): total += 1
    if args.strict:                    total += 1
    prog = Progress (log, total, enabled = not args.no_progress)

    #------------------------------------------------------------------ header
    log (LINE)
    log (' mtnbli benchmark : our codecs against the upstream NBLI / fNBLI')
    log (LINE)
    log (' date        %s' % time.strftime ('%Y-%m-%d %H:%M:%S'))
    log (' host        %s %s   (%s)' % (platform.system(), platform.release(), platform.machine()))
    log (' cpu         %s' % cpu_name())
    q = cpu_quota()
    log (' threads     %d hardware threads%s' % (os.cpu_count() or 1,
         (',   cgroup quota %.2f CPU -- more threads than that only add context switches' % q) if q else ''))
    log (' python      %s' % platform.python_version())
    log (' codec dir   %s' % os.path.abspath (args.codecs))
    for k in ('fnbli', 'nbli', 'mtnbli'):
        if k in bins:
            b = bins[k].path
            log ('   %-7s %-46s %10s B   sha256 %s' % (k, bins[k].tag, num (os.path.getsize (b)),
                                                        sha256_of (b)[:16]))
        else:
            log ('   %-7s (not found -- every comparison that needs it is skipped)' % k)
    log (' images      %s   (%d files, %s bytes of pixels, %s pixels)'
         % (os.path.abspath (args.images), len (refs), num (raw_total), num (npix)))
    for (stem, w, h, nch, raw) in refs[:12]:
        log ('   %-40s %5d x %-5d  %-4s  %10s B' % (stem, w, h, 'RGB' if nch >= 3 else 'gray', num (raw)))
    if len (refs) > 12: log ('   ... and %d more' % (len (refs) - 12))
    if skipped:
        log ('   skipped %d file(s) we cannot read : %s'
             % (len (skipped), ', '.join (skipped[:5])))
    log (' options     mode=%s  runs(batch)=%d  runs(single)=%d  threads=%s  tiles=%s  crc=%s  png -pc %d  %s'
         % (args.mode, args.runs, args.single_runs, args.threads or 'auto', args.tiles or 'auto',
            args.crc, args.pc, ('nbli-opts="%s"' % args.nbli_opts) if args.nbli_opts else ''))
    log ('             -pc 0 writes an uncompressed png, exactly what the author\'s tools write, so')
    log ('             that the decode column compares the codecs and not the png writers.')
    log (' verify      %s of the raw pixels : every decoded image is hashed and deleted right'
         % HASH_NAME)
    log ('             away, so a comparison never keeps two copies of an 8K frame on disk.')
    log (' stages      %d' % prog.total)
    log (' work dir    %s%s' % (work, '' if remove_work else '   (kept : --keep)'))
    try:
        #  the streams of every codec at once (a stream is never much bigger than the raw pixels)
        #  plus two raw sized copies : the one being written and the one being hashed
        need = raw_total * (2 + 1.05 * n_cod)
        free = shutil.disk_usage (work).free
        log (' disk        %s free in the work dir, this run needs about %s'
             % (num (free), num (need)))
        if need > free * 0.6:
            log (' !! that is close to (or past) what is free : every intermediate is removed again')
            log ('    as soon as it has been consumed, but consider --limit N or --mode batch.')
    except Exception: pass

    startup = {k: startup_overhead (b) for k, b in bins.items()}
    log (' start up    one bare invocation costs : %s'
         % '   '.join ('%s %.3f s' % (k, v) for k, v in startup.items() if v))
    if any (b.wine for b in bins.values()):
        log ('             those are Windows binaries started through wine, so every wall clock time')
        log ('             below carries that start up.  The "cpu s" column is then the one that says')
        log ('             something about the codecs themselves.')

    #------------------------------------------------------------------ can this machine be measured at all?
    if args.diag:
        d = os.path.join (work, 'diag'); os.makedirs (d, exist_ok = True)
        m = measure (bins['mtnbli'], ['-f', '-M', 'F', files[0], '-o', os.path.join (d, 'p.fnbli')],
                     d, 1, args.timeout)
        log ()
        log (' measurement probe (--diag), one encode of %s' % os.path.basename (files[0]))
        log ('   wall     %.3f s' % (m['wall'] if m['wall'] is not None else float ('nan')))
        log ('   cpu      %-8s <- %s' % (secs (m['cpu']), _LAST.get ('cpu_from', 'nothing')))
        log ('   peak rss %-8s <- %s'
             % (('%.1f MB' % m['rss']) if m['rss'] else 'n/a', _LAST.get ('rss_from', 'nothing')))
        if m['cpu'] is None:
            log ('   !! no CPU time : the "cpu s" and "cores" columns of this report stay empty.')
        shutil_rmtree (d)

    #------------------------------------------------------------------ the reference
    #  comparing PNG bytes would only compare our PNG writer with the author's, so every stream is
    #  decoded once more into a raw PNM (not timed) and a 128 bit hash of that is compared with the
    #  hash of the reference taken the same way from the source image.  The raw pixels are deleted
    #  as soon as they are hashed -- on a folder of 8K frames they would be ~1 GB per image.
    log ()
    log (' preparing the reference : source -> fNBLI -> raw pixels -> hash   (not timed)')
    ref_dir = os.path.join (work, 'reference'); os.makedirs (ref_dir, exist_ok = True)
    ref_hash  = {}
    plan      = list (zip (files, [r[0] for r in refs]))
    ref_limit = max (1, args.ref_chunk_mb) * (1 << 20)
    chunks    = chunk_by_bytes (plan, lambda fs: raw_of.get (fs[0], 1 << 20), ref_limit)
    ref_enc   = Codec ('ref',  'reference', 'fnbli', bins['mtnbli'], ['-f', '-x', '-M', 'F', '-t', '1'], [], 'fnbli')
    ref_dec   = Codec ('ref2', 'reference', 'fnbli', bins['mtnbli'], [], ['-f'], 'pnm')
    log ('   %d image(s) in %d chunk(s), at most %s of raw pixels on disk at a time'
         % (len (plan), len (chunks), num (ref_limit)))

    def ref_paths (grp):
        enc = [(f, os.path.join (ref_dir, s + '.fnbli')) for f, s in grp]
        dec = [(d, os.path.join (ref_dir, s + '.pnm')) for (f, s), (_, d) in zip (grp, enc)]
        return enc, dec

    prog.begin ('reference : encode the sources with mtnbli', len (chunks), 'chunks')
    enc_chunks = []
    for grp in chunks:
        enc, dec = ref_paths (grp)
        m = run_phase (log, ref_enc, enc, 'enc', ref_dir, 1, args.timeout, False, csv_rows, 'ref', raw_of)
        if m is None:
            raise SystemExit ('*** could not build the reference streams : see above')
        enc_chunks.append ((grp, enc, dec))
        prog.tick()
    prog.end()

    prog.begin ('reference : decode the streams to raw pixels', len (chunks), 'chunks')
    for grp, enc, dec in enc_chunks:
        m = run_phase (log, ref_dec, dec, 'dec', ref_dir, 1, args.timeout, False, csv_rows, 'ref', {})
        if m is None:
            raise SystemExit ('*** could not build the reference pixels : see above')
        for _, d in enc:
            try: os.remove (d)                        # A.fnbli : consumed, gone
            except OSError: pass
        prog.tick()
    prog.end()

    prog.begin ('reference : hash the raw pixels and delete them', len (plan), 'images')
    for grp, enc, dec in enc_chunks:
        for (f, s), (_, p) in zip (grp, dec):
            if not os.path.exists (p):
                raise SystemExit ('*** reference pixel file missing for %s' % s)
            ref_hash[s] = file_hash128 (p)
            os.remove (p)                             # A.pnm : consumed, gone
            prog.tick()
    prog.end ('%d hash(es)' % len (ref_hash))
    if len (ref_hash) != len (plan):
        raise SystemExit ('*** could not hash every reference image')

    #------------------------------------------------------------------ the two modes
    #  which streams must survive for the interoperability check at the end
    keep_for_cross = set()
    if args.compat:
        if bins.get ('fnbli'): keep_for_cross |= {'up_fnbli', 'mt_fnbli'}
        if bins.get ('nbli'):  keep_for_cross |= {'up_nbli',  'mt_nbli'}
    base_mode = 'batch' if 'batch' in modes else modes[0]

    results = {}
    for mode in modes:
        per_file = (mode == 'single')
        runs = args.single_runs if per_file else args.runs
        d = os.path.join (work, mode); os.makedirs (d, exist_ok = True)
        log (); log (DLINE)
        log (' %s : %s' % (mode.upper(), 'one command per file, times summed over the files'
                           if per_file else 'the whole folder in one command line'))
        log (DLINE)

        codecs = build_codecs (bins, args)
        names = {}
        for c in codecs:
            c.per_file = {}
            c.n_img = len (refs)
            sdir = os.path.join (d, c.id + '_stream'); os.makedirs (sdir, exist_ok = True)
            pdir = os.path.join (d, c.id + '_png');    os.makedirs (pdir, exist_ok = True)
            ndir = os.path.join (d, c.id + '_pnm');    os.makedirs (ndir, exist_ok = True)
            names[c.id] = (sdir, pdir, ndir)

        #---- 1) every image into every format
        log (); log ('   1) compress : every image into every format')
        for c in codecs:
            sdir = names[c.id][0]
            streams = [os.path.join (sdir, s + '.' + c.ext) for (s, *_ ) in refs]
            c.enc = run_phase (log, c, list (zip (files, streams)), 'enc', d, runs, args.timeout,
                               per_file, csv_rows, mode, raw_of, prog,
                               '%s : compress %d image(s)' % (c.label.strip(), len (files)))
            if c.enc is None:
                prog.skip (3); continue
            c.stream_bytes = sum (os.path.getsize (s) for s in streams if os.path.exists (s))
            if c.ext == 'tnbli':
                c.tiles = count_tiles (streams)

        #---- 2) every stream back to png
        log (); log ('   2) decompress : every stream back to PNG')
        for c in codecs:
            if c.enc is None: continue
            sdir, pdir, ndir = names[c.id]
            streams = [os.path.join (sdir, s + '.' + c.ext) for (s, *_ ) in refs]
            pngs    = [os.path.join (pdir, s + '.png')      for (s, *_ ) in refs]
            c.dec = run_phase (log, c, list (zip (streams, pngs)), 'dec', d, runs, args.timeout,
                               per_file, csv_rows, mode, {s: raw_of[f] for s, f in zip (streams, files)},
                               prog, '%s : decompress %d stream(s)' % (c.label.strip(), len (streams)))
            if c.dec is None:
                prog.skip (2); continue
            #  the decoded PNGs are never compared byte by byte (that would compare png writers),
            #  the check below re-decodes the stream itself -- so they can go right now, before
            #  the next codec fills the disk with its own
            if not args.keep and not args.strict:
                shutil_rmtree (pdir)

        #---- 3) was it bit exact?  (the same decoder writes the image once more, as raw PNM)
        log (); log ('   3) verify : is what comes back the image we put in?')
        for c in codecs:
            if c.enc is None or c.dec is None: continue
            sdir, pdir, ndir = names[c.id]
            streams = [os.path.join (sdir, s + '.' + c.ext) for (s, *_ ) in refs]
            pnms    = [os.path.join (ndir, s + '.pnm')      for (s, *_ ) in refs]
            r = run_phase (log, Codec (c.id + '_pnm', c.label, c.family, c.bin, [], ['-f'], 'pnm'),
                           list (zip (streams, pnms)), 'dec', d, 1, args.timeout, False, csv_rows,
                           mode + '-check', {}, prog,
                           '%s : decode %d stream(s) again for the check' % (c.label.strip(), len (streams)))
            if r is None:
                c.mark_failed ('the stream could not be decoded again for the bit exact check')
                prog.skip (1); continue

            prog.begin ('%s : hash %d raw image(s) and compare' % (c.label.strip(), len (pnms)),
                        len (pnms), 'images')
            bad = []
            for (stem, *_ ), got in zip (refs, pnms):
                want = ref_hash.get (stem)
                if not os.path.exists (got):
                    bad.append (stem); prog.tick(); continue
                h, sz = file_hash128 (got)
                os.remove (got)                       # hashed : the raw copy goes away at once
                if want is None or (h, sz) != want: bad.append (stem)
                prog.tick()
            c.n_ok = len (refs) - len (bad)
            if bad:
                log ('   !! %-24s %d/%d image(s) are NOT bit identical : %s'
                     % (c.label, len (bad), len (refs), ', '.join (bad[:4])))
                prog.end ('%d differ' % len (bad))
            else:
                prog.end ('%d/%d bit exact' % (c.n_ok, len (refs)))
            shutil_rmtree (ndir)
            #  the streams are the last thing this codec needed -- except the one or two files the
            #  interoperability check at the end reads again
            if not args.keep:
                keep = args.compat if (mode == base_mode and c.id in keep_for_cross) else 0
                if keep > 0:
                    for s in streams[max (0, keep):]:
                        try: os.remove (s)
                        except OSError: pass
                else:
                    shutil_rmtree (sdir)

        results[mode] = codecs
        phase_table (log, codecs, raw_total, npix, startup if per_file else None)
        if per_file: per_image_table (log, codecs, refs)

    #------------------------------------------------------------------ why SINGLE can be slower
    if 'single' in results:
        one, allt, n = thread_pool_cost (bins, files, refs, work, args, prog)
        if one and allt and (allt - one) > 0.005:
            log ()
            log (' one process per file costs us : on the smallest image one mtnbli invocation takes')
            log ('   %.3f s with -t 1 but %.3f s with the default threads, so about %.0f ms of every'
                 % (one, allt, 1000 * (allt - one)))
            log ('   invocation is thread pool set up -- %.2f s over %d files.  BATCH pays it once.'
                 % (n * (allt - one), n))

    #------------------------------------------------------------------ interoperability
    compat = cross_check (log, bins, results, refs, work, args, ref_hash, prog) if args.compat else []

    #------------------------------------------------------------------ optional strict check
    strict = strict_check (log, results, files, work, prog) if args.strict else None

    #------------------------------------------------------------------ analysis
    analyse (log, results, raw_total, npix, startup, compat, strict, q, args,
             any (b.wine for b in bins.values()))

    #------------------------------------------------------------------ csv / json
    with open (os.path.join (out_dir, 'results.csv'), 'w', newline = '', encoding = 'utf-8') as f:
        w = csv.writer (f)
        w.writerow (['mode', 'codec', 'phase', 'file', 'runs', 'wall_s', 'cpu_s', 'peak_rss_mb',
                     'bytes_in', 'bytes_out', 'rc', 'error'])
        w.writerows (csv_rows)
    def jc (c):
        return dict (id = c.id, label = c.label, failed = c.failed,
                     enc_s = c.enc['wall'] if c.enc else None, dec_s = c.dec['wall'] if c.dec else None,
                     cpu_s = c.cpu_total(), stream_bytes = c.stream_bytes, tiles = c.tiles,
                     verified = '%d/%d' % (c.n_ok, c.n_img))
    with open (os.path.join (out_dir, 'results.json'), 'w', encoding = 'utf-8') as f:
        json.dump (dict (date = time.strftime ('%Y-%m-%d %H:%M:%S'), images = len (refs),
                         raw_bytes = raw_total, pixels = npix, hash = HASH_NAME,
                         modes = {m: [jc (c) for c in cs] for m, cs in results.items()},
                         compat = compat), f, indent = 1, default = str)

    #------------------------------------------------------------------ clean up
    log ()
    if remove_work:
        shutil_rmtree (work)
        log (' cleaned up  %s is gone : every stream, every decoded PNG and the reference' % work)
        log ('             that this run produced.  The originals in %s were never touched,'
             % os.path.abspath (args.images))
        log ('             and nothing was ever written into that folder.')
    else:
        log (' work dir kept : %s' % work)
    log (' report      %s' % os.path.join (out_dir, 'report.txt'))
    log (' csv         %s   (%d measurements)' % (os.path.join (out_dir, 'results.csv'), len (csv_rows)))
    log (' json        %s' % os.path.join (out_dir, 'results.json'))
    log (' total       %.1f s' % (time.time() - t0_all))
    log.close()

def shutil_rmtree (path):
    shutil.rmtree (path, ignore_errors = True)

def count_tiles (streams):
    """strips per .tnbli (header : magic 8, width 4, height 4, n_tiles 4, ...) -> (min, max)"""
    got = []
    for s in streams:
        try:
            with open (s, 'rb') as f: head = f.read (24)
            if head[:6] != b'MTNBLI': continue
            got.append (struct.unpack ('<I', head[16:20])[0])
        except Exception: pass
    if not got: return None
    return (min (got), max (got)) if min (got) != max (got) else (min (got), min (got))

def tiles_str (t):
    if not t: return 'several tiles'
    lo, hi = t
    return ('%d tiles' % lo) if lo == hi else ('%d..%d tiles' % (lo, hi))

def per_image_table (log, codecs, refs):
    """SINGLE mode : the round trip of every single image, per codec"""
    live = [c for c in codecs if not c.failed and c.enc and c.dec]
    if not live or len (refs) > 14: return
    log ()
    log ('   round trip per image (encode + decode, seconds) and the compressed size')
    head = ['image'] + [c.label.strip() for c in live] + ['best']
    rows = []
    for (stem, w, h, nch, raw) in refs:
        row, best = [stem], None
        for c in live:
            pf = c.per_file.get (stem, {})
            t = (pf.get ('enc', 0) or 0) + (pf.get ('dec', 0) or 0)
            row.append (secs (t) if t else '-')
            if t and (best is None or t < best[1]): best = (c.label.strip(), t)
        row.append (best[0] if best else '-')
        rows.append (row)
    table (log, head, rows, 'l' + 'r' * (len (live)) + 'l', indent = '   ')

def phase_table (log, codecs, raw_total, npix, startup):
    log ()
    log ('   codec                        encode                            decode                        stream                 bit exact')
    log ('                             sec     MB/s    cpu s  cores       sec     MB/s    cpu s  cores        bytes    %raw      bpp')
    log ('   ' + '-' * 128)
    for c in codecs:
        if c.failed or c.enc is None or c.dec is None:
            log ('   %-26s FAILED : %s' % (c.label, (c.failed or 'no result')[:72])); continue
        e, d = c.enc['wall'], c.dec['wall']
        log ('   %-26s %8s %8s %8s %6s   %8s %8s %8s %6s %12s %7s %7s    %4d/%d' % (
            c.label, secs (e), mbs (raw_total / 1e6 / e), secs (c.enc['cpu']),
            ('%.1f' % (c.enc['cpu'] / e)) if (e > 0 and c.enc['cpu']) else 'n/a',
            secs (d), mbs (raw_total / 1e6 / d), secs (c.dec['cpu']),
            ('%.1f' % (c.dec['cpu'] / d)) if (d > 0 and c.dec['cpu']) else 'n/a',
            num (c.stream_bytes), pct (100.0 * c.stream_bytes / raw_total),
            '%.3f' % (8.0 * c.stream_bytes / npix), c.n_ok, c.n_img))
    log ()
    log ('   MB/s is on the raw pixels (%s bytes) ; bpp = bits per pixel of the stream.' % num (raw_total))
    log ('   "cpu s" is the CPU time the codec burned in all its threads : the cost of the job')
    log ('   measured in core-seconds, so it is the number to compare a single threaded codec with.')
    log ('   "cores" is cpu s / wall s : how many cores it really kept busy.')
    log ('   "bit exact" counts %s hashes of the raw pixels, not the bytes of a png.' % HASH_NAME)
    if startup:
        known = [v for v in startup.values() if v]
        if known:
            log ('   In SINGLE mode one bare process start (%.3f s) is paid per file and is included.'
                 % min (known))

#===================================================================================================
#  interoperability : can the author's tool read our stream, and can we read theirs?
#===================================================================================================
def cross_check (log, bins, results, refs, work, args, ref_hash, prog = None):
    log (); log (DLINE)
    log (' interoperability : our streams read by the author\'s tool, and theirs by ours')
    log (DLINE)
    d = os.path.join (work, 'cross'); os.makedirs (d, exist_ok = True)
    out = []
    want = refs[:max (0, args.compat)]
    plans = []
    if bins.get ('fnbli'): plans.append (('fnbli', 'up_fnbli', 'mt_fnbli'))
    if bins.get ('nbli'):  plans.append (('nbli',  'up_nbli',  'mt_nbli'))
    base_mode = 'batch' if 'batch' in results else list (results)[0]
    jobs = []
    for fmt, up_id, mt_id in plans:
        up_dir = os.path.join (work, base_mode, up_id + '_stream')
        mt_dir = os.path.join (work, base_mode, mt_id + '_stream')
        jobs.append ((fmt, "the author's stream read by mtnbli", up_dir, bins['mtnbli']))
        jobs.append ((fmt, 'our stream read by the author tool', mt_dir, bins[fmt]))
    steps = sum (1 for (fmt, what, srcdir, dec) in jobs
                 for (stem, *_ ) in want
                 if os.path.exists (os.path.join (srcdir, '%s.%s' % (stem, fmt))))
    if prog: prog.begin ('cross decoding the first %d image(s)' % len (want), steps, 'decodes')
    for (fmt, what, srcdir, dec) in jobs:
        for (stem, w, h, nch, raw) in want:
            src = os.path.join (srcdir, '%s.%s' % (stem, fmt))
            ref = ref_hash.get (stem)
            if not os.path.exists (src) or ref is None: continue
            dst = os.path.join (d, '%s.%s.pnm' % (stem, 'mt' if dec is bins['mtnbli'] else 'up'))
            r = run_once (dec, ['-f', src, '-o', dst], d, args.timeout)
            ok = False
            if r[3] == 0 and os.path.exists (dst):
                ok = (file_hash128 (dst) == ref)
                try: os.remove (dst)
                except OSError: pass
            out.append (dict (fmt = fmt, check = what, image = stem, ok = bool (ok)))
            log ('   %-8s %-36s %-26s %s' % (fmt, what, stem, 'bit exact' if ok else 'FAILED'))
            if prog: prog.tick()
    if prog: prog.end ('%d of %d bit exact' % (sum (1 for c in out if c['ok']), len (out)))
    return out

def thread_pool_cost (bins, files, refs, work, args, prog = None):
    """what does one mtnbli invocation pay for setting its thread pool up?  (smallest image)"""
    pairs = sorted (zip (files, refs), key = lambda t: t[1][4])
    if not pairs: return None, None, 0
    img = pairs[0][0]
    d = os.path.join (work, 'poolprobe'); os.makedirs (d, exist_ok = True)
    if prog:
        prog.begin ('measuring what one process start costs (smallest image, 5 runs)', 10, 'runs')
    out = []
    for extra in (['-t', '1'], []):
        best = None
        for i in range (5):
            m = measure (bins['mtnbli'], ['-f', '-M', 'F'] + extra + [img, '-o',
                         os.path.join (d, 'p%d.fnbli' % i)], d, 1, args.timeout)
            if m['wall'] is not None and (best is None or m['wall'] < best): best = m['wall']
            if prog: prog.tick()
        out.append (best)
    if prog: prog.end()
    return out[0], out[1], len (refs)

def strict_check (log, results, files, work, prog = None):
    """independent, pure python pixel comparison of the decoded PNGs against the sources"""
    from imglib import read_image
    log (); log (DLINE)
    log (' strict check : the decoded PNGs read back in pure python and compared pixel by pixel')
    log (DLINE)
    ref = {}
    for f in files:
        stem = os.path.splitext (os.path.basename (f))[0]
        try:    ref[stem] = read_image (f)[2]
        except Exception as e:
            log ('   (cannot read %s : %s)' % (stem, e))
    bad = 0
    if prog: prog.begin ('strict pixel check in pure python', len (ref) * len (results), 'images')
    for mode, codecs in results.items():
        for c in codecs:
            if c.failed: continue
            pdir = os.path.join (work, mode, c.id + '_png')
            for stem, want in ref.items():
                p = os.path.join (pdir, stem + '.png')
                if not os.path.exists (p): continue
                try:    got = read_image (p)[2]
                except Exception as e:
                    log ('   !! %-24s %-24s unreadable : %s' % (c.label, stem, e)); bad += 1; continue
                if got != want:
                    n = sum (1 for a, b in zip (got, want) if a != b)
                    log ('   !! %-24s %-24s %d of %d bytes differ' % (c.label, stem, n, len (want)))
                    bad += 1
                if prog: prog.tick()
    if prog: prog.end ('%d mismatch(es)' % bad)
    log ('   strict check : %s' % ('every decoded PNG is pixel identical to its source' if not bad
                                    else '%d mismatch(es)' % bad))
    return bad

#===================================================================================================
#  the analytical summary (everything here is computed from the measurements above)
#===================================================================================================
def analyse (log, results, raw_total, npix, startup, compat, strict, quota, args, wine = False):
    log (); log (LINE)
    log (' analytical summary')
    log (LINE)

    for mode, codecs in results.items():
        by = {c.id: c for c in codecs}
        log ()
        log (' %s mode' % mode.upper())
        pairs = []
        if 'mt_fnbli' in by:  pairs.append (('mt_fnbli', 'up_fnbli', 'fNBLI  : ours vs upstream'))
        if 'mt_nbli'  in by:  pairs.append (('mt_nbli',  'up_nbli',  'NBLI   : ours vs upstream'))
        if 'mt_tnbli' in by:  pairs.append (('mt_tnbli', 'up_fnbli', 'mtNBLI : tiled vs upstream fNBLI'))
        if 'mt_tnbli_1' in by: pairs.append (('mt_tnbli', 'mt_tnbli_1', 'mtNBLI : all cores vs one core'))
        rows = []
        for ours, base, what in pairs:
            a, b = by.get (ours), by.get (base)
            if not a or not b or a.failed or b.failed or not a.enc or not b.enc:
                rows.append ([what] + ['n/a'] * 5); continue
            e = b.enc['wall'] / a.enc['wall'] if a.enc['wall'] > 0 else None
            d = b.dec['wall'] / a.dec['wall'] if a.dec['wall'] > 0 else None
            t = (b.enc['wall'] + b.dec['wall']) / max (1e-9, a.enc['wall'] + a.dec['wall'])
            sz = (100.0 * (a.stream_bytes - b.stream_bytes) / b.stream_bytes) if b.stream_bytes else None
            ac, aw = a.cpu_total(), (a.enc['wall'] + a.dec['wall'])
            rows.append ([what, rel (e), rel (d), rel (t),
                          ('%+.2f%%' % sz) if sz is not None else 'n/a',
                          ('%.1f' % (ac / aw)) if (ac and aw > 0) else 'n/a'])
        if rows:
            table (log, ['comparison', 'encode', 'decode', 'round trip', 'size', 'cores used'],
                   rows, 'l r r r r r')
        #  the same thing in core-seconds : one core of ours against one core of theirs, with the
        #  number of threads taken out of the comparison
        rows = []
        for ours, base, what in pairs:
            a, b = by.get (ours), by.get (base)
            if not a or not b or a.failed or b.failed or not a.enc or not b.enc: continue
            ac, bc = a.cpu_total(), b.cpu_total()
            if not ac or not bc:
                rows.append ([what, 'n/a', 'n/a', 'n/a']); continue
            rows.append ([what,
                          rel (b.enc['cpu'] / a.enc['cpu']) if a.enc['cpu'] else 'n/a',
                          rel (b.dec['cpu'] / a.dec['cpu']) if a.dec['cpu'] else 'n/a',
                          rel (bc / ac)])
        if rows:
            log ()
            log ('   the same comparison in core-seconds, i.e. one core of ours against one core of theirs')
            table (log, ['comparison (CPU time)', 'encode', 'decode', 'round trip'], rows, 'l r r r')

    #---------------- the story, in words, with the same numbers
    log (); log (DLINE)
    log (' what the numbers say')
    log (DLINE)
    for mode, codecs in results.items():
        by = {c.id: c for c in codecs}
        live = [c for c in codecs if not c.failed and c.enc and c.dec]
        if not live:
            log (); log (' %s : no codec produced a result, nothing to conclude.' % mode.upper()); continue
        log ()
        log (' %s mode' % mode.upper())
        best  = min (live, key = lambda c: c.enc['wall'] + c.dec['wall'])
        slow  = max (live, key = lambda c: c.enc['wall'] + c.dec['wall'])
        log ('   * fastest round trip : %-24s %8.3f s   (%s MB/s over both directions), slowest : %-24s %8.3f s'
             % (best.label, best.enc['wall'] + best.dec['wall'],
                mbs (raw_total / 1e6 / (best.enc['wall'] + best.dec['wall'])),
                slow.label, slow.enc['wall'] + slow.dec['wall']))
        up_live = [c for c in live if c.id.startswith ('up_')]
        if up_live:
            up = min (up_live, key = lambda c: c.enc['wall'] + c.dec['wall'])
            for c in live:
                if c.id.startswith ('up_'): continue
                log ('   * %-24s needs %.3f s where the fastest author codec (%s) needs %.3f s : %.2fx'
                     % (c.label.strip(), c.enc['wall'] + c.dec['wall'], up.label.strip(),
                        up.enc['wall'] + up.dec['wall'],
                        (up.enc['wall'] + up.dec['wall']) / (c.enc['wall'] + c.dec['wall'])))
        for cid in ('mt_fnbli', 'mt_nbli', 'mt_tnbli'):
            c, b = by.get (cid), by.get ('up_fnbli' if cid != 'mt_nbli' else 'up_nbli')
            if not c or c.failed or not c.enc or not b or b.failed: continue
            sz = 100.0 * (c.stream_bytes - b.stream_bytes) / b.stream_bytes if b.stream_bytes else 0.0
            log ('   * %-24s vs %-22s : encode %s, decode %s, size %+.2f%%'
                 % (c.label.strip(), b.label.strip(),
                    rel (b.enc['wall'] / c.enc['wall']), rel (b.dec['wall'] / c.dec['wall']), sz))
        a, b = by.get ('mt_tnbli'), by.get ('mt_fnbli')
        if a and b and not a.failed and not b.failed and a.enc and b.enc and b.stream_bytes:
            over = 100.0 * (a.stream_bytes - b.stream_bytes) / b.stream_bytes
            log ('   * splitting one image into %s costs %+.2f%% of file size and makes it %.2fx faster'
                 ' to encode and %.2fx faster to decode than the same codec in a single stream'
                 % (tiles_str (a.tiles), over, b.enc['wall'] / max (1e-9, a.enc['wall']),
                    b.dec['wall'] / max (1e-9, a.dec['wall'])))
        t1 = by.get ('mt_tnbli_1')
        if a and t1 and not a.failed and not t1.failed and t1.enc:
            log ('   * the tiled codec used %s threads : %.2fx over its own single thread run '
                 '(%.3f s -> %.3f s)'
                 % (args.threads or 'all', (t1.enc['wall'] + t1.dec['wall']) / (a.enc['wall'] + a.dec['wall']),
                    t1.enc['wall'] + t1.dec['wall'], a.enc['wall'] + a.dec['wall']))

    #---------------- batch against single, and why they differ
    if 'batch' in results and 'single' in results:
        byb = {c.id: c for c in results['batch']}
        bys = {c.id: c for c in results['single']}
        rows = []
        for cid, c in bys.items():
            b = byb.get (cid)
            if not b or b.failed or c.failed or not b.enc or not c.enc: continue
            bc = b.cpu_total(); bw = b.enc['wall'] + b.dec['wall']
            rows.append ([c.label, secs (b.enc['wall'] + b.dec['wall']),
                          secs (c.enc['wall'] + c.dec['wall']),
                          rel ((c.enc['wall'] + c.dec['wall']) / max (1e-9, b.enc['wall'] + b.dec['wall'])),
                          ('%.1f' % (bc / bw)) if (bc and bw > 0) else 'n/a'])
        if rows:
            log (); log (DLINE)
            log (' batch against single')
            log (DLINE)
            table (log, ['codec', 'batch s', 'single s', 'single/batch', 'cores in batch'], rows,
                   'l r r r r')
            log ()
            log ('   A codec that can work on several files at once is faster in BATCH (one process,')
            log ('   N files in parallel) than in SINGLE (N processes, one file each); a codec that')
            log ('   is single threaded anyway costs about the same in both -- the table above tells')
            log ('   the two apart without having to know which is which.')

    #---------------- one core against one core, on a machine that has AVX2
    if has_avx2():
        log ()
        log (' note : this CPU has AVX2, so the author\'s fNBLI takes its AVX2 path.  mtnbli never')
        log ('        does (that is the whole point of it), so the "per core" row above is the price')
        log ('        of running on a CPU that has no AVX2 -- not a slower implementation.')

    #---------------- correctness
    live = [c for cs in results.values() for c in cs if not c.failed and c.enc and c.dec]
    dead = [c for cs in results.values() for c in cs if c.failed]
    want = sum (c.n_img for c in live)
    got  = sum (c.n_ok  for c in live)
    log ()
    for c in dead:
        log (' %-24s produced no result at all : %s' % (c.label, c.failed))
    if got == want:
        log (' lossless : every one of the %d round trips came back bit identical to the source.' % want)
    else:
        log (' !! lossless : %d of %d round trips are NOT bit identical -- see the marks above.'
             % (want - got, want))
    if strict is not None:
        log (' strict pixel check : %s' % ('clean' if strict == 0 else '%d mismatch(es)' % strict))
    if compat:
        ok = sum (1 for c in compat if c['ok'])
        log (' interoperability : %d of %d cross decodes bit exact.%s'
             % (ok, len (compat), '' if ok == len (compat) else '   (that is a real problem)'))

    #---------------- honest caveats
    cpu_seen = [c for c in live if c.cpu_total()]
    log ()
    log (' caveats')
    if not cpu_seen:
        log ('   * !! the CPU time of the child processes could not be measured on this machine, so')
        log ('     "cpu s", "cores" and the whole core-seconds comparison are empty.  On POSIX that')
        log ('     number comes from os.times(); on Windows it comes from GetProcessTimes() on a')
        log ('     handle this script owns.  If both fail, install psutil (pip install psutil) -- it')
        log ('     is then sampled while the codec runs.')
    log ('   * the author codecs are single threaded by construction; the speed ups above are')
    log ('     therefore "one core against N cores" and not "a faster algorithm against a slower one".')
    if quota:
        log ('   * this machine reports %d hardware threads but may only use %.2f CPU (cgroup quota),'
             % (os.cpu_count() or 1, quota))
        log ('     so the multi threaded results here are a lower bound for real hardware.')
    log ('   * times are wall clock, best of %d run(s) in BATCH and of %d in SINGLE; they include'
         % (args.runs, args.single_runs))
    log ('     reading the source and writing the file, i.e. everything a user waits for.')
    log ('   * SINGLE mode pays one process start per file, which is why it is slower per byte.')
    log ('   * "bit exact" is a %s hash of the raw pixels plus the byte count, not a byte by'
         % HASH_NAME)
    log ('     byte comparison of two files : the decoded images are deleted as they are hashed.')
    if wine:
        log ('   * the binaries were Windows .exe files run through wine : subtract about %.2f s from'
             % min (v for v in startup.values() if v))
        log ('     every wall clock time before comparing them, or simply read the core-seconds.')

if __name__ == '__main__':
    try:
        main()
    finally:
        #  even if something above went wrong : never leave the intermediates behind
        for d in list (_WORK_DIRS):
            shutil_rmtree (d)
