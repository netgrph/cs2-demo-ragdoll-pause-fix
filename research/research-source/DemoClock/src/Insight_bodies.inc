// insight recorder, part 6: the physics body finder.
//
// The unpause snap is the render bones lagging the physics bodies. To see it we first have to find where the physics bodies keep
// their transforms - the game never told us. So: while the demo is paused on a fresh ragdoll, a background thread scans every private
// read/write region of the process for float triples sitting within a radius of the ragdoll's pose centroid (SSE-prefiltered on the
// dominant axis, both contiguous xyz vectors and matrix3x4 translations, in inches and, when the centroid is far from the origin, in
// meters too). The closest tens of thousands become "candidates". From then on, every hook event re-reads each candidate and records
// which ones changed ('C' records), so across the unpause we can watch the bodies move a frame before the bones do. Candidates that
// change under the physics step are clustered into "body" objects (their owning C++ object found by a back-scan for a vtable) and the
// spare hardware breakpoints are pointed at the best of them. The scan writes 'Y' records (the candidate list) and 'B' records (their
// first/last value and change mask); tools/bodies.py reads all three.

constexpr uint32_t kBMaxRaw = 1u << 18;   // raw candidates held during a scan before compaction (262144)
constexpr uint32_t kBKeep = 32768;        // candidates kept after a scan (closest to the centroid)
constexpr float kBRadiusIn = 128.0f;      // inches: a float triple this close to the pose centroid is a position candidate
constexpr int kBStable = 16;              // frames the target pawn must be unchanged before an idle scan triggers
constexpr uint32_t kBFinalizeAfter = 64;  // frames past the unpause the tracking session is finalized
constexpr uint32_t kBHardCap = 6000;      // a tracking session never runs longer than this many frames
constexpr int kBSpanMax = 0x1100;         // bytes one span (a single SafeCopy) may cover
constexpr int kBSpanDeadFails = 16;       // consecutive read failures before a span is abandoned (its memory was freed)
constexpr size_t kBChunk = 4u << 20;      // ReadProcessMemory read size during a scan
constexpr uint32_t kBCCap = 1u << 16;     // change entries buffered per event (65536)
constexpr uint32_t kBCFrameCap = 1u << 18;  // change entries recorded per frame (a 4 MB ceiling on 'C' records)
constexpr uint32_t kBChunkN = kBKeep;     // entries per Y/C/B record chunk
constexpr int kBEvInnerPre = 7, kBEvInnerPost = 8;  // extra "events": the physics inner step, sampled on the main thread

enum BState { kBIdle = 0, kBScanning, kBScanDone, kBTracking };

struct BRaw {
  uintptr_t addr;
  float d2;      // squared distance from the centroid target, for keeping the closest
  float v[3];
  uint8_t pat;   // 1 contiguous xyz, 2 matrix3x4 translation; |0x10 the value was found in meters
};

struct BCand {
  uintptr_t addr;
  float first[3], last[3];
  uint32_t changes, quietChg;  // total changes, and changes seen while paused and not settling
  int32_t clsWin;              // owning class from clustering, -1 until then
  uint16_t mask;               // bit e set if event e ever changed this candidate
  uint8_t pat, noisy, interest, flags;  // flags: 0x20 inside a tracked object, 0x40 inside the bone transform array
};

struct BSpan {
  uintptr_t addr;
  uint32_t len, first, n;
  uint16_t fails;
  uint8_t dead;
};

struct BGroup {
  uintptr_t lo, hi;
  uint32_t changes, first, n;  // [first, first+n) indices into g_bSel
};

struct BTarget {
  float c[3], R;
  uint8_t patBit;  // 0 inches, 0x10 meters
};

std::atomic<int> g_bState{kBIdle};
uint8_t* g_bBuf = nullptr;
BRaw* g_bRaw = nullptr;
uint32_t g_bRawN = 0;  // scan thread only
std::atomic<uint32_t> g_bRawKept{0};
std::atomic<uint32_t> g_bScanRegions{0}, g_bScanRawTotal{0};
std::atomic<uint64_t> g_bScanBytes{0};
std::atomic<double> g_bScanMs{0.0};
BTarget g_bTgt[2];
int g_bTgtN = 0;
uintptr_t g_bScanPawn = 0, g_bScannedPawn = 0;

BCand* g_bCand = nullptr;
uint32_t g_bCandN = 0;
BSpan* g_bSpan = nullptr;
uint32_t g_bSpanN = 0;
uint8_t* g_bScratch = nullptr;
uint32_t* g_bCIdx = nullptr;
float* g_bCVal = nullptr;
uint32_t g_bCFrameN = 0;
uint32_t* g_bSel = nullptr;

