// insight recorder, part 1: own memory, output file, region map, RTTI classes, schema fields.

constexpr uint32_t kInsFormat = 2;
constexpr int kInsMaxObjs = 256;
constexpr uint32_t kInsMaxObjSize = 0x8000;
constexpr uint64_t kInsMaxFile = 2ull << 30;
constexpr float kInsNear = 96.0f;           // a float triple this close (units) to the ragdoll pose centroid marks a position
constexpr int kInsMaxNodes = 1 << 18;
constexpr int kInsNodeHash = 1 << 19;
constexpr int kInsMaxClasses = 8192;
constexpr int kInsClassHash = 1 << 15;
constexpr int kInsMaxRegions = 1 << 17;
constexpr int kInsMaxHits = 4096;
constexpr int kInsMaxFields = 2048;
constexpr size_t kInsBufCap = 8 << 20;
constexpr size_t kInsRecordMax = 1 << 20;   // largest single record; the buffer always keeps this much room
constexpr uint32_t kInsScanWin = 0x400;     // bytes of each census node read for pointers and positions

enum InsEv : uint8_t { kEvAnimPre = 1, kEvAnimPost, kEvStepPre, kEvStepPost, kEvFramePre, kEvFramePost };
enum InsKind : uint8_t { kObjGlobals = 0, kObjPawn, kObjBones, kObjVt, kObjBlob, kObjCtrl, kObjNode, kObjBody };
enum InsFlag : uint8_t { kNfVt = 1, kNfOwned = 2, kNfNear = 4, kNfEntity = 8, kNfLineage = 16, kNfExpanded = 32 };
enum InsRoot : uint8_t { kRootPawn = 0, kRootCtrl, kRootWorld, kRootPhysSys, kRootAnimSys, kRootPoseCtl, kRootRagSys, kRootXref, kRootCount };
const char* const kInsRootNames[kRootCount] = {"pawn", "animctrl", "world", "physsys", "animsys", "posectl", "ragsys", "xref"};

// ---- own memory: allocated once, never freed, excluded from every scan

Range g_insOwn[64];
std::atomic<int> g_insOwnN{0};

void* InsVAlloc(size_t n) {
  void* p = VirtualAlloc(nullptr, n, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  const int i = g_insOwnN.load();
  if (p && i < 64) {
    g_insOwn[i] = {(uintptr_t)p, n};
    g_insOwnN = i + 1;
  }
  return p;
}

bool InsOwn(uintptr_t a) {
  const int n = g_insOwnN.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i)
    if (g_insOwn[i].has(a)) return true;
  return false;
}

void InsSafeStr(const char* p, char* out, size_t cap) {
  out[0] = 0;
  if ((uintptr_t)p < 0x10000 || (uintptr_t)p >= 0x7FFFFFFFFFFFull) return;
  __try {
    size_t i = 0;
    for (; i + 1 < cap && p[i]; ++i) out[i] = p[i];
    out[i] = 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
  }
}

// ---- output file: tag u8, length u32, payload

HANDLE g_insFile = INVALID_HANDLE_VALUE;
uint8_t* g_insBuf = nullptr;
size_t g_insLen = 0;
uint64_t g_insBytes = 0;

void InsFlush() {
  if (g_insFile != INVALID_HANDLE_VALUE && g_insLen) {
    DWORD w = 0;
    WriteFile(g_insFile, g_insBuf, (DWORD)g_insLen, &w, nullptr);
  }
  g_insLen = 0;
}

void InsPut(const void* p, size_t n) {
  if (g_insFile == INVALID_HANDLE_VALUE || !g_insBuf || g_insLen + n > kInsBufCap) return;
  memcpy(g_insBuf + g_insLen, p, n);
  g_insLen += n;
  g_insBytes += n;
}

template <class T>
void InsPod(const T& v) { InsPut(&v, sizeof(T)); }

void InsStr(const char* s) {
  const size_t n = s ? strnlen(s, 4096) : 0;
  InsPod((uint16_t)n);
  InsPut(s, n);
}

// Game-owned string: copied safely first.
void InsGameStr(const char* s) {
  char tmp[512];
  InsSafeStr(s, tmp, sizeof(tmp));
  InsStr(tmp);
}

size_t InsBegin(char tag) {
  if (g_insLen + kInsRecordMax > kInsBufCap) InsFlush();
  const size_t at = g_insLen;
  InsPod((uint8_t)tag);
  InsPod((uint32_t)0);
  return at;
}

