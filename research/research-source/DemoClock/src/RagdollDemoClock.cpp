// RagdollDemoClock - HLAE addon: CS2 demo ragdolls and physics debris follow demo time.
// Pausing a demo freezes them in place, resuming continues the motion without the snap, tick stepping advances them by exactly
// the stepped demo time.
//
// Load (CS2 started through HLAE with -insecure):
//   mirv_loadlibrary "C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\bin\RagdollDemoClock.dll"
// Then type "ragdollfix" in the console for status and options.
//
// How it works: CS2 steps all client physics worlds (VPhysics2_Interface_001 vtable slot 16) once per engine client tick, whether
// or not demo time moves. The hook keeps a physics clock P, measured in demo ticks, and lets a step through only when the demo
// clock T = demo tick + tick interpolation fraction has advanced far enough to cover it. One step covers
// dt / cl_phys_timescale seconds of demo time, so demo speed and cl_phys_timescale keep working exactly like in vanilla CS2.
//
// Seek settle: seeking (demo_gototick, timeline) reloads a full snapshot and recreates every entity, so a player who is already dead
// at the landing tick gets a brand-new ragdoll that has received no physics yet (T-pose). The addon tracks, per dead player pawn,
// how much physics its ragdoll has received since it was built and compares that with the time since its death (m_flDeathTime).
// A rebuilt ragdoll that is behind gets the missing physics steps in a short burst, also while paused, so it shows the pose it would
// have after playing through. Deaths during normal playback start in sync and are never touched.
//
// Paused pose refresh: a ragdoll's pose is only pulled from its physics bodies by the animation update (CAnimGraphGameSystem client
// tick). That update runs every frame, but it advances each entity by the ticks elapsed since its last update and skips the whole pose
// evaluation when that is 0 - which it always is while the demo is paused. CS2 also draws entities from per-tick interpolation history
// (CInterpolatedVar), where a sample for a tick that is already in the history is rejected. So a settled pose would only reach the
// screen after resuming (visible snap). While paused and right after a settle, the addon gives those ragdolls one tick of animation
// time per frame, so the game's own update evaluates their pose from the settled physics bodies, and rebuilds their history with the
// new pose at the existing history ticks. The settled pose shows immediately and playback continues from it without a snap.

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr const char* kVersion = "1.8.0";

// Builds this addon was verified against (PE TimeDateStamp). Other builds run only if the structural self-check passes.
constexpr uint32_t kClientStamp = 0x6aa1ae5e;
constexpr uint32_t kEngine2Stamp = 0x6aa1ae4f;
constexpr uint32_t kVphysics2Stamp = 0x6aa1add2;

// Game layout (engine2/client/vphysics2 from our own analysis, cvar system as used by HLAE 2.192.2).
constexpr int kEngIsPlayingDemo = 42, kEngGetDemoFile = 69;  // Source2EngineToClient001
constexpr int kDemoGetTick = 3, kDemoIsPaused = 12;           // demo file object returned by GetDemoFile
constexpr int kPhysStepWorlds = 16;                           // VPhysics2_Interface_001
constexpr int kCvarFindConVar = 11, kCvarGetCvar = 41, kCvarRegisterConCommand = 42;  // VEngineCvar007
constexpr uintptr_t kGlobalsCurtime = 0x30;     // client globals: game time (includes the interpolation fraction)
constexpr uintptr_t kGlobalsInterpFrac = 0x38;  // client globals: tick interpolation fraction, wraps when the demo tick increments
constexpr int kOffLifeState = 0x354;            // C_BaseEntity::m_lifeState (uint8, 0 = alive); schema lookup preferred
constexpr int kOffDeathTime = 0x1370;           // C_BasePlayerPawn::m_flDeathTime (GameTime_t); schema lookup preferred
constexpr uintptr_t kEntChunks = 0x10;          // CGameEntitySystem: 64 pointers to chunks of 512 CEntityIdentity (0x70 bytes)
constexpr uintptr_t kIdentitySize = 0x70, kIdentityHandle = 0x10;
constexpr uintptr_t kSchemaScopeCount = 0x190, kSchemaScopes = 0x198;  // CSchemaSystem type scopes
constexpr uintptr_t kCvarValue = 0x58;          // convar data: float value
constexpr uintptr_t kCommandArgc = 0x438, kCommandArgv = 0x440;  // CCommand
constexpr double kTicksPerSecond = 64.0;
constexpr uintptr_t kGlobalsTickcount = 0x44;   // client globals: tick count (int)
constexpr int kOffGameSceneNode = 0x330;        // C_BaseEntity::m_pGameSceneNode; schema lookup preferred
constexpr int kOffModelState = 0x140;           // CSkeletonInstance::m_modelState; schema lookup preferred
constexpr int kOffAnimSched = 0x1099;           // CBaseAnimGraph::m_bAnimationUpdateScheduled; schema lookup preferred
constexpr uintptr_t kModelStateBones = 0x80;    // CModelState: bone transforms (32 bytes each, position first)
// Interpolated vars (CInterpolatedVar family) an entity watches: lists of {var*, ...} 16-byte entries (count, pointer).
constexpr uintptr_t kEntVarLists[3][2] = {{0x210, 0x218}, {0x258, 0x260}, {0x2a0, 0x2a8}};
constexpr int kVarClearHistory = 5, kVarClearPhase = 6, kVarNoteChanged = 29;  // var vtable: clear all phases, clear one, add sample
constexpr uintptr_t kVarFlags = 0x10, kVarChangedBits = 0x11, kVarHistory = 0x20, kHistPhaseSize = 0x20;
// History (per phase): [+0] sample array (int32 tick first), [+8] packed head:6 elems:6 _:1 count:6 cap:6, [+0x18] interp amount.
constexpr uintptr_t kHistPacked = 0x08, kHistInterp = 0x18;
// Game interpolation reset (C_BaseEntity, entity + bool): clears every var history and queues a fresh latch.
constexpr const char* kSigInterpReset = "88 54 24 10 55 53 41 55 48 8B EC 48 81 EC 80 00 00 00 48 8B 01 48 8B D9 FF 90 E8 05 00 00";
// Animation update: CAnimGraphGameSystem vtable slot 28 is its client tick event handler, a thunk into "AnimGraph Client Tick", which
// pulls ragdoll poses from their physics bodies. The game runs it once per demo tick, so never while paused.
constexpr const char* kAnimSysRtti = ".?AVCAnimGraphGameSystem@@";
constexpr int kAnimTickSlot = 28;
constexpr const char* kAnimTickThunkSig = "80 7A 04 00 74 ?? BA 01 00 00 00 E9";  // cmp byte [event+4],0 / je / mov edx,1 / jmp tick
constexpr uintptr_t kAnimTickEventActive = 4;
// Frame boundary event (IGameSystem slot 48) as overridden by CPhysicsGameSystem: runs once per frame, also while paused.
constexpr const char* kPhysSysRtti = ".?AVCPhysicsGameSystem@@";
constexpr int kFrameBoundarySlot = 48;
constexpr const char* kFrameBoundarySig = "4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 41 56 41 57 48 83 EC 60";
// Per-entity animation time. The animation update job asks this helper for the ticks since the entity's last update (it stores the
// current tick in the entity's animation controller) and skips graph update and pose tasks when the result is 0:
//   mov r8d,[rcx+last] / mov r9d,edx / sub r9d,r8d / mov [rcx+last],edx / test r8d,r8d / mov eax,1 / cmovne eax,r9d / ret
constexpr const char* kSigAnimElapsedTicks = "44 8B 81 ?? ?? ?? ?? 44 8B CA 45 2B C8 89 91 ?? ?? ?? ?? 45 85 C0 B8 01 00 00 00 41 0F 45 C1 C3";
// Entity -> animation controller, as the update job gets it: scene node -> vt[14] skeleton instance -> controller.
constexpr const char* kSigAnimController = "48 83 EC 28 48 8B 89 ?? ?? ?? ?? 48 8B 01 FF 50 70 48 85 C0 74 ?? 48 8B 80 ?? ?? ?? ?? 48 83 C4 28 C3";
constexpr int kPoseBones = 24;  // bones averaged for the pose trace metric

// ---------------------------------------------------------------------------------------------------------- output

HMODULE g_self = nullptr;
using MsgFn = void (*)(const char*, ...);
MsgFn g_Msg = nullptr;
LARGE_INTEGER g_qpcFreq{}, g_qpc0{};

double Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return double(t.QuadPart - g_qpc0.QuadPart) / double(g_qpcFreq.QuadPart);
}

// Prints to the CS2 console. Messages must end with '\n'.
void Con(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_Msg) g_Msg("[RagdollDemoClock] %s", buf);
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

bool SafeStrEq(uintptr_t p, const char* want) {
  if (p < 0x10000 || p >= 0x7FFFFFFFFFFFull) return false;
  __try {
    return strcmp((const char*)p, want) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

struct Range {
  uintptr_t b = 0;
  size_t n = 0;
  bool has(uintptr_t a) const { return a >= b && a < b + n; }
};

IMAGE_NT_HEADERS64* Nt(HMODULE m) { return (IMAGE_NT_HEADERS64*)((uintptr_t)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew); }

Range SectionRange(HMODULE m, const char* name) {
  if (!m) return {};
  auto nt = Nt(m);
  auto s = IMAGE_FIRST_SECTION(nt);
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++s)
    if (!strncmp((const char*)s->Name, name, 8)) return {(uintptr_t)m + s->VirtualAddress, s->Misc.VirtualSize};
  return {};
}

Range ImageRange(HMODULE m) { return m ? Range{(uintptr_t)m, Nt(m)->OptionalHeader.SizeOfImage} : Range{}; }

uint32_t Stamp(HMODULE m) { return m ? Nt(m)->FileHeader.TimeDateStamp : 0; }

size_t ParseSig(const char* sig, int* pat, size_t cap) {
  size_t len = 0;
  for (const char* p = sig; *p && len < cap;) {
    if (*p == ' ') { ++p; continue; }
    if (*p == '?') { pat[len++] = -1; while (*p == '?') ++p; continue; }
    char* end = nullptr;
    pat[len++] = (int)strtoul(p, &end, 16);
    p = end;
  }
  return len;
}

// True if the bytes at a match the signature ("??" = any byte).
bool MatchSig(uintptr_t a, const char* sig) {
  int pat[256];
  uint8_t buf[256];
  const size_t len = ParseSig(sig, pat, 256);
  if (!len || a < 0x10000 || !SafeCopy(buf, (void*)a, len)) return false;
  for (size_t j = 0; j < len; ++j)
    if (pat[j] >= 0 && buf[j] != (uint8_t)pat[j]) return false;
  return true;
}

uintptr_t FindSig(HMODULE m, const char* sig) {
  int pat[256];
  const size_t len = ParseSig(sig, pat, 256);
  Range t = SectionRange(m, ".text");
  if (!t.b || !len) return 0;
  const uint8_t* d = (const uint8_t*)t.b;
  for (size_t i = 0; i + len <= t.n; ++i) {
    size_t j = 0;
    for (; j < len; ++j)
      if (pat[j] >= 0 && d[i + j] != (uint8_t)pat[j]) break;
    if (j == len) return t.b + i;
  }
  return 0;
}

void* Iface(const char* mod, const char* name) {
  HMODULE h = GetModuleHandleA(mod);
  if (!h) return nullptr;
  auto f = (void* (*)(const char*, int*))GetProcAddress(h, "CreateInterface");
  return f ? f(name, nullptr) : nullptr;
}

inline void** Vt(void* o) { return *(void***)o; }

bool SlotsInText(uintptr_t vt, std::initializer_list<int> slots, const Range& text) {
  for (int s : slots) {
    uintptr_t fn = 0;
    if (!Rd(vt + (uintptr_t)s * 8, fn) || !text.has(fn)) return false;
  }
  return true;
}

bool PatchPtr(void** where, void* fn, void** orig) {
  DWORD old;
  if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) return false;
  if (orig) *orig = *where;
  *where = fn;
  VirtualProtect(where, sizeof(void*), old, &old);
  return true;
}

