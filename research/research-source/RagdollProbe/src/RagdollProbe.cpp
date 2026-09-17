// RagdollProbe - Phase 1 telemetry addon for the CS2 "ragdolls keep moving while the demo is paused" investigation.
//
// Load into CS2 (started through HLAE with -insecure):   mirv_loadlibrary "C:\.VSCODE\CS2_RagdollDemoFix\RagdollProbe\bin\RagdollProbe.dll"
//
// Writes logs\probe_<date>_<time>.log next to the DLL. Line types (CSV-ish, first field = type):
//   #  console/info message            S  FrameStageNotify call (fc, stage)
//   G  per-frame globals + demo state   W  vphysics world step call (dt, substeps, skipped?)
//   P  CPhysicsGameSystem event call    D  demo player Pause/Resume/SetTimeScale call
//   C  per-frame vtable call counters   E  per-frame ragdoll/physics entity sample
//   T  single-frame position jump (teleport) of a tracked entity
//
// Hotkeys (hold RIGHT CTRL, game window focused):
//   RCtrl+F9  = write a MARK into the log      RCtrl+F10 = toggle entity sampling
//   RCtrl+F11 = experiment: skip client physics world step while the demo is paused
//
// Create an empty file "probe_nocount.txt" next to the DLL to disable the generic vtable call counters.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Builds the build-specific vtable slot indices were verified against (PE TimeDateStamp).
constexpr uint32_t kClientStamp = 0x6aa1ae5e;
constexpr uint32_t kEngine2Stamp = 0x6aa1ae4f;

// ---------------------------------------------------------------------------------------------------------- logging

HMODULE g_self = nullptr;
CRITICAL_SECTION g_logCs;
FILE* g_log = nullptr;
std::atomic<uint64_t> g_logBytes{0};
constexpr uint64_t kMaxLogBytes = 3ull << 30;
using MsgFn = void (*)(const char*, ...);
MsgFn g_Msg = nullptr;
LARGE_INTEGER g_qpcFreq{}, g_qpc0{};
std::wstring g_dir;      // directory of the DLL, with trailing backslash
std::wstring g_logBase;  // logs\probe_<stamp>

double Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart - g_qpc0.QuadPart) / double(g_qpcFreq.QuadPart);
}

void LogV(const char* fmt, va_list ap) {
  if (!g_log || g_logBytes.load(std::memory_order_relaxed) > kMaxLogBytes) return;
  char buf[8192];
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  if (n <= 0) return;
  if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
  EnterCriticalSection(&g_logCs);
  fwrite(buf, 1, (size_t)n, g_log);
  LeaveCriticalSection(&g_logCs);
  g_logBytes.fetch_add((uint64_t)n, std::memory_order_relaxed);
}

void Log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  LogV(fmt, ap);
  va_end(ap);
}

void LogFlush() {
  if (!g_log) return;
  EnterCriticalSection(&g_logCs);
  fflush(g_log);
  LeaveCriticalSection(&g_logCs);
}

// Prints to the CS2 console (tier0 Msg) and mirrors into the log. Messages must end with '\n'.
void Con(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_Msg) g_Msg("[RagdollProbe] %s", buf);
  Log("#,%.6f,%s", Now(), buf);
}

// ---------------------------------------------------------------------------------------------------------- memory

bool SafeCopy(void* dst, const void* src, size_t n) {
  __try {
    memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

template <class T>
bool Rd(uintptr_t a, T& out) {
  return a >= 0x10000 && a < 0x7FFFFFFFFFFFull && SafeCopy(&out, (const void*)a, sizeof(T));
}

bool SafeStr(uintptr_t p, char* out, size_t cap) {
  out[0] = 0;
  if (p < 0x10000 || p >= 0x7FFFFFFFFFFFull) return false;
  __try {
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
      char c = ((const char*)p)[i];
      if (!c) break;
      out[i] = (c >= 32 && c < 127 && c != ',') ? c : '?';
    }
    out[i] = 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
    return false;
  }
}

struct Range {
  uintptr_t b = 0;
  size_t n = 0;
  bool has(uintptr_t a) const { return a >= b && a < b + n; }
};

IMAGE_NT_HEADERS64* Nt(HMODULE m) {
  auto dos = (IMAGE_DOS_HEADER*)m;
  return (IMAGE_NT_HEADERS64*)((uintptr_t)m + dos->e_lfanew);
}

Range SectionRange(HMODULE m, const char* name) {
  if (!m) return {};
  auto nt = Nt(m);
  auto s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
    if (!strncmp((const char*)s->Name, name, 8)) return {(uintptr_t)m + s->VirtualAddress, s->Misc.VirtualSize};
  return {};
}

uint32_t Stamp(HMODULE m) { return m ? Nt(m)->FileHeader.TimeDateStamp : 0; }

uintptr_t FindSig(HMODULE m, const char* sig) {
  std::vector<int> pat;
  for (const char* p = sig; *p;) {
    if (*p == ' ') { ++p; continue; }
    if (*p == '?') { pat.push_back(-1); while (*p == '?') ++p; continue; }
    char* end = nullptr;
    pat.push_back((int)strtoul(p, &end, 16));
    p = end;
  }
  Range t = SectionRange(m, ".text");
  if (!t.b || pat.empty()) return 0;
  const uint8_t* d = (const uint8_t*)t.b;
  const size_t len = pat.size();
  for (size_t i = 0; i + len <= t.n; ++i) {
    size_t j = 0;
    for (; j < len; ++j)
      if (pat[j] >= 0 && d[i + j] != (uint8_t)pat[j]) break;
    if (j == len) return t.b + i;
  }
  return 0;
}

// MSVC RTTI: vtable[-1] -> CompleteObjectLocator -> TypeDescriptor name (".?AVClass@@").
const char* RttiNameOfVtable(uintptr_t vt) {
  if (vt < 0x10000) return nullptr;
  __try {
    uintptr_t col = *(uintptr_t*)(vt - 8);
    if (*(uint32_t*)col != 1) return nullptr;
    uint32_t tdRva = *(uint32_t*)(col + 12), selfRva = *(uint32_t*)(col + 20);
    const char* nm = (const char*)(col - selfRva + tdRva + 16);
    return (nm[0] == '.' && nm[1] == '?') ? nm : nullptr;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return nullptr;
  }
}

uintptr_t FindVtableRaw(uintptr_t base, Range data, Range rdata, const char* td, size_t len) {
  __try {
    for (uintptr_t p = data.b; p + len <= data.b + data.n; ++p) {
      if (*(const char*)p != '.' || memcmp((const char*)p, td, len)) continue;
      uint32_t tdRva = (uint32_t)(p - 16 - base);
      for (uintptr_t q = rdata.b; q + 24 <= rdata.b + rdata.n; q += 4) {
        const uint32_t* c = (const uint32_t*)q;
        if (c[3] != tdRva || c[0] != 1 || c[1] != 0 || c[5] != (uint32_t)(q - base)) continue;
        for (uintptr_t r = rdata.b; r + 8 <= rdata.b + rdata.n; r += 8)
          if (*(uint64_t*)r == (uint64_t)q) return r + 8;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 0;
}

// Primary (offset 0) vtable of a class by its RTTI type name, e.g. ".?AVCPhysicsGameSystem@@".
uintptr_t FindVtable(HMODULE m, const char* td) {
  if (!m) return 0;
  return FindVtableRaw((uintptr_t)m, SectionRange(m, ".data"), SectionRange(m, ".rdata"), td, strlen(td) + 1);
}

void* Iface(const char* mod, const char* name) {
  HMODULE h = GetModuleHandleA(mod);
  if (!h) return nullptr;
  auto f = (void* (*)(const char*, int*))GetProcAddress(h, "CreateInterface");
  return f ? f(name, nullptr) : nullptr;
}

inline void** Vt(void* o) { return *(void***)o; }

bool PatchPtr(void** where, void* fn, void** orig) {
  DWORD old;
  if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) return false;
  if (orig) *orig = *where;
  *where = fn;
  VirtualProtect(where, sizeof(void*), old, &old);
  return true;
}

Range TextOfAddress(uintptr_t a) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)a, &m))
    return {};
  return SectionRange(m, ".text");
}