void InsEnd(size_t at) {
  if (g_insFile == INVALID_HANDLE_VALUE || at + 5 > g_insLen) return;
  const uint32_t len = uint32_t(g_insLen - at - 5);
  memcpy(g_insBuf + at + 1, &len, 4);
}

uint32_t g_insFrame = 0;
std::atomic<uint64_t> g_insStepCalls{0};  // world physics steps counted by Ins_CountingStep (part 3), including the addon's catch-up steps
bool g_insRec = false;
bool g_insPrevPlaying = false, g_insPrevPaused = false;  // demo state as of the last frame boundary

void InsMark(const char* fmt, ...) {
  char text[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(text, sizeof(text), fmt, ap);
  va_end(ap);
  const size_t at = InsBegin('M');
  InsPod(Now());
  InsPod(g_insFrame);
  InsStr(text);
  InsEnd(at);
}

// ---- region map (sorted, adjacent regions with equal attributes merged)

// Defined in part 5 (watch): true while a page we protected (made READONLY/NOACCESS to journal it) is armed or being faulted on. The
// region map and the cross-reference scan treat such a page as the plain read/write heap it really is, so a journalled object is not
// dropped from the census just because the journal is holding its pages protected this instant.
bool WPageOurs(uintptr_t a);

struct InsRegion {
  uintptr_t b, e, alloc;
  uint8_t r, w, x, img;
};
InsRegion* g_insReg = nullptr;
int g_insRegN = 0;
double g_insRegAt = -10.0;

void InsRefreshRegions(bool force) {
  if (!g_insReg || (!force && Now() - g_insRegAt < 1.0)) return;
  g_insRegAt = Now();
  int n = 0;
  MEMORY_BASIC_INFORMATION mi;
  uintptr_t a = 0x10000;
  while (a < 0x7FFFFFFFFFFFull && VirtualQuery((LPCVOID)a, &mi, sizeof(mi)) == sizeof(mi)) {
    const uintptr_t b = (uintptr_t)mi.BaseAddress, e = b + mi.RegionSize;
    if (e <= a) break;
    a = e;
    if (mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD)) continue;
    // A page the journal is holding protected reads back as READONLY/NOACCESS this instant, but it is ordinary private read/write data
    // the moment the journal lets go. Report it that way so its object survives the census while it is being watched.
    const bool ours = (mi.Protect & (PAGE_NOACCESS | PAGE_READONLY)) && mi.Type == MEM_PRIVATE && WPageOurs(b);
    if ((mi.Protect & PAGE_NOACCESS) && !ours) continue;
    const DWORD p = mi.Protect & 0xff;
    InsRegion r{b, e, (uintptr_t)mi.AllocationBase, 0, 0, 0, uint8_t(mi.Type == MEM_IMAGE)};
    r.r = ours || (p & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    r.w = ours || (p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    r.x = !ours && (p & 0xf0) != 0;
    if (!r.r) continue;
    if (n > 0) {
      InsRegion& q = g_insReg[n - 1];
      if (q.e == b && q.w == r.w && q.x == r.x && q.img == r.img && (!r.img || q.alloc == r.alloc)) {
        q.e = e;
        continue;
      }
    }
    if (n >= kInsMaxRegions) break;
    g_insReg[n++] = r;
  }
  g_insRegN = n;
}

const InsRegion* InsFind(uintptr_t a) {
  int lo = 0, hi = g_insRegN - 1;
  while (lo <= hi) {
    const int mid = (lo + hi) / 2;
    const InsRegion& r = g_insReg[mid];
    if (a < r.b)
      hi = mid - 1;
    else if (a >= r.e)
      lo = mid + 1;
    else
      return &r;
  }
  return nullptr;
}

size_t InsReadableLen(uintptr_t a, size_t want) {
  const InsRegion* r = InsFind(a);
  return r ? (std::min)(want, size_t(r->e - a)) : 0;
}

// Heap-like memory a census follows: private writable data, not executable, not ours.
bool InsHeapPtr(uintptr_t v) {
  if (v < 0x10000 || v >= 0x7FFFFFFFFFFFull || (v & 7)) return false;
  const InsRegion* r = InsFind(v);
  return r && !r->img && r->w && !r->x && !InsOwn(v);
}

// ---- RTTI classes

struct InsClass {
  uintptr_t vt;
  const char* raw;  // type descriptor name inside the module image
  char pretty[128];
  char module[48];
  uint8_t interest, entity, stop, written;
  int nBases;
  const char* bases[24];
  int32_t baseDisp[24];
  uint32_t win;  // schema-derived object size estimate (0 = not computed)
};
InsClass* g_insCls = nullptr;
int g_insClsN = 0;
uintptr_t* g_insClsKey = nullptr;
int32_t* g_insClsVal = nullptr;

bool InsRttiName(uintptr_t vt, uintptr_t& base, uintptr_t& col, const char*& name) {
  const InsRegion* rv = InsFind(vt);
  if (!rv || !rv->img || rv->w || rv->x) return false;
  __try {
    col = *(uintptr_t*)(vt - 8);
    const InsRegion* rc = InsFind(col);
    if (!rc || !rc->img || rc->x || *(uint32_t*)col != 1) return false;
    base = col - *(uint32_t*)(col + 20);
    if (base != rc->alloc) return false;
    const InsRegion* rf = InsFind(*(uintptr_t*)vt);
    if (!rf || !rf->img || !rf->x) return false;
    name = (const char*)(base + *(uint32_t*)(col + 12) + 16);
    return name[0] == '.' && name[1] == '?' && name[2] == 'A' && (name[3] == 'V' || name[3] == 'U');
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ".?AVName@Ns@@" -> "Ns::Name"
void InsPretty(const char* raw, char* out, size_t cap) {
  char tmp[256];
  InsSafeStr(raw + 4, tmp, sizeof(tmp));
  if (char* end = strstr(tmp, "@@")) *end = 0;
  const char* seg[16];
  int n = 0;
  char* s = tmp;
  while (n < 16) {
    seg[n++] = s;
    char* at = strchr(s, '@');
    if (!at) break;
    *at = 0;
    s = at + 1;
  }
  out[0] = 0;
  for (int i = n - 1; i >= 0; --i) {
    if (!*seg[i]) continue;
    if (out[0]) strncat_s(out, cap, "::", _TRUNCATE);
    strncat_s(out, cap, seg[i], _TRUNCATE);
  }
}

int InsInterest(const char* n) {
  for (const char* k : {"lambda", "_Func_impl", "Job@", "Factory"})
    if (strstr(n, k)) return 0;
  for (const char* k : {"Ragdoll", "Aggregate"})
    if (strstr(n, k)) return 3;
  for (const char* k : {"PhysicsBody", "CRn", "Phys", "Skeleton", "Pose"})
    if (strstr(n, k)) return 2;
  for (const char* k : {"Joint", "Anim", "SceneNode", "Controller", "Body", "Motion", "Shadow"})
    if (strstr(n, k)) return 1;
  return 1;
}

void InsFillBases(InsClass& c, uintptr_t base, uintptr_t col) {
  c.nBases = 0;
  __try {
    const uintptr_t hier = base + *(uint32_t*)(col + 16);
    const uint32_t n = *(uint32_t*)(hier + 8);
    const uintptr_t arr = base + *(uint32_t*)(hier + 12);
    for (uint32_t i = 0; i < n && c.nBases < 24; ++i) {
      const uintptr_t bcd = base + *(uint32_t*)(arr + i * 4);
      c.bases[c.nBases] = (const char*)(base + *(uint32_t*)bcd + 16);
      c.baseDisp[c.nBases] = *(int32_t*)(bcd + 8);
      if (SafeStrEq((uintptr_t)c.bases[c.nBases], ".?AVC_BaseEntity@@")) c.entity = 1;
      ++c.nBases;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
}

// Class index of a vtable, -1 if it is not a valid vtable. Main thread only.
int InsClassOf(uintptr_t vt) {
  if (!vt || !g_insCls) return -1;
  const InsRegion* r = InsFind(vt);
  if (!r || !r->img || r->w || r->x) return -1;  // vtables live in read-only image data; keeps junk out of the cache
  uint32_t h = uint32_t(((vt >> 3) * 0x9E3779B97F4A7C15ull) >> 49) & (kInsClassHash - 1);
  for (int probe = 0; probe < 64; ++probe, h = (h + 1) & (kInsClassHash - 1)) {
    if (g_insClsKey[h] == vt) return g_insClsVal[h];
    if (g_insClsKey[h]) continue;
    uintptr_t base = 0, col = 0;
    const char* name = nullptr;
    int idx = -1;
    if (InsRttiName(vt, base, col, name) && g_insClsN < kInsMaxClasses) {
      idx = g_insClsN++;
      InsClass& c = g_insCls[idx];
      memset(&c, 0, sizeof(c));
      c.vt = vt;
      c.raw = name;
      InsPretty(name, c.pretty, sizeof(c.pretty));
      OwnerModuleName(vt, c.module, sizeof(c.module));
      c.interest = (uint8_t)InsInterest(c.pretty);
      c.stop = strstr(c.pretty, "EntitySystem") || strstr(c.pretty, "GameResourceService");
      InsFillBases(c, base, col);
    }
    g_insClsKey[h] = vt;
    g_insClsVal[h] = idx;
    return idx;
  }
  return -1;
}

// ---- schema fields (client.dll scope)

uintptr_t g_schemaSysIns = 0;

struct InsField {
  int32_t off;
  const char* name;
  const char* type;
  const char* owner;
};

// Appends the schema fields of one class (shifted by disp). Returns the new count.
int InsSchemaFields(const char* cls, int32_t disp, InsField* out, int n, int cap) {
  const uintptr_t sys = g_schemaSysIns;
  if (!sys || !cls || !*cls) return n;
  __try {
    const uint64_t nScopes = *(uint64_t*)(sys + kSchemaScopeCount);
    const uintptr_t scopes = *(uintptr_t*)(sys + kSchemaScopes);
    if (!scopes || nScopes > 512) return n;
    for (uint64_t i = 0; i < nScopes; ++i) {
      const uintptr_t scope = *(uintptr_t*)(scopes + i * 8);
      if (!scope || strcmp((const char*)(scope + 8), "client.dll")) continue;
      const uint16_t nc = *(uint16_t*)(scope + 0x470);
      const uintptr_t entries = *(uintptr_t*)(scope + 0x478);
      for (uint32_t j = 0; entries && j < nc; ++j) {
        const uintptr_t decl = *(uintptr_t*)(entries + j * 0x18 + 0x10);
        const uintptr_t info = decl ? *(uintptr_t*)(decl + 0x20) : 0;
        const char* name = info ? *(const char**)(info + 8) : nullptr;
        if (!name || strcmp(name, cls)) continue;
        const uint16_t nf = *(uint16_t*)(info + 0x24);
        const uintptr_t fields = *(uintptr_t*)(info + 0x30);
        for (uint32_t k = 0; fields && k < nf && n < cap; ++k) {
          const uintptr_t f = fields + k * 0x20;
          const uintptr_t type = *(uintptr_t*)(f + 8);
          out[n++] = {*(int32_t*)(f + 0x10) + disp, *(const char**)f, type ? *(const char**)(type + 8) : nullptr, name};
        }
        return n;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return n;
}

InsField* g_insFields = nullptr;

// All schema fields of a class and its RTTI bases (the base list starts with the class itself).
int InsClassFields(const InsClass& c, InsField* out, int cap) {
  int n = 0;
  for (int i = 0; i < c.nBases && n < cap; ++i) {
    char pretty[128];
    InsPretty(c.bases[i], pretty, sizeof(pretty));
    n = InsSchemaFields(pretty, c.baseDisp[i], out, n, cap);
  }
  return n;
}

uint32_t InsClassWindow(int cls) {
  if (cls < 0) return 0x1000;
  InsClass& c = g_insCls[cls];
  if (!c.win) {
    const int n = InsClassFields(c, g_insFields, kInsMaxFields);
    int32_t hi = 0;
    for (int i = 0; i < n; ++i) hi = (std::max)(hi, g_insFields[i].off);
    c.win = (std::min)((std::max)((uint32_t((hi + 0x40 + 7) & ~7)), 0x1000u), kInsMaxObjSize);
  }
  return c.win;
}

// K and F records, once per class per file.
void InsWriteClass(int cls) {
  if (cls < 0 || g_insFile == INVALID_HANDLE_VALUE) return;
  InsClass& c = g_insCls[cls];
  if (c.written) return;
  c.written = 1;
  size_t at = InsBegin('K');
  InsPod((uint32_t)cls);
  InsGameStr(c.raw);
  InsStr(c.pretty);
  InsStr(c.module);
  InsPod(c.entity);
  InsPod(c.interest);
  InsEnd(at);
  const int n = InsClassFields(c, g_insFields, kInsMaxFields);
  if (!n) return;
  at = InsBegin('F');
  InsPod((uint32_t)cls);
  InsPod((uint32_t)n);
  for (int i = 0; i < n; ++i) {
    InsPod((uint32_t)g_insFields[i].off);
    InsGameStr(g_insFields[i].name);
    InsGameStr(g_insFields[i].type);
    InsGameStr(g_insFields[i].owner);
  }
  InsEnd(at);
}