uint32_t g_bRun = 0, g_bStartFrame = 0, g_bUnpauseFrame = 0, g_bFinalizeAt = 0;
bool g_bClustered = false;
uint32_t g_bQuietPasses = 0;
DWORD g_bMainTid = 0;
int g_bInnerCalls = 0;
uintptr_t g_bLastPawn = 0;
uint32_t g_bPawnStableSince = 0;
bool g_bPrevSettling = false;

static BGroup g_bGrpBuf[1024];

// ---- helpers

uint32_t BFoot(uint8_t pat) { return (pat & 1) ? 12u : 48u; }

const char* BStateName() {
  switch (g_bState.load()) {
    case kBScanning: return "scanning";
    case kBScanDone: return "scan done";
    case kBTracking: return "tracking";
    default: return "idle";
  }
}

void BReadTriple(const uint8_t* base, uint32_t off, uint8_t pat, float* v) {
  uint32_t oa, ob, oc;
  if (pat & 1) { oa = 0; ob = 4; oc = 8; } else { oa = 12; ob = 28; oc = 44; }
  memcpy(&v[0], base + off + oa, 4);
  memcpy(&v[1], base + off + ob, 4);
  memcpy(&v[2], base + off + oc, 4);
}

// A candidate that falls inside a bone transform array or an already-tracked object is not a fresh physics body: flag it so the
// clustering leaves it out of the "body step" windows.
uint8_t BMergeFlags(uintptr_t addr) {
  uint8_t f = 0;
  if (g_insT.bones && addr >= g_insT.bones && addr < g_insT.bones + 0x1000) f |= 0x40;
  for (int i = 0; i < kInsMaxObjs; ++i) {
    const InsObj& o = g_insObj[i];
    if (o.live && o.kind != kObjBody && addr >= o.addr && addr < o.addr + o.size) { f |= 0x20; break; }
  }
  return f;
}

int BAnyMask(uint16_t m) {
  int n = 0;
  for (uint32_t i = 0; i < g_bCandN; ++i) {
    const BCand& c = g_bCand[i];
    if ((c.mask & m) && !c.noisy && !(c.flags & 0x60)) ++n;
  }
  return n;
}

// ---- the scan (background thread, ReadProcessMemory only)

bool BCheckMem(const uint8_t* buf, uintptr_t base, uint32_t got, uintptr_t cand, uint8_t pat, const BTarget& t, float* v, float* d2) {
  if (cand < base) return false;
  const uint64_t off = cand - base;
  const uint32_t need = (pat & 1) ? 12u : 48u;
  if (off + need > got) return false;
  uint32_t oa, ob, oc;
  if (pat & 1) { oa = 0; ob = 4; oc = 8; } else { oa = 12; ob = 28; oc = 44; }
  float x, y, z;
  memcpy(&x, buf + off + oa, 4);
  memcpy(&y, buf + off + ob, 4);
  memcpy(&z, buf + off + oc, 4);
  if (!(std::isfinite(x) && std::isfinite(y) && std::isfinite(z))) return false;
  if (!(std::fabs(x - t.c[0]) < t.R && std::fabs(y - t.c[1]) < t.R && std::fabs(z - t.c[2]) < t.R)) return false;
  const int zeros = (x == 0.0f) + (y == 0.0f) + (z == 0.0f);
  if (zeros >= 2) return false;
  if (std::fabs(x) + std::fabs(y) + std::fabs(z) <= t.R * 0.01f) return false;
  v[0] = x; v[1] = y; v[2] = z;
  const float dx = x - t.c[0], dy = y - t.c[1], dz = z - t.c[2];
  *d2 = dx * dx + dy * dy + dz * dz;
  return true;
}

void BCompactRaw() {
  if (g_bRawN <= kBKeep) return;
  std::nth_element(g_bRaw, g_bRaw + kBKeep, g_bRaw + g_bRawN, [](const BRaw& a, const BRaw& b) { return a.d2 < b.d2; });
  g_bRawN = kBKeep;
}

void BAddRaw(uintptr_t addr, const float* v, float d2, uint8_t pat) {
  if (g_bRawN >= kBMaxRaw) BCompactRaw();
  BRaw& r = g_bRaw[g_bRawN++];
  r.addr = addr;
  r.v[0] = v[0]; r.v[1] = v[1]; r.v[2] = v[2];
  r.d2 = d2;
  r.pat = pat;
}

