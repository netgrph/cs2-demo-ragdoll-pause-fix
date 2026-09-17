// insight recorder, part 3: per-event recording, markers, console commands, map dump.

constexpr uint32_t kInsCensusEvery = 512;  // frames between routine censuses while recording

char g_insPath[MAX_PATH] = "";
char g_insLabel[48] = "";
StepFn g_insStepReal = nullptr;
uint8_t* g_insCur = nullptr;
uint32_t* g_insChg = nullptr;
bool g_insAllocOk = false, g_insAllocTried = false;
int g_insPrevTick = -1, g_insPrevMode = -1;
uint32_t g_insCensusAt[8];
int g_insCensusAtN = 0;
uint32_t g_insXrefAt = 0, g_insLastCensusFrame = 0;
uint64_t g_insEvents = 0, g_insDiffWords = 0;

// Real physics step behind the addon's step hook: counts world steps, including the addon's own catch-up steps.
void* Ins_CountingStep(void* self, void** worlds, int count, float dt, int substeps, bool b, void* p) {
  g_insStepCalls.fetch_add(1, std::memory_order_relaxed);
  // On the main thread, sample the body candidates immediately around the first couple of inner steps of the frame - this is the
  // instant the physics bodies move, before anything copies the result out to the render bones (events 7 pre / 8 post).
  if (g_bMainTid && GetCurrentThreadId() == g_bMainTid && g_bInnerCalls < 2 && g_bState.load(std::memory_order_relaxed) == kBTracking) {
    ++g_bInnerCalls;
    BodyPass(kBEvInnerPre, false);
    void* r = g_insStepReal(self, worlds, count, dt, substeps, b, p);
    BodyPass(kBEvInnerPost, false);
    return r;
  }
  return g_insStepReal(self, worlds, count, dt, substeps, b, p);
}

void InsNoteStep(void** worlds, int count, float dt, int substeps) {
  g_insDt = dt;
  g_insSub = substeps;
  int n = 0;
  for (int i = 0; i < count && i < 64 && n < 8; ++i) {
    uintptr_t w = 0;
    if (worlds && Rd((uintptr_t)worlds + uintptr_t(i) * 8, w) && w) g_insWorld[n++] = w;
  }
  if (n) g_insWorldN = n;
}

void InsNoteFrame(void* physSys) { g_insPhysSys = physSys; }

bool InsEnsureAlloc() {
  if (g_insAllocTried) return g_insAllocOk;
  g_insAllocTried = true;
  g_insBuf = (uint8_t*)InsVAlloc(kInsBufCap);
  g_insReg = (InsRegion*)InsVAlloc(sizeof(InsRegion) * kInsMaxRegions);
  g_insCls = (InsClass*)InsVAlloc(sizeof(InsClass) * kInsMaxClasses);
  g_insClsKey = (uintptr_t*)InsVAlloc(sizeof(uintptr_t) * kInsClassHash);
  g_insClsVal = (int32_t*)InsVAlloc(sizeof(int32_t) * kInsClassHash);
  g_insFields = (InsField*)InsVAlloc(sizeof(InsField) * kInsMaxFields);
  g_insNodes = (InsNode*)InsVAlloc(sizeof(InsNode) * kInsMaxNodes);
  g_insNodeKey = (uintptr_t*)InsVAlloc(sizeof(uintptr_t) * kInsNodeHash);
  g_insOrder = (int*)InsVAlloc(sizeof(int) * kInsMaxNodes);
  g_insScan = (uint8_t*)InsVAlloc(kInsMaxObjSize);
  g_insPrev = (uint8_t*)InsVAlloc(size_t(kInsMaxObjs) * kInsMaxObjSize);
  g_insCur = (uint8_t*)InsVAlloc(kInsMaxObjSize);
  g_insChg = (uint32_t*)InsVAlloc(sizeof(uint32_t) * (kInsMaxObjSize / 2));
  g_insHits = (InsHit*)InsVAlloc(sizeof(InsHit) * kInsMaxHits);
  g_insXbuf = (uint8_t*)InsVAlloc(kInsXChunk);
  g_insAllocOk = g_insBuf && g_insReg && g_insCls && g_insClsKey && g_insClsVal && g_insFields && g_insNodes && g_insNodeKey &&
                 g_insOrder && g_insScan && g_insPrev && g_insCur && g_insChg && g_insHits && g_insXbuf;
  if (!g_insAllocOk) {
    g_insNodes = nullptr;
    g_insCls = nullptr;
  }
  g_schemaSysIns = g_schemaSys;
  InsRefreshRegions(true);
  return g_insAllocOk;
}

