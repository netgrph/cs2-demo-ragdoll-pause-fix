// insight recorder, part 2: target ragdoll, pointer census, tracked object selection, cross-reference scan.

struct InsTarget {
  int idx = -1;
  uintptr_t pawn = 0, vt = 0, node = 0, bones = 0, ctrl = 0;
  uint32_t win = 0, handle = 0;
  float death = 0.0f;
  float c[3] = {0, 0, 0};
  bool haveC = false;
  int cls = -1;
};
InsTarget g_insT;

// Physics body windows the body finder (part 6) identified: memory the ragdoll's body transforms live in, and where those values are
// copied to. InsSelect re-adds them as tracked objects every census so their diffs and page journal keep pointing at them.
struct InsBodyWin {
  uintptr_t addr;
  uint32_t size;
  int32_t cls;
  bool acc;  // journal reads too (the physics bodies themselves)
  char label[128];
};
InsBodyWin g_insBodyWin[40];
int g_insBodyWinN = 0;

// Census root sources, noted by the hooks.
uintptr_t g_insWorld[8];
int g_insWorldN = 0;
float g_insDt = 0.0f;
int g_insSub = 0;
void* g_insPhysSys = nullptr;
uintptr_t g_insPoseCtl = 0, g_insRagSys = 0;
bool g_insSysSearched = false;
uintptr_t g_insXrefRoot[64];
int g_insXrefRootN = 0;

// Newest dead player. Main thread only.
bool InsPickTarget(InsTarget& t) {
  t = InsTarget{};
  if (!g_settleOk) return false;
  DeadPawn dp[64];
  const int n = ScanDeadPawns(dp, 64);
  int best = -1;
  for (int i = 0; i < n; ++i)
    if (best < 0 || dp[i].deathTime > dp[best].deathTime) best = i;
  if (best < 0) return false;
  t.idx = dp[best].idx;
  t.pawn = dp[best].inst;
  t.death = dp[best].deathTime;
  Rd(t.pawn, t.vt);
  t.cls = InsClassOf(t.vt);
  t.win = InsClassWindow(t.cls);
  uintptr_t ident = 0;
  uint32_t h = 0;
  if (Rd(t.pawn + 0x10, ident) && Rd(ident + kIdentityHandle, h) && int(h & 0x7fff) == t.idx) t.handle = h;
  if (g_offNode > 0 && Rd(t.pawn + g_offNode, t.node) && t.node && g_offModelState > 0)
    Rd(t.node + g_offModelState + kModelStateBones, t.bones);
  if (g_animCtrl) t.ctrl = AnimControllerOf(t.pawn);
  t.haveC = PoseCentroid(t.pawn, t.c);
  return true;
}