// SSE prefilter: find the 4-byte slots whose value is within the radius of the centroid's dominant axis, then verify the full triple
// both as a contiguous xyz vector and as a matrix3x4 translation (components 16 bytes apart).
uint32_t BScanBuffer(const uint8_t* buf, uintptr_t base, uint32_t got) {
  uint32_t added = 0;
  for (int ti = 0; ti < g_bTgtN; ++ti) {
    const BTarget& t = g_bTgt[ti];
    int dom = 0;
    float mx = std::fabs(t.c[0]);
    if (std::fabs(t.c[1]) > mx) { mx = std::fabs(t.c[1]); dom = 1; }
    if (std::fabs(t.c[2]) > mx) { mx = std::fabs(t.c[2]); dom = 2; }
    const __m128 dv = _mm_set1_ps(t.c[dom]);
    const __m128 rr = _mm_set1_ps(t.R);
    const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
    for (uint32_t k = 0; k + 16 <= got; k += 16) {
      const __m128 val = _mm_loadu_ps((const float*)(buf + k));
      const __m128 d = _mm_and_ps(_mm_sub_ps(val, dv), sign);
      const int mask = _mm_movemask_ps(_mm_cmplt_ps(d, rr));
      if (!mask) continue;
      for (int l = 0; l < 4; ++l) {
        if (!(mask & (1 << l))) continue;
        const uintptr_t hit = base + k + (uint32_t)l * 4;
        float v[3], d2;
        const uintptr_t cb = hit - (uintptr_t)(4 * dom);
        if (BCheckMem(buf, base, got, cb, (uint8_t)(1 | t.patBit), t, v, &d2)) { BAddRaw(cb, v, d2, (uint8_t)(1 | t.patBit)); ++added; }
        const uintptr_t mb = hit - (uintptr_t)(12 + 16 * dom);
        if (BCheckMem(buf, base, got, mb, (uint8_t)(2 | t.patBit), t, v, &d2)) { BAddRaw(mb, v, d2, (uint8_t)(2 | t.patBit)); ++added; }
      }
    }
  }
  return added;
}

void BFinishScan() {
  BCompactRaw();
  std::sort(g_bRaw, g_bRaw + g_bRawN, [](const BRaw& a, const BRaw& b) { return a.addr < b.addr; });
  uint32_t w = 0;
  for (uint32_t i = 0; i < g_bRawN; ++i) {
    if (w > 0 && g_bRaw[w - 1].addr == g_bRaw[i].addr) {
      g_bRaw[w - 1].pat |= g_bRaw[i].pat;
      if (g_bRaw[i].d2 < g_bRaw[w - 1].d2) g_bRaw[w - 1].d2 = g_bRaw[i].d2;
      continue;
    }
    g_bRaw[w++] = g_bRaw[i];
  }
  g_bRawKept = w;
}

// Allocation bases of every thread's stack, so the scan can skip them (a stack holds transient copies that vanish and only add noise).
int BStackBases(uintptr_t* out, int cap) {
  int n = 0;
  HMODULE nt = GetModuleHandleA("ntdll.dll");
  typedef LONG(NTAPI * NtQIT)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  NtQIT q = nt ? (NtQIT)GetProcAddress(nt, "NtQueryInformationThread") : nullptr;
  if (!q) return 0;
  const DWORD pid = GetCurrentProcessId();
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 te;
  te.dwSize = sizeof(te);
  if (Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID != pid) continue;
      HANDLE th = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
      if (!th) continue;
      struct {
        LONG ExitStatus;
        PVOID TebBaseAddress;
        ULONG_PTR u[4];
      } tbi;
      memset(&tbi, 0, sizeof(tbi));
      if (q(th, 0, &tbi, sizeof(tbi), nullptr) == 0 && tbi.TebBaseAddress) {
        uintptr_t sb = 0;
        if (SafeCopy(&sb, (uint8_t*)tbi.TebBaseAddress + 0x08, 8) && sb) {
          MEMORY_BASIC_INFORMATION mi;
          if (VirtualQuery((LPCVOID)(sb - 1), &mi, sizeof(mi)) == sizeof(mi) && n < cap) out[n++] = (uintptr_t)mi.AllocationBase;
        }
      }
      CloseHandle(th);
    } while (Thread32Next(snap, &te));
  }
  CloseHandle(snap);
  std::sort(out, out + n);
  return n;
}

