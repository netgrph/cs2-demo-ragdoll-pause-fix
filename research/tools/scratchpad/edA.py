import re
p = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\src\RagdollDemoClock.cpp'
s = open(p, encoding='utf-8').read()
def rep(a, b, cnt=1):
    global s
    n = s.count(a)
    assert n == cnt, (a[:60], n)
    s = s.replace(a, b)
rep('constexpr const char* kVersion = "1.5.0";', 'constexpr const char* kVersion = "1.6.0";')
rep('std::atomic<int> g_catchup{1};', 'std::atomic<int> g_catchup{0};')
rep(r'instead of physics steps (default 1)\n");', r'instead of physics steps (default 0)\n");')
rep('''    long age = std::lround(target);
    if (age < kCatchMinTicks) age = kCatchMinTicks;
    if (age > kCatchMaxTicks) age = kCatchMaxTicks;
    if (!WriteInt32(c + g_offLastAnimTick, tickcount - int32_t(age))) {
      ++faults;
      continue;
    }
    ctrl[i] = c;
    was[i] = last;
    want[i] = tickcount - int32_t(age);''', '''    long age = std::lround(target);
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
    want[i] = last - int32_t(age);''')
rep('''  // The job stores the current tick in every controller it updates, so a controller that still holds our value was not updated.
  int used = 0;
  for (int i = 0; i < g_clk.nPawns; ++i)
    if (ctrl[i] && Rd(ctrl[i] + g_offLastAnimTick, got[i]) && got[i] != want[i]) ++used;
  PoseCentroid(g_clk.pawns[first].inst, post);
  const int32_t age = tickcount - want[first];''', '''  // The job stores its tick in every controller it updates. Only a tick past our value is a real forward update: equal means the
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
  const int32_t age = was[first] - want[first];''')
rep('''    for (int i = 0; i < g_clk.nPawns; ++i)
      if (ctrl[i]) WriteInt32(ctrl[i] + g_offLastAnimTick, was[i]);
    ++g_stats.catchMisses;''', '''    ++g_stats.catchMisses;''')
rep('''    g_clk.pawns[i].sim = double(tickcount - want[i]);  // it has the time now: the physics settle has nothing left to run''',
    '''    g_clk.pawns[i].sim = double(was[i] - want[i]);  // it has the time now: the physics settle has nothing left to run''')
open(p, 'w', encoding='utf-8', newline='\n').write(s)
print('ok')