void InsScheduleCensus(uint32_t inFrames) {
  if (g_insCensusAtN < 8) g_insCensusAt[g_insCensusAtN++] = g_insFrame + inFrames;
}

void InsRunCensus(const char* why) {
  InsCensus(why);
  g_insLastCensusFrame = g_insFrame;
}

void InsWriteEvent(uint8_t ev) {
  DemoView v;
  if (g_engine) ReadDemoRaw(v);
  float c[3] = {0, 0, 0};
  bool haveC = false;
  uintptr_t vt = 0;
  if (g_insT.pawn && Rd(g_insT.pawn, vt) && vt == g_insT.vt) haveC = PoseCentroid(g_insT.pawn, c);
  int32_t lastAnim = -1;
  if (g_insT.ctrl && g_offLastAnimTick > 0) Rd(g_insT.ctrl + g_offLastAnimTick, lastAnim);
  const size_t at = InsBegin('E');
  InsPod(ev);
  InsPod(g_insFrame);
  InsPod(Now());
  InsPod((uint8_t)v.playing);
  InsPod((uint8_t)v.paused);
  InsPod((int32_t)v.tick);
  InsPod(v.frac);
  InsPod((float)CurTime());
  InsPut(c, 12);
  InsPod((uint8_t)haveC);
  InsPod(g_insStepCalls.load(std::memory_order_relaxed));
  InsPod(lastAnim);
  InsPod((uint8_t)g_mode.load(std::memory_order_relaxed));
  InsPod(g_insDt);
  InsPod((int32_t)g_insSub);
  InsPod(g_clk.T);
  InsPod(g_clk.P);
  InsPod((int32_t)g_insT.idx);
  InsEnd(at);
}

// D records: changed 4-byte words of every tracked object since the previous event.
void InsDiffObjects() {
  for (int i = 0; i < kInsMaxObjs; ++i) {
    InsObj& o = g_insObj[i];
    if (!o.live) continue;
    if (!SafeCopy(g_insCur, (void*)o.addr, o.size)) {
      InsDropObj(i, 1);
      continue;
    }
    if (o.vt && memcmp(g_insCur, &o.vt, 8)) {
      InsDropObj(i, 2);
      continue;
    }
    uint8_t* prev = g_insPrev + size_t(i) * kInsMaxObjSize;
    uint32_t n = 0;
    for (uint32_t off = 0; off + 4 <= o.size; off += 4) {
      if (off + 8 <= o.size && !(off & 4)) {
        uint64_t a, b;
        memcpy(&a, g_insCur + off, 8);
        memcpy(&b, prev + off, 8);
        if (a == b) {
          off += 4;
          continue;
        }
      }
      uint32_t a, b;
      memcpy(&a, g_insCur + off, 4);
      memcpy(&b, prev + off, 4);
      if (a == b) continue;
      g_insChg[2 * n] = off;
      g_insChg[2 * n + 1] = a;
      ++n;
    }
    if (!n) continue;
    memcpy(prev, g_insCur, o.size);
    g_insDiffWords += n;
    const size_t at = InsBegin('D');
    InsPod(o.id);
    InsPod(n);
    InsPut(g_insChg, size_t(n) * 8);
    InsEnd(at);
  }
}

void InsRecStop(const char* why) {
  if (!g_insRec) return;
  BodyFinalize("recording stopped");  // flush any in-progress body run's B records into the file
  WDrain();  // last hits first, then take the page protections back off before the file closes
  WDisarm("recording stopped");
  InsMark("STOP %s", why);
  InsFlush();
  CloseHandle(g_insFile);
  g_insFile = INVALID_HANDLE_VALUE;
  g_insRec = false;
  Con("insight: stopped (%s) - %u frames, %.1f MB, %d censuses -> %s\n", why, g_insFrame, double(g_insBytes) / (1 << 20), g_insCensusN,
      g_insPath);
}