DWORD WINAPI BodyScanThread(LPVOID) {
  const double t0 = Now();
  g_bRawN = 0;
  uint64_t bytes = 0;
  uint32_t regions = 0, rawtotal = 0;
  uintptr_t stacks[512];
  const int nstk = BStackBases(stacks, 512);
  MEMORY_BASIC_INFORMATION mi;
  uintptr_t a = 0x10000;
  while (a < 0x7FFFFFFFFFFFull && VirtualQuery((LPCVOID)a, &mi, sizeof(mi)) == sizeof(mi)) {
    const uintptr_t b = (uintptr_t)mi.BaseAddress, e = b + mi.RegionSize;
    if (e <= a) break;
    a = e;
    if (mi.State != MEM_COMMIT || mi.Type != MEM_PRIVATE || (mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) continue;
    const DWORD p = mi.Protect & 0xff;
    if (!(p & (PAGE_READWRITE | PAGE_WRITECOPY))) continue;
    if (InsOwn(b) || (HMODULE)mi.AllocationBase == g_self) continue;
    bool isStack = false;
    for (int k = 0; k < nstk; ++k)
      if (stacks[k] == (uintptr_t)mi.AllocationBase) { isStack = true; break; }
    if (isStack) continue;
    ++regions;
    for (uintptr_t c = b; c < e; c += kBChunk) {
      const size_t len = (std::min)(kBChunk, size_t(e - c));
      SIZE_T got = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c, g_bBuf, len, &got) || got < 16) continue;
      bytes += got;
      rawtotal += BScanBuffer(g_bBuf, c, (uint32_t)got);
    }
    if (g_bState.load() != kBScanning) return 0;  // cancelled
  }
  BFinishScan();
  g_bScanRegions = regions;
  g_bScanBytes = bytes;
  g_bScanRawTotal = rawtotal;
  g_bScanMs = (Now() - t0) * 1000.0;
  g_bState = kBScanDone;
  return 0;
}

bool BEnsure() {
  if (g_bBuf) return true;
  g_bBuf = (uint8_t*)InsVAlloc(kBChunk);
  g_bRaw = (BRaw*)InsVAlloc(sizeof(BRaw) * kBMaxRaw);
  g_bCand = (BCand*)InsVAlloc(sizeof(BCand) * kBKeep);
  g_bSpan = (BSpan*)InsVAlloc(sizeof(BSpan) * kBKeep);
  g_bScratch = (uint8_t*)InsVAlloc(kBSpanMax);
  g_bCIdx = (uint32_t*)InsVAlloc(sizeof(uint32_t) * kBCCap);
  g_bCVal = (float*)InsVAlloc(sizeof(float) * 3 * kBCCap);
  g_bSel = (uint32_t*)InsVAlloc(sizeof(uint32_t) * kBKeep);
  return g_bBuf && g_bRaw && g_bCand && g_bSpan && g_bScratch && g_bCIdx && g_bCVal && g_bSel;
}

// Snapshot the ragdoll's pose centroid on the main thread, then launch the scan.
void BodyScanStart(const char* why) {
  if (!BEnsure()) return (void)Con("bodies: could not allocate the scan buffers\n");
  if (g_bState.load() != kBIdle) return (void)Con("bodies: busy (%s)\n", BStateName());
  float c[3];
  bool ok = false;
  const int li = LockPawn();
  if (li >= 0 && li < g_clk.nPawns && g_clk.pawns[li].inst) ok = PoseCentroid(g_clk.pawns[li].inst, c);
  if (!ok && g_insT.haveC) { c[0] = g_insT.c[0]; c[1] = g_insT.c[1]; c[2] = g_insT.c[2]; ok = true; }
  if (!ok) {
    InsMark("BODIES scan skipped (%s): no ragdoll pose centroid yet", why);
    return (void)Con("bodies: no ragdoll pose yet - pause on a fresh death first\n");
  }
  g_bScanPawn = g_insT.pawn;
  g_bTgtN = 0;
  BTarget& in = g_bTgt[g_bTgtN++];
  in.c[0] = c[0]; in.c[1] = c[1]; in.c[2] = c[2];
  in.R = kBRadiusIn;
  in.patBit = 0;
  int big = 0;
  for (int k = 0; k < 3; ++k)
    if (std::fabs(c[k]) > 128.0f) ++big;
  if (big >= 2 && g_bTgtN < 2) {
    const float m = 0.0254f;
    BTarget& me = g_bTgt[g_bTgtN++];
    me.c[0] = c[0] * m; me.c[1] = c[1] * m; me.c[2] = c[2] * m;
    me.R = kBRadiusIn * m;
    me.patBit = 0x10;
  }
  ++g_bRun;
  g_bState = kBScanning;
  InsMark("BODIES scan start run %u (%s): centroid %.1f %.1f %.1f, %d targets", g_bRun, why, c[0], c[1], c[2], g_bTgtN);
  Con("bodies: scanning memory for the ragdoll's body transforms (run %u)...\n", g_bRun);
  if (HANDLE th = CreateThread(nullptr, 0, BodyScanThread, nullptr, 0, nullptr))
    CloseHandle(th);
  else {
    g_bState = kBIdle;
    Con("bodies: could not start the scan thread\n");
  }
}