// ---------------------------------------------------------------------------------------------------------- schema

std::unordered_map<std::string, std::unordered_map<std::string, int>> g_schema;

void LoadSchema() {
  HMODULE ss = GetModuleHandleA("schemasystem.dll");
  uintptr_t sys = 0;
  if (uintptr_t hit = FindSig(ss, "48 89 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 C0 48 C7 05")) {
    int32_t d = 0;
    if (Rd(hit + 3, d)) sys = hit + 7 + d;
  }
  uintptr_t viaIface = (uintptr_t)Iface("schemasystem.dll", "SchemaSystem_001");
  Con("schema system: pattern=%p iface=%p\n", (void*)sys, (void*)viaIface);
  if (!sys) sys = viaIface;

  uint64_t nScopes = 0;
  uintptr_t scopes = 0;
  if (!Rd(sys + 0x190, nScopes) || !Rd(sys + 0x198, scopes) || nScopes > 512 || !scopes) {
    Con("schema: bad scope array (n=%llu)\n", (unsigned long long)nScopes);
    return;
  }
  FILE* f = _wfopen((g_logBase + L"_schema_client.txt").c_str(), L"wb");
  int classes = 0, fields = 0;
  for (uint64_t i = 0; i < nScopes; ++i) {
    uintptr_t scope = 0;
    if (!Rd(scopes + i * 8, scope) || !scope) continue;
    char sname[256];
    SafeStr(scope + 8, sname, sizeof(sname));
    if (strcmp(sname, "client.dll")) continue;
    uint16_t n = 0;
    uintptr_t entries = 0;
    if (!Rd(scope + 0x470, n) || !Rd(scope + 0x478, entries) || !entries) continue;
    for (uint32_t j = 0; j < n; ++j) {
      uintptr_t decl = 0, cls = 0, namePtr = 0;
      if (!Rd(entries + j * 0x18 + 0x10, decl) || !decl || !Rd(decl + 0x20, cls) || !cls || !Rd(cls + 8, namePtr)) continue;
      char cname[256];
      SafeStr(namePtr, cname, sizeof(cname));
      if (!cname[0]) continue;
      uint32_t size = 0;
      uint16_t nf = 0;
      uintptr_t fp = 0;
      Rd(cls + 0x20, size);
      Rd(cls + 0x24, nf);
      Rd(cls + 0x30, fp);
      ++classes;
      if (f) fprintf(f, "class %s size=0x%x fields=%u\n", cname, size, nf);
      auto& map = g_schema[cname];
      for (uint32_t k = 0; fp && k < nf; ++k) {
        uintptr_t fb = fp + k * 0x20, fnp = 0, tp = 0, tnp = 0;
        uint32_t off = 0;
        if (!Rd(fb, fnp) || !Rd(fb + 8, tp) || !Rd(fb + 0x10, off)) continue;
        char fname[256], tname[256] = "?";
        SafeStr(fnp, fname, sizeof(fname));
        if (!fname[0]) continue;
        if (tp && Rd(tp + 8, tnp)) SafeStr(tnp, tname, sizeof(tname));
        map[fname] = (int)off;
        ++fields;
        if (f) fprintf(f, "  +0x%04x %s : %s\n", off, fname, tname);
      }
    }
  }
  if (f) fclose(f);
  Con("schema: %d classes, %d fields read from client.dll scope\n", classes, fields);
}