// Game system instance by RTTI name: stored in client .data, or pointed to from it.
uintptr_t InsStaticInstance(const char* rtti) {
  HMODULE client = GetModuleHandleA("client.dll");
  const uintptr_t vt = FindVtableByRtti(client, rtti);
  const Range d = SectionRange(client, ".data");
  if (!vt || !d.b) return 0;
  uintptr_t viaPtr = 0;
  __try {
    for (uintptr_t a = d.b; a + 8 <= d.b + d.n; a += 8) {
      const uintptr_t v = *(uintptr_t*)a;
      if (v == vt) return a;
      uintptr_t q = 0;
      if (!viaPtr && InsHeapPtr(v) && Rd(v, q) && q == vt) viaPtr = v;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return viaPtr;
}

// ---- census graph

struct InsNode {
  uintptr_t addr, vt;
  int32_t parent, cls;
  uint16_t parentOff, nearOff;
  uint8_t root, depth, flags, interest;
  uint16_t nearCount;
  int16_t score;
};
InsNode* g_insNodes = nullptr;
int g_insNodeN = 0;
uintptr_t* g_insNodeKey = nullptr;
int* g_insOrder = nullptr;
uint8_t* g_insScan = nullptr;  // kInsMaxObjSize
int g_insRootCount[kRootCount];
int g_insCensusN = 0, g_insNearN = 0, g_insOwnedN = 0;
double g_insCensusMs = 0.0;

int InsAddNode(uintptr_t a, int parent, uint16_t off, uint8_t root, uint8_t depth) {
  if (!a || g_insNodeN >= kInsMaxNodes) return -1;
  uint32_t h = uint32_t(((a >> 3) * 0x9E3779B97F4A7C15ull) >> 45) & (kInsNodeHash - 1);
  for (;; h = (h + 1) & (kInsNodeHash - 1)) {
    if (g_insNodeKey[h] == a) return -1;
    if (!g_insNodeKey[h]) break;
  }
  g_insNodeKey[h] = a;
  const int idx = g_insNodeN++;
  InsNode& n = g_insNodes[idx];
  memset(&n, 0, sizeof(n));
  n.addr = a;
  n.parent = parent;
  n.parentOff = off;
  n.root = root;
  n.depth = depth;
  n.cls = -1;
  uintptr_t vt = 0;
  if (Rd(a, vt)) {
    const int c = InsClassOf(vt);
    if (c >= 0) {
      n.vt = vt;
      n.cls = c;
      n.flags |= kNfVt;
      n.interest = g_insCls[c].interest;
      if (g_insCls[c].entity) n.flags |= kNfEntity;
    }
  }
  ++g_insRootCount[root];
  return idx;
}

size_t InsScanLen(uintptr_t a) {
  if (a == g_insT.pawn) return g_insT.win;
  if (a == g_insT.node) return 0x600;
  if (a == g_insT.ctrl) return 0x800;
  return kInsScanWin;
}

// Reads nodes [first, end): marks positions near the ragdoll and references to the pawn, adds heap pointers as children.
void InsBfs(int first, int maxDepth, int cap) {
  const float* c = g_insT.c;
  const uintptr_t pawnLo = g_insT.pawn, pawnHi = g_insT.pawn + g_insT.win;
  const uint32_t handle = (g_insT.handle & ~0x7fffu) ? g_insT.handle : 0;
  for (int i = first; i < g_insNodeN; ++i) {
    InsNode& n = g_insNodes[i];
    const size_t len = InsReadableLen(n.addr, InsScanLen(n.addr)) & ~size_t(7);
    if (len < 8 || !SafeCopy(g_insScan, (void*)n.addr, len)) continue;
    if (g_insT.haveC) {
      for (size_t o = 0; o + 12 <= len; o += 4) {
        float p[3];
        memcpy(p, g_insScan + o, 12);
        if (std::fabs(p[0] - c[0]) < kInsNear && std::fabs(p[1] - c[1]) < kInsNear && std::fabs(p[2] - c[2]) < kInsNear &&
            std::fabs(p[0]) + std::fabs(p[1]) + std::fabs(p[2]) > 1.0f) {
          if (!n.nearCount) n.nearOff = (uint16_t)o;
          if (n.nearCount < 0xffff) ++n.nearCount;
          n.flags |= kNfNear;
        }
      }
    }
    if (pawnLo && n.addr != pawnLo) {
      for (size_t o = 0; o + 8 <= len; o += 8) {
        uintptr_t v;
        memcpy(&v, g_insScan + o, 8);
        if (v >= pawnLo && v < pawnHi) n.flags |= kNfOwned;
        if (handle && (uint32_t)v == handle) n.flags |= kNfOwned;
      }
    }
    if (n.depth >= maxDepth) continue;
    if (n.flags & kNfVt) {
      const InsClass& k = g_insCls[n.cls];
      if (k.stop || !k.interest || (k.entity && n.addr != pawnLo)) continue;
    }
    n.flags |= kNfExpanded;
    for (size_t o = 0; o + 8 <= len; o += 8) {
      uintptr_t v;
      memcpy(&v, g_insScan + o, 8);
      if (!InsHeapPtr(v)) continue;
      if (g_insNodeN >= first + cap) break;
      InsAddNode(v, i, (uint16_t)o, n.root, uint8_t(n.depth + 1));
    }
  }
}

void InsPath(int i, char* out, size_t cap) {
  int chain[16], n = 0;
  for (int j = i; j >= 0 && n < 16; j = g_insNodes[j].parent) chain[n++] = j;
  snprintf(out, cap, "%s", kInsRootNames[g_insNodes[i].root]);
  for (int k = n - 2; k >= 0; --k) {
    const size_t l = strlen(out);
    snprintf(out + l, cap - l, ">+%x", g_insNodes[chain[k]].parentOff);
  }
  if (g_insNodes[i].cls >= 0) {
    const size_t l = strlen(out);
    snprintf(out + l, cap - l, " %s", g_insCls[g_insNodes[i].cls].pretty);
  }
}

void InsScore() {
  g_insNearN = g_insOwnedN = 0;
  for (int i = 0; i < g_insNodeN; ++i) {
    InsNode& n = g_insNodes[i];
    const bool physRoot = n.root == kRootWorld || n.root == kRootPhysSys || n.root == kRootPoseCtl || n.root == kRootRagSys;
    int hops = 0;
    for (int j = n.parent; j >= 0 && hops < 3; j = g_insNodes[j].parent, ++hops)
      if (g_insNodes[j].interest >= 2) n.flags |= kNfLineage;
    if (physRoot && n.depth <= 3) n.flags |= kNfLineage;
    const bool isOwned = n.flags & kNfOwned, isNear = n.flags & kNfNear, hasVt = n.flags & kNfVt;  // "near" is a windows.h macro
    g_insNearN += isNear;
    g_insOwnedN += isOwned;
    int s = 0;
    if (isOwned) s += 8;
    if (isNear) s += n.nearCount >= 3 ? 8 : 6;
    s += 2 * n.interest;
    if ((n.root == kRootPawn || n.root == kRootCtrl) && n.depth <= 2) s += 3;
    if (n.flags & kNfLineage) s += 2;
    if (!hasVt && !isNear) s -= 4;
    if (!isOwned && !isNear) s -= n.depth;
    if ((n.flags & kNfEntity) && n.addr != g_insT.pawn) s -= 10;
    n.score = (int16_t)s;
  }
}

// ---- tracked objects

struct InsObj {
  uintptr_t addr, vt;
  uint32_t size, id;
  int32_t cls;
  uint8_t kind, flags, root, depth;
  bool live, keep;
  char label[128];
};
InsObj g_insObj[kInsMaxObjs];
uint8_t* g_insPrev = nullptr;  // kInsMaxObjs * kInsMaxObjSize
uint32_t g_insNextId = 1;

void InsDropObj(int i, uint8_t reason) {
  InsObj& o = g_insObj[i];
  if (!o.live) return;
  o.live = false;
  const size_t at = InsBegin('X');
  InsPod(o.id);
  InsPod(reason);
  InsEnd(at);
}

int InsObjAdd(uint8_t kind, uintptr_t addr, uint32_t size, int32_t cls, uint8_t flags, uint8_t root, uint8_t depth, const char* label) {
  if (!addr) return -1;
  for (int i = 0; i < kInsMaxObjs; ++i) {
    InsObj& o = g_insObj[i];
    if (o.live && o.addr == addr && o.kind == kind) {
      o.keep = true;
      return i;
    }
  }
  size = uint32_t(InsReadableLen(addr, (std::min)(size, kInsMaxObjSize)) & ~size_t(3));
  if (size < 8) return -1;
  int slot = -1;
  for (int i = 0; i < kInsMaxObjs && slot < 0; ++i)
    if (!g_insObj[i].live) slot = i;
  if (slot < 0) return -1;
  uint8_t* prev = g_insPrev + size_t(slot) * kInsMaxObjSize;
  if (!SafeCopy(prev, (void*)addr, size)) return -1;
  InsObj& o = g_insObj[slot];
  o.addr = addr;
  o.vt = cls >= 0 ? g_insCls[cls].vt : 0;
  o.size = size;
  o.id = g_insNextId++;
  o.cls = cls;
  o.kind = kind;
  o.flags = flags;
  o.root = root;
  o.depth = depth;
  o.live = o.keep = true;
  snprintf(o.label, sizeof(o.label), "%s", label);
  InsWriteClass(cls);
  size_t at = InsBegin('O');
  InsPod(o.id);
  InsPod(kind);
  InsPod(flags);
  InsPod((uint64_t)addr);
  InsPod((uint64_t)o.vt);
  InsPod(size);
  InsPod((uint32_t)cls);
  InsPod(root);
  InsPod(depth);
  InsStr(o.label);
  InsEnd(at);
  at = InsBegin('S');
  InsPod(o.id);
  InsPut(prev, size);
  InsEnd(at);
  return slot;
}

int InsLiveObjs() {
  int n = 0;
  for (int i = 0; i < kInsMaxObjs; ++i) n += g_insObj[i].live;
  return n;
}

// Pin the ragdoll's physics bodies and the memory holding their world transforms. The heuristic body
// finder cannot isolate these (it collects render/particle/anim scratch), but the pointer census reaches
// them structurally: a physics ragdoll/aggregate/shape class near the corpse, plus the near-centroid child
// memory those objects point at (the per-body CTransform arrays). They score too low to survive the object
// cap against churny render/anim memory and were being dropped a few frames in, so re-add them here as
// pinned kObjBody objects every census, before the scored candidates, guaranteeing their freeze/snap
// timeline is always recorded. Data-driven (class name + centroid proximity), no hard-coded offsets.
bool InsPhysCls(int cls) {
  if (cls < 0) return false;
  const char* p = g_insCls[cls].pretty;
  return strstr(p, "Ragdoll") || strstr(p, "Aggregate") || strstr(p, "PhysicsBody") || strstr(p, "RnBody") ||
         strstr(p, "CapsuleShape") || strstr(p, "HullShape") || strstr(p, "MeshShape") || strstr(p, "SphereShape") ||
         strstr(p, "CRnShape") || strstr(p, "PhysicsShape");
}

int InsPinBodies() {
  static int s_lastPin = -1;
  if (!g_insT.haveC) return 0;
  int pinned = 0;
  for (int i = 0; i < g_insNodeN && pinned < 64; ++i) {
    const InsNode& n = g_insNodes[i];
    const bool phys = InsPhysCls(n.cls);
    // Near-centroid child memory hanging off a physics object = the per-body world transforms (often a raw
    // CTransform array with no vtable of its own, so matched by its physics-class parent, not by class).
    bool nearChild = false;
    if (!phys && (n.flags & kNfNear) && n.parent >= 0) {
      const int pc = g_insNodes[n.parent].cls;
      nearChild = InsPhysCls(pc) || (pc >= 0 && strstr(g_insCls[pc].pretty, "Shape"));
    }
    if (!phys && !nearChild) continue;
    char path[128], label[128];
    InsPath(i, path, sizeof(path));
    snprintf(label, sizeof(label), "%s %s", phys ? "PHYSBODY" : "PHYSXFORM", path);
    if (InsObjAdd(kObjBody, n.addr, InsClassWindow(n.cls), n.cls, n.flags | kNfOwned, n.root, n.depth, label) >= 0)
      ++pinned;
  }
  if (pinned != s_lastPin) {
    InsMark("PIN %d physics body objects tracked (ragdoll/aggregate/shape + near-centroid transforms)", pinned);
    s_lastPin = pinned;
  }
  return pinned;
}

void InsSelect(bool targetChanged) {
  for (int i = 0; i < kInsMaxObjs; ++i) g_insObj[i].keep = false;
  uintptr_t gl = 0;
  if (Rd(g_globalsVar, gl) && gl) InsObjAdd(kObjGlobals, gl, 0x100, -1, 0, 0xff, 0, "client globals");
  if (g_insT.pawn) {
    char label[128];
    snprintf(label, sizeof(label), "pawn #%d (dead player)", g_insT.idx);
    InsObjAdd(kObjPawn, g_insT.pawn, g_insT.win, g_insT.cls, kNfOwned | kNfVt, kRootPawn, 0, label);
    uintptr_t vt = 0;
    if (g_insT.node) InsObjAdd(kObjNode, g_insT.node, 0x600, Rd(g_insT.node, vt) ? InsClassOf(vt) : -1, kNfOwned, kRootPawn, 1, "scene node");
    if (g_insT.bones) InsObjAdd(kObjBones, g_insT.bones, 0x1000, -1, kNfOwned, kRootPawn, 2, "bone transforms (32 bytes each)");
    if (g_insT.ctrl) InsObjAdd(kObjCtrl, g_insT.ctrl, 0x800, Rd(g_insT.ctrl, vt) ? InsClassOf(vt) : -1, kNfOwned, kRootCtrl, 0, "animation controller");
  }
  for (int i = 0; i < g_insBodyWinN; ++i) {
    const InsBodyWin& b = g_insBodyWin[i];
    if (b.addr) InsObjAdd(kObjBody, b.addr, b.size, b.cls, kNfOwned, kRootWorld, 2, b.label);
  }
  InsPinBodies();
  int nc = 0;
  for (int i = 0; i < g_insNodeN; ++i)
    if (g_insNodes[i].score >= 6) g_insOrder[nc++] = i;
  std::sort(g_insOrder, g_insOrder + nc, [](int a, int b) {
    const InsNode& x = g_insNodes[a];
    const InsNode& y = g_insNodes[b];
    return x.score != y.score ? x.score > y.score : x.depth != y.depth ? x.depth < y.depth : a < b;
  });
  for (int k = 0; k < nc && InsLiveObjs() < kInsMaxObjs; ++k) {
    const InsNode& n = g_insNodes[g_insOrder[k]];
    bool inside = false;
    for (int i = 0; i < kInsMaxObjs && !inside; ++i) {
      const InsObj& o = g_insObj[i];
      inside = o.live && o.keep && n.addr >= o.addr && n.addr < o.addr + o.size;
    }
    if (inside) continue;
    char label[128];
    InsPath(g_insOrder[k], label, sizeof(label));
    const bool vt = n.flags & kNfVt;
    InsObjAdd(vt ? kObjVt : kObjBlob, n.addr, vt && n.interest >= 2 ? 0x800 : 0x400, n.cls, n.flags, n.root, n.depth, label);
  }
  for (int i = 0; i < kInsMaxObjs; ++i)
    if (g_insObj[i].live && !g_insObj[i].keep) InsDropObj(i, targetChanged && g_insObj[i].kind != kObjGlobals ? 5 : 3);
}

void InsCensus(const char* why) {
  if (!g_insNodes) return;
  const double t0 = Now();
  InsRefreshRegions(true);
  if (!g_insSysSearched) {
    g_insSysSearched = true;
    g_insPoseCtl = InsStaticInstance(".?AVCRagdollPoseControlSystem@@");
    g_insRagSys = InsStaticInstance(".?AVCRagdollGameSystem@@");
  }
  const int prevIdx = g_insT.idx;
  const uintptr_t prevPawn = g_insT.pawn;
  InsPickTarget(g_insT);
  const bool targetChanged = prevPawn != g_insT.pawn || prevIdx != g_insT.idx;
  if (targetChanged) g_insXrefRootN = 0;
  memset(g_insNodeKey, 0, sizeof(uintptr_t) * kInsNodeHash);
  memset(g_insRootCount, 0, sizeof(g_insRootCount));
  g_insNodeN = 0;
  auto root = [](uintptr_t a, uint8_t r, int depth, int cap) {
    const int first = g_insNodeN;
    if (InsAddNode(a, -1, 0, r, 0) >= 0) InsBfs(first, depth, cap);
  };
  root(g_insT.pawn, kRootPawn, 4, 8000);
  root(g_insT.ctrl, kRootCtrl, 3, 2000);
  for (int i = 0; i < g_insWorldN; ++i) root(g_insWorld[i], kRootWorld, 5, 30000);
  root((uintptr_t)g_insPhysSys, kRootPhysSys, 5, 15000);
  root((uintptr_t)g_animSys.load(), kRootAnimSys, 4, 8000);
  root(g_insPoseCtl, kRootPoseCtl, 5, 8000);
  root(g_insRagSys, kRootRagSys, 5, 8000);
  for (int i = 0; i < g_insXrefRootN; ++i) root(g_insXrefRoot[i], kRootXref, 3, 1500);
  InsScore();
  InsSelect(targetChanged);
  ++g_insCensusN;
  g_insCensusMs = (Now() - t0) * 1000.0;
  if (targetChanged && g_insT.pawn)
    InsMark("TARGET #%d pawn %llx class %s window 0x%x node %llx bones %llx ctrl %llx handle %08x death %.4f centroid %.1f %.1f %.1f",
            g_insT.idx, (unsigned long long)g_insT.pawn, g_insT.cls >= 0 ? g_insCls[g_insT.cls].pretty : "?", g_insT.win,
            (unsigned long long)g_insT.node, (unsigned long long)g_insT.bones, (unsigned long long)g_insT.ctrl, g_insT.handle, g_insT.death,
            g_insT.c[0], g_insT.c[1], g_insT.c[2]);
  else if (targetChanged)
    InsMark("TARGET none (no dead player)");
  InsMark("CENSUS %s: %d nodes (pawn %d, animctrl %d, world %d [%d worlds], physsys %d, animsys %d, posectl %d, ragsys %d, xref %d), "
          "near %d, owned %d, tracked %d, %.1f ms",
          why, g_insNodeN, g_insRootCount[kRootPawn], g_insRootCount[kRootCtrl], g_insRootCount[kRootWorld], g_insWorldN,
          g_insRootCount[kRootPhysSys], g_insRootCount[kRootAnimSys], g_insRootCount[kRootPoseCtl], g_insRootCount[kRootRagSys],
          g_insRootCount[kRootXref], g_insNearN, g_insOwnedN, InsLiveObjs(), g_insCensusMs);
}

// ---- cross-reference scan (background thread, ReadProcessMemory only)

constexpr size_t kInsXChunk = 4 << 20;
struct InsHit {
  uintptr_t at, value, owner;
  int32_t cls;
};
InsHit* g_insHits = nullptr;
int g_insHitMerged = 0;
std::atomic<int> g_insHitN{0};
std::atomic<int> g_insXrefState{0};  // 0 idle, 1 scanning, 2 done (waiting for the main thread)
uint8_t* g_insXbuf = nullptr;
uintptr_t g_insXLo[3], g_insXHi[3], g_insXStackLo = 0, g_insXStackHi = 0;
int g_insXrefGen = 0, g_insXrefRunGen = -1;
std::atomic<double> g_insXrefMs{0.0};
std::atomic<uint64_t> g_insXrefBytes{0};

DWORD WINAPI InsXrefThread(LPVOID) {
  const double t0 = Now();
  uint64_t bytes = 0;
  int hits = 0;
  MEMORY_BASIC_INFORMATION mi;
  uintptr_t a = 0x10000;
  while (a < 0x7FFFFFFFFFFFull && hits < kInsMaxHits && VirtualQuery((LPCVOID)a, &mi, sizeof(mi)) == sizeof(mi)) {
    const uintptr_t b = (uintptr_t)mi.BaseAddress, e = b + mi.RegionSize;
    if (e <= a) break;
    a = e;
    const DWORD p = mi.Protect & 0xff;
    // A page the journal is holding READONLY/NOACCESS is really private read/write data: accept it so references living on watched
    // pages are still found (ReadProcessMemory reads it back regardless of the protection, save while it is momentarily NOACCESS).
    const bool ours = mi.State == MEM_COMMIT && mi.Type == MEM_PRIVATE && (mi.Protect & (PAGE_NOACCESS | PAGE_READONLY)) && WPageOurs(b);
    if (mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD) ||
        (!ours && ((mi.Protect & PAGE_NOACCESS) || !(p & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))))
      continue;
    if ((HMODULE)mi.AllocationBase == g_self || InsOwn(b) || (b < g_insXStackHi && e > g_insXStackLo)) continue;
    for (uintptr_t c = b; c < e && hits < kInsMaxHits; c += kInsXChunk) {
      const size_t len = (std::min)(kInsXChunk, size_t(e - c));
      SIZE_T got = 0;
      if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)c, g_insXbuf, len, &got)) continue;
      bytes += got;
      const uint64_t* q = (const uint64_t*)g_insXbuf;
      const size_t nq = got / 8;
      for (size_t k = 0; k < nq; ++k) {
        const uint64_t v = q[k];
        for (int t = 0; t < 3; ++t) {
          if (v - g_insXLo[t] >= g_insXHi[t] - g_insXLo[t]) continue;
          if (hits < kInsMaxHits) g_insHits[hits++] = {c + k * 8, v, 0, -1};
          break;
        }
      }
    }
  }
  g_insHitN = hits;
  g_insXrefBytes = bytes;
  g_insXrefMs = (Now() - t0) * 1000.0;
  g_insXrefState = 2;
  return 0;
}