// ---- records

void BEmitCandidates() {
  if (!g_insRec) return;
  for (uint32_t start = 0; start < g_bCandN; start += kBChunkN) {
    const uint32_t cnt = (std::min)(kBChunkN, g_bCandN - start);
    const size_t at = InsBegin('Y');
    InsPod(g_bRun);
    InsPod(start);
    InsPod(cnt);
    for (uint32_t k = 0; k < cnt; ++k) {
      const BCand& c = g_bCand[start + k];
      InsPod((uint64_t)c.addr);
      InsPod(c.pat);
      InsPut(c.first, 12);
    }
    InsEnd(at);
  }
}

void BEmitChanges(int ev, uint32_t nchg) {
  const uint64_t sc = g_insStepCalls.load(std::memory_order_relaxed);
  for (uint32_t start = 0; start < nchg; start += kBChunkN) {
    const uint32_t cnt = (std::min)(kBChunkN, nchg - start);
    const size_t at = InsBegin('C');
    InsPod(g_bRun);
    InsPod((uint8_t)ev);
    InsPod(g_insFrame);
    InsPod(sc);
    InsPod(start);
    InsPod(cnt);
    for (uint32_t k = 0; k < cnt; ++k) {
      InsPod(g_bCIdx[start + k]);
      InsPut(&g_bCVal[(start + k) * 3], 12);
    }
    InsEnd(at);
  }
}

// ---- the per-event pass

// Re-reads every candidate and records the ones that moved. baseline=true only refreshes 'last' (no counting, no records): used once
// right after a scan so the first real pass reports genuine changes, not the difference from the scan a moment ago.
void BodyPass(int ev, bool baseline) {
  if (g_bState.load(std::memory_order_relaxed) != kBTracking) return;
  if (!baseline && g_wWin.load(std::memory_order_relaxed)) return;
  ++t_wQuiet;  // our reads of journalled pages heal, never record
  const bool paused = !g_insPrevPlaying || g_insPrevPaused;
  const bool quiet = paused && !g_clk.settling;
  const uint16_t evbit = (ev >= 1 && ev <= 15) ? (uint16_t)(1u << ev) : 0;
  uint32_t nchg = 0;
  for (uint32_t si = 0; si < g_bSpanN; ++si) {
    BSpan& sp = g_bSpan[si];
    if (sp.dead) continue;
    if (!SafeCopy(g_bScratch, (const void*)sp.addr, sp.len)) {
      if (++sp.fails >= kBSpanDeadFails) sp.dead = 1;
      continue;
    }
    sp.fails = 0;
    for (uint32_t ci = sp.first; ci < sp.first + sp.n; ++ci) {
      BCand& cd = g_bCand[ci];
      const uint32_t off = (uint32_t)(cd.addr - sp.addr);
      float v[3];
      BReadTriple(g_bScratch, off, cd.pat, v);
      if (v[0] == cd.last[0] && v[1] == cd.last[1] && v[2] == cd.last[2]) continue;
      cd.last[0] = v[0]; cd.last[1] = v[1]; cd.last[2] = v[2];
      if (baseline) continue;
      ++cd.changes;
      cd.mask |= evbit;
      if (quiet) ++cd.quietChg;
      if (g_bQuietPasses >= 32 && cd.quietChg > (uint32_t)(0.6 * g_bQuietPasses)) cd.noisy = 1;
      const bool eligible = g_bClustered ? (cd.interest != 0) : (cd.noisy == 0);
      if (eligible && nchg < kBCCap && g_bCFrameN < kBCFrameCap) {
        g_bCIdx[nchg] = ci;
        g_bCVal[nchg * 3] = v[0];
        g_bCVal[nchg * 3 + 1] = v[1];
        g_bCVal[nchg * 3 + 2] = v[2];
        ++nchg;
        ++g_bCFrameN;
      }
    }
  }
  if (!baseline && quiet) ++g_bQuietPasses;
  if (!baseline && nchg && g_insRec) BEmitChanges(ev, nchg);
  --t_wQuiet;
}

// ---- merge (main thread, once a scan is done)

