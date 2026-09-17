import io, sys
BS = chr(92)
NL = BS + 'n'   # the two characters a C string literal uses
p = r'C:\.VSCODE\CS2_RagdollDemoFix\DemoClock\src\RagdollDemoClock.cpp'
s = io.open(p, encoding='utf-8', newline='').read()
def rep(old, new, why):
    global s
    old, new = old.replace('@NL@', NL), new.replace('@NL@', NL)
    if s.count(old) != 1:
        print('FAIL (%d hits): %s' % (s.count(old), why)); sys.exit(1)
    s = s.replace(old, new)
    print('ok:', why)

# fix the two string literals the heredoc broke
rep('''      Con("note: the animation update did not reach the rebuilt ragdolls - falling back to the physics settle
");''',
    '''      Con("note: the animation update did not reach the rebuilt ragdolls - falling back to the physics settle@NL@");''',
    'fix miss note')
rep('''    Con("%d rebuilt ragdoll(s) caught up with one animation update (%d demo ticks of animation time)
", used, age);''',
    '''    Con("%d rebuilt ragdoll(s) caught up with one animation update (%d demo ticks of animation time)@NL@", used, age);''',
    'fix verbose note')

# help, status, command
rep('''  Con("  ragdollfix settlechunk <0-%d> paused settle: demo ticks per frame, each after the same animation time (default 4, 0 = one burst)@NL@",
      kSettleChunkMax);''',
'''  Con("  ragdollfix settlechunk <0-%d> paused settle: demo ticks per frame, each after the same animation time (default 4, 0 = one burst)@NL@",
      kSettleChunkMax);
  Con("  ragdollfix animcatchup <0|1> paused seek: catch rebuilt ragdolls up with one animation update instead of physics steps (default 1)@NL@");''',
 'help line')

rep('''  if (g_settleOk)
    Con("  lockstep settle: settlechunk %d%s''',
'''  if (g_settleOk)
    Con("  paused catch-up: animcatchup %d%s | %llu catch-ups, %llu ragdolls, %llu without an update, %llu faults@NL@", g_catchup.load(),
        g_catchup.load() > 0 ? (AnimTimeAvailable() ? "" : " (animation time not found in this build)") : " (off, physics settle)",
        (unsigned long long)g_stats.catchRuns, (unsigned long long)g_stats.catchPawns, (unsigned long long)g_stats.catchMisses,
        (unsigned long long)g_stats.catchFaults);
  if (g_settleOk)
    Con("  lockstep settle: settlechunk %d%s''',
 'status line')

rep('''  } else if (!_stricmp(sub, "show")) {''',
'''  } else if (!_stricmp(sub, "animcatchup")) {
    if (!ParseInt(val, 0, 1, n)) return Con("usage: ragdollfix animcatchup <0|1>   (current %d)@NL@", g_catchup.load());
    g_catchup = n;
    Con("animcatchup %d%s@NL@", n, n ? " (one animation update catches rebuilt ragdolls up while paused)" : " (physics settle only)");
    if (n && !AnimTimeAvailable())
      Con("note: the per-entity animation time was not found in this CS2 build - the catch-up cannot run@NL@");
  } else if (!_stricmp(sub, "show")) {''',
 'command')
io.open(p, 'w', encoding='utf-8', newline='').write(s)
print('written')