// Frame housekeeping after the frame's last event: markers, target check, censuses, cross-reference scan, flush.
void InsFrameEnd() {
  ++g_insFrame;
  DemoView v;
  if (g_engine) ReadDemoRaw(v);
  const int mode = g_mode.load();
  if (mode != g_insPrevMode) InsMark("MODE %d", mode);
  g_insPrevMode = mode;
  bool changed = false;
  if (v.playing != g_insPrevPlaying) InsMark(v.playing ? "PLAYING tick %d" : "STOPPED tick %d", v.tick);
  if (v.playing) {
    if (v.paused != g_insPrevPaused) {
      InsMark(v.paused ? "PAUSE tick %d" : "UNPAUSE tick %d", v.tick);
      changed = true;
    }
    if (g_insPrevTick >= 0 && (v.tick < g_insPrevTick || v.tick > g_insPrevTick + 8)) {
      InsMark("JUMP tick %d -> %d", g_insPrevTick, v.tick);
      changed = true;
      g_insXrefAt = g_insFrame + 12;
    }
  }
  g_insPrevPlaying = v.playing;
  g_insPrevPaused = v.paused;
  g_insPrevTick = v.playing ? v.tick : -1;
  if (changed) {
    InsScheduleCensus(2);
    InsScheduleCensus(12);
  }
  uintptr_t gl = 0;
  if (Rd(g_globalsVar, gl) && gl) {
    for (int i = 0; i < kInsMaxObjs; ++i) {
      if (!g_insObj[i].live || g_insObj[i].kind != kObjGlobals || g_insObj[i].addr == gl) continue;
      InsDropObj(i, 4);
      InsObjAdd(kObjGlobals, gl, 0x100, -1, 0, 0xff, 0, "client globals");
    }
  }
  if (g_insFrame % 4 == 0) {
    InsTarget t;
    InsPickTarget(t);
    if (t.pawn != g_insT.pawn || t.idx != g_insT.idx) {
      ++g_insXrefGen;
      InsRunCensus("target changed");
      g_insXrefAt = g_insFrame + 2;
    } else if (t.node != g_insT.node || t.bones != g_insT.bones || t.ctrl != g_insT.ctrl) {
      InsMark("TARGET POINTERS node %llx -> %llx, bones %llx -> %llx, ctrl %llx -> %llx", (unsigned long long)g_insT.node,
              (unsigned long long)t.node, (unsigned long long)g_insT.bones, (unsigned long long)t.bones, (unsigned long long)g_insT.ctrl,
              (unsigned long long)t.ctrl);
      InsRunCensus("target pointers changed");
    }
  }
  bool due = false;
  for (int i = 0; i < g_insCensusAtN;) {
    if (g_insCensusAt[i] <= g_insFrame) {
      due = true;
      g_insCensusAt[i] = g_insCensusAt[--g_insCensusAtN];
    } else {
      ++i;
    }
  }
  if (due)
    InsRunCensus("scheduled (after pause/unpause/jump)");
  else if (g_insFrame - g_insLastCensusFrame >= kInsCensusEvery)
    InsRunCensus("periodic");
  if (g_insXrefAt && g_insFrame >= g_insXrefAt && g_insXrefState.load() == 0) {  // a running scan finishes first
    g_insXrefAt = 0;
    InsXrefStart();
  }
  InsXrefMerge();
  BodyFrame();  // drive the physics-body finder (scan/merge/cluster/finalize) before the journal is re-armed
  WTick();
  if (g_insFrame % 64 == 0) InsFlush();
  if (g_insBytes > kInsMaxFile) InsRecStop("file size limit (2 GB) reached");
}

void InsEvent(uint8_t ev) {
  if (!g_insRec) return;
  ++g_insEvents;
  InsWriteEvent(ev);
  InsDiffObjects();
  BodyPass(ev, false);  // re-read the physics body candidates for this event (self-gates: only while tracking)
  if (ev == kEvFramePost || (ev == kEvStepPost && !o_frame)) InsFrameEnd();
}

// logs folder next to the DLL, with trailing backslash
void InsLogsDir(char* dir, size_t cap) {
  GetModuleFileNameA(g_self, dir, (DWORD)cap);
  if (char* slash = strrchr(dir, '\\')) slash[1] = 0;
  strcat_s(dir, cap, "logs\\");
  CreateDirectoryA(dir, nullptr);
}