void BodyMerge() {
  const uint32_t nraw = g_bRawKept.load();
  g_bCandN = 0;
  for (uint32_t i = 0; i < nraw && g_bCandN < kBKeep; ++i) {
    const BRaw& r = g_bRaw[i];
    BCand& c = g_bCand[g_bCandN++];
    memset(&c, 0, sizeof(c));
    c.addr = r.addr;
    c.pat = r.pat;
    c.clsWin = -1;
    c.first[0] = c.last[0] = r.v[0];
    c.first[1] = c.last[1] = r.v[1];
    c.first[2] = c.last[2] = r.v[2];
    c.flags = BMergeFlags(r.addr);
  }
  g_bSpanN = 0;
  for (uint32_t i = 0; i < g_bCandN && g_bSpanN < kBKeep;) {
    BSpan& sp = g_bSpan[g_bSpanN];
    sp.addr = g_bCand[i].addr;
    sp.first = i;
    sp.fails = 0;
    sp.dead = 0;
    uintptr_t maxEnd = g_bCand[i].addr + BFoot(g_bCand[i].pat);
    uint32_t j = i + 1;
    for (; j < g_bCandN; ++j) {
      const uintptr_t end = g_bCand[j].addr + BFoot(g_bCand[j].pat);
      if (end - sp.addr > (uintptr_t)kBSpanMax) break;
      if (end > maxEnd) maxEnd = end;
    }
    sp.n = j - i;
    sp.len = (uint32_t)(maxEnd - sp.addr);
    ++g_bSpanN;
    i = j;
  }
  g_bQuietPasses = 0;
  g_bClustered = false;
  g_bUnpauseFrame = 0;
  g_bFinalizeAt = 0;
  g_bStartFrame = g_insFrame;
  BEmitCandidates();
  g_bState = kBTracking;
  BodyPass(kEvFramePost, true);  // baseline: bring 'last' up to the current memory
  InsMark("BODIES tracking run %u: %u candidates in %u spans, from %u raw (%u regions, %.0f MB, %.0f ms)", g_bRun, g_bCandN, g_bSpanN,
          g_bScanRawTotal.load(), g_bScanRegions.load(), double(g_bScanBytes.load()) / (1 << 20), g_bScanMs.load());
  Con("bodies: tracking %u candidate transforms (run %u). you can unpause now.\n", g_bCandN, g_bRun);
}

// ---- clustering

// Builds up to maxWin tracked "body" windows from candidates matching wantBits (and not notBits, not noisy, not exclFlags): groups
// nearby candidates, ranks the groups by total change count, and finds each group's owning C++ object by scanning back for a vtable.
int BMakeWindows(uint16_t wantBits, uint16_t notBits, uint8_t exclFlags, const char* prefix, int maxWin, bool acc) {
  uint32_t ns = 0;
  for (uint32_t i = 0; i < g_bCandN && ns < kBKeep; ++i) {
    const BCand& c = g_bCand[i];
    if (!(c.mask & wantBits) || (c.mask & notBits) || c.noisy || (c.flags & exclFlags)) continue;
    g_bSel[ns++] = i;
  }
  uint32_t ng = 0;
  for (uint32_t k = 0; k < ns && ng < 1024;) {
    BGroup& g = g_bGrpBuf[ng];
    g.first = k;
    g.lo = g_bCand[g_bSel[k]].addr;
    g.hi = g.lo + BFoot(g_bCand[g_bSel[k]].pat);
    g.changes = g_bCand[g_bSel[k]].changes;
    uint32_t m = k + 1;
    for (; m < ns; ++m) {
      const uintptr_t addr = g_bCand[g_bSel[m]].addr;
      if (addr - g.hi > 0x200) break;
      const uintptr_t end = addr + BFoot(g_bCand[g_bSel[m]].pat);
      if (end > g.hi) g.hi = end;
      g.changes += g_bCand[g_bSel[m]].changes;
    }
    g.n = m - k;
    k = m;
    ++ng;
  }
  static uint32_t order[1024];
  for (uint32_t i = 0; i < ng; ++i) order[i] = i;
  std::sort(order, order + ng, [](uint32_t a, uint32_t b) { return g_bGrpBuf[a].changes > g_bGrpBuf[b].changes; });
  int made = 0;
  for (uint32_t oi = 0; oi < ng && made < maxWin && g_insBodyWinN < 40; ++oi) {
    BGroup& g = g_bGrpBuf[order[oi]];
    if (!g.changes) break;
    uintptr_t owner = 0;
    int cls = -1;
    for (uintptr_t back = 0; back <= 0x800; back += 8) {
      uintptr_t qv = 0;
      if (!Rd(g.lo - back, qv)) break;
      const int c = InsClassOf(qv);
      if (c >= 0) { owner = g.lo - back; cls = c; break; }
    }
    uintptr_t start, end;
    if (owner) { start = owner; end = (std::min)(g.hi + 0x100, owner + 0x2000); }
    else { start = g.lo - 0x80; end = g.hi + 0x100; }
    char label[128];
    const char* pretty = cls >= 0 ? g_insCls[cls].pretty : "?";
    _snprintf_s(label, sizeof(label), _TRUNCATE, "%s %s+0x%x", prefix, pretty, owner ? (unsigned)(g.lo - owner) : 0u);
    InsBodyWin& w = g_insBodyWin[g_insBodyWinN++];
    w.addr = start;
    w.size = (uint32_t)(end - start);
    w.cls = cls;
    w.acc = acc;
    _snprintf_s(w.label, sizeof(w.label), _TRUNCATE, "%s", label);
    InsObjAdd(kObjBody, start, w.size, cls, kNfOwned, kRootWorld, 2, label);
    for (uint32_t t = g.first; t < g.first + g.n; ++t) {
      BCand& cc = g_bCand[g_bSel[t]];
      cc.interest = 1;
      cc.clsWin = cls;
    }
    ++made;
  }
  return made;
}

