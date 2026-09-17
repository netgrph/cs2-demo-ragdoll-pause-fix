// RagdollPauseFix - HLAE addon for Counter-Strike 2 demos: ragdolls stay still while the demo is paused.
//
// The problem: CS2 keeps simulating client physics (player ragdolls, debris) while a demo is paused. The picture looks frozen, but
// the physics bodies keep falling in the background, so when playback resumes the ragdoll snaps to where the physics took it.
//
// What this DLL does: CS2 steps all client physics worlds once per engine tick (VPhysics2_Interface_001 vtable slot 16), whether or
// not demo time moves. The hook keeps a physics clock P, measured in demo ticks, and lets a step through only when the demo clock
// T = demo tick + tick interpolation fraction has advanced far enough to cover it. A paused demo gets no steps. Demo speed, tick
// stepping and cl_phys_timescale keep working exactly like in vanilla CS2.
//
// Known limit: after a seek across a death (the player dies between the old and the new tick) the ragdoll still moves when playback
// resumes, because its bones only take their pose from the physics bodies when demo time really advances. See research/PAPER.md.
//
// Loading: HLAE -> Tools -> Developer -> Custom Loader, with AfxHookSource2.dll first and this DLL second in "DLLs to inject".
// Or, in a CS2 already started through HLAE: mirv_loadlibrary "<folder>\RagdollPauseFix.dll". Then type mirv_ragdollfix.

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <initializer_list>