void InsXrefStart() {
  if (g_insXrefState.load() != 0 || !g_insT.pawn || !g_insXbuf) return;
  g_insXLo[0] = g_insT.pawn, g_insXHi[0] = g_insT.pawn + g_insT.win;
  g_insXLo[1] = g_insT.node, g_insXHi[1] = g_insT.node ? g_insT.node + 0x600 : 0;
  g_insXLo[2] = g_insT.ctrl, g_insXHi[2] = g_insT.ctrl ? g_insT.ctrl + 0x800 : 0;
  const NT_TIB* tib = (const NT_TIB*)NtCurrentTeb();
  g_insXStackLo = (uintptr_t)tib->StackLimit;
  g_insXStackHi = (uintptr_t)tib->StackBase;
  g_insXrefRunGen = g_insXrefGen;
  g_insXrefState = 1;
  if (HANDLE t = CreateThread(nullptr, 0, InsXrefThread, nullptr, 0, nullptr))
    CloseHandle(t);
  else
    g_insXrefState = 0;
}

// Main thread: resolves the object owning each hit (nearest vtable at or before it), writes R records, re-runs the census with owners.
void InsXrefMerge() {
  if (g_insXrefState.load() != 2) return;
  const int n = g_insHitN.load();
  g_insXrefState = 0;
  if (g_insXrefRunGen != g_insXrefGen) return;
  InsRefreshRegions(false);
  g_insXrefRootN = 0;
  for (int i = 0; i < n; ++i) {
    InsHit& h = g_insHits[i];
    for (uintptr_t back = 0; back <= 0x800; back += 8) {
      uintptr_t q = 0;
      const uintptr_t o = h.at - back;
      if (!Rd(o, q)) break;
      const int c = InsClassOf(q);
      if (c < 0) continue;
      h.owner = o;
      h.cls = c;
      break;
    }
    const size_t at = InsBegin('R');
    InsPod((uint64_t)h.at);
    InsPod((uint64_t)h.value);
    InsPod((uint64_t)h.owner);
    InsPod((uint32_t)h.cls);
    InsEnd(at);
    InsWriteClass(h.cls);
    const bool self = h.owner >= g_insT.pawn && h.owner < g_insT.pawn + g_insT.win;
    bool dup = !h.owner || self;
    for (int k = 0; k < g_insXrefRootN && !dup; ++k) dup = g_insXrefRoot[k] == h.owner;
    if (!dup && g_insXrefRootN < 64) g_insXrefRoot[g_insXrefRootN++] = h.owner;
  }
  g_insHitMerged = n;
  InsMark("XREF %d references to the pawn/node/controller in %.0f MB (%.0f ms), %d owner objects", n,
          double(g_insXrefBytes.load()) / (1 << 20), g_insXrefMs.load(), g_insXrefRootN);
  if (g_insXrefRootN) InsCensus("xref owners");
}