// Points the spare hardware breakpoints at the best physics-body position, and at the best place its value is copied to.
void BodyPick() {
  for (int i = 0; i < kWSlots; ++i)
    if (g_wS[i].body) WSlotDisarm(i, "body finder re-picking");
  int best = -1;
  double bestScore = -1e30;
  for (uint32_t i = 0; i < g_bCandN; ++i) {
    const BCand& c = g_bCand[i];
    if (!c.interest || !(c.mask & 0x110)) continue;  // a step candidate (bit 8 or bit 4)
    double s = c.changes;
    if (c.clsWin >= 0) {
      const char* p = g_insCls[c.clsWin].pretty;
      if (strstr(p, "Body")) s += 4;
      if (strstr(p, "Rn")) s += 2;
      if (strstr(p, "Phys")) s += 2;
    }
    if (c.pat & 1) s += 1;
    if (s > bestScore) { bestScore = s; best = (int)i; }
  }
  if (best >= 0) {
    char lbl[64];
    _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "body step pos (%s)", (g_bCand[best].pat & 1) ? "xyz" : "m3x4");
    WArm(g_bCand[best].addr, 4, 3, lbl, -1, 0, kFollowNone, 0, false, true);
    InsMark("BODIES pick step %llx (%s, %u changes, class %s)", (unsigned long long)g_bCand[best].addr,
            (g_bCand[best].pat & 1) ? "contiguous" : "matrix", g_bCand[best].changes,
            g_bCand[best].clsWin >= 0 ? g_insCls[g_bCand[best].clsWin].pretty : "?");
  }
  int bestc = -1;
  uint32_t bc = 0;
  for (uint32_t i = 0; i < g_bCandN; ++i) {
    const BCand& c = g_bCand[i];
    if (!c.interest || (c.mask & 0x110)) continue;  // a copy candidate (interest but not a step candidate)
    if (c.changes > bc) { bc = c.changes; bestc = (int)i; }
  }
  if (bestc >= 0) {
    WArm(g_bCand[bestc].addr, 4, 3, "body copy pos", -1, 0, kFollowNone, 0, false, true);
    InsMark("BODIES pick copy %llx (%u changes)", (unsigned long long)g_bCand[bestc].addr, g_bCand[bestc].changes);
  }
}

void BodyCluster(const char* why) {
  if (g_bClustered) return;
  g_insBodyWinN = 0;
  int steps = BMakeWindows(0x100, 0, 0x60, "body step", 24, true);           // bit 8 (inner step), not bones/tracked
  if (!steps) steps = BMakeWindows(0x10, 0, 0x60, "body step", 24, true);    // fallback: bit 4 (step post)
  const int copies = BMakeWindows((uint16_t)((1 << 2) | (1 << 6)), (uint16_t)((1 << 3) | (1 << 4) | (1 << 7) | (1 << 8)), 0x40,
                                  "body copy", 16, false);
  g_bClustered = true;
  g_wPageScanAt = 0;  // rejournal now, body windows at top priority
  WBudgetEdge();
  InsMark("BODIES cluster run %u (%s): %d step windows, %d copy windows", g_bRun, why, steps, copies);
  Con("bodies: clustered %d step + %d copy windows (run %u)\n", steps, copies, g_bRun);
  BodyPick();
  if (g_bUnpauseFrame) g_bFinalizeAt = g_bUnpauseFrame + kBFinalizeAfter;
}

// ---- finalize