// Primary vtable (complete object locator offset 0) of a class, found through its MSVC RTTI type descriptor. 0 if not found.
uintptr_t FindVtableByRtti(HMODULE m, const char* rttiName) {
  const Range data = SectionRange(m, ".data"), rdata = SectionRange(m, ".rdata");
  const size_t nameLen = strlen(rttiName) + 1;
  if (!data.b || !rdata.b) return 0;
  __try {
    const auto* d = (const uint8_t*)data.b;
    for (size_t i = 16; i + nameLen <= data.n; ++i) {
      if (d[i] != '.' || memcmp(d + i, rttiName, nameLen)) continue;
      const uint32_t tdRva = uint32_t(data.b + i - 16 - (uintptr_t)m);  // TypeDescriptor: vftable, spare, name
      for (uintptr_t c = rdata.b; c + 24 <= rdata.b + rdata.n; c += 4) {
        const auto* col = (const uint32_t*)c;  // signature, offset, cdOffset, type descriptor, hierarchy, self (RVAs)
        if (col[3] != tdRva || col[0] != 1 || col[1] != 0 || col[5] != uint32_t(c - (uintptr_t)m)) continue;
        for (uintptr_t v = rdata.b; v + 8 <= rdata.b + rdata.n; v += 8)
          if (*(uintptr_t*)v == c) return v + 8;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return 0;
}

void OwnerModuleName(uintptr_t a, char* out, size_t cap) {
  snprintf(out, cap, "unknown module");
  HMODULE m = nullptr;
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m)) return;
  char path[MAX_PATH];
  if (!GetModuleFileNameA(m, path, MAX_PATH)) return;
  const char* slash = strrchr(path, '\\');
  snprintf(out, cap, "%s", slash ? slash + 1 : path);
}

// ---------------------------------------------------------------------------------------------------------- settings / health

enum Mode { kModeOff = 0, kModeFreeze = 1, kModeClock = 2 };
const char* ModeName(int m) {
  switch (m) {
    case kModeOff: return "off (vanilla CS2 physics)";
    case kModeFreeze: return "freeze (physics stops while the demo is paused)";
    default: return "demo clock (physics follows demo time)";
  }
}

std::atomic<int> g_mode{kModeClock};
std::atomic<int> g_maxSteps{4};     // physics steps allowed per engine tick while catching up
std::atomic<int> g_resyncTicks{16}; // demo time jumps larger than this re-sync the physics clock instead of fast-forwarding
std::atomic<bool> g_verbose{true};
std::atomic<int> g_epoch{0};        // bumped when settings change; resets the physics clock
std::atomic<int> g_settleMax{384};  // seek settle: most demo ticks of missed physics a rebuilt ragdoll catches up on (0 = off)
std::atomic<bool> g_settleAll{false};  // false: stop once the most recent death is in sync (default); true: settle every rebuilt ragdoll
// Paused settle in lockstep: demo ticks per frame, each chunk first gives the ragdolls the same animation time (0 = one burst, v1.2.3)
std::atomic<int> g_settleChunk{4};
constexpr int kSettleChunkMax = 16;
// Paused seek catch-up (v1.4): one animation update that hands each rebuilt ragdoll its real age, run while the demo is still
// paused, instead of settling it with physics steps. See CatchUpAnim for why that is the update that actually moves a ragdoll.
std::atomic<int> g_catchup{0};
// Paused pose copy (v1.8): once the burst settle has run a rebuilt ragdoll's missed physics while paused, one active animation update
// with its real age copies the settled physics bodies onto the bones. Measured: physics steps alone never write the bones; the game's
// own first update after the resume does, which is the snap. Physics first, then the update, is the order normal playback uses.
std::atomic<int> g_poseCopy{1};
constexpr long kCatchMinTicks = 1;        // animation ticks: an update with 0 elapsed does nothing at all
constexpr long kCatchMaxTicks = 4096;     // a ragdoll is long at rest by then; keeps a bad death time from asking for millions
constexpr int kLockWaitCalls = 4;         // engine ticks a lockstep chunk waits for its animation update before stepping without it
constexpr int kLockAnimTicksMax = 32;     // most animation time given with one lockstep chunk
constexpr int kPoseRefCalls = 96;         // trace only: pose rows after playback resumes (shows a snap)
constexpr int kSettleStepsPerCall = 24;   // catch-up burst size per engine tick
constexpr int kSettleWaitCalls = 2;       // engine ticks to wait after a ragdoll appears, so its physics bodies exist
constexpr int kJumpWindowCalls = 8;       // ragdolls appearing this soon after a demo time jump were rebuilt by the seek
constexpr double kSettleTolerance = 2.0;  // demo ticks; the clock gate keeps ragdolls within this of their death age
constexpr double kLiveDeathTicks = 16.0;  // a ragdoll first seen this soon after death (and not right after a time jump) is in sync

enum Show {
  kShowOff = 0, kShowSched = 1, kShowReset = 2, kShowAnim = 3, kShowAll = 4, kShowTick = 5, kShowTickOnly = 6, kShowAnimTime = 7
};
constexpr int kShowMax = kShowAnimTime;
const char* ShowName(int s) {
  switch (s) {
    case kShowOff: return "off (a settled pose appears when playback resumes)";
    case kShowSched: return "set the animation update flag only";
    case kShowReset: return "game interpolation reset + animation update flag";
    case kShowAnim: return "rebuild the animation pose history";
    case kShowAll: return "rebuild all interpolation history";
    case kShowTick: return "run an extra animation update + rebuild the animation pose history";
    case kShowTickOnly: return "run an extra animation update only";
    default: return "give ragdolls animation time while paused + rebuild the animation pose history";
  }
}
bool ShowRunsTick(int s) { return s == kShowTick || s == kShowTickOnly; }
bool ShowAdvancesAnim(int s) { return s == kShowAnimTime; }
bool ShowRebuilds(int s) { return s == kShowAnim || s == kShowAll || s == kShowTick || s == kShowAnimTime; }
// Paused pose refresh. Show 7 gives ragdolls animation time, which v1.4 measured to be a no-op: while paused the game passes its
// animation tick event with the active byte clear, so the update it would feed never runs. The catch-up runs its own active update.
std::atomic<int> g_show{kShowAnim};       // paused pose refresh strategy
enum TickSite { kSiteStep = 0, kSiteFrame = 1 };
std::atomic<int> g_tickSite{kSiteStep};   // where show 5/6 run the animation update: inside the physics step or at the frame boundary
const char* TickSiteName(int s) { return s == kSiteFrame ? "frame (next frame boundary)" : "step (inside the physics step)"; }
constexpr int kShowWindowCalls = 8;       // engine ticks the paused pose refresh keeps running after the last settle burst
constexpr int kAnimWindowCalls = 32;      // same for show 7: animation ticks given, so blends inside the animation graph can finish
constexpr int kShowStaleCalls = 16;       // a pause this many engine ticks after a settle/catch-up/jump still refreshes the pose
constexpr int kShowStaleTicks = 2;        // ... if the demo tick is still within this many ticks of it (skips wobble by one tick)
constexpr int kShowWatchCalls = 64;       // engine ticks of pose trace rows after a refresh window (trace only)
constexpr int kSuspectJumpTicks = 4096;   // a demo tick jump this large is only trusted once the next engine tick confirms it

enum Health { kInit = 0, kReady = 1, kDisabled = 2 };
std::atomic<int> g_health{kInit};
std::atomic<bool> g_buildVerified{false};
char g_disableReason[256] = "";

void Disable(const char* why) {
  if (g_health.exchange(kDisabled) == kDisabled) return;
  snprintf(g_disableReason, sizeof(g_disableReason), "%s", why);
  Con("DISABLED: %s\n", why);
  Con("CS2 physics now runs unmodified (vanilla). The game build probably changed; the addon needs an update.\n");
}

// ---------------------------------------------------------------------------------------------------------- game access

void* g_engine = nullptr;     // Source2EngineToClient001
HMODULE g_engine2 = nullptr;
uintptr_t g_globalsVar = 0;   // address of client's globals pointer
void* g_cvar = nullptr;       // VEngineCvar007
uintptr_t g_physCvar = 0;     // cl_phys_timescale convar data
bool g_cmdRegistered = false;
uintptr_t g_checkedDemoVt = 0;
uintptr_t g_entSysVar = 0;    // address of client's CGameEntitySystem pointer
Range g_clientImg;
int g_offLife = -1, g_offDeath = -1;
bool g_settleOk = false;      // entity list and pawn fields resolved: seek settle available
uintptr_t g_schemaSys = 0;    // schema system that resolved the pawn fields (insight recorder field names)
const char* g_settleSrc = "unavailable";
Range g_clientText;
int g_offNode = -1, g_offModelState = -1, g_offAnimSched = -1;
using InterpResetFn = void (*)(uintptr_t, bool);
InterpResetFn g_interpReset = nullptr;
// Game system event handlers are (this, const Event&); the extra register arguments are passed through untouched.
using EventFn = void (*)(void*, void*, void*, void*);
EventFn o_animTick = nullptr;           // CAnimGraphGameSystem client tick (original)
uintptr_t g_animVt = 0;
std::atomic<void*> g_animSys{nullptr};  // CAnimGraphGameSystem instance, captured from its first tick event
EventFn o_frame = nullptr;              // CPhysicsGameSystem frame boundary (original)
using AnimCtrlFn = uintptr_t (*)(uintptr_t);
AnimCtrlFn g_animCtrl = nullptr;        // entity -> animation controller
int g_offLastAnimTick = -1;             // animation controller: tick of the entity's last animation update
uintptr_t g_animElapsedFnAddr = 0;      // the helper above, for forensic watchpoints (rcx = controller, rdx = the caller's tick)
bool AnimTimeAvailable() { return o_animTick && g_animCtrl && g_offLastAnimTick > 0; }
// Strategy actually used: show 7 needs the animation time helpers and acts as show 5 without them.
int EffectiveShow() {
  const int s = g_show.load(std::memory_order_relaxed);
  return ShowAdvancesAnim(s) && !AnimTimeAvailable() ? kShowTick : s;
}

struct DemoView {
  bool playing = false, paused = false, badLayout = false;
  int tick = -1;
  float frac = 0.0f;
};

bool DemoVtableOk(uintptr_t vt) {
  Range img = ImageRange(g_engine2), text = SectionRange(g_engine2, ".text");
  return img.has(vt) && SlotsInText(vt, {0, 1, 2, kDemoGetTick, kDemoIsPaused}, text);
}

// Queried inside the physics step itself, so pause state is never a frame late.
bool ReadDemoRaw(DemoView& v) {
  __try {
    v.playing = ((bool (*)(void*))Vt(g_engine)[kEngIsPlayingDemo])(g_engine);
    if (!v.playing) return true;
    void* d = ((void* (*)(void*))Vt(g_engine)[kEngGetDemoFile])(g_engine);
    if (!d) {
      v.playing = false;
      return true;
    }
    uintptr_t dvt = (uintptr_t)Vt(d);
    if (dvt != g_checkedDemoVt) {
      if (!DemoVtableOk(dvt)) {
        v.badLayout = true;
        return true;
      }
      g_checkedDemoVt = dvt;
    }
    v.paused = ((bool (*)(void*))Vt(d)[kDemoIsPaused])(d);
    v.tick = ((int (*)(void*))Vt(d)[kDemoGetTick])(d);
    uintptr_t gl = *(uintptr_t*)g_globalsVar;
    v.frac = gl ? *(float*)(gl + kGlobalsInterpFrac) : 0.0f;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

int g_badFracRun = 0;

bool ReadDemo(DemoView& v) {
  if (!ReadDemoRaw(v)) {
    Disable("reading the demo state crashed (engine interface changed)");
    return false;
  }
  if (v.badLayout) {
    Disable("the demo file object does not look like the analysed one (engine2 layout changed)");
    return false;
  }
  if (!v.playing) return true;
  if (!std::isfinite(v.frac) || v.frac < -0.001f || v.frac > 1.001f) {
    if (++g_badFracRun > 256) {
      Disable("the tick interpolation fraction is out of range (client globals layout changed)");
      return false;
    }
    v.frac = 0.0f;
  } else {
    g_badFracRun = 0;
  }
  v.frac = (std::min)((std::max)(v.frac, 0.0f), 0.999999f);
  return true;
}

float PhysTimescale() {
  float v = 1.0f;
  if (g_physCvar && Rd(g_physCvar + kCvarValue, v) && std::isfinite(v) && v >= 0.0f && v <= 1000.0f) return v;
  return 1.0f;
}

double CurTime() {
  uintptr_t gl = 0;
  float t = 0.0f;
  if (Rd(g_globalsVar, gl) && Rd(gl + kGlobalsCurtime, t) && std::isfinite(t)) return t;
  return -1.0;
}

// MSVC RTTI: vtable[-1] -> CompleteObjectLocator -> TypeDescriptor name (".?AVClass@@").
bool VtableIsClass(uintptr_t vt, const char* rttiName) {
  __try {
    uintptr_t col = *(uintptr_t*)(vt - 8);
    if (*(uint32_t*)col != 1) return false;
    uint32_t tdRva = *(uint32_t*)(col + 12), selfRva = *(uint32_t*)(col + 20);
    return strcmp((const char*)(col - selfRva + tdRva + 16), rttiName) == 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Main thread only.
bool IsPlayerPawnVtable(uintptr_t vt) {
  if (!g_clientImg.has(vt)) return false;
  static uintptr_t cacheVt[256];
  static bool cachePawn[256];
  static int next = 0;
  for (int i = 0; i < 256; ++i)
    if (cacheVt[i] == vt) return cachePawn[i];
  const bool pawn = VtableIsClass(vt, ".?AVC_CSPlayerPawn@@");
  cacheVt[next] = vt;
  cachePawn[next] = pawn;
  next = (next + 1) % 256;
  return pawn;
}

struct DeadPawn {
  int idx;
  uintptr_t inst;
  float deathTime;
};

// Dead C_CSPlayerPawn entities (the pawn itself is the ragdoll), from the client entity list.
int ScanDeadPawns(DeadPawn* out, int cap) {
  static uint8_t chunk[512 * kIdentitySize];
  uintptr_t es = 0;
  if (!Rd(g_entSysVar, es) || !es) return 0;
  int n = 0;
  for (int c = 0; c < 64 && n < cap; ++c) {
    uintptr_t ch = 0;
    if (!Rd(es + kEntChunks + uintptr_t(c) * 8, ch) || !ch || !SafeCopy(chunk, (void*)ch, sizeof(chunk))) continue;
    for (int i = 0; i < 512 && n < cap; ++i) {
      const uint8_t* id = chunk + i * kIdentitySize;
      uintptr_t inst = 0, vt = 0;
      uint32_t handle = 0;
      memcpy(&inst, id, 8);
      memcpy(&handle, id + kIdentityHandle, 4);
      const int idx = c * 512 + i;
      if (!inst || (int)(handle & 0x7fff) != idx || !Rd(inst, vt) || !IsPlayerPawnVtable(vt)) continue;
      uint8_t life = 0;
      float death = 0.0f;
      if (!Rd(inst + g_offLife, life) || life == 0 || !Rd(inst + g_offDeath, death)) continue;
      out[n++] = {idx, inst, death};
    }
  }
  return n;
}

// Field offset from the client.dll schema scope, -1 if not found.
int SchemaFieldOffset(uintptr_t sys, const char* cls, const char* field) {
  __try {
    const uint64_t nScopes = *(uint64_t*)(sys + kSchemaScopeCount);
    const uintptr_t scopes = *(uintptr_t*)(sys + kSchemaScopes);
    if (!scopes || nScopes > 512) return -1;
    for (uint64_t i = 0; i < nScopes; ++i) {
      const uintptr_t scope = *(uintptr_t*)(scopes + i * 8);
      if (!scope || strcmp((const char*)(scope + 8), "client.dll")) continue;
      const uint16_t n = *(uint16_t*)(scope + 0x470);
      const uintptr_t entries = *(uintptr_t*)(scope + 0x478);
      for (uint32_t j = 0; entries && j < n; ++j) {
        const uintptr_t decl = *(uintptr_t*)(entries + j * 0x18 + 0x10);
        const uintptr_t info = decl ? *(uintptr_t*)(decl + 0x20) : 0;
        const char* name = info ? *(const char**)(info + 8) : nullptr;
        if (!name || strcmp(name, cls)) continue;
        const uint16_t nf = *(uint16_t*)(info + 0x24);
        const uintptr_t fields = *(uintptr_t*)(info + 0x30);
        for (uint32_t k = 0; fields && k < nf; ++k) {
          const char* fname = *(const char**)(fields + k * 0x20);
          if (fname && !strcmp(fname, field)) return *(int32_t*)(fields + k * 0x20 + 0x10);
        }
        return -1;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return -1;
}

void ResolveSettle(HMODULE client) {
  g_clientImg = ImageRange(client);
  if (uintptr_t hit = FindSig(client, "40 55 53 48 8D AC 24 ?? ?? ?? ?? 48 81 EC ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 33 D2 E8")) {
    int32_t d = 0;
    if (Rd(hit + 21, d)) g_entSysVar = hit + 25 + d;
  }
  uintptr_t viaSig = 0;
  if (uintptr_t hit = FindSig(GetModuleHandleA("schemasystem.dll"), "48 89 05 ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 33 C0 48 C7 05")) {
    int32_t d = 0;
    if (Rd(hit + 3, d)) viaSig = hit + 7 + d;
  }
  for (uintptr_t sys : {(uintptr_t)Iface("schemasystem.dll", "SchemaSystem_001"), viaSig}) {
    if (!sys) continue;
    const int life = SchemaFieldOffset(sys, "C_BaseEntity", "m_lifeState");
    const int death = SchemaFieldOffset(sys, "C_BasePlayerPawn", "m_flDeathTime");
    if (life > 0 && life < 0x10000 && death > 0 && death < 0x10000) {
      g_offLife = life;
      g_offDeath = death;
      g_settleSrc = "ok";
      const int node = SchemaFieldOffset(sys, "C_BaseEntity", "m_pGameSceneNode");
      const int modelState = SchemaFieldOffset(sys, "CSkeletonInstance", "m_modelState");
      const int sched = SchemaFieldOffset(sys, "CBaseAnimGraph", "m_bAnimationUpdateScheduled");
      if (node > 0 && node < 0x10000) g_offNode = node;
      if (modelState > 0 && modelState < 0x10000) g_offModelState = modelState;
      if (sched > 0 && sched < 0x10000) g_offAnimSched = sched;
      g_schemaSys = sys;
      break;
    }
  }
  if (g_offLife < 0 && g_buildVerified) {
    g_offLife = kOffLifeState;
    g_offDeath = kOffDeathTime;
    g_settleSrc = "ok (built-in offsets)";
  }
  if (g_buildVerified) {
    if (g_offNode < 0) g_offNode = kOffGameSceneNode;
    if (g_offModelState < 0) g_offModelState = kOffModelState;
    if (g_offAnimSched < 0) g_offAnimSched = kOffAnimSched;
  }
  g_clientText = SectionRange(client, ".text");
  g_interpReset = (InterpResetFn)FindSig(client, kSigInterpReset);
  if (uintptr_t hit = FindSig(client, kSigAnimElapsedTicks)) {
    int32_t readOff = 0, writeOff = 0;
    if (Rd(hit + 3, readOff) && Rd(hit + 15, writeOff) && readOff == writeOff && readOff > 0 && readOff < 0x10000)
      g_offLastAnimTick = readOff;
    g_animElapsedFnAddr = hit;  // forensic watchpoints break here to read rcx (controller) and rdx (the caller's current tick)
  }
  g_animCtrl = (AnimCtrlFn)FindSig(client, kSigAnimController);
  uintptr_t es = 0;
  g_settleOk = g_offLife > 0 && g_entSysVar && g_clientImg.has(g_entSysVar) && Rd(g_entSysVar, es);
  if (!g_settleOk) g_settleSrc = "unavailable (entity list or player fields not found; everything else works)";
}

// ---------------------------------------------------------------------------------------------------------- trace file

CRITICAL_SECTION g_traceCs;
FILE* g_trace = nullptr;
std::atomic<bool> g_traceOn{false};
char g_tracePath[MAX_PATH] = "";
unsigned g_traceLines = 0;

void TraceOpen() {
  EnterCriticalSection(&g_traceCs);
  if (!g_trace) {
    char dir[MAX_PATH];
    GetModuleFileNameA(g_self, dir, MAX_PATH);
    if (char* slash = strrchr(dir, '\\')) slash[1] = 0;
    strcat_s(dir, "logs\\");
    CreateDirectoryA(dir, nullptr);
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(g_tracePath, sizeof(g_tracePath), "%sdemoclock_%04u%02u%02u_%02u%02u%02u.csv", dir, st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond);
    g_trace = fopen(g_tracePath, "wb");
    if (g_trace) {
      setvbuf(g_trace, nullptr, _IOFBF, 1 << 16);
      fprintf(g_trace, "wall,mode,playing,paused,tick,frac,T,P_before,P_after,dt,substeps,cl_phys_timescale,step_ticks,steps,action\n");
    }
  }
  g_traceOn = g_trace != nullptr;
  LeaveCriticalSection(&g_traceCs);
}

void TraceClose() {
  EnterCriticalSection(&g_traceCs);
  g_traceOn = false;
  if (g_trace) fclose(g_trace);
  g_trace = nullptr;
  LeaveCriticalSection(&g_traceCs);
}

void TraceFlush() {
  if (!g_traceOn.load(std::memory_order_relaxed)) return;
  EnterCriticalSection(&g_traceCs);
  if (g_trace) fflush(g_trace);
  LeaveCriticalSection(&g_traceCs);
}

void Trace(int mode, const DemoView& v, double T, double p0, double p1, float dt, int substeps, float phys, double stepTicks, int steps,
           const char* action) {
  if (!g_traceOn.load(std::memory_order_relaxed)) return;
  EnterCriticalSection(&g_traceCs);
  if (g_trace) {
    fprintf(g_trace, "%.6f,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.7f,%d,%.4f,%.4f,%d,%s\n", Now(), mode, (int)v.playing, (int)v.paused, v.tick,
            v.frac, T, p0, p1, dt, substeps, phys, stepTicks, steps, action);
    if (++g_traceLines % 256 == 0) fflush(g_trace);
  }
  LeaveCriticalSection(&g_traceCs);
}

// ---------------------------------------------------------------------------------------------------------- physics gate

using StepFn = void* (*)(void*, void**, int, float, int, bool, void*);
StepFn o_step = nullptr;

// A dead player pawn whose ragdoll the seek settle tracks.
struct PawnSlot {
  int idx = -1;
  uintptr_t inst = 0;
  float deathTime = 0.0f;
  double sim = 0.0;      // demo ticks of physics this ragdoll has received since it was built
  int64_t bornCall = 0;  // physics step call on which it was first seen
  int32_t animWrote = 0; // last-update tick the addon wrote to give it animation time, until the game uses it (0 = none pending)
  int32_t animGiven = 0; // animation ticks that write gave (taken back if the game never uses them)
  bool lockBehind = false;  // behind its death age on the last settle check: receives lockstep animation time
  int32_t caughtTick = 0;   // engine tick on which the addon ran its catch-up animation update for this ragdoll (0 = none)
};

enum LockPhase { kLockIdle = 0, kLockGive = 1, kLockStep = 2 };
constexpr int kMaxPawns = 64;

enum TickResult { kTickNone = 0, kTickRan = 1, kTickUnavailable = 2, kTickCrashed = 3 };
const char* TickName(int t) {
  static const char* names[] = {"-", "ran", "n/a", "CRASH"};
  return names[t & 3];
}

// What one paused pose refresh call did.
struct ShowDiag {
  int ents = 0, vars = 0, matched = 0, rebuilt = 0, samples = 0, faults = 0;
  int tick = kTickNone;  // forced animation update
  int given = 0, used = 0, waiting = 0;  // show 7: ragdolls given a tick of animation time, updated with it, previous tick still unused
};

// Only touched from the physics step (game main thread); the console command reads it for status output.
struct ClockState {
  int64_t calls = 0, lastJumpCall = INT64_MIN / 2;
  int lastTick = -1;
  bool suspect = false;  // the previous engine tick saw a huge demo tick jump that is not trusted yet
  int showLeft = 0, watchLeft = 0;
  bool showActive = false;
  // Last engine tick on which physics moved ragdolls beyond normal playback (settle, catch-up, demo tick jump). Demo skips report
  // "unpaused" for a few engine ticks, so this carries the stale-pose state over to the pause that follows.
  int64_t staleCall = INT64_MIN / 2;
  int staleTick = -1;
  const char* staleWhy = "";
  ShowDiag showLast;
  int winTicks = 0;                     // animation updates run in the current refresh window
  bool framePending = false;            // ticksite frame: a refresh waits for the next frame boundary
  const char* frameArmWhy = nullptr;
  bool animPending = false;             // show 7: the next animation update gives ragdolls animation time
  const char* animArmWhy = nullptr;
  int animOwed = 0;                     // show 7: ragdolls still holding animation time the game did not use
  bool rebase = true;    // next pawn update treats every ragdoll as in sync (clock reset, physics timescale 0, settle switched on)
  bool settling = false;
  // Lockstep settle (paused seek): each chunk first gives the rebuilt ragdolls animation time in the next animation update, then runs
  // the same demo time of physics in the physics step after it, so the death animation's ragdoll pose control keeps pace with physics.
  int lockPhase = kLockIdle;
  int lockSteps = 0, lockAnimTicks = 0, lockTick = -1, lockWait = 0, lockChunks = 0, lockUsed = 0, lockGiven = 0;
  bool lockRun = false;   // the current settle runs in lockstep
  bool lockBusy = false;  // a lockstep settle runs or is about to start: the paused show window stays off
  // A burst settle finished: the paused pose copy runs once the demo is paused (skips report unpaused for a few engine ticks).
  bool copyPending = false;
  int64_t copyCall = INT64_MIN / 2;
  int copyTick = -1;
  int poseRefLeft = 0;
  PawnSlot pawns[kMaxPawns];
  int nPawns = 0;
  double curtime = -1.0;
  int epoch = -1;
  bool valid = false;
  double P = 0.0;  // physics clock in demo ticks: demo time the ragdoll simulation has reached
  bool playing = false, paused = false;
  int pauseTick = -1;
  uint64_t heldWhilePaused = 0;
  DemoView last;
  double T = 0.0, stepTicks = 0.0;
  float phys = 1.0f, dt = 0.0f;
};
ClockState g_clk;

struct Stats {
  uint64_t passed = 0, gated = 0, extra = 0, resyncs = 0, settleBursts = 0, settleSteps = 0, suspects = 0;
  uint64_t showWindows = 0, showRebuilt = 0, showFaults = 0;
  uint64_t animCalls = 0, animPaused = 0, animForced = 0, animFaults = 0, frameCalls = 0;
  uint64_t animGiven = 0, animUsed = 0;
  uint64_t lockSettles = 0, lockChunks = 0, lockNoAnim = 0, lockCancels = 0;
  uint64_t catchRuns = 0, catchPawns = 0, catchFaults = 0, catchMisses = 0;
  uint64_t copyRuns = 0, copyPawns = 0, copyMisses = 0, copyDropped = 0;
} g_stats;

void NoteTransitions(const DemoView& v, int mode) {
  const bool verbose = g_verbose.load(std::memory_order_relaxed);
  if (v.playing != g_clk.playing && verbose) {
    if (v.playing)
      Con("demo playback detected - mode %d: %s\n", mode, ModeName(mode));
    else
      Con("demo playback ended - addon idle\n");
  }
  const bool paused = v.playing && v.paused;
  if (paused && !g_clk.paused) {
    g_clk.pauseTick = v.tick;
    g_clk.heldWhilePaused = 0;
    if (verbose) Con("paused at tick %d - ragdoll physics frozen\n", v.tick);
  } else if (!paused && g_clk.paused && v.playing) {
    g_clk.poseRefLeft = kPoseRefCalls;
    if (verbose)
      Con("resumed at tick %d - held back %llu physics steps while paused\n", v.tick, (unsigned long long)g_clk.heldWhilePaused);
  }
  g_clk.playing = v.playing;
  g_clk.paused = paused;
  g_clk.last = v;
}

// Demo ticks of physics a tracked ragdoll should have received: time since death, capped. -1 when unknown.
// moving = its death is recent enough that it is presumably still in motion.
double SettleTarget(const PawnSlot& s, double cap, bool& moving) {
  moving = false;
  const double age = g_clk.curtime > 0.0 && s.deathTime > 0.0f ? (g_clk.curtime - s.deathTime) * kTicksPerSecond : -1e9;
  if (age >= -kSettleTolerance && age < 1e7) {
    moving = age < cap;
    return (std::min)((std::max)(age, 0.0), cap);
  }
  // No usable death time: a ragdoll that appeared right after a demo time jump was rebuilt by the seek, settle it fully.
  const int64_t sinceJump = s.bornCall - g_clk.lastJumpCall;
  return sinceJump >= -kJumpWindowCalls && sinceJump <= kJumpWindowCalls ? cap : -1.0;
}

// A pawn that is new, has a new entity instance or a new death time has a freshly built ragdoll with no physics received yet.
void UpdatePawns(double cap) {
  DeadPawn dead[kMaxPawns];
  const int n = ScanDeadPawns(dead, kMaxPawns);
  PawnSlot next[kMaxPawns];
  for (int i = 0; i < n; ++i) {
    PawnSlot& s = next[i];
    const PawnSlot* old = nullptr;
    for (int j = 0; j < g_clk.nPawns; ++j)
      if (g_clk.pawns[j].idx == dead[i].idx) old = &g_clk.pawns[j];
    if (old && old->inst == dead[i].inst && old->deathTime == dead[i].deathTime) {
      s = *old;
      continue;
    }
    s.idx = dead[i].idx;
    s.inst = dead[i].inst;
    s.deathTime = dead[i].deathTime;
    s.bornCall = g_clk.calls;
    const double age = (g_clk.curtime - s.deathTime) * kTicksPerSecond;
    const bool ageOk = s.deathTime > 0.0f && g_clk.curtime > 0.0 && age >= -kSettleTolerance && age < 1e7;
    const int64_t sinceJump = s.bornCall - g_clk.lastJumpCall;
    // A death seen during normal playback is in sync even if it shows up a few ticks late; one built by a seek starts from nothing.
    s.sim = ageOk && age < kLiveDeathTicks && !(sinceJump >= -kJumpWindowCalls && sinceJump <= kJumpWindowCalls) ? (std::max)(age, 0.0) : 0.0;
    static bool noted = false;
    if (!noted && !ageOk) {
      noted = true;
      Con("note: player #%d has no usable death time (%.3f, game time %.3f) - ragdolls without one settle fully after a seek\n", s.idx,
          s.deathTime, g_clk.curtime);
    }
  }
  for (int i = 0; i < n; ++i) g_clk.pawns[i] = next[i];
  g_clk.nPawns = n;
  if (g_clk.rebase) {
    for (int i = 0; i < n; ++i) {
      PawnSlot& s = g_clk.pawns[i];
      bool moving = false;
      const double target = SettleTarget(s, cap, moving);
      s.sim = target >= 0.0 ? target : 0.0;
      s.bornCall = g_clk.calls - kSettleWaitCalls;
    }
    g_clk.rebase = false;
  }
}

void RunShow(const DemoView& v, int strategy, const char* armedWhy, const char* site);
bool PoseCentroid(uintptr_t ent, float out[3]);
int CatchUpAnim(const DemoView& v, double cap, bool afterSettle = false);
void InsMark(const char* fmt, ...);  // insight recorder mark (Insight.inc, included further down)

// Lockstep settle is used while the demo is paused and show 7 can give animation time.
bool LockstepActive(const DemoView& v) {
  return v.paused && g_settleChunk.load(std::memory_order_relaxed) > 0 && EffectiveShow() == kShowAnimTime;
}

void CancelLock(const char* why) {
  if (g_clk.lockPhase == kLockIdle) return;
  g_clk.lockPhase = kLockIdle;
  ++g_stats.lockCancels;
  char act[96];
  snprintf(act, sizeof(act), "settle lock cancel %s", why);
  Trace(kModeClock, g_clk.last, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
}

// First behind ragdoll (the one the lockstep trace follows), else the first tracked one.
int LockPawn() {
  for (int i = 0; i < g_clk.nPawns; ++i)
    if (g_clk.pawns[i].lockBehind) return i;
  return 0;
}

// Queues the animation time of the next lockstep chunk; the physics step after that animation update runs its physics.
void ArmLock(const DemoView& v, long steps) {
  g_clk.lockPhase = kLockGive;
  g_clk.lockSteps = int(steps);
  g_clk.lockAnimTicks = (std::min)((std::max)(int(std::lround(double(steps) * g_clk.stepTicks)), 1), kLockAnimTicksMax);
  g_clk.lockTick = v.tick;
  g_clk.lockWait = 0;
  g_clk.lockUsed = 0;
  g_clk.lockGiven = 0;
  if (!g_traceOn.load(std::memory_order_relaxed)) return;
  char act[96];
  snprintf(act, sizeof(act), "settle lock arm chunk %d steps %ld anim %d", g_clk.lockChunks + 1, steps, g_clk.lockAnimTicks);
  Trace(kModeClock, v, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
}

// Paused pose copy: runs the animation update queued by a finished burst settle once the demo is paused on the settled tick. A skip
// reports unpaused for a few engine ticks; if playback really resumed instead, the game's own update copies the pose and this drops it.
void PoseCopyStep(const DemoView& v, double cap, bool building) {
  if (!g_clk.copyPending) return;
  if (g_poseCopy.load(std::memory_order_relaxed) <= 0 || g_clk.nPawns == 0) {
    g_clk.copyPending = false;
    return;
  }
  const bool moved = std::abs(v.tick - g_clk.copyTick) > kShowStaleTicks;
  if (moved || (!v.paused && g_clk.calls - g_clk.copyCall > kShowStaleCalls)) {
    g_clk.copyPending = false;
    ++g_stats.copyDropped;
    InsMark("POSECOPY dropped: %s (tick %d, settled at tick %d, %lld engine ticks ago)", moved ? "demo moved" : "playback resumed",
            v.tick, g_clk.copyTick, (long long)(g_clk.calls - g_clk.copyCall));
    return;
  }
  if (!v.paused || building || g_clk.settling) return;
  g_clk.copyPending = false;
  CatchUpAnim(v, cap, true);
}

// Runs missed physics steps for ragdolls that are behind their death age (rebuilt by a seek). Returns the steps run.
// Paused with lockstep: one chunk per frame, animation time first (Hk_AnimTick), physics in the following step.
int SettleStep(const DemoView& v, double cap, void* self, void** worlds, int count, float dt, int substeps, bool b, void* p, void*& r) {
  const bool all = g_settleAll.load(std::memory_order_relaxed);
  double fill = 0.0;
  int behind = 0;
  bool blocked = false, building = false;
  for (int i = 0; i < g_clk.nPawns; ++i) {
    PawnSlot& s = g_clk.pawns[i];
    s.lockBehind = false;
    if (g_clk.calls - s.bornCall < kSettleWaitCalls) {
      blocked = building = true;  // a ragdoll is still being built; settle the whole batch together
      continue;
    }
    bool moving = false;
    const double target = SettleTarget(s, cap, moving);
    if (target < 0.0) continue;
    const double missing = target - s.sim;
    if (missing > kSettleTolerance) {
      ++behind;
      s.lockBehind = true;
      fill = all ? (std::max)(fill, missing) : (fill > 0.0 ? (std::min)(fill, missing) : missing);
    } else if (moving && !all) {
      blocked = true;  // a ragdoll in motion is in sync with the demo; more steps would push it ahead
    }
  }
  // Paused seek: one animation update with the ragdolls' real age puts them where resuming would, so there is nothing left to snap.
  if (v.paused && behind > 0 && !building && g_catchup.load(std::memory_order_relaxed) > 0 && CatchUpAnim(v, cap) > 0) {
    CancelLock("catchup");
    g_clk.settling = false;
    g_clk.lockRun = false;
    g_clk.lockBusy = false;
    return 0;
  }
  const bool lock = LockstepActive(v);
  const long want = blocked || fill <= 0.0 ? 0 : std::lround(fill / g_clk.stepTicks);
  const long chunkSteps = (std::max)(1L, std::lround(g_settleChunk.load(std::memory_order_relaxed) / g_clk.stepTicks));
  const long steps = (std::min)(want, lock ? chunkSteps : long(kSettleStepsPerCall));
  if (!lock) CancelLock("unpaused");
  g_clk.lockBusy = lock && (building || steps > 0 || g_clk.lockPhase != kLockIdle);
  if (steps <= 0) {
    CancelLock("in sync");
    if (g_clk.settling && g_clk.lockRun) {
      if (g_verbose.load(std::memory_order_relaxed))
        Con("lockstep settle done: %d chunks (ragdoll animation and physics advanced together)\n", g_clk.lockChunks);
      // Bones follow the last chunk's physics after this step; push that pose into the interpolation history.
      if (lock && g_clk.nPawns) RunShow(v, kShowAnim, "lock_end", "step");
    }
    if (g_clk.settling && !g_clk.lockRun && !building && g_poseCopy.load(std::memory_order_relaxed) > 0) {
      // The burst settle moved the physics bodies, but the bones only take their pose from an animation update with time in it.
      g_clk.copyPending = true;
      g_clk.copyCall = g_clk.calls;
      g_clk.copyTick = v.tick;
    }
    g_clk.settling = false;
    g_clk.lockRun = false;
    PoseCopyStep(v, cap, building);
    return 0;
  }
  if (!g_clk.settling) {
    ++g_stats.settleBursts;
    g_clk.copyPending = false;  // a new settle (another seek): the copy runs when this one is done
    g_clk.lockRun = lock;
    g_clk.lockChunks = 0;
    if (lock) ++g_stats.lockSettles;
    if (g_verbose.load(std::memory_order_relaxed)) {
      char detail[256] = "";
      int len = 0, shown = 0;
      for (int i = 0; i < g_clk.nPawns && shown < 4 && len < (int)sizeof(detail) - 64; ++i) {
        const PawnSlot& s = g_clk.pawns[i];
        bool moving = false;
        const double target = SettleTarget(s, cap, moving);
        if (target < 0.0 || target - s.sim <= kSettleTolerance) continue;
        len += snprintf(detail + len, sizeof(detail) - len, "%s#%d died %.2fs ago", shown ? ", " : "",
                        s.idx, s.deathTime > 0.0f ? g_clk.curtime - s.deathTime : -1.0);
        ++shown;
      }
      Con("%d rebuilt ragdoll(s) behind the demo (%s) - simulating %.0f demo ticks of missed physics (%s%s)\n", behind, detail, fill,
          all ? "settlemode all" : "up to the most recent death", lock ? ", lockstep with animation" : "");
    }
  }
  g_clk.settling = true;
  if (lock) {
    if (!g_clk.lockRun) {
      g_clk.lockRun = true;  // the demo got paused during a burst settle: finish it in lockstep
      g_clk.lockChunks = 0;
    }
    if (g_clk.lockPhase == kLockIdle) {
      ArmLock(v, steps);
      return 0;
    }
    const bool noAnim = g_clk.lockPhase == kLockGive;
    if (noAnim && ++g_clk.lockWait < kLockWaitCalls) return 0;  // its animation update has not run yet
    const long n = (std::max)(1L, (std::min)(steps, long(g_clk.lockSteps)));
    for (long k = 0; k < n; ++k) r = o_step(self, worlds, count, dt, substeps, b, p);
    for (int i = 0; i < g_clk.nPawns; ++i) g_clk.pawns[i].sim += double(n) * g_clk.stepTicks;
    g_stats.settleSteps += n;
    ++g_stats.lockChunks;
    if (noAnim) ++g_stats.lockNoAnim;
    ++g_clk.lockChunks;
    g_clk.lockPhase = kLockIdle;
    if (g_traceOn.load(std::memory_order_relaxed)) {
      float c[3] = {0, 0, 0};
      const int li = LockPawn();
      PoseCentroid(g_clk.pawns[li].inst, c);
      char act[200];
      snprintf(act, sizeof(act), "settle lock step chunk %d steps %ld anim %d given %d used %d%s left %ld #%d pose %.2f %.2f %.2f",
               g_clk.lockChunks, n, g_clk.lockAnimTicks, g_clk.lockGiven, g_clk.lockUsed, noAnim ? " NO-ANIM" : "", want - n,
               g_clk.pawns[li].idx, c[0], c[1], c[2]);
      Trace(kModeClock, v, g_clk.T, g_clk.P, g_clk.P, dt, substeps, g_clk.phys, g_clk.stepTicks, int(n), act);
    }
    if (want - n > 0) ArmLock(v, (std::min)(want - n, chunkSteps));  // next chunk: animation next frame, physics after it
    return int(n);
  }
  g_clk.lockRun = false;
  for (long k = 0; k < steps; ++k) r = o_step(self, worlds, count, dt, substeps, b, p);
  for (int i = 0; i < g_clk.nPawns; ++i) g_clk.pawns[i].sim += double(steps) * g_clk.stepTicks;
  g_stats.settleSteps += steps;
  return int(steps);
}

// ---------------------------------------------------------------------------------------------------------- paused pose refresh

bool RttiName(uintptr_t vt, char* out, size_t cap) {
  __try {
    const uintptr_t col = *(uintptr_t*)(vt - 8);
    if (*(uint32_t*)col != 1) return false;
    const uint32_t tdRva = *(uint32_t*)(col + 12), selfRva = *(uint32_t*)(col + 20);
    strncpy_s(out, cap, (const char*)(col - selfRva + tdRva + 16), _TRUNCATE);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

enum VarKind : int8_t { kVarNone = 0, kVarAnimPose = 1, kVarOtherInterp = 2 };

// Main thread only.
VarKind VarKindOf(uintptr_t vt) {
  static uintptr_t cacheVt[128];
  static VarKind cacheKind[128];
  static int next = 0, logged = 0;
  for (int i = 0; i < 128; ++i)
    if (cacheVt[i] == vt) return cacheKind[i];
  char name[512] = "";
  VarKind kind = kVarNone;
  if (g_clientImg.has(vt) && RttiName(vt, name, sizeof(name)) && strstr(name, "CInterpolatedVar") &&
      SlotsInText(vt, {kVarClearHistory, kVarClearPhase, kVarNoteChanged}, g_clientText))
    kind = strstr(name, "InterpolatedAnimGraph2State_t") ? kVarAnimPose : kVarOtherInterp;
  cacheVt[next] = vt;
  cacheKind[next] = kind;
  next = (next + 1) % 128;
  if (kind != kVarNone && g_traceOn.load(std::memory_order_relaxed) && logged < 32) {
    ++logged;
    Con("interpolated var type%s: %s\n", kind == kVarAnimPose ? " (animation pose)" : "", name);
  }
  return kind;
}

struct HistInfo {
  bool ok = false;
  int count = 0, cap = 0, newest = 0;
  float interp = 0.0f;
};

HistInfo ReadHist(uintptr_t h) {
  HistInfo r;
  uintptr_t base = 0;
  uint32_t packed = 0;
  if (!Rd(h, base) || !Rd(h + kHistPacked, packed) || !Rd(h + kHistInterp, r.interp)) return r;
  const int head = int(packed & 0x3f);
  r.count = int((packed >> 13) & 0x3f);
  r.cap = int((packed >> 19) & 0x3f);
  if (r.cap < 1 || r.count > r.cap || head >= 2 * r.cap || !std::isfinite(r.interp) || r.interp < 0.0f || r.interp > 1.0f) return r;
  if (r.count > 0) {
    int32_t t = 0;
    if (!base || !Rd(base + uintptr_t(head < r.cap ? head : head - r.cap) * 8, t)) return r;
    r.newest = t;
  }
  r.ok = true;
  return r;
}

struct VarPlan {
  int phases = 1;
  int from[2] = {0, 0}, to[2] = {0, 0};
};

// Clears the var's history and stores the current value at ticks from..to per phase. False if the game code crashed.
bool RebuildVarRaw(uintptr_t var, const VarPlan& plan) {
  __try {
    void** vt = *(void***)var;
    ((void (*)(uintptr_t))vt[kVarClearHistory])(var);
    for (int ph = 0; ph < plan.phases; ++ph) {
      bool changed = false;
      for (int t = plan.from[ph]; t <= plan.to[ph]; ++t) changed |= ((bool (*)(uintptr_t, int, int))vt[kVarNoteChanged])(var, ph, t);
      if (changed) *(uint8_t*)(var + kVarChangedBits) |= uint8_t(1 << (6 + ph));  // as the game's own latch does
    }
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Rebuilds the interpolation history of one entity from its current state, keeping the newest history tick, so the next tick
// latched after resuming is accepted as usual.
void RebuildHistory(uintptr_t ent, bool allVars, ShowDiag& d) {
  int tickcount = -1;
  uintptr_t gl = 0;
  if (Rd(g_globalsVar, gl) && gl) Rd(gl + kGlobalsTickcount, tickcount);
  for (int li = 0; li < 3; ++li) {
    int n = 0;
    uintptr_t arr = 0;
    if (!Rd(ent + kEntVarLists[li][0], n) || !Rd(ent + kEntVarLists[li][1], arr) || n <= 0 || n > 1024 || !arr) continue;
    for (int i = 0; i < n; ++i) {
      uintptr_t var = 0, vt = 0;
      if (!Rd(arr + uintptr_t(i) * 16, var) || !Rd(var, vt)) continue;
      ++d.vars;
      const VarKind kind = VarKindOf(vt);
      if (kind == kVarNone || (kind != kVarAnimPose && !allVars)) continue;
      ++d.matched;
      uint8_t flags = 0;
      uintptr_t h = 0;
      if (!Rd(var + kVarFlags, flags) || !Rd(var + kVarHistory, h) || !h) continue;
      VarPlan plan;
      plan.phases = (flags & 0x20) ? 2 : 1;
      bool ok = true;
      for (int ph = 0; ph < plan.phases && ok; ++ph) {
        const HistInfo hi = ReadHist(h + uintptr_t(ph) * kHistPhaseSize);
        const int newest = hi.ok && hi.count > 0 ? hi.newest : tickcount;
        ok = hi.ok && newest > 0;
        // Enough identical samples to cover the interpolation window, so every render time resolves to the current pose.
        int keep = int((hi.interp + 0.05f) * 64.0f + 0.5f) + 1;
        keep = (std::min)((std::min)((std::max)(keep, 2), 30), hi.cap);
        plan.from[ph] = (std::max)(1, newest - keep + 1);
        plan.to[ph] = newest;
      }
      if (!ok) continue;
      if (!RebuildVarRaw(var, plan)) {
        ++d.faults;
        return;
      }
      ++d.rebuilt;
      for (int ph = 0; ph < plan.phases; ++ph) {
        const HistInfo hi = ReadHist(h + uintptr_t(ph) * kHistPhaseSize);
        if (hi.ok) d.samples += hi.count;
      }
    }
  }
}

bool WriteByte(uintptr_t a, uint8_t v) {
  __try {
    *(uint8_t*)a = v;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool CallInterpReset(uintptr_t ent) {
  __try {
    g_interpReset(ent, false);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool WriteInt32(uintptr_t a, int32_t v) {
  __try {
    *(int32_t*)a = v;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Trace metric: mean position of the first skeleton bones (bone 0 is the model root and does not follow the ragdoll).
bool PoseCentroid(uintptr_t ent, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  uintptr_t node = 0, bones = 0;
  float buf[kPoseBones * 8];  // 32-byte transforms: position, scale, rotation
  if (g_offNode <= 0 || g_offModelState <= 0 || !Rd(ent + g_offNode, node) || !node ||
      !Rd(node + g_offModelState + kModelStateBones, bones) || !bones || !SafeCopy(buf, (void*)bones, sizeof(buf)))
    return false;
  double sum[3] = {0, 0, 0};
  int n = 0;
  for (int i = 1; i < kPoseBones; ++i) {
    const float* p = buf + i * 8;
    bool ok = true;
    for (int k = 0; k < 3; ++k) ok &= std::isfinite(p[k]) && std::fabs(p[k]) < 1e6f;
    if (!ok) continue;
    for (int k = 0; k < 3; ++k) sum[k] += p[k];
    ++n;
  }
  if (!n) return false;
  for (int k = 0; k < 3; ++k) out[k] = float(sum[k] / n);
  return true;
}

uintptr_t AnimControllerOf(uintptr_t ent) {
  __try {
    return g_animCtrl(ent);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

void ShowPose(const PawnSlot& s, int strategy, ShowDiag& d) {
  uintptr_t vt = 0;
  if (!Rd(s.inst, vt) || !IsPlayerPawnVtable(vt)) return;
  ++d.ents;
  if ((strategy == kShowSched || strategy == kShowReset) && g_offAnimSched > 0 && !WriteByte(s.inst + g_offAnimSched, 1)) ++d.faults;
  if (strategy == kShowReset) {
    if (g_interpReset && !CallInterpReset(s.inst)) ++d.faults;
  } else if (ShowRebuilds(strategy)) {
    RebuildHistory(s.inst, strategy == kShowAll, d);
  }
}

// One-time call stack of a hooked game function (module+offset per frame) to the console and trace.
void LogBacktraceOnce(int which, const char* what) {
  static bool done[2];
  if (done[which]) return;
  done[which] = true;
  void* frames[12];
  const USHORT n = RtlCaptureStackBackTrace(1, 12, frames, nullptr);
  char line[1024];
  int len = snprintf(line, sizeof(line), "%s call stack:", what);
  for (USHORT i = 0; i < n && len < (int)sizeof(line) - 80; ++i) {
    HMODULE m = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)frames[i], &m))
      GetModuleFileNameA(m, path, MAX_PATH);
    const char* slash = strrchr(path, '\\');
    len += snprintf(line + len, sizeof(line) - len, " %s+0x%llx", slash ? slash + 1 : path,
                    (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)m));
  }
  Con("%s\n", line);
  Trace(kModeClock, g_clk.last, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, line);
}

int ForceAnimTickRaw(void* sys) {
  uint8_t ev[64] = {};
  ev[kAnimTickEventActive] = 1;
  __try {
    o_animTick(sys, ev, nullptr, nullptr);
    return 1;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// Runs the game's client animation update once, as a demo tick would. Main thread only.
int ForceAnimTick() {
  void* sys = g_animSys.load(std::memory_order_relaxed);
  if (!o_animTick || !sys) return kTickUnavailable;
  TraceFlush();  // a crash inside the game still leaves a complete trace
  if (ForceAnimTickRaw(sys) < 0) {
    ++g_stats.animFaults;
    return kTickCrashed;
  }
  ++g_stats.animForced;
  return kTickRan;
}

// ------------------------------------------------------------------------------------------------ paused catch-up (v1.4)
// How a ragdoll actually moves: the game's animation job asks a helper for the ticks since the entity's last animation update (the
// helper stores the current tick in the entity's animation controller) and skips the graph and pose work when that is 0. The
// ragdoll's own simulation runs on that elapsed time, not on the physics step - one update with 50 ticks of elapsed time drops a
// corpse all the way to the floor. While a demo is paused the job never runs: the animation system's tick event arrives with its
// active byte clear. So a ragdoll a seek rebuilt keeps the pose of the moment it died, and the first update after the resume hands
// it every missed tick at once. That is the snap this addon exists to remove.
// This runs that catch-up while the demo is still paused: it writes each tracked ragdoll's real age (time since its death) into its
// controller and runs one active animation update, which is exactly the elapsed time resuming would have given it. The controller
// is left at the current tick, so the resume has nothing left to catch up on. Returns the ragdolls that took it. Main thread only.
// afterSettle (v1.8 pose copy): the same update, but run after the burst settle already gave the physics bodies their missed time.
// Run instead of the physics (above) it launched rebuilt ragdolls; run after it, it only has settled bodies to copy onto the bones.
int CatchUpAnim(const DemoView& v, double cap, bool afterSettle) {
  const char* what = afterSettle ? "POSECOPY" : "CATCHUP";
  void* sys = g_animSys.load(std::memory_order_relaxed);
  uintptr_t gl = 0;
  int32_t tickcount = 0;
  if (!sys || !o_animTick || !AnimTimeAvailable() || !Rd(g_globalsVar, gl) || !gl || !Rd(gl + kGlobalsTickcount, tickcount) ||
      tickcount <= 0) {
    if (afterSettle) {
      ++g_stats.copyMisses;
      InsMark("%s unavailable: sys %p animTick %d animTime %d globals tick %d", what, sys, o_animTick != nullptr, AnimTimeAvailable(),
              tickcount);
    }
    return 0;
  }
  uintptr_t ctrl[kMaxPawns] = {};
  int32_t want[kMaxPawns] = {}, was[kMaxPawns] = {}, got[kMaxPawns] = {};
  int n = 0, faults = 0, first = -1;
  for (int i = 0; i < g_clk.nPawns; ++i) {
    PawnSlot& s = g_clk.pawns[i];
    if (s.caughtTick == tickcount) continue;  // already caught up while paused here
    bool moving = false;
    const double target = SettleTarget(s, cap, moving);
    uintptr_t vt = 0;
    if (target < 0.0 || !Rd(s.inst, vt) || !IsPlayerPawnVtable(vt)) continue;
    const uintptr_t c = AnimControllerOf(s.inst);
    int32_t last = 0;
    if (!c || !Rd(c + g_offLastAnimTick, last)) continue;
    long age = std::lround(target);
    if (age < kCatchMinTicks) age = kCatchMinTicks;
    if (age > kCatchMaxTicks) age = kCatchMaxTicks;
    // After a seek while paused the update job keeps handing out the tick it had before the seek, not the globals tickcount. The
    // elapsed time it computes is (its tick - our value), so the age has to be taken off the controller's own last tick: taking it off
    // the globals tickcount gave a negative elapsed time and flung the ragdoll away on the resume.
    if (last <= int32_t(age)) continue;
    if (!WriteInt32(c + g_offLastAnimTick, last - int32_t(age))) {
      ++faults;
      continue;
    }
    ctrl[i] = c;
    was[i] = last;
    want[i] = last - int32_t(age);
    if (first < 0) first = i;
    ++n;
  }
  g_stats.catchFaults += faults;
  if (!n) {
    if (afterSettle) {
      ++g_stats.copyMisses;
      InsMark("%s skipped: no ragdoll to update (tick %d globals %d pawns %d faults %d)", what, v.tick, tickcount, g_clk.nPawns, faults);
    }
    return 0;
  }
  float pre[3] = {0, 0, 0}, post[3] = {0, 0, 0};
  PoseCentroid(g_clk.pawns[first].inst, pre);
  TraceFlush();  // a crash inside the game still leaves a complete trace
  const int ran = ForceAnimTickRaw(sys);
  if (ran < 0)
    ++g_stats.animFaults;
  else
    ++g_stats.animForced;
  // The job stores its tick in every controller it updates. Only a tick past our value is a real forward update: equal means the
  // update did not reach the controller, and below it means the job ran with a negative elapsed time.
  int used = 0;
  for (int i = 0; i < g_clk.nPawns; ++i) {
    if (!ctrl[i]) continue;
    if (Rd(ctrl[i] + g_offLastAnimTick, got[i]) && got[i] > want[i]) {
      ++used;
      continue;
    }
    WriteInt32(ctrl[i] + g_offLastAnimTick, was[i]);
    ctrl[i] = 0;
  }
  PoseCentroid(g_clk.pawns[first].inst, post);
  const int32_t age = was[first] - want[first];
  InsMark("%s tick %d globals %d pawns %d used %d faults %d ran %d #%d age %d last %d -> %d (wrote %d) sim %.1f pose %.2f %.2f %.2f -> "
          "%.2f %.2f %.2f", what, v.tick, tickcount, n, used, faults, ran, g_clk.pawns[first].idx, age, was[first], got[first],
          want[first], g_clk.pawns[first].sim, pre[0], pre[1], pre[2], post[0], post[1], post[2]);
  if (g_traceOn.load(std::memory_order_relaxed)) {
    char act[320];
    snprintf(act, sizeof(act),
             "%s pawns %d used %d faults %d globals %d #%d age %d last %d -> %d pre %.2f %.2f %.2f pose %.2f %.2f %.2f",
             afterSettle ? "posecopy" : "catchup", n, used, faults, tickcount, g_clk.pawns[first].idx, age, was[first], got[first], pre[0],
             pre[1], pre[2], post[0], post[1], post[2]);
    Trace(kModeClock, v, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
  }
  if (!used) {
    // The update did not reach them: put the controllers back the way the game had them and let the physics settle have a go.
    if (afterSettle) {
      ++g_stats.copyMisses;
    } else {
      ++g_stats.catchMisses;
    }
    static bool noted[2] = {false, false};
    if (!noted[afterSettle]) {
      noted[afterSettle] = true;
      Con(afterSettle ? "note: the pose copy animation update did not reach the settled ragdolls - they take their pose when playback resumes\n"
                      : "note: the animation update did not reach the rebuilt ragdolls - falling back to the physics settle\n");
    }
    return 0;
  }
  for (int i = 0; i < g_clk.nPawns; ++i) {
    if (!ctrl[i]) continue;
    g_clk.pawns[i].caughtTick = tickcount;
    if (!afterSettle) g_clk.pawns[i].sim = double(was[i] - want[i]);  // it has the time now: the physics settle has nothing left to run
  }
  if (afterSettle) {
    ++g_stats.copyRuns;
    g_stats.copyPawns += used;
  } else {
    ++g_stats.catchRuns;
    g_stats.catchPawns += used;
  }
  // The pose the update just produced still has to reach the renderer, or the paused frames keep showing the interpolated old one.
  const char* why = afterSettle ? "posecopy" : "catchup";
  g_clk.staleCall = g_clk.calls;
  g_clk.staleTick = v.tick;
  g_clk.staleWhy = why;
  RunShow(v, kShowAnim, why, why);
  if (g_verbose.load(std::memory_order_relaxed))
    Con(afterSettle ? "%d settled ragdoll(s) got their pose while paused (one animation update, %d demo ticks of animation time)\n"
                    : "%d rebuilt ragdoll(s) caught up with one animation update (%d demo ticks of animation time)\n",
        used, age);
  return used;
}

// One paused pose refresh: the animation update (show 5/6), then the per-ragdoll strategy. Writes its trace row.
void RunShow(const DemoView& v, int strategy, const char* armedWhy, const char* site) {
  float pre[3] = {0, 0, 0}, post[3] = {0, 0, 0};
  ShowDiag d;
  PoseCentroid(g_clk.pawns[0].inst, pre);
  if (ShowRunsTick(strategy)) {
    d.tick = ForceAnimTick();
    if (d.tick == kTickRan) ++g_clk.winTicks;
    if (d.tick == kTickCrashed) ++d.faults;
  }
  for (int i = 0; i < g_clk.nPawns && !d.faults; ++i) ShowPose(g_clk.pawns[i], strategy, d);
  PoseCentroid(g_clk.pawns[0].inst, post);
  g_clk.showLast = d;
  g_stats.showRebuilt += d.rebuilt;
  g_stats.showFaults += d.faults;
  char act[320];
  snprintf(act, sizeof(act),
           "show s%d %s %s%s left %d tick %s ents %d vars %d match %d rebuilt %d samples %d faults %d #%d pre %.2f %.2f %.2f pose %.2f %.2f %.2f",
           strategy, site, armedWhy ? "arm " : "", armedWhy ? armedWhy : "", g_clk.showLeft, TickName(d.tick), d.ents, d.vars, d.matched,
           d.rebuilt, d.samples, d.faults, g_clk.pawns[0].idx, pre[0], pre[1], pre[2], post[0], post[1], post[2]);
  Trace(kModeClock, v, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
  if (d.tick == kTickCrashed) {
    g_show = kShowAnim;
    Con("the animation update crashed inside the game while paused - switched to show 3 (pose history rebuild only)\n");
  } else if (d.faults) {
    g_show = kShowOff;
    g_clk.showActive = false;
    g_clk.showLeft = 0;
    Con("paused pose refresh (show %d) failed inside the game - switched off. Try another strategy: ragdollfix show <1-6>\n", strategy);
  }
}

void EndShowWindow(bool verbose) {
  if (!g_clk.showActive) return;
  g_clk.showActive = false;
  g_clk.showLeft = 0;
  g_clk.watchLeft = kShowWatchCalls;
  if (!verbose) return;
  const ShowDiag& d = g_clk.showLast;
  const int strategy = g_show.load(std::memory_order_relaxed);
  Con("paused: brought the settled pose of %d ragdoll(s) to the screen (show %d: %s; %d animation updates, %d of %d interpolation "
      "histories rebuilt, %d samples)\n",
      d.ents, strategy, ShowName(strategy), g_clk.winTicks, d.rebuilt, d.vars, d.samples);
  if (ShowRebuilds(strategy) && d.ents && !d.matched)
    Con("note: no animation pose history found on these ragdolls - try 'ragdollfix show 4' or 'ragdollfix show 2'\n");
  if (ShowRunsTick(strategy) && !g_clk.winTicks)
    Con("note: the animation update did not run (%s)\n",
        !o_animTick ? "not found in this CS2 build" : "not captured yet - let the demo play for a moment, then pause again");
  if (ShowAdvancesAnim(strategy) && d.ents && !g_clk.winTicks)
    Con("note: the game's animation update did not use the animation time given to these ragdolls - try 'ragdollfix show 5'\n");
  if (strategy == kShowReset && !g_interpReset) Con("note: the game interpolation reset was not found in this CS2 build - use show 3 or 4\n");
}

// While paused, keeps pushing the pose of ragdolls that were just settled into their interpolation history.
void PausedShow(const DemoView& v, int settled) {
  const bool verbose = g_verbose.load(std::memory_order_relaxed);
  const int strategy = EffectiveShow();
  if (!v.paused || strategy == kShowOff || g_clk.nPawns == 0) {
    EndShowWindow(verbose);
    g_clk.watchLeft = 0;
    g_clk.framePending = false;
    g_clk.animPending = false;
    return;
  }
  if (g_clk.lockBusy) {
    // Lockstep settle gives animation time in step with physics; a show 7 window now would run the animation ahead of the physics.
    g_clk.showActive = false;
    g_clk.showLeft = 0;
    g_clk.framePending = false;
    g_clk.animPending = false;
    g_clk.staleCall = INT64_MIN / 2;
    return;
  }
  // Arm on a stale pose from this engine tick or from the skip that just ended (a few engine ticks back, within a couple of demo
  // ticks). A plain pause during playback never arms, so it cannot pop the pose.
  const bool armed = settled || (g_clk.calls - g_clk.staleCall <= kShowStaleCalls && std::abs(v.tick - g_clk.staleTick) <= kShowStaleTicks);
  const char* why = settled ? "settle" : g_clk.staleWhy;
  if (armed) {
    g_clk.showLeft = ShowAdvancesAnim(strategy) ? kAnimWindowCalls : kShowWindowCalls;
    g_clk.staleCall = INT64_MIN / 2;
  }
  float b[3] = {0, 0, 0};
  char act[200];
  if (g_clk.showLeft <= 0) {
    EndShowWindow(verbose);
    if (g_clk.watchLeft > 0 && g_traceOn.load(std::memory_order_relaxed)) {
      --g_clk.watchLeft;
      PoseCentroid(g_clk.pawns[0].inst, b);
      snprintf(act, sizeof(act), "pose #%d pose %.2f %.2f %.2f", g_clk.pawns[0].idx, b[0], b[1], b[2]);
      Trace(kModeClock, v, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
    }
    return;
  }
  if (!armed) --g_clk.showLeft;
  if (!g_clk.showActive) {
    g_clk.showActive = true;
    g_clk.winTicks = 0;
    ++g_stats.showWindows;
  }
  if (ShowAdvancesAnim(strategy)) {
    g_clk.animPending = true;  // Hk_AnimTick gives the animation time with the next animation update (it runs before the next step)
    g_clk.animArmWhy = armed ? why : nullptr;
  } else if (ShowRunsTick(strategy) && g_tickSite.load(std::memory_order_relaxed) == kSiteFrame && o_frame) {
    g_clk.framePending = true;  // Hk_Frame runs the refresh at the next frame boundary
    g_clk.frameArmWhy = armed ? why : nullptr;
    snprintf(act, sizeof(act), "show s%d step %s%s left %d queued for the frame boundary", strategy, armed ? "arm " : "", armed ? why : "",
             g_clk.showLeft);
    Trace(kModeClock, v, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
  } else {
    RunShow(v, strategy, armed ? why : nullptr, "step");
  }
  if (g_clk.showActive && g_clk.showLeft == 0) EndShowWindow(verbose);
}

// CPhysicsGameSystem frame boundary: runs a paused pose refresh queued by the physics step (ticksite frame).
void FrameHooked(void* self, void* ev, void* a3, void* a4) {
  o_frame(self, ev, a3, a4);
  ++g_stats.frameCalls;
  if (!g_clk.framePending) return;
  g_clk.framePending = false;
  const int strategy = EffectiveShow();
  if (g_health.load(std::memory_order_relaxed) != kReady || g_mode.load(std::memory_order_relaxed) != kModeClock || !g_clk.paused ||
      g_clk.nPawns == 0 || !ShowRunsTick(strategy))
    return;
  RunShow(g_clk.last, strategy, g_clk.frameArmWhy, "frame");
}

// Show 7: takes back animation time the game never used (e.g. an off-screen ragdoll skipped by the update) once the refresh is over,
// so it does not get an extra tick of animation on resume.
void RestoreAnimTime() {
  g_clk.animOwed = 0;
  for (int i = 0; i < g_clk.nPawns; ++i) {
    PawnSlot& s = g_clk.pawns[i];
    const int32_t wrote = s.animWrote, given = s.animGiven > 0 ? s.animGiven : 1;
    s.animWrote = 0;
    s.animGiven = 0;
    uintptr_t vt = 0, c = 0;
    int32_t now = 0;
    if (!wrote || !Rd(s.inst, vt) || !IsPlayerPawnVtable(vt) || !(c = AnimControllerOf(s.inst))) continue;
    if (Rd(c + g_offLastAnimTick, now) && now == wrote) WriteInt32(c + g_offLastAnimTick, wrote + given);
  }
}

// Runs the game's animation update with `ticks` of animation time for the tracked ragdolls (only the lockstep-behind ones when
// onlyBehind), so their death animation advances although the demo tick is frozen, then rebuilds their pose history. Main thread only.
void GiveAnimTime(void* self, void* ev, void* a3, void* a4, int ticks, bool onlyBehind, int li, ShowDiag& d, int32_t first[2],
                  float pre[3], float post[3]) {
  PoseCentroid(g_clk.pawns[li].inst, pre);
  uintptr_t ctrl[kMaxPawns] = {};
  int tickcount = 0;
  uintptr_t gl = 0;
  if (Rd(g_globalsVar, gl) && gl) Rd(gl + kGlobalsTickcount, tickcount);
  for (int i = 0; i < g_clk.nPawns; ++i) {
    PawnSlot& s = g_clk.pawns[i];
    if (onlyBehind && !s.lockBehind) continue;
    uintptr_t vt = 0;
    if (!Rd(s.inst, vt) || !IsPlayerPawnVtable(vt)) continue;
    const uintptr_t c = AnimControllerOf(s.inst);
    int32_t last = 0;
    if (!c || !Rd(c + g_offLastAnimTick, last) || last <= ticks || (tickcount > 0 && std::abs(last - tickcount) > (1 << 20))) continue;
    if (i == li) first[0] = last;
    if (s.animWrote && last == s.animWrote) {
      ++d.waiting;  // time given last frame was not used yet (not in this frame's update); do not stack more time on it
      continue;
    }
    if (!WriteInt32(c + g_offLastAnimTick, last - ticks)) {
      ++d.faults;
      continue;
    }
    s.animWrote = last - ticks;
    s.animGiven = ticks;
    ctrl[i] = c;
    ++d.given;
  }
  TraceFlush();  // a crash inside the game still leaves a complete trace
  o_animTick(self, ev, a3, a4);
  for (int i = 0; i < g_clk.nPawns; ++i) {
    int32_t now = 0;
    if (!ctrl[i] || !Rd(ctrl[i] + g_offLastAnimTick, now)) continue;
    if (i == li) first[1] = now;
    if (now != g_clk.pawns[i].animWrote) {
      ++d.used;
      g_clk.pawns[i].animWrote = 0;
      g_clk.pawns[i].animGiven = 0;
    }
  }
  g_clk.animOwed = 0;
  for (int i = 0; i < g_clk.nPawns; ++i) g_clk.animOwed += g_clk.pawns[i].animWrote != 0;
  for (int i = 0; i < g_clk.nPawns && !d.faults; ++i) ShowPose(g_clk.pawns[i], kShowAnimTime, d);
  PoseCentroid(g_clk.pawns[li].inst, post);
  g_stats.animGiven += d.given;
  g_stats.animUsed += d.used;
  g_stats.showRebuilt += d.rebuilt;
  g_stats.showFaults += d.faults;
}

// Lockstep settle: the animation half of a chunk. The physics step that follows runs the chunk's physics.
void LockAnimUpdate(void* self, void* ev, void* a3, void* a4) {
  float pre[3] = {0, 0, 0}, post[3] = {0, 0, 0};
  int32_t first[2] = {0, 0};
  ShowDiag d;
  const int li = LockPawn();
  GiveAnimTime(self, ev, a3, a4, g_clk.lockAnimTicks, true, li, d, first, pre, post);
  g_clk.lockGiven = d.given;
  g_clk.lockUsed = d.used;
  g_clk.lockPhase = kLockStep;
  g_clk.showLast = d;
  if (g_traceOn.load(std::memory_order_relaxed)) {
    char act[320];
    snprintf(act, sizeof(act),
             "settle lock anim chunk %d give %d given %d used %d waiting %d last %d->%d rebuilt %d faults %d #%d pre %.2f %.2f %.2f "
             "pose %.2f %.2f %.2f",
             g_clk.lockChunks + 1, g_clk.lockAnimTicks, d.given, d.used, d.waiting, first[0], first[1], d.rebuilt, d.faults,
             g_clk.pawns[li].idx, pre[0], pre[1], pre[2], post[0], post[1], post[2]);
    Trace(kModeClock, g_clk.last, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
  }
  if (d.faults) {
    g_settleChunk = 0;
    Con("giving ragdolls animation time during the settle failed - switched to settlechunk 0 (burst settle)\n");
  }
}

// Show 7: runs the game's animation update with one tick of animation time for every tracked ragdoll, so it evaluates their pose from
// the (settled) physics bodies although the demo tick is frozen, then rebuilds their pose history. Main thread only.
void AnimTimeUpdate(void* self, void* ev, void* a3, void* a4) {
  float pre[3] = {0, 0, 0}, post[3] = {0, 0, 0};
  int32_t first[2] = {0, 0};
  ShowDiag d;
  GiveAnimTime(self, ev, a3, a4, 1, false, 0, d, first, pre, post);
  if (d.used) ++g_clk.winTicks;
  g_clk.showLast = d;
  if (g_traceOn.load(std::memory_order_relaxed)) {
    char act[360];
    const char* why = g_clk.animArmWhy;
    snprintf(act, sizeof(act),
             "show s7 anim %s%s left %d given %d used %d waiting %d last %d->%d ents %d vars %d match %d rebuilt %d samples %d faults %d #%d "
             "pre %.2f %.2f %.2f pose %.2f %.2f %.2f",
             why ? "arm " : "", why ? why : "", g_clk.showLeft, d.given, d.used, d.waiting, first[0], first[1], d.ents, d.vars, d.matched,
             d.rebuilt, d.samples, d.faults, g_clk.pawns[0].idx, pre[0], pre[1], pre[2], post[0], post[1], post[2]);
    Trace(kModeClock, g_clk.last, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
  }
  if (d.faults) {
    g_show = kShowTick;
    Con("giving ragdolls animation time while paused failed (show 7) - switched to show 5\n");
  }
}

// CAnimGraphGameSystem client tick: runs every frame (also while paused). Captures the system instance for show 5/6 and gives
// ragdolls animation time for show 7.
void AnimTickHooked(void* self, void* ev, void* a3, void* a4) {
  if (!g_animSys.load(std::memory_order_relaxed) && (uintptr_t)Vt(self) == g_animVt) g_animSys = self;
  ++g_stats.animCalls;
  if (g_clk.playing && g_clk.paused) ++g_stats.animPaused;
  if (g_traceOn.load(std::memory_order_relaxed) && g_clk.playing) LogBacktraceOnce(1, "animation tick");
  const bool pending = g_clk.animPending;
  g_clk.animPending = false;
  const bool clockReady = g_health.load(std::memory_order_relaxed) == kReady && g_mode.load(std::memory_order_relaxed) == kModeClock &&
                          g_clk.playing && g_clk.paused && g_clk.nPawns > 0 && AnimTimeAvailable();
  if (g_clk.lockPhase == kLockGive || pending) {
    // Only while the demo is still paused on the tick the time was queued for: a seek in between rebuilds everything anyway.
    DemoView now;
    const bool still = clockReady && ReadDemoRaw(now) && now.playing && !now.badLayout && now.paused &&
                       std::abs(now.tick - (g_clk.lockPhase == kLockGive ? g_clk.lockTick : g_clk.last.tick)) <= kShowStaleTicks;
    if (g_clk.lockPhase == kLockGive) {
      if (still) {
        LockAnimUpdate(self, ev, a3, a4);
        return;
      }
      CancelLock("demo moved");
    } else if (still && ShowAdvancesAnim(g_show.load(std::memory_order_relaxed))) {
      AnimTimeUpdate(self, ev, a3, a4);
      return;
    }
  }
  if (g_clk.animOwed > 0 && AnimTimeAvailable()) RestoreAnimTime();
  o_animTick(self, ev, a3, a4);
}

void* DemoClockStep(const DemoView& v, void* self, void** worlds, int count, float dt, int substeps, bool b, void* p) {
  ++g_clk.calls;
  if (g_traceOn.load(std::memory_order_relaxed)) LogBacktraceOnce(0, "physics step");
  // A single engine tick with a wildly different demo tick (seen right after seeks) must not re-sync everything twice.
  if (g_clk.valid && g_clk.lastTick >= 0 && std::abs(v.tick - g_clk.lastTick) > kSuspectJumpTicks && !g_clk.suspect) {
    g_clk.suspect = true;
    ++g_stats.gated;
    ++g_stats.suspects;
    CancelLock("suspect jump");
    g_clk.animPending = false;
    Trace(kModeClock, v, v.tick + double(v.frac), g_clk.P, g_clk.P, dt, substeps, g_clk.phys, g_clk.stepTicks, 0, "suspect_jump");
    return nullptr;
  }
  g_clk.suspect = false;
  const int prevTick = g_clk.lastTick;
  g_clk.lastTick = v.tick;
  const float phys = PhysTimescale();
  const double stepTicks = phys > 0.0f ? double(dt) * (substeps > 1 ? substeps : 1) / phys * kTicksPerSecond : 0.0;
  const double T = v.tick + double(v.frac);
  g_clk.T = T;
  g_clk.phys = phys;
  g_clk.dt = dt;
  g_clk.stepTicks = stepTicks;
  if (!(stepTicks > 1e-4 && stepTicks <= 64.0)) {
    // cl_phys_timescale 0 or a zero dt: the step does not move anything, leave it alone and restart the clock later.
    g_clk.valid = false;
    g_clk.rebase = true;  // ragdolls do not move during these steps; do not count that as missed physics later
    CancelLock("no physics time");
    g_clk.lockBusy = false;
    ++g_stats.passed;
    Trace(kModeClock, v, T, 0, 0, dt, substeps, phys, stepTicks, 1, "passthrough");
    return o_step(self, worlds, count, dt, substeps, b, p);
  }

  // Unpaused, physics may run up to one tick ahead of the demo clock (absorbs the uneven engine tick cadence so normal playback
  // steps exactly like vanilla), plus half a tick because the catch-up step at a pause can leave physics slightly ahead of the frame
  // playback resumes from. Paused, it may only close the gap to the paused moment (plus tick stepping).
  const bool paused = v.paused;
  const double lead = paused ? 0.5 * stepTicks : (std::max)(1.0, 0.5 * stepTicks) + 0.5;
  const double lag = (std::max)(1.0, stepTicks);
  const double resyncFwd = (std::max)(double(g_resyncTicks.load(std::memory_order_relaxed)), 2.0 * stepTicks + lag);
  const double resyncBack = (std::max)(3.0, lead + stepTicks + 1.0);

  const char* action = "gate";
  if (!g_clk.valid) {
    g_clk.P = T - stepTicks;
    g_clk.valid = true;
    g_clk.lastJumpCall = g_clk.calls;
    action = "start";
  } else {
    const double e = g_clk.P - T;
    if (e < -resyncFwd || e > resyncBack) {
      g_clk.P = T - stepTicks;
      ++g_stats.resyncs;
      action = "resync";
      g_clk.lastJumpCall = g_clk.calls;
      CancelLock("resync");
      g_clk.animPending = false;
      if (g_verbose.load(std::memory_order_relaxed))
        Con("demo time jumped %+.1f ticks (now tick %d) - physics clock re-synced\n", -e, v.tick);
    }
  }

  const int settleMax = g_settleOk ? g_settleMax.load(std::memory_order_relaxed) : 0;
  if (settleMax > 0) {
    g_clk.curtime = CurTime();
    UpdatePawns(settleMax);
  } else {
    g_clk.nPawns = 0;
    g_clk.rebase = true;
    g_clk.settling = false;
    CancelLock("settle off");
    g_clk.lockBusy = g_clk.lockRun = false;
  }

  const double p0 = g_clk.P;
  const int cap = (std::max)(1, g_maxSteps.load(std::memory_order_relaxed));
  int steps = 0;
  void* r = nullptr;
  if (g_clk.P + stepTicks <= T + lead + 1e-9) {
    const double extraLimit = paused ? T + lead : T - lag;
    do {
      r = o_step(self, worlds, count, dt, substeps, b, p);
      g_clk.P += stepTicks;
      ++steps;
    } while (steps < cap && g_clk.P + stepTicks <= extraLimit + 1e-9);
  }
  if (steps) {
    ++g_stats.passed;
    g_stats.extra += steps - 1;
    if (action[0] == 'g') action = steps > 1 ? "catchup" : "step";
  } else {
    ++g_stats.gated;
    if (paused) ++g_clk.heldWhilePaused;
  }
  Trace(kModeClock, v, T, p0, g_clk.P, dt, substeps, phys, stepTicks, steps, action);
  for (int i = 0; i < g_clk.nPawns; ++i) g_clk.pawns[i].sim += steps * stepTicks;

  int settled = 0;
  if (settleMax > 0) {
    settled = SettleStep(v, settleMax, self, worlds, count, dt, substeps, b, p, r);
    if (settled && !g_clk.lockRun) Trace(kModeClock, v, T, g_clk.P, g_clk.P, dt, substeps, phys, stepTicks, settled, "settle");
    const bool jumped = prevTick >= 0 && (v.tick < prevTick || v.tick > prevTick + (std::max)(1, int(std::ceil(stepTicks - 1e-6))));
    const char* why = settled ? "settle" : steps > 1 ? "catchup" : jumped && steps ? "jump" : nullptr;
    if (why && !g_clk.lockBusy) {
      g_clk.staleCall = g_clk.calls;
      g_clk.staleTick = v.tick;
      g_clk.staleWhy = why;
    }
    PausedShow(v, g_clk.lockBusy ? 0 : settled);
  }
  if (!v.paused && g_clk.poseRefLeft > 0 && g_clk.nPawns > 0 && g_traceOn.load(std::memory_order_relaxed)) {
    --g_clk.poseRefLeft;  // after resuming: shows whether the ragdoll snaps
    float c[3] = {0, 0, 0};
    PoseCentroid(g_clk.pawns[0].inst, c);
    char act[96];
    snprintf(act, sizeof(act), "poseref #%d pose %.2f %.2f %.2f", g_clk.pawns[0].idx, c[0], c[1], c[2]);
    Trace(kModeClock, v, T, g_clk.P, g_clk.P, dt, substeps, phys, stepTicks, 0, act);
  }
  return steps || settled ? r : nullptr;
}

bool KeyEdge(int vk) {
  static bool prev[256];
  const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  const bool edge = down && !prev[vk];
  prev[vk] = down;
  return edge;
}

void PrintStatus();

// Always active (the fallback if the console command does not work): RIGHT CTRL + F11 cycles the mode, RIGHT CTRL + F12 prints status.
void PollFallbackHotkeys() {
  const bool f11 = KeyEdge(VK_F11), f12 = KeyEdge(VK_F12);
  if (!(f11 || f12) || !(GetAsyncKeyState(VK_RCONTROL) & 0x8000)) return;
  HWND w = GetForegroundWindow();
  DWORD pid = 0;
  if (w) GetWindowThreadProcessId(w, &pid);
  if (pid != GetCurrentProcessId()) return;
  if (f11) {
    g_mode = (g_mode.load() + 1) % 3;
    ++g_epoch;
    Con("mode %d: %s\n", g_mode.load(), ModeName(g_mode.load()));
  }
  if (f12) PrintStatus();
}

void* StepHooked(void* self, void** worlds, int count, float dt, int substeps, bool b, void* p) {
  if (g_health.load(std::memory_order_relaxed) != kReady) return o_step(self, worlds, count, dt, substeps, b, p);
  PollFallbackHotkeys();
  const int epoch = g_epoch.load(std::memory_order_relaxed);
  if (g_clk.epoch != epoch) {
    const bool playing = g_clk.playing, paused = g_clk.paused;
    g_clk = ClockState();
    g_clk.epoch = epoch;
    g_clk.playing = playing;
    g_clk.paused = paused;
  }
  const int mode = g_mode.load(std::memory_order_relaxed);
  if (mode == kModeOff) {
    g_clk.valid = false;
    return o_step(self, worlds, count, dt, substeps, b, p);
  }
  DemoView v;
  if (!ReadDemo(v)) return o_step(self, worlds, count, dt, substeps, b, p);
  NoteTransitions(v, mode);
  if (!v.playing || v.tick < 0) {
    g_clk.valid = false;
    return o_step(self, worlds, count, dt, substeps, b, p);
  }
  if (mode == kModeFreeze) {
    g_clk.valid = false;
    if (v.paused) {
      ++g_stats.gated;
      ++g_clk.heldWhilePaused;
      Trace(mode, v, 0, 0, 0, dt, substeps, 0, 0, 0, "freeze");
      return nullptr;
    }
    ++g_stats.passed;
    Trace(mode, v, 0, 0, 0, dt, substeps, 0, 0, 1, "step");
    return o_step(self, worlds, count, dt, substeps, b, p);
  }
  return DemoClockStep(v, self, worlds, count, dt, substeps, b, p);
}

// ---------------------------------------------------------------------------------------------------------- console command

void PrintHelp() {
  Con("commands:\n");
  Con("  ragdollfix                   show status\n");
  Con("  ragdollfix mode <0|1|2>      0 = off (vanilla CS2), 1 = freeze (physics stops while paused), 2 = demo clock (default)\n");
  Con("  ragdollfix catchup <1-16>    max physics steps per engine tick when catching up after tick stepping (default 4)\n");
  Con("  ragdollfix resync <2-6400>   demo time jumps larger than this many ticks re-sync instead of fast-forwarding (default 16)\n");
  Con("  ragdollfix settle <0-1280>   after a seek, rebuilt ragdolls catch up on up to this many ticks of missed physics (default 384, 0 = off)\n");
  Con("  ragdollfix settlemode <newest|all>  newest: stop once the most recent death is in sync (default); all: settle every rebuilt ragdoll\n");
  Con("  ragdollfix settlechunk <0-%d> paused settle: demo ticks per frame, each after the same animation time (default 4, 0 = one burst)\n",
      kSettleChunkMax);
  Con("  ragdollfix animcatchup <0|1> paused seek: catch rebuilt ragdolls up with one animation update instead of physics steps (default 0)\n");
  Con("  ragdollfix posecopy <0|1>    paused seek: after the physics settle, one animation update puts the settled pose on the bones (default 1)\n");
  Con("  ragdollfix show <0-7>        while paused, how a settled ragdoll pose reaches the screen: 0 off, 1 animation update flag,\n");
  Con("                               2 game interpolation reset, 3 rebuild pose history, 4 rebuild all history,\n");
  Con("                               5 extra animation update + rebuild pose history, 6 extra animation update only,\n");
  Con("                               7 give ragdolls animation time while paused + rebuild pose history (default)\n");
  Con("  ragdollfix ticksite <step|frame>  where show 5/6 run the animation update: inside the physics step (default) or next frame\n");
  Con("  ragdollfix verbose <0|1>     print pause / resume / re-sync / settle messages (default 1)\n");
  Con("  ragdollfix trace <0|1>       write every physics step decision to a CSV file next to the DLL (logs folder)\n");
  Con("  ragdollfix insight [status]  insight recorder: memory changes around the newest dead player, per hooked game call\n");
  Con("  ragdollfix insight rec <0|1> [label]  start / stop recording to logs\\insight_*.bin (read with insight.py)\n");
  Con("  ragdollfix insight map       write logs\\insight_map_*.txt: what the census found around the ragdoll right now\n");
  Con("  ragdollfix insight mark <text>  put a note into the recording\n");
  Con("  ragdollfix forensic [frames]  arm the standard watchpoint set: who writes the anim tick, the bones and the ragdoll pages\n");
  Con("  ragdollfix watch [status|off|anim|tick|bone <i>|addr <hex> [len] [w|rw|x]|page <hex>|obj <id>|frames <n>]\n");
  Con("  hotkeys (during demo playback): RIGHT CTRL+F11 cycles the mode, RIGHT CTRL+F12 prints status\n");
}

void PrintStatus() {
  const int health = g_health.load();
  const int mode = g_mode.load();
  if (health == kInit) {
    Con("v%s still starting up, try again in a second\n", kVersion);
    return;
  }
  if (health == kDisabled) {
    Con("v%s DISABLED: %s\n", kVersion, g_disableReason);
    return;
  }
  Con("v%s ACTIVE (%s) - mode %d: %s\n", kVersion, g_buildVerified ? "verified CS2 build" : "unverified CS2 build, self-check passed",
      mode, ModeName(mode));
  const DemoView& v = g_clk.last;
  if (!g_clk.playing) {
    Con("  demo: not playing (addon idle outside demo playback)\n");
  } else {
    Con("  demo: %s at tick %d + %.3f\n", v.paused ? "PAUSED" : "playing", v.tick, v.frac);
    if (mode == kModeClock && g_clk.valid)
      Con("  physics clock: %.3f ticks (%+.3f vs demo), one step = %.4f demo ticks (dt %.6f, cl_phys_timescale %.3f)\n", g_clk.P,
          g_clk.P - g_clk.T, g_clk.stepTicks, g_clk.dt, g_clk.phys);
  }
  Con("  totals: %llu engine ticks stepped (%llu extra catch-up steps), %llu held back, %llu re-syncs\n",
      (unsigned long long)g_stats.passed, (unsigned long long)g_stats.extra, (unsigned long long)g_stats.gated,
      (unsigned long long)g_stats.resyncs);
  Con("  settings: catchup %d, resync %d ticks, verbose %d, trace %s%s\n", g_maxSteps.load(), g_resyncTicks.load(), (int)g_verbose.load(),
      g_traceOn.load() ? "on -> " : "off", g_traceOn.load() ? g_tracePath : "");
  if (!g_settleOk)
    Con("  seek settle: %s\n", g_settleSrc);
  else
    Con("  seek settle: %s, max %d ticks, mode %s | tracking %d dead players | %llu catch-up bursts, %llu steps\n",
        g_settleMax.load() > 0 ? "on" : "off", g_settleMax.load(), g_settleAll.load() ? "all" : "newest", g_clk.nPawns,
        (unsigned long long)g_stats.settleBursts, (unsigned long long)g_stats.settleSteps);
  if (g_settleOk)
    Con("  paused catch-up: animcatchup %d%s | %llu catch-ups, %llu ragdolls, %llu without an update, %llu faults\n", g_catchup.load(),
        g_catchup.load() > 0 ? (AnimTimeAvailable() ? "" : " (animation time not found in this build)") : " (off, physics settle)",
        (unsigned long long)g_stats.catchRuns, (unsigned long long)g_stats.catchPawns, (unsigned long long)g_stats.catchMisses,
        (unsigned long long)g_stats.catchFaults);
  if (g_settleOk)
    Con("  paused pose copy: posecopy %d%s | %llu copies, %llu ragdolls, %llu without an update, %llu dropped (playback resumed)\n",
        g_poseCopy.load(),
        g_poseCopy.load() > 0 ? (!AnimTimeAvailable() ? " (animation time not found in this build)"
                                 : EffectiveShow() == kShowAnimTime && g_settleChunk.load() > 0 ? " (not used: show 7 lockstep settle)" : "")
                              : " (off)",
        (unsigned long long)g_stats.copyRuns, (unsigned long long)g_stats.copyPawns, (unsigned long long)g_stats.copyMisses,
        (unsigned long long)g_stats.copyDropped);
  if (g_settleOk)
    Con("  lockstep settle: settlechunk %d%s | %llu settles, %llu chunks (%llu without animation time), %llu cancelled\n",
        g_settleChunk.load(), g_settleChunk.load() <= 0 ? " (off, burst settle)" : EffectiveShow() != kShowAnimTime ? " (needs show 7)" : "",
        (unsigned long long)g_stats.lockSettles, (unsigned long long)g_stats.lockChunks, (unsigned long long)g_stats.lockNoAnim,
        (unsigned long long)g_stats.lockCancels);
  Con("  paused pose refresh: show %d (%s) | %llu windows, %llu histories rebuilt, %llu faults | anim update flag %s, game reset %s\n",
      g_show.load(), ShowName(g_show.load()), (unsigned long long)g_stats.showWindows, (unsigned long long)g_stats.showRebuilt,
      (unsigned long long)g_stats.showFaults, g_offAnimSched > 0 ? "ok" : "not found", g_interpReset ? "ok" : "not found");
  Con("  animation update: %s | game ran it %llu times (%llu while paused), addon ran it %llu times while paused, %llu crashes | ticksite %s"
      " | frame boundary %s (%llu calls)\n",
      !o_animTick ? "NOT FOUND" : g_animSys.load() ? "hooked" : "hooked, not captured yet (let the demo play for a moment)",
      (unsigned long long)g_stats.animCalls, (unsigned long long)g_stats.animPaused, (unsigned long long)g_stats.animForced,
      (unsigned long long)g_stats.animFaults, TickSiteName(g_tickSite.load()), o_frame ? "hooked" : "not found",
      (unsigned long long)g_stats.frameCalls);
  Con("  animation time while paused (show 7): %s | given %llu times, used by the game %llu times\n",
      AnimTimeAvailable() ? "ok" : "NOT FOUND (show 7 falls back to show 5)", (unsigned long long)g_stats.animGiven,
      (unsigned long long)g_stats.animUsed);
  if (g_stats.suspects) Con("  ignored %llu one-tick demo tick glitches\n", (unsigned long long)g_stats.suspects);
  if (!g_physCvar) Con("  note: cl_phys_timescale could not be found, assuming 1.0\n");
}

bool ParseInt(const char* s, int lo, int hi, int& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  long v = strtol(s, &end, 10);
  if (*end || v < lo || v > hi) return false;
  out = (int)v;
  return true;
}

#include "Insight.inc"

// Installed hooks: the insight recorder observes before and after each hooked game call; the addon logic runs in between.
void Hk_Frame(void* self, void* ev, void* a3, void* a4) {
  InsNoteFrame(self);
  InsEvent(kEvFramePre);
  InsWindow(true);  // open the access window: journalled pages go NOACCESS so the game's reads of them are caught too
  FrameHooked(self, ev, a3, a4);
  InsWindow(false);
  InsEvent(kEvFramePost);
}

void Hk_AnimTick(void* self, void* ev, void* a3, void* a4) {
  InsEvent(kEvAnimPre);
  InsWindow(true);
  AnimTickHooked(self, ev, a3, a4);
  InsWindow(false);
  InsEvent(kEvAnimPost);
}

void* Hk_Step(void* self, void** worlds, int count, float dt, int substeps, bool b, void* p) {
  InsNoteStep(worlds, count, dt, substeps);
  InsEvent(kEvStepPre);
  void* r = StepHooked(self, worlds, count, dt, substeps, b, p);
  InsEvent(kEvStepPost);
  return r;
}

void RunCommand(int argc, const char** argv) {
  if (argc < 2 || !_stricmp(argv[1], "status")) {
    PrintStatus();
    if (argc < 2) PrintHelp();
    return;
  }
  const char* sub = argv[1];
  const char* val = argc >= 3 ? argv[2] : nullptr;
  int n = 0;
  if (!_stricmp(sub, "help")) {
    PrintHelp();
  } else if (!_stricmp(sub, "mode")) {
    if (val && !_stricmp(val, "off")) n = 0, val = "0";
    if (val && !_stricmp(val, "freeze")) n = 1, val = "1";
    if (val && !_stricmp(val, "clock")) n = 2, val = "2";
    if (!ParseInt(val, 0, 2, n)) return Con("usage: ragdollfix mode <0|1|2>   (current %d: %s)\n", g_mode.load(), ModeName(g_mode.load()));
    g_mode = n;
    ++g_epoch;
    Con("mode %d: %s\n", n, ModeName(n));
  } else if (!_stricmp(sub, "catchup")) {
    if (!ParseInt(val, 1, 16, n)) return Con("usage: ragdollfix catchup <1-16>   (current %d)\n", g_maxSteps.load());
    g_maxSteps = n;
    Con("catchup %d\n", n);
  } else if (!_stricmp(sub, "resync")) {
    if (!ParseInt(val, 2, 6400, n)) return Con("usage: ragdollfix resync <2-6400>   (current %d)\n", g_resyncTicks.load());
    g_resyncTicks = n;
    ++g_epoch;
    Con("resync %d ticks\n", n);
  } else if (!_stricmp(sub, "settle")) {
    if (!ParseInt(val, 0, 1280, n)) return Con("usage: ragdollfix settle <0-1280>   (current %d)\n", g_settleMax.load());
    g_settleMax = n;
    Con("settle %d ticks%s\n", n, n ? "" : " (off)");
  } else if (!_stricmp(sub, "settlemode")) {
    if (val && !_stricmp(val, "newest"))
      g_settleAll = false;
    else if (val && !_stricmp(val, "all"))
      g_settleAll = true;
    else
      return Con("usage: ragdollfix settlemode <newest|all>   (current %s)\n", g_settleAll.load() ? "all" : "newest");
    Con("settlemode %s\n", g_settleAll.load() ? "all" : "newest");
  } else if (!_stricmp(sub, "settlechunk")) {
    if (!ParseInt(val, 0, kSettleChunkMax, n))
      return Con("usage: ragdollfix settlechunk <0-%d>   (current %d)\n", kSettleChunkMax, g_settleChunk.load());
    g_settleChunk = n;
    Con("settlechunk %d%s\n", n, n ? " ticks per frame (lockstep with animation)" : " (one burst, no animation time)");
  } else if (!_stricmp(sub, "animcatchup")) {
    if (!ParseInt(val, 0, 1, n)) return Con("usage: ragdollfix animcatchup <0|1>   (current %d)\n", g_catchup.load());
    g_catchup = n;
    Con("animcatchup %d%s\n", n, n ? " (one animation update catches rebuilt ragdolls up while paused)" : " (physics settle only)");
    if (n && !AnimTimeAvailable())
      Con("note: the per-entity animation time was not found in this CS2 build - the catch-up cannot run\n");
  } else if (!_stricmp(sub, "posecopy")) {
    if (!ParseInt(val, 0, 1, n)) return Con("usage: ragdollfix posecopy <0|1>   (current %d)\n", g_poseCopy.load());
    g_poseCopy = n;
    Con("posecopy %d%s\n", n, n ? " (settled ragdolls get their pose while paused)" : " (off: the pose appears when playback resumes)");
    if (n && !AnimTimeAvailable())
      Con("note: the per-entity animation time was not found in this CS2 build - the pose copy cannot run\n");
  } else if (!_stricmp(sub, "show")) {
    if (!ParseInt(val, 0, kShowMax, n))
      return Con("usage: ragdollfix show <0-%d>   (current %d: %s)\n", kShowMax, g_show.load(), ShowName(g_show.load()));
    g_show = n;
    Con("show %d: %s\n", n, ShowName(n));
    if (ShowRunsTick(n) && !o_animTick) Con("note: the animation update was not found in this CS2 build - it will not run\n");
    if (ShowAdvancesAnim(n) && !AnimTimeAvailable()) Con("note: animation time is not available in this CS2 build - show 7 acts as show 5\n");
  } else if (!_stricmp(sub, "ticksite")) {
    if (val && !_stricmp(val, "step")) {
      g_tickSite = kSiteStep;
    } else if (val && !_stricmp(val, "frame")) {
      if (!o_frame) return Con("ticksite frame is unavailable: the frame boundary event was not found in this CS2 build\n");
      g_tickSite = kSiteFrame;
    } else {
      return Con("usage: ragdollfix ticksite <step|frame>   (current %s)\n", TickSiteName(g_tickSite.load()));
    }
    Con("ticksite %s\n", TickSiteName(g_tickSite.load()));
  } else if (!_stricmp(sub, "verbose")) {
    if (!ParseInt(val, 0, 1, n)) return Con("usage: ragdollfix verbose <0|1>   (current %d)\n", (int)g_verbose.load());
    g_verbose = n != 0;
    Con("verbose %d\n", n);
  } else if (!_stricmp(sub, "trace")) {
    if (!ParseInt(val, 0, 1, n)) return Con("usage: ragdollfix trace <0|1>   (current %d)\n", (int)g_traceOn.load());
    if (n) {
      TraceOpen();
      if (g_traceOn)
        Con("trace on -> %s\n", g_tracePath);
      else
        Con("trace: could not create %s\n", g_tracePath);
    } else {
      TraceClose();
      Con("trace off, file: %s\n", g_tracePath[0] ? g_tracePath : "(none)");
    }
  } else if (!_stricmp(sub, "insight")) {
    InsCommand(argc - 1, argv + 1);
  } else if (!_stricmp(sub, "watch") || !_stricmp(sub, "forensic")) {
    const char* fwd[8] = {"insight", sub};  // short aliases for 'ragdollfix insight watch/forensic ...'
    int n = 2;
    for (int i = 2; i < argc && n < 8; ++i) fwd[n++] = argv[i];
    InsCommand(n, fwd);
  } else {
    Con("unknown option '%s'\n", sub);
    PrintHelp();
  }
}

int ReadCommandArgs(void* ccommand, const char** argv, int cap) {
  __try {
    const int argc = *(int*)((uintptr_t)ccommand + kCommandArgc);
    if (argc < 0 || argc > 512) return -1;
    const char* const* av = *(const char***)((uintptr_t)ccommand + kCommandArgv);
    const int n = argc < cap ? argc : cap;
    for (int i = 0; i < n; ++i) argv[i] = av[i] ? av[i] : "";
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

// ICommandCallback: vtable slot 0 = CommandCallback(this, context, const CCommand&).
void CommandCallback(void*, void*, void* ccommand) {
  const char* argv[16];
  const int argc = ReadCommandArgs(ccommand, argv, 16);
  RunCommand(argc < 0 ? 1 : argc, argv);
}
void CommandCallbackUnused(void*) {}

void* g_cmdCallbackVtable[4] = {(void*)&CommandCallback, (void*)&CommandCallbackUnused, (void*)&CommandCallbackUnused,
                                (void*)&CommandCallbackUnused};
struct {
  void** vtable;
} g_cmdCallback = {g_cmdCallbackVtable};

// Console command descriptor as passed to RegisterConCommand (must stay alive).
struct ConCommandDesc {
  const char* name;
  const char* help;
  int64_t flags;
  void* callback;
  uint64_t u20, u28, u30, u38;
};
ConCommandDesc g_cmdDesc = {"ragdollfix", "RagdollDemoClock: demo ragdoll physics follow demo time. 'ragdollfix' for status and options.",
                            0, &g_cmdCallback, 0x101, 0, 1, 0xffff};

uint64_t g_cmdHandle = 0;

void SetupConsoleRaw() {
  __try {
    int64_t cv = -1;
    ((void* (*)(void*, int64_t*, const char*, int))Vt(g_cvar)[kCvarFindConVar])(g_cvar, &cv, "cl_phys_timescale", 0);
    if ((uint32_t)cv != 0xffffffffu) {
      auto data = (uintptr_t)((void* (*)(void*, int))Vt(g_cvar)[kCvarGetCvar])(g_cvar, (int)(uint32_t)cv);
      uintptr_t name = 0;
      if (data && Rd(data, name) && SafeStrEq(name, "cl_phys_timescale")) g_physCvar = data;
    }
    // Returns the command handle through the hidden out pointer; index 0xffff = registration failed.
    g_cmdHandle = ~0ull;
    ((void* (*)(void*, uint64_t*, ConCommandDesc*, int64_t))Vt(g_cvar)[kCvarRegisterConCommand])(g_cvar, &g_cmdHandle, &g_cmdDesc, 0);
    g_cmdRegistered = (g_cmdHandle & 0xffff) != 0xffff;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    g_cmdRegistered = false;
  }
}

// ---------------------------------------------------------------------------------------------------------- init

DWORD WINAPI InitThread(LPVOID) {
  const char* mods[] = {"tier0.dll", "engine2.dll", "vphysics2.dll", "client.dll"};
  for (int i = 0;; ++i) {
    bool all = true;
    for (const char* m : mods) all &= GetModuleHandleA(m) != nullptr;
    if (all) break;
    if (i > 480) return 0;  // not CS2, stay inert
    Sleep(250);
  }
  g_Msg = (MsgFn)GetProcAddress(GetModuleHandleA("tier0.dll"), "Msg");
  HMODULE client = GetModuleHandleA("client.dll"), vphys = GetModuleHandleA("vphysics2.dll");
  g_engine2 = GetModuleHandleA("engine2.dll");

  Con("v%s loading\n", kVersion);
  g_buildVerified = Stamp(client) == kClientStamp && Stamp(g_engine2) == kEngine2Stamp && Stamp(vphys) == kVphysics2Stamp;
  if (!g_buildVerified)
    Con("CS2 build differs from the tested one (client %08x/%08x, engine2 %08x/%08x, vphysics2 %08x/%08x) - checking structure\n",
        Stamp(client), kClientStamp, Stamp(g_engine2), kEngine2Stamp, Stamp(vphys), kVphysics2Stamp);

  g_engine = Iface("engine2.dll", "Source2EngineToClient001");
  uintptr_t engVt = 0;
  const bool engineOk = g_engine && Rd((uintptr_t)g_engine, engVt) && ImageRange(g_engine2).has(engVt) &&
                        SlotsInText(engVt, {0, kEngIsPlayingDemo, kEngGetDemoFile}, SectionRange(g_engine2, ".text"));

  void* phys = Iface("vphysics2.dll", "VPhysics2_Interface_001");
  uintptr_t physVt = 0, stepFn = 0;
  const bool physOk = phys && Rd((uintptr_t)phys, physVt) && ImageRange(vphys).has(physVt) &&
                      Rd(physVt + kPhysStepWorlds * 8, stepFn) && stepFn;
  const bool stepForeign = physOk && !SectionRange(vphys, ".text").has(stepFn);

  if (uintptr_t hit = FindSig(client, "40 53 48 83 EC 20 0F B6 D9 BA FF FF FF FF 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 0B 48 8B 05 ?? "
                                      "?? ?? ?? 48 8B 40 08 80 38 00 75 16 84 DB 75 12 48 8B 05 ?? ?? ?? ?? F3 0F 10 40 34")) {
    int32_t d = 0;
    if (Rd(hit + 0x36, d)) g_globalsVar = hit + 0x3a + d;
  }
  uintptr_t globalsProbe = 0;
  const bool globalsOk = g_globalsVar && ImageRange(client).has(g_globalsVar) && Rd(g_globalsVar, globalsProbe);

  ResolveSettle(client);
  Con("self-check: demo interface %s | physics step %s | client globals %s | cl_phys_timescale %s | console command %s | seek settle %s\n",
      engineOk ? "ok" : "FAIL", physOk ? "ok" : "FAIL", globalsOk ? "ok" : "FAIL", g_physCvar ? "ok" : "missing (assume 1.0)",
      g_cmdRegistered ? "ok" : "FAIL (hotkeys: RIGHT CTRL+F11 cycle mode, RIGHT CTRL+F12 status)", g_settleSrc);
  g_animVt = FindVtableByRtti(client, kAnimSysRtti);
  uintptr_t animFn = 0;
  const bool animOk =
      g_animVt && Rd(g_animVt + kAnimTickSlot * 8, animFn) && g_clientText.has(animFn) && MatchSig(animFn, kAnimTickThunkSig);
  const uintptr_t physSysVt = FindVtableByRtti(client, kPhysSysRtti);
  uintptr_t frameFn = 0;
  const bool frameOk =
      physSysVt && Rd(physSysVt + kFrameBoundarySlot * 8, frameFn) && g_clientText.has(frameFn) && MatchSig(frameFn, kFrameBoundarySig);
  Con("self-check: animation update %s | frame boundary %s | animation time %s (controller %s, last tick +0x%x)\n",
      animOk ? "ok" : "not found (paused refresh falls back to history rebuild)", frameOk ? "ok" : "not found (ticksite frame unavailable)",
      animOk && g_animCtrl && g_offLastAnimTick > 0 ? "ok" : "not found (show 7 falls back to show 5)", g_animCtrl ? "ok" : "missing",
      (unsigned)(g_offLastAnimTick > 0 ? g_offLastAnimTick : 0));

  if (!engineOk) return Disable("the engine demo interface is not where expected"), 0;
  if (!physOk) return Disable("the client physics step interface is not where expected"), 0;
  if (!globalsOk) return Disable("the client globals signature was not found"), 0;
  if (stepForeign) {
    char owner[MAX_PATH];
    OwnerModuleName(stepFn, owner, sizeof(owner));
    Con("WARNING: the physics step is already hooked by %s. Do not load RagdollProbe together with this addon (restart CS2).\n", owner);
  }

  g_health = kReady;
  // The addon calls o_step for every physics step, including its own catch-up steps; the recorder counts them on the way through.
  g_insStepReal = (StepFn)stepFn;
  o_step = &Ins_CountingStep;
  if (!PatchPtr((void**)(physVt + kPhysStepWorlds * 8), &Hk_Step, nullptr)) {
    Disable("could not install the physics step hook");
    return 0;
  }
  if (animOk && !PatchPtr((void**)(g_animVt + kAnimTickSlot * 8), &Hk_AnimTick, (void**)&o_animTick)) o_animTick = nullptr;
  if (frameOk && !PatchPtr((void**)(physSysVt + kFrameBoundarySlot * 8), &Hk_Frame, (void**)&o_frame)) o_frame = nullptr;
  Con("ACTIVE - mode %d: %s. Works during demo playback only. Type 'ragdollfix' for status and options.\n", g_mode.load(),
      ModeName(g_mode.load()));
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    g_self = h;
    DisableThreadLibraryCalls(h);
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)h, &pinned);
    InitializeCriticalSection(&g_traceCs);
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpc0);
    // mirv_loadlibrary calls LoadLibraryW from inside a console command, i.e. on CS2's main thread. Registering the console
    // command right here keeps it off other threads, so it cannot race the game's own command list.
    if (GetModuleHandleA("tier0.dll")) {
      g_cvar = Iface("tier0.dll", "VEngineCvar007");
      if (g_cvar) SetupConsoleRaw();
    }
    if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
  }
  return TRUE;
}