int Off(const char* cls, const char* field) {
  auto c = g_schema.find(cls);
  if (c == g_schema.end()) return -1;
  auto fl = c->second.find(field);
  return fl == c->second.end() ? -1 : fl->second;
}

struct Offsets {
  int idDesigner = -1;
  int entSceneNode = -1, entLife = -1, entSimTime = -1;
  int nodeAbsOrigin = -1, nodeAbsRot = -1;
  int skelModelState = -1;
  int agRagdollPose = -1, agClientsideRagdoll = -1, agBuiltRagdoll = -1, agRagdollEnabled = -1, agRagdollClientSide = -1;
  int poseTransforms = -1;
  int ragPos = -1;
} g_off;

void ResolveOffsets() {
  g_off.idDesigner = Off("CEntityIdentity", "m_designerName");
  g_off.entSceneNode = Off("C_BaseEntity", "m_pGameSceneNode");
  g_off.entLife = Off("C_BaseEntity", "m_lifeState");
  g_off.entSimTime = Off("C_BaseEntity", "m_flSimulationTime");
  g_off.nodeAbsOrigin = Off("CGameSceneNode", "m_vecAbsOrigin");
  g_off.nodeAbsRot = Off("CGameSceneNode", "m_angAbsRotation");
  g_off.skelModelState = Off("CSkeletonInstance", "m_modelState");
  g_off.agRagdollPose = Off("CBaseAnimGraph", "m_RagdollPose");
  g_off.agClientsideRagdoll = Off("CBaseAnimGraph", "m_pClientsideRagdoll");
  g_off.agBuiltRagdoll = Off("CBaseAnimGraph", "m_bBuiltRagdoll");
  g_off.agRagdollEnabled = Off("CBaseAnimGraph", "m_bRagdollEnabled");
  g_off.agRagdollClientSide = Off("CBaseAnimGraph", "m_bRagdollClientSide");
  g_off.poseTransforms = Off("PhysicsRagdollPose_t", "m_Transforms");
  g_off.ragPos = Off("C_RagdollProp", "m_ragPos");
  const Offsets& o = g_off;
  Con("offsets: designer=%x sceneNode=%x life=%x simTime=%x absOrigin=%x absRot=%x modelState=%x\n", o.idDesigner, o.entSceneNode,
      o.entLife, o.entSimTime, o.nodeAbsOrigin, o.nodeAbsRot, o.skelModelState);
  Con("offsets: ragdollPose=%x clientsideRagdoll=%x builtRagdoll=%x ragdollEnabled=%x ragdollClientSide=%x poseTransforms=%x ragPos=%x\n",
      o.agRagdollPose, o.agClientsideRagdoll, o.agBuiltRagdoll, o.agRagdollEnabled, o.agRagdollClientSide, o.poseTransforms, o.ragPos);
}

// ---------------------------------------------------------------------------------------------------------- call counters

constexpr int kMaxSlots = 320;
struct VtSet {
  char name[128] = {};
  uintptr_t vt = 0;
  int n = 0;
  uint64_t cnt[kMaxSlots] = {};
  uint64_t last[kMaxSlots] = {};
  void* orig[kMaxSlots] = {};
};
CRITICAL_SECTION g_setsCs;
std::vector<VtSet*> g_sets;
uint8_t* g_thunks = nullptr;
size_t g_thunkUsed = 0;
constexpr size_t kThunkCap = 1 << 20;
bool g_enableCounting = true;

// push rax; mov rax, counter; inc qword [rax]; pop rax; jmp [rip+0] target
void* MakeCountThunk(uint64_t* counter, void* target) {
  if (!g_thunks || g_thunkUsed + 32 > kThunkCap) return nullptr;
  uint8_t* t = g_thunks + g_thunkUsed;
  g_thunkUsed += 32;
  size_t i = 0;
  t[i++] = 0x50;
  t[i++] = 0x48; t[i++] = 0xB8; memcpy(t + i, &counter, 8); i += 8;
  t[i++] = 0x48; t[i++] = 0xFF; t[i++] = 0x00;
  t[i++] = 0x58;
  t[i++] = 0xFF; t[i++] = 0x25; memset(t + i, 0, 4); i += 4;
  memcpy(t + i, &target, 8);
  return t;
}

bool SetExists(uintptr_t vt) {
  EnterCriticalSection(&g_setsCs);
  bool found = false;
  for (auto* s : g_sets) found |= s->vt == vt;
  LeaveCriticalSection(&g_setsCs);
  return found;
}

VtSet* InstallCountSet(const char* name, uintptr_t vt) {
  if (!vt || SetExists(vt)) return nullptr;
  uintptr_t first = 0;
  if (!Rd(vt, first)) return nullptr;
  Range text = TextOfAddress(first);
  auto* s = new VtSet();
  snprintf(s->name, sizeof(s->name), "%s", name);
  s->vt = vt;
  for (int i = 0; i < kMaxSlots; ++i) {
    uintptr_t fn = 0;
    if (!Rd(vt + i * 8, fn) || !text.has(fn)) break;
    s->n = i + 1;
    s->orig[i] = (void*)fn;
    if (!g_enableCounting) continue;
    if (void* th = MakeCountThunk(&s->cnt[i], (void*)fn)) PatchPtr((void**)(vt + i * 8), th, nullptr);
  }
  EnterCriticalSection(&g_setsCs);
  g_sets.push_back(s);
  LeaveCriticalSection(&g_setsCs);
  Con("vtable %s @ %p: %d slots%s\n", name, (void*)vt, s->n, g_enableCounting ? " (counting)" : "");
  return s;
}

