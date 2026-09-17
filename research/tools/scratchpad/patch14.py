import io, sys
p = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\src\RagdollDemoClock.cpp'
s = io.open(p, encoding='utf-8', newline='').read()
orig = s
def rep(old, new, why):
    global s
    if s.count(old) != 1:
        print('FAIL (%d hits): %s' % (s.count(old), why)); sys.exit(1)
    s = s.replace(old, new)
    print('ok:', why)

rep('constexpr const char* kVersion = "1.3.0";', 'constexpr const char* kVersion = "1.4.0";', 'version')

rep('''std::atomic<int> g_settleChunk{4};
constexpr int kSettleChunkMax = 16;''',
'''std::atomic<int> g_settleChunk{4};
constexpr int kSettleChunkMax = 16;
// Paused seek catch-up (v1.4): one animation update that hands each rebuilt ragdoll its real age, run while the demo is still
// paused, instead of settling it with physics steps. See CatchUpAnim for why that is the update that actually moves a ragdoll.
std::atomic<int> g_catchup{1};
constexpr long kCatchMinTicks = 1;        // animation ticks: an update with 0 elapsed does nothing at all
constexpr long kCatchMaxTicks = 4096;     // a ragdoll is long at rest by then; keeps a bad death time from asking for millions''',
 'catchup settings')

rep('std::atomic<int> g_show{kShowAnimTime};   // paused pose refresh strategy',
'''// Paused pose refresh. Show 7 gives ragdolls animation time, which v1.4 measured to be a no-op: while paused the game passes its
// animation tick event with the active byte clear, so the update it would feed never runs. The catch-up runs its own active update.
std::atomic<int> g_show{kShowAnim};       // paused pose refresh strategy''',
 'default show')

rep('''  bool lockBehind = false;  // behind its death age on the last settle check: receives lockstep animation time
};''',
'''  bool lockBehind = false;  // behind its death age on the last settle check: receives lockstep animation time
  int32_t caughtTick = 0;   // engine tick on which the addon ran its catch-up animation update for this ragdoll (0 = none)
};''',
 'PawnSlot.caughtTick')

rep('  uint64_t lockSettles = 0, lockChunks = 0, lockNoAnim = 0, lockCancels = 0;',
    '''  uint64_t lockSettles = 0, lockChunks = 0, lockNoAnim = 0, lockCancels = 0;
  uint64_t catchRuns = 0, catchPawns = 0, catchFaults = 0, catchMisses = 0;''',
    'catch stats')

rep('''void RunShow(const DemoView& v, int strategy, const char* armedWhy, const char* site);
bool PoseCentroid(uintptr_t ent, float out[3]);''',
'''void RunShow(const DemoView& v, int strategy, const char* armedWhy, const char* site);
bool PoseCentroid(uintptr_t ent, float out[3]);
int CatchUpAnim(const DemoView& v, double cap);
void InsMark(const char* fmt, ...);  // insight recorder mark (Insight.inc, included further down)''',
 'forward decls')
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('written, delta %+d bytes' % (len(s) - len(orig)))