void InsRecStart(const char* label) {
  if (g_insRec) return Con("insight: already recording -> %s\n", g_insPath);
  if (!InsEnsureAlloc()) return Con("insight: could not allocate the recorder buffers\n");
  size_t k = 0;
  for (const char* s = label ? label : ""; *s && k + 1 < sizeof(g_insLabel); ++s)
    if (isalnum((unsigned char)*s) || *s == '_' || *s == '-') g_insLabel[k++] = *s;
  g_insLabel[k] = 0;
  char dir[MAX_PATH];
  InsLogsDir(dir, sizeof(dir));
  SYSTEMTIME st;
  GetLocalTime(&st);
  snprintf(g_insPath, sizeof(g_insPath), "%sinsight_%04u%02u%02u_%02u%02u%02u%s%s.bin", dir, st.wYear, st.wMonth, st.wDay, st.wHour,
           st.wMinute, st.wSecond, g_insLabel[0] ? "_" : "", g_insLabel);
  g_insFile = CreateFileA(g_insPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_insFile == INVALID_HANDLE_VALUE) return Con("insight: could not create %s\n", g_insPath);
  g_insLen = 0;
  g_insBytes = 0;
  g_insFrame = 0;
  g_insEvents = g_insDiffWords = 0;
  g_insCensusN = 0;
  for (int i = 0; i < g_insClsN; ++i) g_insCls[i].written = 0;
  for (InsObj& o : g_insObj) o.live = false;
  g_insT = InsTarget{};
  // reset the physics-body finder for the new recording
  g_bState = kBIdle;
  g_bCandN = g_bSpanN = g_insBodyWinN = 0;
  g_bRun = g_bStartFrame = g_bUnpauseFrame = g_bFinalizeAt = g_bQuietPasses = 0;
  g_bClustered = g_bPrevSettling = false;
  g_bInnerCalls = 0;
  g_bScannedPawn = g_bLastPawn = 0;
  g_bPawnStableSince = 0;
  g_bTgtN = 0;
  g_insXrefRootN = 0;
  ++g_insXrefGen;
  g_insCensusAtN = 0;
  g_insXrefAt = 0;
  InsPut("INSIGHT", 8);
  InsPod(kInsFormat);
  InsStr(kVersion);
  InsStr(g_insLabel);
  HMODULE mods[1024];
  DWORD need = 0;
  if (K32EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
    for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
      MODULEINFO mi{};
      char name[MAX_PATH];
      if (!K32GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) ||
          !K32GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH))
        continue;
      const size_t at = InsBegin('L');
      InsPod((uint64_t)(uintptr_t)mi.lpBaseOfDll);
      InsPod((uint32_t)mi.SizeOfImage);
      InsStr(name);
      InsEnd(at);
    }
  }
  g_insRec = true;
  DemoView v;
  if (g_engine) ReadDemoRaw(v);
  g_insPrevPlaying = v.playing;
  g_insPrevPaused = v.paused;
  g_insPrevTick = v.playing ? v.tick : -1;
  g_insPrevMode = g_mode.load();
  InsMark("START label '%s' mode %d playing %d paused %d tick %d cl_phys_timescale %.3f schema %s worlds %d physsys %llx animsys %llx", g_insLabel,
          g_insPrevMode, (int)v.playing, (int)v.paused, v.tick, PhysTimescale(), g_schemaSysIns ? "ok" : "missing", g_insWorldN,
          (unsigned long long)(uintptr_t)g_insPhysSys, (unsigned long long)(uintptr_t)g_animSys.load());
  InsRunCensus("start");
  InsMark("SYSTEMS posectl %llx ragsys %llx", (unsigned long long)g_insPoseCtl, (unsigned long long)g_insRagSys);
  InsScheduleCensus(8);
  g_insXrefAt = 2;
  InsFlush();
  Con("insight: recording -> %s\n", g_insPath);
  Con("insight: target %s #%d, %d objects tracked, census %d nodes (near %d, owned %d) in %.0f ms\n",
      g_insT.pawn ? "dead player" : "NONE (no dead player yet - the recorder picks one up when someone dies)", g_insT.idx, InsLiveObjs(),
      g_insNodeN, g_insNearN, g_insOwnedN, g_insCensusMs);
}

void InsStatus() {
  Con("insight: %s%s | %u frames, %llu events, %.1f MB, %llu changed words\n", g_insRec ? "RECORDING -> " : "idle",
      g_insRec ? g_insPath : "", g_insFrame, (unsigned long long)g_insEvents, double(g_insBytes) / (1 << 20),
      (unsigned long long)g_insDiffWords);
  Con("  target: %s #%d pawn %llx window 0x%x | census: %d runs, last %d nodes (near %d, owned %d) %.0f ms | tracked %d objects\n",
      g_insT.pawn ? "dead player" : "none", g_insT.idx, (unsigned long long)g_insT.pawn, g_insT.win, g_insCensusN, g_insNodeN, g_insNearN,
      g_insOwnedN, g_insCensusMs, InsLiveObjs());
  Con("  xref: %s, last %d hits | physics worlds %d, physics system %s, anim system %s, pose control %s, ragdoll system %s, schema %s\n",
      g_insXrefState.load() == 1 ? "scanning" : "idle", g_insHitMerged, g_insWorldN, g_insPhysSys ? "ok" : "not seen",
      g_animSys.load() ? "ok" : "not seen", g_insPoseCtl ? "ok" : g_insSysSearched ? "not found" : "-",
      g_insRagSys ? "ok" : g_insSysSearched ? "not found" : "-", g_schemaSys ? "ok" : "missing");
}
