import io, sys
p = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\src\RagdollDemoClock.cpp'
s = io.open(p, encoding='utf-8', newline='').read()
def rep(old, new, why):
    global s
    if s.count(old) != 1:
        print('FAIL (%d hits): %s' % (s.count(old), why)); sys.exit(1)
    s = s.replace(old, new)
    print('ok:', why)

# ---- the catch-up itself, right after ForceAnimTick()
rep('''// One paused pose refresh: the animation update (show 5/6), then the per-ragdoll strategy. Writes its trace row.''',
'''// ------------------------------------------------------------------------------------------------ paused catch-up (v1.4)
// How a ragdoll actually moves: the game's animation job asks a helper for the ticks since the entity's last animation update (the
// helper stores the current tick in the entity's animation controller) and skips the graph and pose work when that is 0. The
// ragdoll's own simulation runs on that elapsed time, not on the physics step - one update with 50 ticks of elapsed time drops a
// corpse all the way to the floor. While a demo is paused the job never runs: the animation system's tick event arrives with its
// active byte clear. So a ragdoll a seek rebuilt keeps the pose of the moment it died, and the first update after the resume hands
// it every missed tick at once. That is the snap this addon exists to remove.
// This runs that catch-up while the demo is still paused: it writes each tracked ragdoll's real age (time since its death) into its
// controller and runs one active animation update, which is exactly the elapsed time resuming would have given it. The controller
// is left at the current tick, so the resume has nothing left to catch up on. Returns the ragdolls that took it. Main thread only.
int CatchUpAnim(const DemoView& v, double cap) {
  void* sys = g_animSys.load(std::memory_order_relaxed);
  uintptr_t gl = 0;
  int32_t tickcount = 0;
  if (!sys || !o_animTick || !AnimTimeAvailable() || !Rd(g_globalsVar, gl) || !gl || !Rd(gl + kGlobalsTickcount, tickcount) ||
      tickcount <= 0)
    return 0;
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
    if (!WriteInt32(c + g_offLastAnimTick, tickcount - int32_t(age))) {
      ++faults;
      continue;
    }
    ctrl[i] = c;
    was[i] = last;
    want[i] = tickcount - int32_t(age);
    if (first < 0) first = i;
    ++n;
  }
  g_stats.catchFaults += faults;
  if (!n) return 0;
  float pre[3] = {0, 0, 0}, post[3] = {0, 0, 0};
  PoseCentroid(g_clk.pawns[first].inst, pre);
  TraceFlush();  // a crash inside the game still leaves a complete trace
  const int ran = ForceAnimTickRaw(sys);
  if (ran < 0)
    ++g_stats.animFaults;
  else
    ++g_stats.animForced;
  // The job stores the current tick in every controller it updates, so a controller that still holds our value was not updated.
  int used = 0;
  for (int i = 0; i < g_clk.nPawns; ++i)
    if (ctrl[i] && Rd(ctrl[i] + g_offLastAnimTick, got[i]) && got[i] != want[i]) ++used;
  PoseCentroid(g_clk.pawns[first].inst, post);
  const int32_t age = tickcount - want[first];
  InsMark("CATCHUP tick %d globals %d pawns %d used %d faults %d ran %d #%d age %d last %d -> %d (wrote %d) pose %.2f %.2f %.2f -> "
          "%.2f %.2f %.2f", v.tick, tickcount, n, used, faults, ran, g_clk.pawns[first].idx, age, was[first], got[first], want[first],
          pre[0], pre[1], pre[2], post[0], post[1], post[2]);
  if (g_traceOn.load(std::memory_order_relaxed)) {
    char act[320];
    snprintf(act, sizeof(act),
             "catchup pawns %d used %d faults %d globals %d #%d age %d last %d -> %d pre %.2f %.2f %.2f pose %.2f %.2f %.2f", n, used,
             faults, tickcount, g_clk.pawns[first].idx, age, was[first], got[first], pre[0], pre[1], pre[2], post[0], post[1], post[2]);
    Trace(kModeClock, v, g_clk.T, g_clk.P, g_clk.P, g_clk.dt, 0, g_clk.phys, g_clk.stepTicks, 0, act);
  }
  if (!used) {
    // The update did not reach them: put the controllers back the way the game had them and let the physics settle have a go.
    for (int i = 0; i < g_clk.nPawns; ++i)
      if (ctrl[i]) WriteInt32(ctrl[i] + g_offLastAnimTick, was[i]);
    ++g_stats.catchMisses;
    static bool noted = false;
    if (!noted) {
      noted = true;
      Con("note: the animation update did not reach the rebuilt ragdolls - falling back to the physics settle\n");
    }
    return 0;
  }
  for (int i = 0; i < g_clk.nPawns; ++i) {
    if (!ctrl[i]) continue;
    g_clk.pawns[i].caughtTick = tickcount;
    g_clk.pawns[i].sim = double(tickcount - want[i]);  // it has the time now: the physics settle has nothing left to run
  }
  ++g_stats.catchRuns;
  g_stats.catchPawns += used;
  // The pose the update just produced still has to reach the renderer, or the paused frames keep showing the interpolated old one.
  g_clk.staleCall = g_clk.calls;
  g_clk.staleTick = v.tick;
  g_clk.staleWhy = "catchup";
  RunShow(v, kShowAnim, "catchup", "catchup");
  if (g_verbose.load(std::memory_order_relaxed))
    Con("%d rebuilt ragdoll(s) caught up with one animation update (%d demo ticks of animation time)\n", used, age);
  return used;
}

// One paused pose refresh: the animation update (show 5/6), then the per-ragdoll strategy. Writes its trace row.''',
 'CatchUpAnim')

# ---- trigger inside SettleStep
rep('''  const bool lock = LockstepActive(v);
  const long want = blocked || fill <= 0.0 ? 0 : std::lround(fill / g_clk.stepTicks);''',
'''  // Paused seek: one animation update with the ragdolls' real age puts them where resuming would, so there is nothing left to snap.
  if (v.paused && behind > 0 && !building && g_catchup.load(std::memory_order_relaxed) > 0 && CatchUpAnim(v, cap) > 0) {
    CancelLock("catchup");
    g_clk.settling = false;
    g_clk.lockRun = false;
    g_clk.lockBusy = false;
    return 0;
  }
  const bool lock = LockstepActive(v);
  const long want = blocked || fill <= 0.0 ? 0 : std::lround(fill / g_clk.stepTicks);''',
 'SettleStep trigger')
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('written')