void BodyFinalize(const char* why) {
  if (g_bState.load() != kBTracking) return;
  if (g_insRec) {
    for (uint32_t start = 0; start < g_bCandN; start += kBChunkN) {
      const uint32_t cnt = (std::min)(kBChunkN, g_bCandN - start);
      const size_t at = InsBegin('B');
      InsPod(g_bRun);
      InsPod(start);
      InsPod(cnt);
      for (uint32_t k = 0; k < cnt; ++k) {
        const BCand& c = g_bCand[start + k];
        InsPod((uint64_t)c.addr);
        InsPod(c.pat);
        InsPut(c.first, 12);
        InsPut(c.last, 12);
        InsPod(c.mask);
        InsPod(c.changes);
        InsPod(c.noisy);
      }
      InsEnd(at);
    }
  }
  int nb = 0;
  for (int i = 0; i < g_insBodyWinN; ++i)
    if (g_insBodyWin[i].addr) ++nb;
  InsMark("BODIES finalized run %u (%s): %u candidates, %d windows, unpause frame %u", g_bRun, why, g_bCandN, nb, g_bUnpauseFrame);
  Con("bodies: run %u done (%s) - %u candidates, %d windows. see the B/C records\n", g_bRun, why, g_bCandN, nb);
  g_bScannedPawn = g_bScanPawn;
  g_bState = kBIdle;
}

// ---- per-frame driver (called from InsFrameEnd, before WTick)

void BodyFrame() {
  g_bMainTid = GetCurrentThreadId();
  g_bInnerCalls = 0;
  g_bCFrameN = 0;
  const int st = g_bState.load();
  const bool settling = g_clk.settling;
  const bool settleRise = settling && !g_bPrevSettling;
  const bool settleFall = !settling && g_bPrevSettling;
  if (st == kBScanDone) {
    BodyMerge();
    g_bPrevSettling = settling;
    return;
  }
  if (g_wForensic && g_insRec) {
    if (g_insT.pawn != g_bLastPawn) { g_bLastPawn = g_insT.pawn; g_bPawnStableSince = g_insFrame; }
    const bool paused = !g_insPrevPlaying || g_insPrevPaused;
    if (paused && st == kBIdle) {
      const bool idleStable = !settling && g_insT.pawn && g_insT.pawn != g_bScannedPawn && (g_insFrame - g_bPawnStableSince) >= (uint32_t)kBStable;
      if (settleRise || idleStable) BodyScanStart(settleRise ? "settle edge" : "idle stable");
    }
  }
  if (st == kBTracking) {
    const bool paused = !g_insPrevPlaying || g_insPrevPaused;
    if (!paused && !g_bUnpauseFrame) { g_bUnpauseFrame = g_insFrame; g_bFinalizeAt = g_insFrame + kBFinalizeAfter; }
    if (!g_bClustered) {
      const int step8 = BAnyMask(0x100), step4 = BAnyMask(0x10);
      if (settleFall && (step8 || step4))
        BodyCluster("settle end");
      else if (g_bUnpauseFrame && g_insFrame >= g_bUnpauseFrame + 1 && (step8 || g_insFrame >= g_bUnpauseFrame + 3))
        BodyCluster("post-unpause");
    }
    if (g_bFinalizeAt && g_insFrame >= g_bFinalizeAt)
      BodyFinalize("post-unpause window");
    else if (g_insFrame - g_bStartFrame > kBHardCap)
      BodyFinalize("frame cap");
  }
  g_bPrevSettling = settling;
}

// ---- console

void BStatus() {
  Con("bodies: %s | run %u | %u candidates, %u spans | scan: %u regions, %.0f MB, %.0f ms, %u raw hits\n", BStateName(), g_bRun,
      g_bCandN, g_bSpanN, g_bScanRegions.load(), double(g_bScanBytes.load()) / (1 << 20), g_bScanMs.load(), g_bScanRawTotal.load());
  int wins = 0;
  for (int i = 0; i < g_insBodyWinN; ++i)
    if (g_insBodyWin[i].addr) ++wins;
  Con("  clustered %s, %d windows, unpause frame %u, quiet passes %u, %d step-changing candidates\n", g_bClustered ? "yes" : "no",
      wins, g_bUnpauseFrame, g_bQuietPasses, BAnyMask(0x110));
}

void BCommand(int argc, const char** argv) {
  const char* sub = argc > 1 ? argv[1] : "status";
  if (!_stricmp(sub, "status")) {
    BStatus();
  } else if (!_stricmp(sub, "start")) {
    g_bScannedPawn = 0;
    BodyScanStart("console");
  } else if (!_stricmp(sub, "cluster")) {
    if (g_bState.load() != kBTracking)
      Con("bodies: not tracking (run 'bodies start' first)\n");
    else {
      g_bClustered = false;
      BodyCluster("console");
    }
  } else if (!_stricmp(sub, "end")) {
    if (g_bState.load() == kBTracking)
      BodyFinalize("console");
    else
      Con("bodies: not tracking\n");
  } else {
    Con("usage: ragdollfix insight watch bodies [start | cluster | end | status]\n");
  }
}