namespace {

constexpr const char* kVersion = "1.0.0";
constexpr const char* kCommandName = "mirv_ragdollfix";

// Builds this fix was tested on (PE TimeDateStamp). Other builds run only if the structural self-check passes.
constexpr uint32_t kClientStamp = 0x6aa1ae5e;
constexpr uint32_t kEngine2Stamp = 0x6aa1ae4f;
constexpr uint32_t kVphysics2Stamp = 0x6aa1add2;

// Game layout (engine2/client/vphysics2 from our own analysis, cvar system as used by HLAE 2.192.2).
constexpr int kEngIsPlayingDemo = 42, kEngGetDemoFile = 69;  // Source2EngineToClient001
constexpr int kDemoGetTick = 3, kDemoIsPaused = 12;           // demo file object returned by GetDemoFile
constexpr int kPhysStepWorlds = 16;                           // VPhysics2_Interface_001
constexpr int kCvarFindConVar = 11, kCvarGetCvar = 41, kCvarRegisterConCommand = 42;  // VEngineCvar007
constexpr uintptr_t kGlobalsInterpFrac = 0x38;  // client globals: tick interpolation fraction, wraps when the demo tick increments
constexpr uintptr_t kCvarValue = 0x58;          // convar data: float value
constexpr uintptr_t kCommandArgc = 0x438, kCommandArgv = 0x440;  // CCommand
constexpr double kTicksPerSecond = 64.0;

// client.dll: function that reads the globals pointer (rel32 at +0x36, next instruction at +0x3a).
constexpr const char* kSigGlobals = "40 53 48 83 EC 20 0F B6 D9 BA FF FF FF FF 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 0B 48 8B 05 ?? "
                                    "?? ?? ?? 48 8B 40 08 80 38 00 75 16 84 DB 75 12 48 8B 05 ?? ?? ?? ?? F3 0F 10 40 34";

// ---------------------------------------------------------------------------------------------------------- output

HMODULE g_self = nullptr;
using MsgFn = void (*)(const char*, ...);
MsgFn g_Msg = nullptr;

// Prints to the CS2 console. Messages must end with '\n'.
void Con(const char* fmt, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (g_Msg) g_Msg("[RagdollPauseFix] %s", buf);
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

// Address stored in a RIP-relative instruction found by a signature: rel32 at hit + relAt, relative to hit + nextAt.
uintptr_t SigTarget(HMODULE m, const char* sig, uintptr_t relAt, uintptr_t nextAt) {
  const uintptr_t hit = FindSig(m, sig);
  int32_t d = 0;
  return hit && Rd(hit + relAt, d) ? hit + nextAt + d : 0;
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

bool IsExecutable(uintptr_t a) {
  MEMORY_BASIC_INFORMATION mbi{};
  if (a < 0x10000 || !VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
  return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// Writes a function pointer (vtable slot). *orig receives the previous value before the slot changes.
bool PatchPtr(void** where, void* fn, void** orig) {
  DWORD old;
  if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) return false;
  if (orig) *orig = *where;
  *where = fn;
  VirtualProtect(where, sizeof(void*), old, &old);
  return true;
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

std::atomic<bool> g_on{true};          // false = vanilla CS2 physics
std::atomic<int> g_maxSteps{4};        // physics steps allowed per engine tick while catching up
std::atomic<int> g_resyncTicks{16};    // demo time jumps larger than this re-sync the physics clock instead of fast-forwarding
std::atomic<bool> g_verbose{false};
std::atomic<int> g_epoch{0};           // bumped when settings change; resets the physics clock

constexpr int kSuspectJumpTicks = 4096;  // a demo tick jump this large is only trusted once the next engine tick confirms it

enum Health { kInit = 0, kReady = 1, kDisabled = 2 };
std::atomic<int> g_health{kInit};
std::atomic<bool> g_buildVerified{false};
std::atomic<const char*> g_waitingFor{"tier0.dll"};
char g_disableReason[256] = "";

void Disable(const char* why) {
  if (g_health.exchange(kDisabled) == kDisabled) return;
  snprintf(g_disableReason, sizeof(g_disableReason), "%s", why);
  Con("DISABLED: %s\n", why);
  Con("CS2 physics now run unmodified. The game was probably updated and this version of the fix needs an update.\n");
}

// ---------------------------------------------------------------------------------------------------------- game access

void* g_engine = nullptr;               // Source2EngineToClient001
HMODULE g_engine2 = nullptr;
uintptr_t g_globalsVar = 0;             // address of client's globals pointer
std::atomic<void*> g_cvar{nullptr};     // VEngineCvar007
std::atomic<uintptr_t> g_physCvar{0};   // cl_phys_timescale convar data
uintptr_t g_checkedDemoVt = 0;
bool g_lateLoad = false;                // loaded into a running CS2 (mirv_loadlibrary) instead of at process start

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
  const uintptr_t cv = g_physCvar.load(std::memory_order_relaxed);
  if (cv && Rd(cv + kCvarValue, v) && std::isfinite(v) && v >= 0.0f && v <= 1000.0f) return v;
  return 1.0f;
}

// ---------------------------------------------------------------------------------------------------------- console command

void RunCommand(int argc, const char** argv);

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
  const char* argv[16] = {kCommandName};
  const int argc = ReadCommandArgs(ccommand, argv, 16);
  RunCommand(argc < 1 ? 1 : argc, argv);
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
ConCommandDesc g_cmdDesc = {kCommandName,
                            "RagdollPauseFix: demo ragdolls stay still while the demo is paused. Type mirv_ragdollfix for status and options.",
                            0, &g_cmdCallback, 0x101, 0, 1, 0xffff};

using RegisterFn = void* (*)(void*, uint64_t*, ConCommandDesc*, int64_t);

std::atomic<bool> g_cmdRegistered{false};
std::atomic<bool> g_cmdBusy{false};
std::atomic<const char*> g_cmdWhere{""};
std::atomic<int> g_hookTries{0}, g_stepTries{0}, g_lateTries{0};
constexpr int kRegisterTries = 8;

// Returns the command handle through the hidden out pointer; index 0xffff = registration failed.
bool RegisterCommandRaw(RegisterFn fn, void* cvar) {
  __try {
    uint64_t handle = ~0ull;
    fn(cvar, &handle, &g_cmdDesc, 0);
    return (handle & 0xffff) != 0xffff;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Registers mirv_ragdollfix once. Only called on a thread where the game itself uses the cvar system at that moment.
bool TryRegister(RegisterFn fn, const char* where, std::atomic<int>& tries) {
  if (g_cmdRegistered.load()) return true;
  void* cvar = g_cvar.load();
  if (!fn || !cvar || tries.load() >= kRegisterTries || g_cmdBusy.exchange(true)) return false;
  bool ok = g_cmdRegistered.load();
  if (!ok) {
    ++tries;
    ok = RegisterCommandRaw(fn, cvar);
    if (ok) {
      g_cmdWhere = where;
      g_cmdRegistered = true;
    }
  }
  g_cmdBusy = false;
  return ok;
}

// Injected at process start, the DLL is loaded long before the game registers its console commands. The game registers them
// through the cvar system's RegisterConCommand (vtable slot 42) while it starts; the hook registers mirv_ragdollfix right after one
// of the client's own commands, on the same thread, then removes itself.
std::atomic<RegisterFn> o_register{nullptr};
std::atomic<void**> g_registerSlot{nullptr};

void RemoveRegisterHook();

void* Hk_Register(void* self, uint64_t* out, ConCommandDesc* desc, int64_t flags) {
  const RegisterFn orig = o_register.load(std::memory_order_relaxed);
  void* r = orig(self, out, desc, flags);
  if (!g_cmdRegistered.load(std::memory_order_relaxed) && self == g_cvar.load(std::memory_order_relaxed) &&
      GetModuleHandleA("client.dll") && TryRegister(orig, "while CS2 started", g_hookTries))
    RemoveRegisterHook();
  return r;
}

bool InstallRegisterHook(void* cvar) {
  void** slot = &Vt(cvar)[kCvarRegisterConCommand];
  const auto cur = (RegisterFn)*slot;
  if (!cur || cur == &Hk_Register) return false;
  o_register = cur;
  if (!PatchPtr(slot, (void*)&Hk_Register, nullptr)) return false;
  g_registerSlot = slot;
  return true;
}

void RemoveRegisterHook() {
  void** slot = g_registerSlot.exchange(nullptr);
  if (!slot || *slot != (void*)&Hk_Register) return;  // someone hooked the slot after us: leave the chain alone
  DWORD old;
  if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
  InterlockedCompareExchangePointer(slot, (void*)o_register.load(), (void*)&Hk_Register);
  VirtualProtect(slot, sizeof(void*), old, &old);
}

bool CvarUsable(void* cvar, HMODULE tier0) {
  uintptr_t vt = 0;
  if (!cvar || !Rd((uintptr_t)cvar, vt) || !ImageRange(tier0).has(vt)) return false;
  for (int s : {kCvarFindConVar, kCvarGetCvar, kCvarRegisterConCommand}) {
    uintptr_t fn = 0;
    if (!Rd(vt + (uintptr_t)s * 8, fn) || !IsExecutable(fn)) return false;
  }
  return true;
}

uintptr_t FindPhysCvarRaw(void* cvar) {
  __try {
    int64_t h = -1;
    ((void* (*)(void*, int64_t*, const char*, int))Vt(cvar)[kCvarFindConVar])(cvar, &h, "cl_phys_timescale", 0);
    if ((uint32_t)h == 0xffffffffu) return 0;
    const auto data = (uintptr_t)((void* (*)(void*, int))Vt(cvar)[kCvarGetCvar])(cvar, (int)(uint32_t)h);
    uintptr_t name = 0;
    return data && Rd(data, name) && SafeStrEq(name, "cl_phys_timescale") ? data : 0;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

// Main thread only (the game's own physics step).
void FindPhysCvar() {
  if (void* cvar = g_cvar.load(std::memory_order_relaxed))
    if (uintptr_t data = FindPhysCvarRaw(cvar)) g_physCvar = data;
}

// ---------------------------------------------------------------------------------------------------------- physics clock

using StepFn = void* (*)(void*, void**, int, float, int, bool, void*);
StepFn o_step = nullptr;

// Only touched from the physics step (game main thread); the console command reads it for status output.
struct ClockState {
  int64_t calls = 0;
  int lastTick = -1;
  bool suspect = false;  // the previous engine tick saw a huge demo tick jump that is not trusted yet
  int epoch = -1;
  bool valid = false;
  double P = 0.0;  // physics clock in demo ticks: demo time the ragdoll simulation has reached
  bool playing = false, paused = false;
  uint64_t heldWhilePaused = 0;
  DemoView last;
  double T = 0.0, stepTicks = 0.0;
  float phys = 1.0f, dt = 0.0f;
};
ClockState g_clk;

struct Stats {
  uint64_t passed = 0, gated = 0, extra = 0, resyncs = 0, suspects = 0;
} g_stats;

void NoteTransitions(const DemoView& v) {
  const bool verbose = g_verbose.load(std::memory_order_relaxed);
  if (v.playing != g_clk.playing) {
    if (v.playing)
      Con("demo playback detected - ragdoll physics follow demo time\n");
    else if (verbose)
      Con("demo playback ended\n");
  }
  const bool paused = v.playing && v.paused;
  if (paused && !g_clk.paused) {
    g_clk.heldWhilePaused = 0;
    if (verbose) Con("paused at tick %d - ragdoll physics frozen\n", v.tick);
  } else if (!paused && g_clk.paused && v.playing) {
    if (verbose)
      Con("resumed at tick %d - held back %llu physics steps while paused\n", v.tick, (unsigned long long)g_clk.heldWhilePaused);
  }
  g_clk.playing = v.playing;
  g_clk.paused = paused;
  g_clk.last = v;
}

void* ClockStep(const DemoView& v, void* self, void** worlds, int count, float dt, int substeps, bool b, void* p) {
  ++g_clk.calls;
  // A single engine tick with a wildly different demo tick (seen right after seeks) must not re-sync everything twice.
  if (g_clk.valid && g_clk.lastTick >= 0 && std::abs(v.tick - g_clk.lastTick) > kSuspectJumpTicks && !g_clk.suspect) {
    g_clk.suspect = true;
    ++g_stats.gated;
    ++g_stats.suspects;
    return nullptr;
  }
  g_clk.suspect = false;
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
    ++g_stats.passed;
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

  if (!g_clk.valid) {
    g_clk.P = T - stepTicks;
    g_clk.valid = true;
  } else {
    const double e = g_clk.P - T;
    if (e < -resyncFwd || e > resyncBack) {
      g_clk.P = T - stepTicks;
      ++g_stats.resyncs;
      if (g_verbose.load(std::memory_order_relaxed))
        Con("demo time jumped %+.1f ticks (now tick %d) - physics clock re-synced\n", -e, v.tick);
    }
  }

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
  } else {
    ++g_stats.gated;
    if (paused) ++g_clk.heldWhilePaused;
  }
  return steps ? r : nullptr;
}

std::atomic<int> g_physCvarTries{0};

void* StepHooked(void* self, void** worlds, int count, float dt, int substeps, bool b, void* p) {
  // Fallbacks, on the game's main thread: the console command (if it could not be registered at startup) and cl_phys_timescale.
  if (!g_cmdRegistered.load(std::memory_order_relaxed) && (g_physCvarTries.load(std::memory_order_relaxed) & 63) == 0)
    if (void* cvar = g_cvar.load(std::memory_order_relaxed)) {
      const RegisterFn orig = o_register.load(std::memory_order_relaxed);
      if (TryRegister(orig ? orig : (RegisterFn)Vt(cvar)[kCvarRegisterConCommand], "on the first physics step", g_stepTries))
        RemoveRegisterHook();
    }
  if (!g_physCvar.load(std::memory_order_relaxed) && (g_physCvarTries++ & 63) == 0) FindPhysCvar();

  if (g_health.load(std::memory_order_relaxed) != kReady) return o_step(self, worlds, count, dt, substeps, b, p);
  const int epoch = g_epoch.load(std::memory_order_relaxed);
  if (g_clk.epoch != epoch) {
    const bool playing = g_clk.playing, paused = g_clk.paused;
    g_clk = ClockState();
    g_clk.epoch = epoch;
    g_clk.playing = playing;
    g_clk.paused = paused;
  }
  if (!g_on.load(std::memory_order_relaxed)) {
    g_clk.valid = false;
    return o_step(self, worlds, count, dt, substeps, b, p);
  }
  DemoView v;
  if (!ReadDemo(v)) return o_step(self, worlds, count, dt, substeps, b, p);
  NoteTransitions(v);
  if (!v.playing || v.tick < 0) {
    g_clk.valid = false;
    return o_step(self, worlds, count, dt, substeps, b, p);
  }
  return ClockStep(v, self, worlds, count, dt, substeps, b, p);
}

// ---------------------------------------------------------------------------------------------------------- commands

void PrintHelp() {
  Con("commands:\n");
  Con("  %s                  status and this help\n", kCommandName);
  Con("  %s status           status only\n", kCommandName);
  Con("  %s on | off         on = ragdoll physics follow demo time (default), off = vanilla CS2\n", kCommandName);
  Con("  %s catchup <1-16>   max physics steps per engine tick when catching up after tick stepping (default 4)\n", kCommandName);
  Con("  %s resync <2-6400>  demo time jumps larger than this many ticks re-sync instead of fast-forwarding (default 16)\n",
      kCommandName);
  Con("  %s verbose <0|1>    print pause / resume / seek messages (default 0)\n", kCommandName);
}

void PrintStatus() {
  const int health = g_health.load();
  if (health == kInit) {
    Con("v%s starting up (waiting for %s) - try again in a moment\n", kVersion, g_waitingFor.load());
    return;
  }
  if (health == kDisabled) {
    Con("v%s DISABLED: %s\n", kVersion, g_disableReason);
    Con("  CS2 physics run unmodified. The game was probably updated and this version of the fix needs an update.\n");
    return;
  }
  const bool on = g_on.load();
  Con("v%s ACTIVE on a %s CS2 build - %s\n", kVersion, g_buildVerified.load() ? "tested" : "untested (self-check passed)",
      on ? "on (ragdoll physics follow demo time)" : "off (vanilla CS2 physics)");
  Con("  console command registered %s\n", g_cmdWhere.load());
  const DemoView& v = g_clk.last;
  if (!g_clk.playing) {
    Con("  demo: not playing (the fix only acts during demo playback)\n");
  } else {
    Con("  demo: %s at tick %d\n", v.paused ? "PAUSED" : "playing", v.tick);
    if (on && g_clk.valid)
      Con("  physics clock: %+.2f ticks from the demo, one physics step = %.3f demo ticks\n", g_clk.P - g_clk.T, g_clk.stepTicks);
  }
  if (g_physCvar.load())
    Con("  cl_phys_timescale: %.3f\n", PhysTimescale());
  else
    Con("  cl_phys_timescale: not found yet (assuming 1.0)\n");
  Con("  physics steps: %llu run (%llu extra catch-up), %llu held back, %llu re-syncs after seeks\n", (unsigned long long)g_stats.passed,
      (unsigned long long)g_stats.extra, (unsigned long long)g_stats.gated, (unsigned long long)g_stats.resyncs);
  Con("  settings: catchup %d, resync %d, verbose %d\n", g_maxSteps.load(), g_resyncTicks.load(), (int)g_verbose.load());
  if (g_stats.suspects) Con("  ignored %llu one-tick demo tick glitches\n", (unsigned long long)g_stats.suspects);
}

bool ParseInt(const char* s, int lo, int hi, int& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  long v = strtol(s, &end, 10);
  if (*end || v < lo || v > hi) return false;
  out = (int)v;
  return true;
}

void RunCommand(int argc, const char** argv) {
  if (argc < 2) {
    PrintStatus();
    PrintHelp();
    return;
  }
  const char* sub = argv[1];
  const char* val = argc >= 3 ? argv[2] : nullptr;
  int n = 0;
  if (!_stricmp(sub, "status")) {
    PrintStatus();
  } else if (!_stricmp(sub, "help")) {
    PrintHelp();
  } else if (!_stricmp(sub, "on") || !_stricmp(sub, "off")) {
    g_on = !_stricmp(sub, "on");
    ++g_epoch;
    Con("%s\n", g_on.load() ? "on - ragdoll physics follow demo time" : "off - vanilla CS2 physics");
  } else if (!_stricmp(sub, "catchup")) {
    if (!ParseInt(val, 1, 16, n)) return Con("usage: %s catchup <1-16>   (current %d)\n", kCommandName, g_maxSteps.load());
    g_maxSteps = n;
    Con("catchup %d\n", n);
  } else if (!_stricmp(sub, "resync")) {
    if (!ParseInt(val, 2, 6400, n)) return Con("usage: %s resync <2-6400>   (current %d)\n", kCommandName, g_resyncTicks.load());
    g_resyncTicks = n;
    ++g_epoch;
    Con("resync %d ticks\n", n);
  } else if (!_stricmp(sub, "verbose")) {
    if (!ParseInt(val, 0, 1, n)) return Con("usage: %s verbose <0|1>   (current %d)\n", kCommandName, (int)g_verbose.load());
    g_verbose = n != 0;
    Con("verbose %d\n", n);
  } else {
    Con("unknown option '%s'\n", sub);
    PrintHelp();
  }
}

// ---------------------------------------------------------------------------------------------------------- init

bool IsCs2Process() {
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
  if (!n || n >= MAX_PATH) return false;
  const wchar_t* slash = wcsrchr(path, L'\\');
  return _wcsicmp(slash ? slash + 1 : path, L"cs2.exe") == 0;
}

// Waits until the game has loaded a module. GetModuleHandle already succeeds while the module is still initialising on another
// thread; LoadLibrary on the same file returns only after that finished (the extra reference is kept, CS2 never unloads these).
HMODULE WaitForModule(const wchar_t* name, DWORD pollMs, const char* label) {
  g_waitingFor = label;
  HMODULE h = nullptr;
  while (!(h = GetModuleHandleW(name))) Sleep(pollMs);
  wchar_t path[MAX_PATH];
  const DWORD n = GetModuleFileNameW(h, path, MAX_PATH);
  if (n && n < MAX_PATH)
    if (HMODULE held = LoadLibraryW(path)) return held;
  return h;
}

void* WaitForInterface(const char* mod, const char* name, DWORD timeoutMs) {
  const ULONGLONG start = GetTickCount64();
  for (;;) {
    if (void* i = Iface(mod, name)) return i;
    if (GetTickCount64() - start > timeoutMs) return nullptr;
    Sleep(50);
  }
}

DWORD WINAPI InitThread(LPVOID) {
  // 1. tier0: console output and the cvar system. Injected at process start, this runs before CS2 has loaded any game module.
  HMODULE tier0 = WaitForModule(L"tier0.dll", 1, "tier0.dll");
  if (!g_Msg) g_Msg = (MsgFn)GetProcAddress(tier0, "Msg");
  if (!g_cvar.load()) {
    g_waitingFor = "the console command system";
    const ULONGLONG start = GetTickCount64();
    for (;;) {
      void* cv = Iface("tier0.dll", "VEngineCvar007");
      if (cv && CvarUsable(cv, tier0)) {
        g_cvar = cv;
        if (!g_cmdRegistered.load()) InstallRegisterHook(cv);
        break;
      }
      if (GetTickCount64() - start > 10 * 60 * 1000ull) break;
      Sleep(GetModuleHandleW(L"client.dll") ? 50 : 1);
    }
  }

  // 2. The game modules.
  g_engine2 = WaitForModule(L"engine2.dll", 10, "engine2.dll");
  HMODULE client = WaitForModule(L"client.dll", 10, "client.dll");
  HMODULE vphys = WaitForModule(L"vphysics2.dll", 10, "vphysics2.dll");
  g_waitingFor = "the game interfaces";

  Con("v%s loading\n", kVersion);
  g_buildVerified = Stamp(client) == kClientStamp && Stamp(g_engine2) == kEngine2Stamp && Stamp(vphys) == kVphysics2Stamp;
  if (!g_buildVerified)
    Con("this CS2 build differs from the tested one (client %08x, engine2 %08x, vphysics2 %08x) - checking the game structures\n",
        Stamp(client), Stamp(g_engine2), Stamp(vphys));

  g_engine = WaitForInterface("engine2.dll", "Source2EngineToClient001", 60000);
  uintptr_t engVt = 0;
  const bool engineOk = g_engine && Rd((uintptr_t)g_engine, engVt) && ImageRange(g_engine2).has(engVt) &&
                        SlotsInText(engVt, {0, kEngIsPlayingDemo, kEngGetDemoFile}, SectionRange(g_engine2, ".text"));

  void* phys = WaitForInterface("vphysics2.dll", "VPhysics2_Interface_001", 60000);
  uintptr_t physVt = 0, stepFn = 0;
  const bool physOk = phys && Rd((uintptr_t)phys, physVt) && ImageRange(vphys).has(physVt) &&
                      Rd(physVt + kPhysStepWorlds * 8, stepFn) && stepFn;
  const bool stepForeign = physOk && !SectionRange(vphys, ".text").has(stepFn);

  g_globalsVar = SigTarget(client, kSigGlobals, 0x36, 0x3a);
  uintptr_t globalsProbe = 0;
  const bool globalsOk = g_globalsVar && ImageRange(client).has(g_globalsVar) && Rd(g_globalsVar, globalsProbe);

  Con("self-check: demo interface %s | physics step %s | client globals %s | console command %s\n", engineOk ? "ok" : "FAIL",
      physOk ? "ok" : "FAIL", globalsOk ? "ok" : "FAIL",
      g_cmdRegistered.load() ? "ok" : g_cvar.load() ? "waiting for the game" : "FAIL (cvar system not found)");

  if (!engineOk) return Disable("the engine demo interface is not where expected"), 0;
  if (!physOk) return Disable("the client physics step interface is not where expected"), 0;
  if (!globalsOk) return Disable("the client globals signature was not found"), 0;
  if (stepForeign) {
    char owner[MAX_PATH];
    OwnerModuleName(stepFn, owner, sizeof(owner));
    Con("WARNING: the physics step is already hooked by %s. Load only one ragdoll fix (for example not RagdollDemoClock as well) "
        "and restart CS2.\n", owner);
  }

  // 3. The physics step hook.
  o_step = (StepFn)stepFn;
  g_health = kReady;
  if (!PatchPtr((void**)(physVt + kPhysStepWorlds * 8), (void*)&StepHooked, nullptr)) {
    Disable("could not install the physics step hook");
    return 0;
  }
  Con("v%s ACTIVE - demo ragdolls stay still while the demo is paused. Type %s in the console for status and options.\n", kVersion,
      kCommandName);
  if (g_lateLoad && !g_cmdRegistered.load())
    Con("note: the console command %s could not be registered yet; it is retried when a map or demo is loaded\n", kCommandName);
  return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
  if (reason != DLL_PROCESS_ATTACH) return TRUE;
  g_self = h;
  if (!IsCs2Process()) return TRUE;  // injected somewhere else: stay inert
  // One copy per process: a second copy (other folder, or loaded twice) stays inert.
  wchar_t mutexName[64];
  swprintf_s(mutexName, L"Local\\RagdollPauseFix_%lu", GetCurrentProcessId());
  HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName);
  if (!mutex) return TRUE;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    return TRUE;
  }
  // The hooks point into this DLL, so it must never be unloaded.
  HMODULE pinned = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, (LPCWSTR)h, &pinned);
  // mirv_loadlibrary into a running CS2 calls LoadLibrary from a console command, on the game's main thread: register right here.
  HMODULE tier0 = GetModuleHandleW(L"tier0.dll");
  if (tier0 && GetModuleHandleW(L"client.dll")) {
    g_lateLoad = true;
    g_Msg = (MsgFn)GetProcAddress(tier0, "Msg");
    void* cv = Iface("tier0.dll", "VEngineCvar007");
    if (cv && CvarUsable(cv, tier0)) {
      g_cvar = cv;
      TryRegister((RegisterFn)Vt(cv)[kCvarRegisterConCommand], "with mirv_loadlibrary", g_lateTries);
    }
  }
  if (HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr)) CloseHandle(t);
  return TRUE;
}