void LogCounters(int fc) {
  EnterCriticalSection(&g_setsCs);
  for (auto* s : g_sets) {
    char line[8000];
    int p = snprintf(line, sizeof(line), "C,%d,%s", fc, s->name);
    bool any = false;
    for (int i = 0; i < s->n && p < (int)sizeof(line) - 32; ++i) {
      uint64_t c = s->cnt[i];
      if (c == s->last[i]) continue;
      p += snprintf(line + p, sizeof(line) - p, ",%d:%llu", i, (unsigned long long)(c - s->last[i]));
      s->last[i] = c;
      any = true;
    }
    if (any) Log("%s\n", line);
  }
  LeaveCriticalSection(&g_setsCs);
}

// ---------------------------------------------------------------------------------------------------------- shared state

void* g_engine = nullptr;  // ISource2EngineToClient
uintptr_t g_globalsVar = 0;  // address of client's CGlobalVars pointer
uintptr_t g_entSysVar = 0;   // address of client's CGameEntitySystem pointer
std::atomic<int> g_curFc{0};
std::atomic<int> g_demoTick{-1};
std::atomic<bool> g_demoPlaying{false}, g_demoPaused{false};
std::atomic<bool> g_sampleEntities{true};
std::atomic<bool> g_expSkipStepPaused{false};
std::atomic<uint64_t> g_stepCalls{0}, g_stepSkipped{0};
std::atomic<int64_t> g_stepDtUs{0};

struct DemoState {
  bool playing = false, paused = false;
  int tick = -1;
};

