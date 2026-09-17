// insight recorder, part 5: forensic watchpoints.
//
// Answers "which instruction touched this, with what values, called from where" without patching a single byte of game code:
//   - up to 4 hardware breakpoints (execute, data write, or data read/write) applied to every thread in the process, and
//   - a page journal that protects whole 4 KB pages: every write is reported with an exact word-level diff, and pages marked "access"
//     are made unreadable while the animation update and the frame boundary run, so every READ of them is reported too.
// Every recorded hit carries the faulting instruction, an unwound call stack, all 16 general registers, all 16 XMM registers, 256
// bytes of the thread's stack, 64 bytes behind every register that points at readable memory, and the bytes that changed.
// Hits are produced by a vectored exception handler on whichever thread did the access, buffered lock-free, and written into the
// recording by the main thread. Budgets (per watch, per instruction, per frame) keep hot writers from drowning the recording;
// writes that are let through unrecorded are still caught by a per-frame page diff ('P' records), so no change goes unseen.

constexpr int kWSlots = 4;      // x86-64 has four debug address registers
constexpr int kWRing = 4096;    // hits buffered between main-thread drains
constexpr int kWStack = 20;     // unwound frames kept per hit
constexpr int kWDiff = 24;      // changed 8-byte words kept per page-journal hit
constexpr int kWPages = 256;    // page table entries
constexpr int kWPagesArmed = 64;
constexpr int kWPend = 4;       // pages one instruction may fault on before its single step
constexpr int kWKeys = kWSlots + kWPages;
constexpr int kWRips = 8;       // distinct instructions budgeted per watch (the last entry collects the rest)
constexpr int kWStk = 256;
constexpr int kWRegMem = 64;
constexpr LONG kWFrameCap = 384;      // recorded hits per frame, all watches together
constexpr LONG kWRipBurst = 64;       // after this many records an instruction is sampled once every 32 frames
constexpr LONG kWPageExcCap = 96;     // exceptions one page may raise per frame before it is left open until the frame diff
constexpr LONG kWExcFrameCap = 4096;  // exceptions all pages may raise per frame
constexpr uintptr_t kWPageSize = 0x1000;

enum WKind : uint8_t { kWExec = 0, kWWrite = 1, kWPage = 2, kWAccess = 3, kWPageRead = 4 };

// A seek rebuilds the ragdoll somewhere else. A slot that follows a target pointer is re-pointed at the new address every frame
// instead of sitting on freed memory through exactly the moment we are trying to observe.
enum WFollow : uint8_t { kFollowNone = 0, kFollowCtrl = 1, kFollowBones = 2, kFollowPawn = 3 };

struct WHit {
  uint64_t stamp;  // written last: sequence + 1, so 0 means "never filled"
  double when;
  uint32_t frame, tid;
  uint8_t slot, kind, nStack, nDiff;
  uint8_t access;  // 0 read, 1 write, 3 hardware read-or-write, 8 execute; |0x80: the diff also holds earlier unrecorded writes
  uint8_t xmmOk;
  uint16_t stkN, regMask, diffTotal;
  uint64_t addr, rip;
  uint64_t gpr[16];
  uint64_t stack[kWStack];
  uint32_t dOff[kWDiff];
  uint64_t dOld[kWDiff], dNew[kWDiff];
  uint8_t mem[32];
  uint8_t xmm[256];
  uint8_t stk[kWStk];
  uint8_t regLen[16];
  uint8_t regMem[16 * kWRegMem];
};

struct WSlot {
  uintptr_t addr;
  uint8_t len;  // 1, 2, 4 or 8 bytes
  uint8_t rw;   // 0 execute, 1 write, 3 read or write
  bool on;      // the debug registers carry it
  bool pend;    // follows a target pointer that does not exist yet: armed by WRefollow as soon as it does
  bool body;    // picked by the body finder, replaced when it picks again
  // Optional filter: only record the hit when general register fReg holds fVal. Keeps an execute breakpoint on a function that every
  // entity calls from drowning the recording (and the frame rate) in hits that are not about our ragdoll.
  bool fOn;
  uint8_t fReg;
  uint64_t fVal;
  bool fTracksCtrl;  // the filter value is "the current animation controller"; nothing matches until it is known
  uint8_t follow;    // kFollowNone, or the target pointer this slot's address is an offset from
  uint32_t fOff;
  uint64_t hits;
  char label[64];
};

struct WPage {
  uintptr_t page;  // never changes while the entry can be faulted on (see WPageSlotFor)
  uint8_t* shadow;
  uint32_t objId;
  std::atomic<bool> armed, sat, dirty;
  bool acc;             // reads are journalled too while the access window is open
  volatile LONG lock;   // guards the shadow
  volatile LONG inflight;  // threads between their fault and their single step on this page
  volatile LONG exc;    // exceptions this frame
  uint64_t hits;
  char label[64];
};

WSlot g_wS[kWSlots];
WPage g_wJ[kWPages];
std::atomic<int> g_wJN{0};
uint8_t* g_wShadow = nullptr;
WHit* g_wRing = nullptr;
std::atomic<uint64_t> g_wPut{0};
uint64_t g_wTake = 0;
std::atomic<uint64_t> g_wDrops{0}, g_wHits{0}, g_wPageHits{0}, g_wExcTotal{0}, g_wSkipped{0}, g_wSatTotal{0}, g_wPRecs{0};
std::atomic<bool> g_wOn{false};
std::atomic<bool> g_wWin{false};  // the access window: animation update or frame boundary in progress
int g_wAccN = 0;                  // armed pages that trap reads inside the window
PVOID g_wVeh = nullptr;
uintptr_t g_wSelfLo = 0, g_wSelfHi = 0;
uint32_t g_wUntil = 0;    // frame at which everything disarms (0 = stay armed)
uint32_t g_wRearmAt = 0;  // next frame the hardware slots are re-applied, to catch threads created since
int g_wThreads = 0;
bool g_wForensic = false;     // the standard set is armed: keep it pointed at the live ragdoll as the demo rebuilds it
uint32_t g_wPlayUntil = 0;    // while the demo is playing the page journal only runs this long (see WPageGate)
uint32_t g_wPageScanAt = 0;   // next frame the journalled page set is recomputed
bool g_wEdgePlaying = false, g_wEdgePaused = false;
constexpr uint32_t kWPagePlay = 64;   // frames of playback the journal survives past an unpause - the snap happens here
constexpr uint32_t kWPageScan = 60;

// Budgets. Counters are bumped from any thread; the main thread resets them.
volatile LONG g_wFrameRec = 0, g_wExcFrame = 0;
LONG g_wKeyRec[kWKeys];
LONG64 g_wRipAddr[kWKeys][kWRips];
LONG g_wRipNow[kWKeys][kWRips], g_wRipLife[kWKeys][kWRips];

// P record scratch (main thread)
uint32_t g_wPOff[512];
uint64_t g_wPOld[512], g_wPNew[512];

// Per-thread handler state.
__declspec(thread) int t_wVeh = 0;    // inside our own handler: accesses we cause are not journalled
__declspec(thread) int t_wQuiet = 0;  // the recorder itself is reading: heal, never record
__declspec(thread) int t_wRetry = 0;
__declspec(thread) bool t_wTf = false;  // we set the trap flag and owe ourselves a single step
__declspec(thread) int t_wPendN = 0;
__declspec(thread) int t_wPendPage[kWPend];
__declspec(thread) bool t_wPendRec[kWPend];
__declspec(thread) bool t_wPendWrite[kWPend];
__declspec(thread) WHit t_wPendHit[kWPend];
__declspec(thread) WHit t_wHit;

bool WInSelf(uintptr_t a) { return a >= g_wSelfLo && a < g_wSelfHi; }

// ---- stack unwinding