bool QueryDemo(DemoState& s) {
  s.playing = false;
  s.paused = false;
  s.tick = -1;
  if (!g_engine) return false;
  __try {
    s.playing = ((bool (*)(void*))Vt(g_engine)[42])(g_engine);
    void* d = ((void* (*)(void*))Vt(g_engine)[69])(g_engine);
    if (d && s.playing) {
      s.paused = ((bool (*)(void*))Vt(d)[12])(d);
      s.tick = ((int (*)(void*))Vt(d)[3])(d);
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// ---------------------------------------------------------------------------------------------------------- typed hooks

// CPhysicsGameSystem game-system events (slot 28 and 32 both run CPhysicsGameSystem::OnSimulate in this build).
using PgsEvFn = void* (*)(void*, void*);
PgsEvFn o_pgs28 = nullptr, o_pgs32 = nullptr;

void LogPgs(int slot, void* self, void* ev, double dur) {
  uint8_t e[8] = {};
  SafeCopy(e, ev, 8);
  int32_t i104 = 0, substeps = 0, t528 = 0, t52c = 0;
  float subDt = 0;
  uint8_t b110 = 0;
  uintptr_t p = (uintptr_t)self;
  Rd(p + 0x104, i104);
  Rd(p + 0x108, substeps);
  Rd(p + 0x10c, subDt);
  Rd(p + 0x110, b110);
  Rd(p + 0x528, t528);
  Rd(p + 0x52c, t52c);
  Log("P,%.6f,%d,%d,%d,slot=%d,ev=%02x%02x%02x%02x%02x%02x%02x%02x,i104=%d,substeps=%d,subdt=%.7f,b110=%u,t528=%d,t52c=%d,ms=%.3f\n",
      Now(), g_curFc.load(), g_demoTick.load(), (int)g_demoPaused.load(), slot, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], i104,
      substeps, subDt, b110, t528, t52c, dur * 1000.0);
}

void* Hk_Pgs28(void* self, void* ev) {
  double t0 = Now();
  void* r = o_pgs28(self, ev);
  LogPgs(28, self, ev, Now() - t0);
  return r;
}

void* Hk_Pgs32(void* self, void* ev) {
  double t0 = Now();
  void* r = o_pgs32(self, ev);
  LogPgs(32, self, ev, Now() - t0);
  return r;
}

// VPhysics2_Interface_001 slot 16: steps all client physics worlds.
// Called from client CPhysicsGameSystem with dt = cl_phys_timescale * per-substep dt.
using StepFn = void* (*)(void*, void**, int, float, int, bool, void*);
StepFn o_step = nullptr;
std::atomic<bool> g_worldsHooked{false};

void HookWorlds(void** worlds, int count) {
  for (int i = 0; i < count && i < 16; ++i) {
    uintptr_t w = 0, vt = 0;
    if (!Rd((uintptr_t)worlds + i * 8, w) || !Rd(w, vt)) continue;
    const char* nm = RttiNameOfVtable(vt);
    char name[128];
    snprintf(name, sizeof(name), "world:%s", nm ? nm : "?");
    Con("physics world[%d] = %p vtable %p %s\n", i, (void*)w, (void*)vt, name);
    InstallCountSet(name, vt);
  }
}

void* Hk_Step(void* self, void** worlds, int count, float dt, int substeps, bool b, void* p) {
  bool skip = g_expSkipStepPaused.load() && g_demoPlaying.load() && g_demoPaused.load();
  g_stepCalls.fetch_add(1);
  g_stepDtUs.fetch_add((int64_t)(dt * 1e6));
  if (skip) g_stepSkipped.fetch_add(1);
  if (count > 0 && !g_worldsHooked.exchange(true)) HookWorlds(worlds, count);
  Log("W,%.6f,%d,%d,%d,count=%d,dt=%.7f,substeps=%d,b=%d,skip=%d,thread=%lu\n", Now(), g_curFc.load(), g_demoTick.load(),
      (int)g_demoPaused.load(), count, dt, substeps, (int)b, (int)skip, GetCurrentThreadId());
  if (skip) return nullptr;
  return o_step(self, worlds, count, dt, substeps, b, p);
}

// CDemoPlayer (engine2) slots 16/18/20.
using DemoFloatFn = void* (*)(void*, float);
using DemoVoidFn = void* (*)(void*);
DemoFloatFn o_demoTimeScale = nullptr, o_demoPause = nullptr;
DemoVoidFn o_demoResume = nullptr;

void* Hk_DemoTimeScale(void* self, float v) {
  static float last = -12345.0f;
  Log("D,%.6f,%d,%d,SetTimeScale,%.4f\n", Now(), g_curFc.load(), g_demoTick.load(), v);
  if (v != last) Con("demo SetTimeScale(%.3f) at tick %d\n", v, g_demoTick.load());
  last = v;
  return o_demoTimeScale(self, v);
}

void* Hk_DemoPause(void* self, float v) {
  Log("D,%.6f,%d,%d,Pause,%.4f\n", Now(), g_curFc.load(), g_demoTick.load(), v);
  Con("demo Pause(%.3f) at tick %d\n", v, g_demoTick.load());
  return o_demoPause(self, v);
}

void* Hk_DemoResume(void* self) {
  Log("D,%.6f,%d,%d,Resume\n", Now(), g_curFc.load(), g_demoTick.load());
  Con("demo Resume() at tick %d\n", g_demoTick.load());
  return o_demoResume(self);
}

// ---------------------------------------------------------------------------------------------------------- entity sampling

std::unordered_map<uintptr_t, std::string> g_classCache;

const std::string& ClassOf(uintptr_t vt) {
  auto it = g_classCache.find(vt);
  if (it != g_classCache.end()) return it->second;
  std::string name = "?";
  if (const char* nm = RttiNameOfVtable(vt)) {
    name = nm + 4;  // skip ".?AV"
    auto at = name.find("@@");
    if (at != std::string::npos) name.resize(at);
  }
  return g_classCache.emplace(vt, name).first->second;
}

bool InterestingClass(const std::string& c) {
  static const char* keys[] = {"Ragdoll", "Physics", "Breakable", "Debris", "Gib", "Shard"};
  for (const char* k : keys)
    if (c.find(k) != std::string::npos) return true;
  return false;
}

bool Finite3(const float* v) {
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(v[i]) || std::fabs(v[i]) > 1e6f) return false;
  return true;
}

float Dist(const float* a, const float* b) {
  float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct Track {
  std::string cls;
  float last[3] = {};
  float pauseStart[3] = {};
  float resumeStart[3] = {};
  float maxPauseMove = 0, maxResumeStep = 0, maxResumeMove = 0;
  bool inPause = false;
  int lastFc = -1000000;
  double lastTeleportCon = -100;
};
std::unordered_map<int, Track> g_tracks;
int g_resumeFramesLeft = -1;

void SampleEntity(int fc, const DemoState& ds, int idx, uintptr_t inst, const std::string& cls, const uint8_t* identity) {
  char designer[64] = "";
  if (g_off.idDesigner >= 0 && g_off.idDesigner + 8 <= 0x70) {
    uintptr_t dn = 0;
    memcpy(&dn, identity + g_off.idDesigner, 8);
    SafeStr(dn, designer, sizeof(designer));
  }
  uint8_t life = 0xff;
  if (g_off.entLife >= 0) Rd(inst + g_off.entLife, life);
  bool pawn = cls.find("PlayerPawn") != std::string::npos;
  if (!InterestingClass(cls) && !(pawn && life != 0)) return;

  float simTime = 0;
  if (g_off.entSimTime >= 0) Rd(inst + g_off.entSimTime, simTime);
  uintptr_t node = 0;
  float org[3] = {}, ang[3] = {};
  bool hasOrg = false;
  if (g_off.entSceneNode >= 0 && Rd(inst + g_off.entSceneNode, node) && node) {
    if (g_off.nodeAbsOrigin >= 0) hasOrg = SafeCopy(org, (void*)(node + g_off.nodeAbsOrigin), 12) && Finite3(org);
    if (g_off.nodeAbsRot >= 0) SafeCopy(ang, (void*)(node + g_off.nodeAbsRot), 12);
  }
  float bones[4][8] = {};
  bool hasBones = false;
  uintptr_t bonePtr = 0;
  if (node && g_off.skelModelState >= 0 && Rd(node + g_off.skelModelState + 0x80, bonePtr) && bonePtr)
    hasBones = SafeCopy(bones, (void*)bonePtr, sizeof(bones)) && Finite3(bones[0]);

  int32_t poseN = -1;
  float pose0[3] = {};
  if (g_off.agRagdollPose >= 0 && g_off.poseTransforms >= 0) {
    uintptr_t v = inst + g_off.agRagdollPose + g_off.poseTransforms, data = 0;
    if (Rd(v, poseN) && poseN > 0 && Rd(v + 8, data) && data) SafeCopy(pose0, (void*)data, 12);
  }
  int32_t ragN = -1;
  float rag0[3] = {};
  if (g_off.ragPos >= 0 && cls.find("RagdollProp") != std::string::npos) {
    uintptr_t data = 0;
    if (Rd(inst + g_off.ragPos, ragN) && ragN > 0 && Rd(inst + g_off.ragPos + 8, data) && data) SafeCopy(rag0, (void*)data, 12);
  }
  uint8_t built = 0xff, enabled = 0xff, clientSide = 0xff;
  uintptr_t csRag = 0;
  if (g_off.agBuiltRagdoll >= 0) Rd(inst + g_off.agBuiltRagdoll, built);
  if (g_off.agRagdollEnabled >= 0) Rd(inst + g_off.agRagdollEnabled, enabled);
  if (g_off.agRagdollClientSide >= 0) Rd(inst + g_off.agRagdollClientSide, clientSide);
  if (g_off.agClientsideRagdoll >= 0) Rd(inst + g_off.agClientsideRagdoll, csRag);

  Log("E,%d,%d,%d,%d,%s,%s,life=%u,org=%.3f;%.3f;%.3f,ang=%.3f;%.3f;%.3f,b0=%.3f;%.3f;%.3f,q0=%.4f;%.4f;%.4f;%.4f,b1=%.3f;%.3f;%.3f,"
      "b2=%.3f;%.3f;%.3f,b3=%.3f;%.3f;%.3f,sim=%.4f,pose=%d;%.3f;%.3f;%.3f,ragpos=%d;%.3f;%.3f;%.3f,built=%u,enabled=%u,clientside=%u,"
      "csrag=%p,bones=%d\n",
      fc, ds.tick, (int)ds.paused, idx, cls.c_str(), designer, life, org[0], org[1], org[2], ang[0], ang[1], ang[2], bones[0][0],
      bones[0][1], bones[0][2], bones[0][4], bones[0][5], bones[0][6], bones[0][7], bones[1][0], bones[1][1], bones[1][2], bones[2][0],
      bones[2][1], bones[2][2], bones[3][0], bones[3][1], bones[3][2], simTime, poseN, pose0[0], pose0[1], pose0[2], ragN, rag0[0],
      rag0[1], rag0[2], built, enabled, clientSide, (void*)csRag, (int)hasBones);

  const float* pos = hasBones ? bones[0] : (hasOrg ? org : nullptr);
  if (!pos) return;
  Track& tr = g_tracks[idx];
  bool fresh = fc - tr.lastFc > 5 || tr.cls != cls;
  if (fresh) {
    tr = Track();
    tr.cls = cls;
    memcpy(tr.last, pos, 12);
    memcpy(tr.pauseStart, pos, 12);
    memcpy(tr.resumeStart, pos, 12);
    tr.inPause = ds.paused;
  }
  float step = Dist(pos, tr.last);
  if (!fresh && step > 24.0f) {
    Log("T,%.6f,%d,%d,%d,%d,%s,jump=%.2f,from=%.2f;%.2f;%.2f,to=%.2f;%.2f;%.2f\n", Now(), fc, ds.tick, (int)ds.paused, idx, cls.c_str(),
        step, tr.last[0], tr.last[1], tr.last[2], pos[0], pos[1], pos[2]);
    if (Now() - tr.lastTeleportCon > 1.0) {
      Con("TELEPORT? #%d %s jumped %.1f units in one frame (tick %d, paused=%d)\n", idx, cls.c_str(), step, ds.tick, (int)ds.paused);
      tr.lastTeleportCon = Now();
    }
  }
  if (tr.inPause) tr.maxPauseMove = (std::max)(tr.maxPauseMove, Dist(pos, tr.pauseStart));
  if (g_resumeFramesLeft >= 0) {
    tr.maxResumeStep = (std::max)(tr.maxResumeStep, step);
    tr.maxResumeMove = (std::max)(tr.maxResumeMove, Dist(pos, tr.resumeStart));
  }
  memcpy(tr.last, pos, 12);
  tr.lastFc = fc;
}

void SampleEntities(int fc, const DemoState& ds) {
  uintptr_t es = 0;
  if (!g_entSysVar || !Rd(g_entSysVar, es) || !es) return;
  static uint8_t chunk[512 * 0x70];
  for (int c = 0; c < 64; ++c) {
    uintptr_t ch = 0;
    if (!Rd(es + 0x10 + c * 8, ch) || !ch) continue;
    bool whole = SafeCopy(chunk, (void*)ch, sizeof(chunk));
    for (int i = 0; i < 512; ++i) {
      uint8_t* id = chunk + i * 0x70;
      if (!whole && !SafeCopy(id, (void*)(ch + i * 0x70), 0x70)) continue;
      uintptr_t inst = 0, vt = 0;
      uint32_t handle = 0;
      memcpy(&inst, id, 8);
      memcpy(&handle, id + 0x10, 4);
      int idx = c * 512 + i;
      if (!inst || (int)(handle & 0x7fff) != idx || !Rd(inst, vt)) continue;
      SampleEntity(fc, ds, idx, inst, ClassOf(vt), id);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------- frame logic

bool g_prevPaused = false;
double g_pauseT0 = 0;
int g_pauseTick0 = -1;
uint64_t g_pauseSteps0 = 0, g_pauseSkipped0 = 0;
int64_t g_pauseDtUs0 = 0;

void PauseTransitions(const DemoState& ds, int fc, float frametime) {
  bool paused = ds.playing && ds.paused;
  if (paused && !g_prevPaused) {
    g_pauseT0 = Now();
    g_pauseTick0 = ds.tick;
    g_pauseSteps0 = g_stepCalls.load();
    g_pauseSkipped0 = g_stepSkipped.load();
    g_pauseDtUs0 = g_stepDtUs.load();
    for (auto& kv : g_tracks) {
      memcpy(kv.second.pauseStart, kv.second.last, 12);
      kv.second.maxPauseMove = 0;
      kv.second.inPause = true;
    }
    Con("== PAUSED at tick %d (globals frametime=%.5f, skip-physics experiment %s)\n", ds.tick, frametime,
        g_expSkipStepPaused.load() ? "ON" : "off");
  } else if (!paused && g_prevPaused) {
    uint64_t steps = g_stepCalls.load() - g_pauseSteps0, skipped = g_stepSkipped.load() - g_pauseSkipped0;
    double dt = (g_stepDtUs.load() - g_pauseDtUs0) / 1e6;
    Con("== RESUMED at tick %d after %.2fs paused (paused at tick %d). Physics world steps during pause: %llu (skipped %llu), "
        "summed step dt %.3fs\n", ds.tick, Now() - g_pauseT0, g_pauseTick0, (unsigned long long)steps, (unsigned long long)skipped, dt);
    int shown = 0;
    for (auto& kv : g_tracks) {
      Track& tr = kv.second;
      if (fc - tr.lastFc > 3) continue;
      if (shown++ < 12)
        Con("   #%d %s: moved up to %.1f units while paused (net %.1f)\n", kv.first, tr.cls.c_str(), tr.maxPauseMove,
            Dist(tr.last, tr.pauseStart));
      tr.inPause = false;
      memcpy(tr.resumeStart, tr.last, 12);
      tr.maxResumeStep = 0;
      tr.maxResumeMove = 0;
    }
    g_resumeFramesLeft = 10;
  }
  g_prevPaused = paused;

  if (g_resumeFramesLeft >= 0 && g_resumeFramesLeft-- == 0) {
    int shown = 0;
    for (auto& kv : g_tracks) {
      Track& tr = kv.second;
      if (fc - tr.lastFc > 3 || shown++ >= 12) continue;
      Con("   after resume (10 frames) #%d %s: biggest single-frame jump %.1f, total move %.1f\n", kv.first, tr.cls.c_str(),
          tr.maxResumeStep, tr.maxResumeMove);
    }
  }
}

bool Edge(int vk) {
  static bool prev[256];
  bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  bool r = down && !prev[vk];
  prev[vk] = down;
  return r;
}

void HandleHotkeys(const DemoState& ds) {
  bool k9 = Edge(VK_F9), k10 = Edge(VK_F10), k11 = Edge(VK_F11);
  if (!(GetAsyncKeyState(VK_RCONTROL) & 0x8000)) return;
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  if (w) GetWindowThreadProcessId(w, &pid);
  if (pid != GetCurrentProcessId()) return;
  if (k9) {
    static int mark = 0;
    Con("MARK %d at tick %d (paused=%d)\n", ++mark, ds.tick, (int)ds.paused);
    LogFlush();
  }
  if (k10) {
    g_sampleEntities = !g_sampleEntities.load();
    Con("entity sampling %s\n", g_sampleEntities.load() ? "ON" : "OFF");
  }
  if (k11) {
    g_expSkipStepPaused = !g_expSkipStepPaused.load();
    Con("EXPERIMENT skip physics world step while demo paused: %s\n", g_expSkipStepPaused.load() ? "ON" : "OFF");
  }
}

void LogGlobals(uintptr_t gl, int fc, const DemoState& ds, float& frametimeOut) {
  uint32_t raw[24] = {};
  if (!gl || !SafeCopy(raw, (void*)gl, sizeof(raw))) {
    Log("G,%.6f,%d,noglobals\n", Now(), fc);
    return;
  }
  auto F = [&](int i) { float f; memcpy(&f, &raw[i], 4); return f; };
  char hex[24 * 9 + 1];
  for (int i = 0; i < 24; ++i) snprintf(hex + i * 9, 10, "%08x%c", raw[i], i == 23 ? '\0' : ' ');
  frametimeOut = F(13);
  Log("G,%.6f,%d,playing=%d,paused=%d,tick=%d,realtime=%.5f,ft08=%.6f,absft0c=%.6f,curtime30=%.5f,physft34=%.6f,f38=%.6f,f3c=%.6f,raw=%s\n", Now(),
      fc, (int)ds.playing, (int)ds.paused, ds.tick, F(0), F(2), F(3), F(12), F(13), F(14), F(15), hex);
}

using FsnFn = void (*)(void*, int);
FsnFn o_fsn = nullptr;
double g_lastFlush = 0;

void OnFrameStage(int stage) {
  DemoState ds;
  QueryDemo(ds);
  g_demoPlaying = ds.playing;
  g_demoPaused = ds.paused;
  g_demoTick = ds.tick;
  uintptr_t gl = 0;
  if (g_globalsVar) Rd(g_globalsVar, gl);
  int32_t fc = 0;
  if (gl) Rd(gl + 4, fc);
  HandleHotkeys(ds);
  if (ds.playing) Log("S,%.6f,%d,%d\n", Now(), fc, stage);

  static int lastFc = -1;
  if (fc == lastFc) return;
  lastFc = fc;
  g_curFc = fc;
  float frametime = 0;
  if (ds.playing) {
    LogGlobals(gl, fc, ds, frametime);
    LogCounters(fc);
    if (g_sampleEntities.load()) SampleEntities(fc, ds);
  }
  PauseTransitions(ds, fc, frametime);
  if (Now() - g_lastFlush > 0.5) {
    LogFlush();
    g_lastFlush = Now();
  }
}

void Hk_Fsn(void* self, int stage) {
  OnFrameStage(stage);
  o_fsn(self, stage);
}

// ---------------------------------------------------------------------------------------------------------- init

void OpenLog() {
  wchar_t path[MAX_PATH];
  GetModuleFileNameW(g_self, path, MAX_PATH);
  g_dir = path;
  g_dir.resize(g_dir.find_last_of(L'\\') + 1);
  std::wstring logs = g_dir + L"logs\\";
  CreateDirectoryW(logs.c_str(), nullptr);
  SYSTEMTIME st;
  GetLocalTime(&st);
  wchar_t name[64];
  swprintf(name, 64, L"probe_%04u%02u%02u_%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  g_logBase = logs + name;
  g_log = _wfopen((g_logBase + L".log").c_str(), L"wb");
  if (g_log) setvbuf(g_log, nullptr, _IOFBF, 1 << 20);
}

void InstallTypedSlot(uintptr_t vt, int slot, void* hook, void** orig, const char* what) {
  if (!vt) return;
  if (PatchPtr((void**)(vt + slot * 8), hook, orig))
    Con("hooked %s (vtable %p slot %d)\n", what, (void*)vt, slot);
  else
    Con("FAILED to hook %s\n", what);
}

DWORD WINAPI InitThread(LPVOID) {
  const char* mods[] = {"tier0.dll", "engine2.dll", "schemasystem.dll", "vphysics2.dll", "client.dll"};
  for (;;) {
    bool all = true;
    for (const char* m : mods) all &= GetModuleHandleA(m) != nullptr;
    if (all) break;
    Sleep(250);
  }
  Sleep(500);
  g_Msg = (MsgFn)GetProcAddress(GetModuleHandleA("tier0.dll"), "Msg");
  OpenLog();
  g_enableCounting = GetFileAttributesW((g_dir + L"probe_nocount.txt").c_str()) == INVALID_FILE_ATTRIBUTES;
  g_thunks = (uint8_t*)VirtualAlloc(nullptr, kThunkCap, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);

  HMODULE client = GetModuleHandleA("client.dll"), engine2 = GetModuleHandleA("engine2.dll"), vphys = GetModuleHandleA("vphysics2.dll");
  Con("RagdollProbe loading. client.dll stamp %08x, engine2.dll %08x, vphysics2.dll %08x\n", Stamp(client), Stamp(engine2), Stamp(vphys));
  bool clientKnown = Stamp(client) == kClientStamp, engineKnown = Stamp(engine2) == kEngine2Stamp;
  if (!clientKnown || !engineKnown)
    Con("WARNING: game build differs from the analysed one; build-specific slot hooks are disabled.\n");
  Log("#,legend,S=FrameStageNotify G=globals W=physics world step P=CPhysicsGameSystem event D=demo player C=call counters "
      "E=entity sample T=teleport\n");

  g_engine = Iface("engine2.dll", "Source2EngineToClient001");
  LoadSchema();
  ResolveOffsets();

  if (uintptr_t hit = FindSig(client, "40 53 48 83 EC 20 0F B6 D9 BA FF FF FF FF 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 0B 48 8B 05 ?? "
                                      "?? ?? ?? 48 8B 40 08 80 38 00 75 16 84 DB 75 12 48 8B 05 ?? ?? ?? ?? F3 0F 10 40 34")) {
    int32_t d = 0;
    Rd(hit + 0x36, d);
    g_globalsVar = hit + 0x3a + d;
  }
  if (uintptr_t hit = FindSig(client, "40 55 53 48 8d ac 24 ?? ?? ?? ?? 48 81 ec ?? ?? ?? ?? 48 8b 0d ?? ?? ?? ?? 33 d2 e8")) {
    int32_t d = 0;
    Rd(hit + 21, d);
    g_entSysVar = hit + 25 + d;
  }
  Con("engine=%p globalsVar=%p (client+%llx) entitySystemVar=%p (client+%llx)\n", g_engine, (void*)g_globalsVar,
      (unsigned long long)(g_globalsVar ? g_globalsVar - (uintptr_t)client : 0), (void*)g_entSysVar,
      (unsigned long long)(g_entSysVar ? g_entSysVar - (uintptr_t)client : 0));

  // Generic call counters (show which systems run while paused).
  uintptr_t pgsVt = FindVtable(client, ".?AVCPhysicsGameSystem@@");
  InstallCountSet("CPhysicsGameSystem", pgsVt);
  InstallCountSet("CRagdollGameSystem", FindVtable(client, ".?AVCRagdollGameSystem@@"));
  InstallCountSet("CRagdollPoseControlSystem", FindVtable(client, ".?AVCRagdollPoseControlSystem@@"));
  InstallCountSet("CRagdollManager", FindVtable(client, ".?AVCRagdollManager@@"));
  InstallCountSet("C_ClientRagdoll", FindVtable(client, ".?AVC_ClientRagdoll@@"));
  InstallCountSet("C_RagdollProp", FindVtable(client, ".?AVC_RagdollProp@@"));
  uintptr_t demoVt = FindVtable(engine2, ".?AVCDemoPlayer@@");
  InstallCountSet("CDemoPlayer", demoVt);
  void* phys = Iface("vphysics2.dll", "VPhysics2_Interface_001");
  uintptr_t physVt = phys ? (uintptr_t)Vt(phys) : 0;
  if (phys) InstallCountSet("VPhysics2_Interface_001", physVt);

  // Typed hooks (slot indices verified for the analysed build only).
  if (clientKnown) {
    InstallTypedSlot(pgsVt, 28, &Hk_Pgs28, (void**)&o_pgs28, "CPhysicsGameSystem event slot 28");
    InstallTypedSlot(pgsVt, 32, &Hk_Pgs32, (void**)&o_pgs32, "CPhysicsGameSystem event slot 32");
    InstallTypedSlot(physVt, 16, &Hk_Step, (void**)&o_step, "VPhysics2 world step");
  }
  if (engineKnown) {
    InstallTypedSlot(demoVt, 16, &Hk_DemoTimeScale, (void**)&o_demoTimeScale, "CDemoPlayer::SetTimeScale");
    InstallTypedSlot(demoVt, 18, &Hk_DemoPause, (void**)&o_demoPause, "CDemoPlayer::Pause");
    InstallTypedSlot(demoVt, 20, &Hk_DemoResume, (void**)&o_demoResume, "CDemoPlayer::Resume");
  }

  if (void* s2c = Iface("client.dll", "Source2Client002"))
    InstallTypedSlot((uintptr_t)Vt(s2c), 36, &Hk_Fsn, (void**)&o_fsn, "Source2Client::FrameStageNotify");
  else
    Con("FAILED: Source2Client002 not found, no per-frame logging\n");

  Con("ready. Hold RIGHT CTRL + F9 = mark, F10 = toggle entity sampling, F11 = toggle 'skip physics while paused' experiment\n");
  Con("log: %ls.log\n", g_logBase.c_str());
  LogFlush();
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = h;
    DisableThreadLibraryCalls(h);
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)h, &pinned);
    InitializeCriticalSection(&g_logCs);
    InitializeCriticalSection(&g_setsCs);
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpc0);
    if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
  }
  return TRUE;
}