// Proper table-driven unwind from an exception context. POD only: this function uses __try.
uint32_t WStack(const CONTEXT* src, uint64_t* out, uint32_t max) {
  CONTEXT c = *src;
  uint32_t n = 0;
  __try {
    while (n < max && c.Rip) {
      out[n++] = c.Rip;
      DWORD64 base = 0;
      PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
      if (rf) {
        PVOID handler = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &handler, &establisher, nullptr);
      } else {
        if (!InsFind((uintptr_t)c.Rsp)) break;  // leaf function: the return address is on top of the stack
        c.Rip = *(DWORD64*)c.Rsp;
        c.Rsp += 8;
      }
      const InsRegion* r = InsFind((uintptr_t)c.Rip);
      if (!r || !r->x) break;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

void WGpr(const CONTEXT* c, uint64_t* g) {
  g[0] = c->Rax;
  g[1] = c->Rcx;
  g[2] = c->Rdx;
  g[3] = c->Rbx;
  g[4] = c->Rsp;
  g[5] = c->Rbp;
  g[6] = c->Rsi;
  g[7] = c->Rdi;
  g[8] = c->R8;
  g[9] = c->R9;
  g[10] = c->R10;
  g[11] = c->R11;
  g[12] = c->R12;
  g[13] = c->R13;
  g[14] = c->R14;
  g[15] = c->R15;
}

// Bytes readable at a without faulting, up to cap: committed, not guard, not no-access.
uint32_t WReadable(uintptr_t a, uint32_t cap) {
  if (a < 0x10000 || a >= 0x7FFFFFFFFFFFull) return 0;
  MEMORY_BASIC_INFORMATION mi;
  if (VirtualQuery((LPCVOID)a, &mi, sizeof(mi)) != sizeof(mi) || mi.State != MEM_COMMIT) return 0;
  if (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return 0;
  const DWORD p = mi.Protect & 0xff;
  if (!(p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
    return 0;
  const uintptr_t end = (uintptr_t)mi.BaseAddress + mi.RegionSize;
  return uint32_t((std::min)(uintptr_t(cap), end - a));
}

// ---- hit ring

void WPush(const WHit& src) {
  if (!g_wRing) return;
  const uint64_t s = g_wPut.fetch_add(1, std::memory_order_relaxed);
  WHit& h = g_wRing[s % kWRing];
  memcpy(&h, &src, sizeof(WHit));
  ((std::atomic<uint64_t>*)&h.stamp)->store(s + 1, std::memory_order_relaxed);
  g_wHits.fetch_add(1, std::memory_order_relaxed);
}

void WFill(WHit& h, uint8_t kind, uint8_t slot, uint8_t access, uintptr_t addr, const CONTEXT* c) {
  memset(&h, 0, sizeof(h));
  h.when = Now();
  h.frame = g_insFrame;
  h.tid = GetCurrentThreadId();
  h.slot = slot;
  h.kind = kind;
  h.access = access;
  h.addr = addr;
  h.rip = c->Rip;
  WGpr(c, h.gpr);
  h.nStack = (uint8_t)WStack(c, h.stack, kWStack);
  if (addr) SafeCopy(h.mem, (const void*)addr, sizeof(h.mem));
  if ((c->ContextFlags & CONTEXT_FLOATING_POINT) == CONTEXT_FLOATING_POINT) {
    memcpy(h.xmm, &c->Xmm0, sizeof(h.xmm));
    h.xmmOk = 1;
  }
  const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
  const uintptr_t sp = c->Rsp, hi = (uintptr_t)tib->StackBase;
  if (sp >= (uintptr_t)tib->StackLimit && sp < hi) {
    const uint32_t n = uint32_t((std::min)(uintptr_t(kWStk), hi - sp));
    if (SafeCopy(h.stk, (const void*)sp, n)) h.stkN = (uint16_t)n;
  }
  for (int i = 0; i < 16; ++i) {
    if (i == 4) continue;  // rsp: the stack bytes above
    const uint32_t n = WReadable((uintptr_t)h.gpr[i], kWRegMem);
    if (!n || !SafeCopy(h.regMem + i * kWRegMem, (const void*)h.gpr[i], n)) continue;
    h.regLen[i] = (uint8_t)n;
    h.regMask |= uint16_t(1u << i);
  }
}

// ---- budgets

bool WBudget(int key, uint64_t rip, LONG keyCap, LONG ripCap) {
  if (g_wFrameRec >= kWFrameCap || g_wKeyRec[key] >= keyCap) {
    g_wSkipped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  int e = kWRips - 1;
  for (int i = 0; i < kWRips - 1; ++i) {
    const LONG64 cur = g_wRipAddr[key][i];
    if (cur == (LONG64)rip) {
      e = i;
      break;
    }
    if (!cur) {
      const LONG64 prev = InterlockedCompareExchange64(&g_wRipAddr[key][i], (LONG64)rip, 0);
      if (!prev || prev == (LONG64)rip) {
        e = i;
        break;
      }
    }
  }
  const LONG now = InterlockedIncrement(&g_wRipNow[key][e]);
  const LONG cap = e == kWRips - 1 ? ripCap * 2 : ripCap;
  if (now > cap || (g_wRipLife[key][e] >= kWRipBurst && (now > 1 || (g_insFrame & 31)))) {
    g_wSkipped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  InterlockedIncrement(&g_wRipLife[key][e]);
  InterlockedIncrement(&g_wKeyRec[key]);
  InterlockedIncrement(&g_wFrameRec);
  return true;
}

void WBudgetFrame() {
  memset(g_wKeyRec, 0, sizeof(g_wKeyRec));
  memset(g_wRipNow, 0, sizeof(g_wRipNow));
  g_wFrameRec = 0;
  g_wExcFrame = 0;
}

// Fresh sample budget for every instruction: called on pause, unpause, seeks and body-finder milestones.
void WBudgetEdge() {
  memset(g_wRipAddr, 0, sizeof(g_wRipAddr));
  memset(g_wRipLife, 0, sizeof(g_wRipLife));
}

// ---- page journal

int WPageFind(uintptr_t a) {
  const uintptr_t p = a & ~(kWPageSize - 1);
  const int n = g_wJN.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i)
    if (g_wJ[i].page == p) return i;
  return -1;
}

// Declared in part 1: lets the region map treat pages we protected as the ordinary heap they are.
bool WPageOurs(uintptr_t a) {
  const int i = WPageFind(a);
  return i >= 0 && (g_wJ[i].armed.load(std::memory_order_relaxed) || g_wJ[i].inflight > 0);
}

DWORD WPageWant(int i) {
  const WPage& p = g_wJ[i];
  if (!p.armed.load(std::memory_order_relaxed) || p.sat.load(std::memory_order_relaxed)) return PAGE_READWRITE;
  return p.acc && g_wWin.load(std::memory_order_relaxed) ? PAGE_NOACCESS : PAGE_READONLY;
}

void WProtect(int i, DWORD prot) {
  DWORD old = 0;
  VirtualProtect((LPVOID)g_wJ[i].page, kWPageSize, prot, &old);
}

void WLock(WPage& p) {
  while (InterlockedCompareExchange(&p.lock, 1, 0)) YieldProcessor();
}

void WUnlock(WPage& p) { InterlockedExchange(&p.lock, 0); }

// Word-level diff of a journalled page against its shadow; brings the shadow up to date. Handler-safe. POD only: uses __try.
uint32_t WPageDiff(uintptr_t page, uint8_t* sh, uint32_t* off, uint64_t* oldv, uint64_t* newv, uint32_t cap, uint32_t* total) {
  uint32_t n = 0, t = 0;
  __try {
    const uint64_t* cur = (const uint64_t*)page;
    uint64_t* s = (uint64_t*)sh;
    for (uint32_t k = 0; k < kWPageSize / 8; ++k) {
      const uint64_t a = cur[k];
      if (a == s[k]) continue;
      if (n < cap) {
        off[n] = k * 8;
        oldv[n] = s[k];
        newv[n] = a;
        ++n;
      }
      s[k] = a;
      ++t;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  *total = t;
  return n;
}

// ---- the handler

void WFinishPending() {
  for (int k = 0; k < t_wPendN; ++k) {
    const int j = t_wPendPage[k];
    WPage& p = g_wJ[j];
    if (t_wPendWrite[k]) {
      if (t_wPendRec[k]) {
        WHit& h = t_wPendHit[k];
        WLock(p);
        const bool earlier = p.dirty.exchange(false);
        uint32_t total = 0;
        h.nDiff = (uint8_t)WPageDiff(p.page, p.shadow, h.dOff, h.dOld, h.dNew, kWDiff, &total);
        WUnlock(p);
        h.diffTotal = (uint16_t)total;
        if (earlier) h.access |= 0x80;
      } else {
        p.dirty = true;
      }
    }
    if (t_wPendRec[k]) {
      WPush(t_wPendHit[k]);
      g_wPageHits.fetch_add(1, std::memory_order_relaxed);
      ++p.hits;
    }
    LONG left = InterlockedDecrement(&p.inflight);
    if (left < 0) {
      InterlockedCompareExchange(&p.inflight, 0, left);
      left = 0;
    }
    if (!left) WProtect(j, WPageWant(j));
  }
  t_wPendN = 0;
}

LONG WOnStep(CONTEXT* c) {
  const bool hw = (c->Dr6 & 0xf) != 0;
  if (t_wVeh) {  // our own handler read a watched address
    if (!hw) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    c->EFlags |= 0x10000;
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  if (!hw && !t_wPendN && !t_wTf) return EXCEPTION_CONTINUE_SEARCH;
  ++t_wVeh;
  if (hw) {
    // Data breakpoints are traps (Rip is the instruction after the access); execute breakpoints are faults and need RF to resume.
    uint64_t g[16];
    WGpr(c, g);
    for (int i = 0; i < kWSlots; ++i) {
      if (!((c->Dr6 >> i) & 1)) continue;
      WSlot& s = g_wS[i];
      if (!s.on) continue;
      if (s.fOn && (!s.fVal || g[s.fReg & 15] != s.fVal)) continue;
      if (t_wQuiet || WInSelf(c->Rip) || !WBudget(i, c->Rip, 48, 6)) continue;
      const uint8_t kind = s.rw == 0 ? kWExec : (s.rw == 1 ? kWWrite : kWAccess);
      const uint8_t access = s.rw == 0 ? 8 : (s.rw == 1 ? 1 : 3);
      WFill(t_wHit, kind, (uint8_t)i, access, s.rw == 0 ? c->Rip : s.addr, c);
      WPush(t_wHit);
      ++s.hits;
    }
    c->Dr6 = 0;
    c->EFlags |= 0x10000;
  }
  if (t_wPendN) WFinishPending();
  if (t_wTf) {
    c->EFlags &= ~0x100u;
    t_wTf = false;
  }
  --t_wVeh;
  return EXCEPTION_CONTINUE_EXECUTION;
}

LONG WOnAccess(EXCEPTION_POINTERS* ep, CONTEXT* c) {
  const EXCEPTION_RECORD* er = ep->ExceptionRecord;
  if (er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
  const ULONG_PTR type = er->ExceptionInformation[0];
  if (type != 0 && type != 1) return EXCEPTION_CONTINUE_SEARCH;  // 8 = execute (DEP): never ours
  const uintptr_t at = (uintptr_t)er->ExceptionInformation[1];
  const int j = WPageFind(at);
  if (j < 0 || t_wVeh) return EXCEPTION_CONTINUE_SEARCH;
  const bool write = type == 1;
  WPage& p = g_wJ[j];
  MEMORY_BASIC_INFORMATION mi;
  if (VirtualQuery((LPCVOID)p.page, &mi, sizeof(mi)) != sizeof(mi) || mi.State != MEM_COMMIT) return EXCEPTION_CONTINUE_SEARCH;
  const DWORD prot = mi.Protect;
  if (prot != PAGE_READONLY && prot != PAGE_READWRITE && prot != PAGE_NOACCESS) return EXCEPTION_CONTINUE_SEARCH;
  if (prot == PAGE_READWRITE || (prot == PAGE_READONLY && !write)) {
    // Another thread opened the page between the fault and now: retry. The write goes through unrecorded, so flag the page.
    if (write) p.dirty = true;
    if (++t_wRetry > 64) {
      t_wRetry = 0;
      return EXCEPTION_CONTINUE_SEARCH;
    }
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  t_wRetry = 0;
  for (int k = 0; k < t_wPendN; ++k)
    if (t_wPendPage[k] == j) {  // same instruction faulting again after another thread re-protected: just reopen
      WProtect(j, PAGE_READWRITE);
      return EXCEPTION_CONTINUE_EXECUTION;
    }
  if (!p.armed.load(std::memory_order_relaxed)) {  // dropped while protected: heal, never crash the game
    WProtect(j, PAGE_READWRITE);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  const DWORD want = WPageWant(j);
  if (want == PAGE_READWRITE || (want == PAGE_READONLY && !write)) {  // the window closed or the page saturated meanwhile
    WProtect(j, p.inflight > 0 ? PAGE_READWRITE : want);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  g_wExcTotal.fetch_add(1, std::memory_order_relaxed);
  const LONG exc = InterlockedIncrement(&p.exc);
  const LONG all = InterlockedIncrement(&g_wExcFrame);
  if (exc > kWPageExcCap || all > kWExcFrameCap || t_wPendN >= kWPend) {
    // Too hot to single-step: leave the page open for the rest of the frame; the frame diff ('P' record) still shows every change.
    if (!p.sat.exchange(true)) g_wSatTotal.fetch_add(1, std::memory_order_relaxed);
    WProtect(j, PAGE_READWRITE);
    return EXCEPTION_CONTINUE_EXECUTION;
  }
  ++t_wVeh;
  const bool rec = !t_wQuiet && !WInSelf(c->Rip) && WBudget(kWSlots + j, c->Rip, 24, 4);
  InterlockedIncrement(&p.inflight);
  DWORD old = 0;
  if (!VirtualProtect((LPVOID)p.page, kWPageSize, PAGE_READWRITE, &old)) {
    InterlockedDecrement(&p.inflight);
    --t_wVeh;
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const int k = t_wPendN++;
  t_wPendPage[k] = j;
  t_wPendRec[k] = rec;
  t_wPendWrite[k] = write;
  if (rec) WFill(t_wPendHit[k], write ? kWPage : kWPageRead, (uint8_t)j, write ? 1 : 0, at, c);  // mem = the bytes before a write
  --t_wVeh;
  c->EFlags |= 0x100u;  // let this one access through, then single-step back into the handler and re-protect
  t_wTf = true;
  return EXCEPTION_CONTINUE_EXECUTION;
}

LONG CALLBACK WVeh(EXCEPTION_POINTERS* ep) {
  const DWORD code = ep->ExceptionRecord->ExceptionCode;
  if (code != EXCEPTION_SINGLE_STEP && code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
  if (!g_wOn.load(std::memory_order_relaxed)) return EXCEPTION_CONTINUE_SEARCH;
  return code == EXCEPTION_SINGLE_STEP ? WOnStep(ep->ContextRecord) : WOnAccess(ep, ep->ContextRecord);
}

// ---- hardware slots

uint64_t WDr7() {
  uint64_t d = 0;
  for (int i = 0; i < kWSlots; ++i) {
    if (!g_wS[i].on) continue;
    d |= 1ull << (2 * i);  // local enable
    d |= uint64_t(g_wS[i].rw) << (16 + 4 * i);
    uint64_t len = 3;  // 4 bytes
    if (g_wS[i].rw == 0)
      len = 0;  // execute breakpoints are always 1 byte
    else if (g_wS[i].len == 1)
      len = 0;
    else if (g_wS[i].len == 2)
      len = 1;
    else if (g_wS[i].len == 8)
      len = 2;
    d |= len << (18 + 4 * i);
  }
  return d ? (d | 0x100) : 0;  // LE
}

// Writes the debug registers of every thread in this process. Returns the number of threads updated.
int WApplyAll() {
  const DWORD me = GetCurrentThreadId(), pid = GetCurrentProcessId();
  const uint64_t dr7 = WDr7();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  int n = 0;
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != pid) continue;
      HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
      if (!th) continue;
      const bool self = te.th32ThreadID == me;
      if (!self) SuspendThread(th);
      CONTEXT c;
      memset(&c, 0, sizeof(c));
      c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext(th, &c)) {
        c.Dr0 = g_wS[0].on ? g_wS[0].addr : 0;
        c.Dr1 = g_wS[1].on ? g_wS[1].addr : 0;
        c.Dr2 = g_wS[2].on ? g_wS[2].addr : 0;
        c.Dr3 = g_wS[3].on ? g_wS[3].addr : 0;
        c.Dr6 = 0;
        c.Dr7 = dr7;
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (SetThreadContext(th, &c)) ++n;
      }
      if (!self) ResumeThread(th);
      CloseHandle(th);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  g_wThreads = n;
  return n;
}

bool WEnsure() {
  if (!g_wRing) g_wRing = (WHit*)InsVAlloc(sizeof(WHit) * kWRing);
  if (!g_wShadow) g_wShadow = (uint8_t*)InsVAlloc(size_t(kWPages) * kWPageSize);
  if (!g_wRing || !g_wShadow) return false;
  if (!g_wSelfHi) {
    MODULEINFO mi{};
    if (K32GetModuleInformation(GetCurrentProcess(), g_self, &mi, sizeof(mi))) {
      g_wSelfLo = (uintptr_t)mi.lpBaseOfDll;
      g_wSelfHi = g_wSelfLo + mi.SizeOfImage;
    }
  }
  if (!g_wVeh) g_wVeh = AddVectoredExceptionHandler(1, WVeh);
  g_wOn = g_wVeh != nullptr;
  return g_wVeh != nullptr;
}

int WSlotFree() {
  for (int i = 0; i < kWSlots; ++i)
    if (!g_wS[i].on && !g_wS[i].pend) return i;
  return -1;
}

uintptr_t WFollowBase(uint8_t follow) {
  return follow == kFollowCtrl ? g_insT.ctrl : follow == kFollowBones ? g_insT.bones : follow == kFollowPawn ? g_insT.pawn : 0;
}

const char* WRwName(uint8_t rw) { return rw == 0 ? "exec" : (rw == 3 ? "access" : "write"); }

// A following slot whose target does not exist yet is kept pending and armed by WRefollow the moment it appears.
bool WArm(uintptr_t addr, uint8_t len, uint8_t rw, const char* label, int fReg = -1, uint64_t fVal = 0, uint8_t follow = kFollowNone,
          uint32_t fOff = 0, bool tracksCtrl = false, bool body = false) {
  if (!WEnsure()) return false;
  if (rw == 0) len = 1;
  if (len != 1 && len != 2 && len != 4 && len != 8) return false;
  const uintptr_t base = WFollowBase(follow);
  const bool pend = follow != kFollowNone && !base;
  if (follow != kFollowNone && base) addr = base + fOff;
  if (!pend && !addr) return false;
  if (!pend && rw != 0 && (addr & (len - 1))) return false;  // data breakpoints must be aligned to their length
  const int i = WSlotFree();
  if (i < 0) return false;
  WSlot& s = g_wS[i];
  s.addr = pend ? 0 : addr;
  s.len = len;
  s.rw = rw;
  s.fTracksCtrl = tracksCtrl;
  s.fOn = tracksCtrl || (fReg >= 0 && fReg < 16);
  s.fReg = uint8_t(tracksCtrl ? 1 : (s.fOn ? fReg : 0));
  s.fVal = tracksCtrl ? (uint64_t)g_insT.ctrl : fVal;
  s.follow = follow;
  s.fOff = fOff;
  s.body = body;
  s.hits = 0;
  strncpy_s(s.label, label ? label : "", _TRUNCATE);
  s.pend = pend;
  s.on = !pend;
  if (pend) {
    InsMark("WATCH pending %s %s len %d (slot %d): armed when its target appears", WRwName(rw), s.label, len, i);
    return true;
  }
  InsRefreshRegions(false);
  WApplyAll();
  InsMark("WATCH arm %s %s %llx len %d (slot %d, %d threads)%s", WRwName(rw), s.label, (unsigned long long)addr, len, i, g_wThreads,
          s.fOn ? " filtered" : "");
  return true;
}

void WSlotDisarm(int i, const char* why) {
  WSlot& s = g_wS[i];
  if (!s.on && !s.pend) return;
  const bool wasOn = s.on;
  s.on = s.pend = false;
  if (wasOn) WApplyAll();
  InsMark("WATCH slot %d off (%s): %s, %llu hits", i, why, s.label, (unsigned long long)s.hits);
}

// Page table entries keep their page while any thread could still fault on it. An entry is only handed to a different page once it is
// disarmed, idle, and its old page is back to plain read/write (or gone).
int WPageSlotFor(uintptr_t page) {
  const int n = g_wJN.load();
  for (int k = 0; k < n; ++k)
    if (g_wJ[k].page == page) return k;
  if (n < kWPages) {
    g_wJ[n].page = page;
    g_wJN = n + 1;
    return n;
  }
  for (int k = 0; k < n; ++k) {
    WPage& p = g_wJ[k];
    if (p.armed || p.inflight) continue;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((LPCVOID)p.page, &mi, sizeof(mi)) == sizeof(mi) && mi.State == MEM_COMMIT && mi.Protect != PAGE_READWRITE &&
        (mi.Protect == PAGE_READONLY || mi.Protect == PAGE_NOACCESS))
      continue;
    p.page = page;
    return k;
  }
  return -1;
}

bool WPageArm(uintptr_t addr, uint32_t objId, const char* label, bool acc = false) {
  if (!WEnsure()) return false;
  const uintptr_t page = addr & ~(kWPageSize - 1);
  const int n = g_wJN.load();
  for (int k = 0; k < n; ++k) {
    WPage& p = g_wJ[k];
    if (p.page != page || !p.armed) continue;
    if (objId && !p.objId) p.objId = objId;
    if (acc != p.acc) {
      p.acc = acc;
      if (!p.inflight) WProtect(k, WPageWant(k));
    }
    return true;
  }
  if (InsOwn(page) || WInSelf(page)) return false;
  MEMORY_BASIC_INFORMATION mi;
  if (VirtualQuery((LPCVOID)page, &mi, sizeof(mi)) != sizeof(mi) || mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE ||
      mi.Protect != PAGE_READWRITE)
    return false;  // private plain read/write data only
  const int j = WPageSlotFor(page);
  if (j < 0) return false;
  WPage& p = g_wJ[j];
  p.shadow = g_wShadow + size_t(j) * kWPageSize;
  p.objId = objId;
  strncpy_s(p.label, label ? label : "", _TRUNCATE);
  p.acc = acc;
  p.sat = false;
  p.dirty = false;
  p.exc = 0;
  p.hits = 0;
  p.armed = true;
  DWORD old = 0;
  if (!VirtualProtect((LPVOID)page, kWPageSize, PAGE_READONLY, &old)) {  // freeze first, then take the shadow
    p.armed = false;
    return false;
  }
  ++t_wVeh;
  const bool ok = SafeCopy(p.shadow, (const void*)page, kWPageSize);
  --t_wVeh;
  if (!ok) {
    p.armed = false;
    WProtect(j, PAGE_READWRITE);
    return false;
  }
  if (WPageWant(j) != PAGE_READONLY) WProtect(j, WPageWant(j));
  InsMark("WATCH page %d %llx%s #%u %s", j, (unsigned long long)page, acc ? " ACCESS" : "", objId, p.label);
  return true;
}

bool WPageDrop(int i) {
  if (i < 0 || i >= g_wJN.load() || !g_wJ[i].armed) return false;
  WPage& p = g_wJ[i];
  p.armed = false;
  p.sat = false;
  p.dirty = false;
  if (p.inflight <= 0) WProtect(i, PAGE_READWRITE);  // otherwise the thread in flight opens it on its single step
  return true;
}

int WPagesOn() {
  int n = 0;
  const int N = g_wJN.load();
  for (int i = 0; i < N; ++i)
    if (g_wJ[i].armed) ++n;
  return n;
}

// The access window: while the animation update or the frame boundary runs, "access" pages trap reads as well as writes. Main thread.
void InsWindow(bool open) {
  g_wWin.store(open, std::memory_order_relaxed);
  if (!g_wAccN) return;
  const int n = g_wJN.load(std::memory_order_relaxed);
  for (int j = 0; j < n; ++j) {
    const WPage& p = g_wJ[j];
    if (!p.acc || !p.armed.load(std::memory_order_relaxed) || p.sat.load(std::memory_order_relaxed) || p.inflight) continue;
    WProtect(j, WPageWant(j));
  }
}

void WDisarm(const char* why) {
  bool any = false;
  for (int i = 0; i < kWSlots; ++i) {
    if (g_wS[i].on || g_wS[i].pend) any = true;
    g_wS[i].on = g_wS[i].pend = false;
  }
  if (any) WApplyAll();
  const int n = g_wJN.load();
  for (int i = 0; i < n; ++i)
    if (WPageDrop(i)) any = true;
  g_wAccN = 0;
  g_wForensic = false;
  g_wUntil = 0;
  if (any)
    InsMark("WATCH off (%s) - %llu hits, %llu page hits, %llu page exceptions, %llu skipped by budget, %llu dropped", why,
            (unsigned long long)g_wHits.load(), (unsigned long long)g_wPageHits.load(), (unsigned long long)g_wExcTotal.load(),
            (unsigned long long)g_wSkipped.load(), (unsigned long long)g_wDrops.load());
}

bool WAnyArmed() {
  for (int i = 0; i < kWSlots; ++i)
    if (g_wS[i].on || g_wS[i].pend) return true;
  return WPagesOn() > 0;
}

// ---- module list, so the reader can turn every captured address into module+offset

// Re-emits the module table ('L' records, same layout the recording header writes) so addresses captured after a late module load
// can still be named.
void WWriteModules() {
  HMODULE mods[1024];
  DWORD need = 0;
  if (!K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) return;
  for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
    MODULEINFO mi{};
    char name[MAX_PATH];
    if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) ||
        !K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH))
      continue;
    const size_t at = InsBegin('L');
    InsPod((uint64_t)(uintptr_t)mi.lpBaseOfDll);
    InsPod((uint32_t)mi.SizeOfImage);
    InsStr(name);
    InsEnd(at);
  }
}

// ---- draining into the recording

void WDrain() {
  if (!g_wRing) return;
  const uint64_t put = g_wPut.load(std::memory_order_acquire);
  if (put - g_wTake > kWRing) {
    g_wDrops.fetch_add(put - g_wTake - kWRing, std::memory_order_relaxed);
    g_wTake = put - kWRing;
  }
  for (; g_wTake < put; ++g_wTake) {
    const WHit& h = g_wRing[g_wTake % kWRing];
    if (((std::atomic<uint64_t>*)&h.stamp)->load(std::memory_order_acquire) != g_wTake + 1) continue;
    if (!g_insRec) continue;
    const size_t at = InsBegin('W');
    InsPod(g_wTake);
    InsPod(h.when);
    InsPod(h.frame);
    InsPod(h.tid);
    InsPod(h.slot);
    InsPod(h.kind);
    InsPod(h.addr);
    InsPod(h.rip);
    InsPut(h.gpr, sizeof(h.gpr));
    const uint8_t ns = (std::min)(h.nStack, (uint8_t)kWStack);
    InsPod(ns);
    InsPut(h.stack, size_t(ns) * 8);
    const uint8_t nd = (std::min)(h.nDiff, (uint8_t)kWDiff);
    InsPod(nd);
    for (int i = 0; i < nd; ++i) {
      InsPod(h.dOff[i]);
      InsPod(h.dOld[i]);
      InsPod(h.dNew[i]);
    }
    InsPut(h.mem, sizeof(h.mem));
    // format 2
    InsPod(h.access);
    InsPod(h.xmmOk);
    if (h.xmmOk) InsPut(h.xmm, sizeof(h.xmm));
    const uint16_t sn = (std::min)(h.stkN, (uint16_t)kWStk);
    InsPod(sn);
    InsPut(h.stk, sn);
    InsPod(h.regMask);
    for (int i = 0; i < 16; ++i) {
      if (!((h.regMask >> i) & 1)) continue;
      const uint8_t rl = (std::min)(h.regLen[i], (uint8_t)kWRegMem);
      InsPod(rl);
      InsPut(h.regMem + i * kWRegMem, rl);
    }
    InsPod(h.diffTotal);
    InsEnd(at);
  }
}

// Once per frame: pages that were left open (saturated) or let writes through unrecorded get a full word diff against their shadow,
// so the recording still holds every value change, then go back under protection.
void WPageTick() {
  const int n = g_wJN.load();
  ++t_wVeh;  // our reads below are not journal events
  for (int j = 0; j < n; ++j) {
    WPage& p = g_wJ[j];
    const LONG exc = InterlockedExchange(&p.exc, 0);
    if (!p.armed) continue;
    const bool sat = p.sat, dirty = p.dirty;
    if (!sat && !dirty) continue;
    WLock(p);
    p.dirty = false;
    uint32_t total = 0;
    const uint32_t nd = WPageDiff(p.page, p.shadow, g_wPOff, g_wPOld, g_wPNew, 512, &total);
    WUnlock(p);
    p.sat = false;
    if (!p.inflight) WProtect(j, WPageWant(j));
    if (!g_insRec || !total) continue;
    const size_t at = InsBegin('P');
    InsPod(g_insFrame);
    InsPod((uint64_t)p.page);
    InsPod(p.objId);
    InsPod(uint8_t((sat ? 1 : 0) | (dirty ? 2 : 0) | (p.acc ? 4 : 0)));
    InsPod((uint32_t)exc);
    InsPod(total);
    InsPod(nd);
    for (uint32_t k = 0; k < nd; ++k) {
      InsPod(g_wPOff[k]);
      InsPod(g_wPOld[k]);
      InsPod(g_wPNew[k]);
    }
    InsEnd(at);
    g_wPRecs.fetch_add(1, std::memory_order_relaxed);
  }
  --t_wVeh;
}

// Journal priority: body windows the body finder picked (the physics bodies themselves, and where their values are copied to), then
// the bone transforms, the animation controller, and ragdoll/physics/pose objects owned by the pawn or controller.
int WPagePrio(const InsObj& o) {
  if (o.kind == kObjBody) return strstr(o.label, "body step") ? 0 : 1;
  if (o.kind == kObjBones) return 2;
  if (o.kind == kObjCtrl) return 3;
  if ((o.root == kRootPawn || o.root == kRootCtrl) && (o.kind == kObjVt || o.kind == kObjBlob)) {
    static const char* const kKeys[] = {"Ragdoll", "Phys", "Aggregate", "Body", "Pose", "Skeleton", "Anim"};
    for (const char* k : kKeys)
      if (strstr(o.label, k)) return 4;
  }
  return -1;
}

// Journals the pages of the objects above (at most kWPagesArmed, best first), and releases pages whose object is gone or was
// outranked. Run again whenever the demo rebuilds the ragdoll, so the journal keeps pointing at the live one.
int WForensicPages() {
  struct Want {
    uintptr_t page;
    uint32_t objId;
    bool acc;
    const char* label;
  };
  static Want want[kWPagesArmed];
  int nw = 0;
  for (int prio = 0; prio <= 4 && nw < kWPagesArmed; ++prio) {
    for (int i = 0; i < kInsMaxObjs && nw < kWPagesArmed; ++i) {
      const InsObj& o = g_insObj[i];
      if (!o.live || WPagePrio(o) != prio) continue;
      for (uintptr_t a = o.addr & ~(kWPageSize - 1); a < o.addr + o.size && nw < kWPagesArmed; a += kWPageSize) {
        int k = 0;
        while (k < nw && want[k].page != a) ++k;
        if (k < nw) {
          want[k].acc = want[k].acc || prio == 0;
          continue;
        }
        want[nw++] = {a, o.id, prio == 0, o.label};
      }
    }
  }
  const int n = g_wJN.load();
  for (int j = 0; j < n; ++j) {
    if (!g_wJ[j].armed || !g_wJ[j].objId) continue;  // console pages stay until 'watch off'
    bool keep = false;
    for (int k = 0; k < nw && !keep; ++k) keep = want[k].page == g_wJ[j].page;
    if (!keep) WPageDrop(j);
  }
  int armed = 0;
  for (int k = 0; k < nw; ++k)
    if (WPageArm(want[k].page, want[k].objId, want[k].label, want[k].acc)) ++armed;
  int acc = 0;
  const int n2 = g_wJN.load();
  for (int j = 0; j < n2; ++j)
    if (g_wJ[j].armed && g_wJ[j].acc) ++acc;
  g_wAccN = acc;
  return armed;
}

// Re-points every following slot at the target's current address (arming pending ones), and keeps controller filters current.
bool WRefollow() {
  bool apply = false;
  for (int i = 0; i < kWSlots; ++i) {
    WSlot& sl = g_wS[i];
    if (!sl.on && !sl.pend) continue;
    if (sl.fTracksCtrl && sl.fVal != (uint64_t)g_insT.ctrl) {
      sl.fVal = (uint64_t)g_insT.ctrl;
      InsMark("WATCH slot %d now only reports calls for controller %llx", i, (unsigned long long)g_insT.ctrl);
    }
    const uintptr_t base = WFollowBase(sl.follow);
    if (!sl.follow || !base) continue;
    const uintptr_t want = base + sl.fOff;
    if (sl.rw != 0 && (want & (sl.len - 1))) continue;
    if (sl.pend) {
      sl.addr = want;
      sl.pend = false;
      sl.on = true;
      apply = true;
      InsMark("WATCH slot %d armed now that its target exists: %s %s %llx len %d", i, WRwName(sl.rw), sl.label,
              (unsigned long long)want, sl.len);
      continue;
    }
    if (want == sl.addr) continue;
    InsMark("WATCH slot %d follows %s: %llx -> %llx", i, sl.label, (unsigned long long)sl.addr, (unsigned long long)want);
    sl.addr = want;
    apply = true;
  }
  return apply;
}

// Protecting pages costs one exception per access. While the demo is paused that is a handful and it tells us exactly who touches
// the ragdoll; during playback it is thousands per frame. So the journal runs while paused and for a window past the unpause - where
// the snap happens - and is released for the rest of playback. The hardware breakpoints are cheap and stay armed throughout.
void WPageGate() {
  if (!g_wForensic) return;
  const bool paused = !g_insPrevPlaying || g_insPrevPaused;
  if (paused) {
    if (g_wPlayUntil) {
      g_wPlayUntil = 0;
      g_wPageScanAt = 0;  // paused again: put the journal straight back on
    }
    if (g_insFrame >= g_wPageScanAt) {
      g_wPageScanAt = g_insFrame + kWPageScan;
      const int before = WPagesOn();
      const int n = WForensicPages();
      if (WPagesOn() != before) InsMark("WATCH journal on, %d pages (%d access), %d armed or kept", WPagesOn(), g_wAccN, n);
    }
    return;
  }
  if (!g_wPlayUntil) {
    g_wPlayUntil = g_insFrame + kWPagePlay;
    g_wPageScanAt = 0;
  }
  if (g_insFrame < g_wPlayUntil) {
    if (g_insFrame >= g_wPageScanAt) {
      g_wPageScanAt = g_insFrame + 8;  // the demo rebuilds the ragdoll right about here, so keep up with it
      WForensicPages();
    }
    return;
  }
  int n = 0;
  const int N = g_wJN.load();
  for (int i = 0; i < N; ++i)
    if (g_wJ[i].objId && WPageDrop(i)) ++n;
  g_wAccN = 0;
  if (n)
    InsMark("WATCH journal off for playback (%d pages released, %llu page hits, %llu exceptions so far)", n,
            (unsigned long long)g_wPageHits.load(), (unsigned long long)g_wExcTotal.load());
}

// Called once per frame from InsFrameEnd: drain, diff open pages, follow the target, re-apply to new threads, honour the frame budget.
void WTick() {
  if (!g_wRing) return;
  WDrain();
  WPageTick();
  WBudgetFrame();
  if (g_insPrevPlaying != g_wEdgePlaying || g_insPrevPaused != g_wEdgePaused) {
    g_wEdgePlaying = g_insPrevPlaying;
    g_wEdgePaused = g_insPrevPaused;
    WBudgetEdge();
  }
  if (!WAnyArmed()) return;
  bool apply = WRefollow();
  WPageGate();
  if (g_wRearmAt && g_insFrame >= g_wRearmAt) {
    g_wRearmAt = g_insFrame + 120;  // catch threads created since the last pass
    apply = true;
  }
  if (apply)
    for (int i = 0; i < kWSlots; ++i)
      if (g_wS[i].on) {
        WApplyAll();
        break;
      }
  if (g_wUntil && g_insFrame >= g_wUntil) WDisarm("frame budget spent");
}

// ---- the standard forensic set

// Hardware: the bone transform the renderer reads (bone 5 x, follows the bone array, armed as soon as a ragdoll exists) and the
// animation elapsed-ticks helper, filtered to our controller (rcx) so its rdx says what "now" the animation update uses. The other two
// slots are left for the body finder, which puts read/write breakpoints on the physics bodies it identifies. The page journal covers
// body windows, bones, controller and ragdoll objects (see WPagePrio).
int WForensic(int frames) {
  if (!WEnsure()) return -1;
  WDisarm("re-arming");
  WWriteModules();
  int n = 0;
  if (WArm(0, 4, 1, "bone 5 x (bones+0xa0)", -1, 0, kFollowBones, 0xa0)) ++n;
  if (g_animElapsedFnAddr && WArm(g_animElapsedFnAddr, 1, 0, "anim elapsed helper (rcx=controller rdx=tick)", 1, 0, kFollowNone, 0, true))
    ++n;
  g_wForensic = true;
  const int pages = WForensicPages();
  g_wUntil = frames > 0 ? g_insFrame + uint32_t(frames) : 0;
  g_wRearmAt = g_insFrame + 120;
  g_wPageScanAt = g_insFrame + kWPageScan;
  g_wHits = 0;
  g_wPageHits = 0;
  g_wDrops = 0;
  g_wExcTotal = 0;
  g_wSkipped = 0;
  g_wSatTotal = 0;
  g_wPRecs = 0;
  WBudgetEdge();
  InsMark("FORENSIC armed %d hardware, %d pages, %d threads, budget %d frames (format %u)", n, pages, g_wThreads, frames, kInsFormat);
  return n + pages;
}

// ---- status

void WStatus() {
  Con("watch: %s - %llu hits (%llu page), %llu page exceptions, %llu skipped by budget, %llu saturations, %llu page diffs, %llu dropped, "
      "%d threads carry the debug registers\n",
      WAnyArmed() ? "ARMED" : "off", (unsigned long long)g_wHits.load(), (unsigned long long)g_wPageHits.load(),
      (unsigned long long)g_wExcTotal.load(), (unsigned long long)g_wSkipped.load(), (unsigned long long)g_wSatTotal.load(),
      (unsigned long long)g_wPRecs.load(), (unsigned long long)g_wDrops.load(), g_wThreads);
  if (!g_insRec) Con("  NOT RECORDING - hits are counted but thrown away. run 'ragdollfix insight rec 1 <label>' first.\n");
  for (int i = 0; i < kWSlots; ++i) {
    const WSlot& s = g_wS[i];
    if (s.on)
      Con("  hw%d  %-6s %016llx len %d  %s%s%s - %llu hits\n", i, WRwName(s.rw), (unsigned long long)s.addr, s.len, s.label,
          s.fOn ? " [filtered]" : "", s.body ? " [body]" : "", (unsigned long long)s.hits);
    else if (s.pend)
      Con("  hw%d  %-6s pending (target not there yet)  %s\n", i, WRwName(s.rw), s.label);
  }
  int on = 0, acc = 0, sat = 0;
  const int n = g_wJN.load();
  for (int i = 0; i < n; ++i) {
    const WPage& p = g_wJ[i];
    if (!p.armed) continue;
    ++on;
    acc += p.acc;
    sat += p.sat.load();
    if (on <= 24)
      Con("  page %3d %016llx%s #%u %s - %llu hits\n", i, (unsigned long long)p.page, p.acc ? " ACCESS" : "", p.objId, p.label,
          (unsigned long long)p.hits);
  }
  Con("  pages: %d journalled (%d access, %d open this frame), table %d/%d\n", on, acc, sat, n, kWPages);
  if (g_wUntil) Con("  disarms at frame %u (now %u)\n", g_wUntil, g_insFrame);
  Con("  targets: anim helper %llx | controller %llx+0x%x | bones %llx | pawn %llx\n", (unsigned long long)g_animElapsedFnAddr,
      (unsigned long long)g_insT.ctrl, g_offLastAnimTick, (unsigned long long)g_insT.bones, (unsigned long long)g_insT.pawn);
}
