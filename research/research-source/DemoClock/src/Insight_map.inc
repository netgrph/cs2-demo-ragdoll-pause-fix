// insight recorder, part 4: readable map dump and the console command.

const char* const kInsKindNames[] = {"globals", "pawn", "bones", "object", "blob", "animctrl", "scenenode", "body"};

void InsFlagStr(uint8_t f, char* out) {
  int k = 0;
  for (auto [bit, ch] : {std::pair<uint8_t, char>{kNfVt, 'V'}, {kNfOwned, 'O'}, {kNfNear, 'N'}, {kNfEntity, 'E'}, {kNfLineage, 'L'}, {kNfExpanded, 'X'}})
    out[k++] = (f & bit) ? ch : '.';
  out[k] = 0;
}

// Current value of a schema field, formatted by its schema type name.
void InsFmtField(const InsField& f, uintptr_t base, char* type, size_t typeCap, char* out, size_t cap) {
  InsSafeStr(f.type, type, typeCap);
  const uintptr_t a = base + uintptr_t(f.off);
  snprintf(out, cap, "?");
  float fl[3];
  int32_t i32 = 0;
  uint32_t u32 = 0;
  uint16_t u16 = 0;
  uint8_t u8 = 0;
  uint64_t u64 = 0;
  if (!strcmp(type, "float32") || !strcmp(type, "GameTime_t")) {
    if (Rd(a, fl[0])) snprintf(out, cap, "%g", fl[0]);
  } else if (!strcmp(type, "int32") || !strcmp(type, "GameTick_t")) {
    if (Rd(a, i32)) snprintf(out, cap, "%d", i32);
  } else if (!strcmp(type, "uint32")) {
    if (Rd(a, u32)) snprintf(out, cap, "%u", u32);
  } else if (!strcmp(type, "int16") || !strcmp(type, "uint16")) {
    if (Rd(a, u16)) snprintf(out, cap, "%d", type[0] == 'i' ? int(int16_t(u16)) : int(u16));
  } else if (!strcmp(type, "bool") || !strcmp(type, "uint8") || !strcmp(type, "int8")) {
    if (Rd(a, u8)) snprintf(out, cap, "%d", type[0] == 'i' ? int(int8_t(u8)) : int(u8));
  } else if (!strcmp(type, "Vector") || !strcmp(type, "QAngle")) {
    if (SafeCopy(fl, (void*)a, sizeof(fl))) snprintf(out, cap, "%.3f %.3f %.3f", fl[0], fl[1], fl[2]);
  } else if (!strncmp(type, "CHandle", 7)) {
    if (Rd(a, u32)) snprintf(out, cap, "%08x (index %u)", u32, u32 & 0x7fff);
  } else if (Rd(a, u64)) {
    snprintf(out, cap, "raw %016llx", (unsigned long long)u64);
  }
}

void InsMap() {
  if (!InsEnsureAlloc()) return Con("insight: could not allocate the recorder buffers\n");
  if (g_insXrefState.load() == 2) InsXrefMerge();
  InsCensus("map");
  const bool xrefStarted = g_insXrefState.load() == 0 && g_insT.pawn && !g_insRec;
  if (xrefStarted) {
    ++g_insXrefGen;
    InsXrefStart();
  }
  char dir[MAX_PATH], path[MAX_PATH];
  InsLogsDir(dir, sizeof(dir));
  SYSTEMTIME st;
  GetLocalTime(&st);
  snprintf(path, sizeof(path), "%sinsight_map_%04u%02u%02u_%02u%02u%02u.txt", dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond);
  FILE* f = nullptr;
  if (fopen_s(&f, path, "w") || !f) return Con("insight: could not create %s\n", path);
  DemoView v;
  if (g_engine) ReadDemoRaw(v);
  fprintf(f, "RagdollDemoClock %s insight map\n", kVersion);
  fprintf(f, "demo: playing %d paused %d tick %d frac %.3f | curtime %.4f | mode %d | cl_phys_timescale %.3f | clock T %.3f P %.3f\n",
          (int)v.playing, (int)v.paused, v.tick, v.frac, CurTime(), g_mode.load(), PhysTimescale(), g_clk.T, g_clk.P);
  if (g_insT.pawn)
    fprintf(f, "target: player #%d pawn %llx class %s window 0x%x node %llx bones %llx animctrl %llx handle %08x death %.4f centroid %.1f %.1f %.1f\n",
            g_insT.idx, (unsigned long long)g_insT.pawn, g_insT.cls >= 0 ? g_insCls[g_insT.cls].pretty : "?", g_insT.win,
            (unsigned long long)g_insT.node, (unsigned long long)g_insT.bones, (unsigned long long)g_insT.ctrl, g_insT.handle, g_insT.death,
            g_insT.c[0], g_insT.c[1], g_insT.c[2]);
  else
    fprintf(f, "target: none (no dead player)\n");
  fprintf(f, "systems: physics worlds %d, physics system %llx, anim system %llx, pose control %llx, ragdoll system %llx, schema %s\n",
          g_insWorldN, (unsigned long long)(uintptr_t)g_insPhysSys, (unsigned long long)(uintptr_t)g_animSys.load(),
          (unsigned long long)g_insPoseCtl, (unsigned long long)g_insRagSys, g_schemaSysIns ? "ok" : "missing");
  fprintf(f, "census: %d nodes in %.1f ms, near %d, owned %d |", g_insNodeN, g_insCensusMs, g_insNearN, g_insOwnedN);
  for (int r = 0; r < kRootCount; ++r) fprintf(f, " %s %d", kInsRootNames[r], g_insRootCount[r]);
  fprintf(f, "\nflags: V has vtable, O references the pawn, N holds a position near the ragdoll, E entity, L physics lineage, X expanded\n");

  fprintf(f, "\n== tracked objects (%d)\n", InsLiveObjs());
  char fl[8];
  for (int i = 0; i < kInsMaxObjs; ++i) {
    const InsObj& o = g_insObj[i];
    if (!o.live) continue;
    InsFlagStr(o.flags, fl);
    fprintf(f, "#%-4u %-9s %s %016llx size 0x%-5x %s\n", o.id, o.kind < 8 ? kInsKindNames[o.kind] : "?", fl, (unsigned long long)o.addr,
            o.size, o.label);
  }

  fprintf(f, "\n== references to the pawn / scene node / animation controller (%d, last merged scan)%s\n", g_insHitMerged,
          xrefStarted ? " - a new scan was started, run the map again in a few seconds" : "");
  for (int i = 0; i < g_insHitMerged; ++i) {
    const InsHit& h = g_insHits[i];
    const char* what = h.value >= g_insT.pawn && h.value < g_insT.pawn + g_insT.win ? "pawn"
                       : g_insT.node && h.value >= g_insT.node && h.value < g_insT.node + 0x600 ? "node"
                                                                                              : "animctrl";
    fprintf(f, "%016llx -> %s+0x%llx | owner %016llx+0x%llx %s\n", (unsigned long long)h.at, what,
            (unsigned long long)(h.value - (what[0] == 'p' ? g_insT.pawn : what[0] == 'n' ? g_insT.node : g_insT.ctrl)),
            (unsigned long long)h.owner, (unsigned long long)(h.owner ? h.at - h.owner : 0), h.cls >= 0 ? g_insCls[h.cls].pretty : "(no class)");
  }

  int nc = 0;
  for (int i = 0; i < g_insNodeN; ++i)
    if (g_insNodes[i].flags & (kNfNear | kNfOwned)) g_insOrder[nc++] = i;
  std::sort(g_insOrder, g_insOrder + nc, [](int a, int b) { return g_insNodes[a].score > g_insNodes[b].score; });
  fprintf(f, "\n== census nodes near the ragdoll or referencing the pawn (%d, top 250 by score)\n", nc);
  char pathStr[256];
  for (int k = 0; k < nc && k < 250; ++k) {
    const InsNode& n = g_insNodes[g_insOrder[k]];
    InsFlagStr(n.flags, fl);
    InsPath(g_insOrder[k], pathStr, sizeof(pathStr));
    fprintf(f, "score %3d %s %016llx near %3u (first +0x%x) | %s\n", n.score, fl, (unsigned long long)n.addr, n.nearCount, n.nearOff, pathStr);
  }

  if (g_insT.pawn && g_insT.cls >= 0) {
    const int n = InsClassFields(g_insCls[g_insT.cls], g_insFields, kInsMaxFields);
    fprintf(f, "\n== pawn schema fields (%d)\n", n);
    char name[128], type[96], owner[96], val[96];
    for (int i = 0; i < n; ++i) {
      InsSafeStr(g_insFields[i].name, name, sizeof(name));
      InsSafeStr(g_insFields[i].owner, owner, sizeof(owner));
      InsFmtField(g_insFields[i], g_insT.pawn, type, sizeof(type), val, sizeof(val));
      fprintf(f, "+0x%04x %-44s %-36s %-28s %s\n", g_insFields[i].off, name, type, owner, val);
    }
  }
  fclose(f);
  Con("insight: map written -> %s\n", path);
}

// ragdollfix watch ... - the forensic watchpoint surface. Arms hardware breakpoints and page write journals so the recording carries
// the exact instruction, call stack, registers and byte diff behind every write we care about.
void WCommand(int argc, const char** argv) {
  const char* sub = argc > 1 ? argv[1] : "status";
  if (!_stricmp(sub, "status")) {
    WStatus();
  } else if (!_stricmp(sub, "off")) {
    WDisarm("console");
    WStatus();
  } else if (!_stricmp(sub, "anim")) {
    if (!g_animElapsedFnAddr) return Con("watch: this build has no located animation elapsed-ticks helper\n");
    Con("watch: %s\n",
        WArm(g_animElapsedFnAddr, 1, 0, "anim elapsed helper (rcx=controller rdx=tick)") ? "armed" : "failed (no free slot?)");
  } else if (!_stricmp(sub, "tick")) {
    if (!g_insT.ctrl || g_offLastAnimTick <= 0)
      return Con("watch: no animation controller / tick offset yet, run 'ragdollfix insight census'\n");
    Con("watch: %s\n", WArm(g_insT.ctrl + g_offLastAnimTick, 4, 1, "m_nPrevAnimUpdateTick") ? "armed" : "failed (no free slot?)");
  } else if (!_stricmp(sub, "bone")) {
    int b = 5;
    if (argc > 2 && !ParseInt(argv[2], 0, 255, b)) return Con("usage: ragdollfix watch bone <index>\n");
    if (!g_insT.bones) return Con("watch: no bone transform array yet\n");
    char lbl[64];
    _snprintf_s(lbl, sizeof(lbl), _TRUNCATE, "bone %d x/y", b);
    Con("watch: %s\n", WArm(g_insT.bones + uintptr_t(b) * 32, 8, 1, lbl) ? "armed" : "failed (no free slot?)");
  } else if (!_stricmp(sub, "addr")) {
    if (argc < 3) return Con("usage: ragdollfix watch addr <hex> [1|2|4|8] [w|rw|x]\n");
    const uintptr_t a = (uintptr_t)_strtoui64(argv[2], nullptr, 16);
    int len = 4;
    if (argc > 3 && !ParseInt(argv[3], 1, 8, len)) return Con("usage: ragdollfix watch addr <hex> [1|2|4|8] [w|rw|x]\n");
    uint8_t rw = 1;
    if (argc > 4) rw = !_stricmp(argv[4], "x") ? 0 : (!_stricmp(argv[4], "rw") ? 3 : 1);
    if (rw == 0) len = 1;
    Con("watch: %s\n", WArm(a, (uint8_t)len, rw, "console") ? "armed" : "failed (bad address, misaligned, or no free slot)");
  } else if (!_stricmp(sub, "page")) {
    if (argc < 3) return Con("usage: ragdollfix watch page <hex>\n");
    const uintptr_t a = (uintptr_t)_strtoui64(argv[2], nullptr, 16);
    Con("watch: %s\n", WPageArm(a, 0, "console") ? "journalled" : "failed (not private writable data, or the table is full)");
  } else if (!_stricmp(sub, "obj")) {
    int id = 0;
    if (argc < 3 || !ParseInt(argv[2], 0, 1 << 30, id)) return Con("usage: ragdollfix watch obj <id from the census>\n");
    int pages = 0;
    for (int i = 0; i < kInsMaxObjs; ++i) {
      const InsObj& o = g_insObj[i];
      if (!o.live || (int)o.id != id) continue;
      for (uintptr_t a = o.addr & ~(kWPageSize - 1); a < o.addr + o.size; a += kWPageSize)
        if (WPageArm(a, o.id, o.label)) ++pages;
    }
    Con("watch: %d page(s) journalled for object #%d\n", pages, id);
  } else if (!_stricmp(sub, "frames")) {
    int n = 0;
    if (argc < 3 || !ParseInt(argv[2], 0, 100000, n)) return Con("usage: ragdollfix watch frames <n>   (0 = until 'watch off')\n");
    g_wUntil = n ? g_insFrame + uint32_t(n) : 0;
    Con("watch: %s\n", n ? "budget set" : "budget cleared, stays armed until 'ragdollfix watch off'");
  } else if (!_stricmp(sub, "bodies")) {
    BCommand(argc - 1, argv + 1);
  } else {
    Con("usage: ragdollfix watch [status | off | anim | tick | bone <i> | addr <hex> [len] [w|rw|x] | page <hex> | obj <id> | "
        "frames <n> | bodies [start|cluster|end|status]]\n");
  }
}

void InsCommand(int argc, const char** argv) {
  const char* sub = argc > 1 ? argv[1] : "status";
  if (!_stricmp(sub, "status")) {
    InsStatus();
  } else if (!_stricmp(sub, "rec")) {
    int on = 0;
    if (argc < 3 || !ParseInt(argv[2], 0, 1, on)) return Con("usage: ragdollfix insight rec <0|1> [label]\n");
    if (on)
      InsRecStart(argc > 3 ? argv[3] : "");
    else if (g_insRec)
      InsRecStop("console");
    else
      Con("insight: not recording\n");
  } else if (!_stricmp(sub, "map")) {
    InsMap();
  } else if (!_stricmp(sub, "census")) {
    if (!InsEnsureAlloc()) return Con("insight: could not allocate the recorder buffers\n");
    InsCensus("console");
    InsStatus();
  } else if (!_stricmp(sub, "watch")) {
    if (!InsEnsureAlloc()) return Con("insight: could not allocate the recorder buffers\n");
    WCommand(argc - 1, argv + 1);
  } else if (!_stricmp(sub, "forensic")) {
    if (!InsEnsureAlloc()) return Con("insight: could not allocate the recorder buffers\n");
    int frames = 0;
    if (argc > 2 && !ParseInt(argv[2], 0, 100000, frames)) return Con("usage: ragdollfix insight forensic [frames]\n");
    if (WForensic(frames) < 0) return Con("insight: could not install the vectored exception handler\n");
    WStatus();
  } else if (!_stricmp(sub, "mark")) {
    char text[256] = "";
    for (int i = 2; i < argc; ++i) {
      if (i > 2) strncat_s(text, sizeof(text), " ", _TRUNCATE);
      strncat_s(text, sizeof(text), argv[i], _TRUNCATE);
    }
    InsMark("USER %s", text);
    Con("insight: %s '%s'\n", g_insRec ? "marked" : "not recording, mark ignored:", text);
  } else {
    Con("usage: ragdollfix insight [status | rec <0|1> [label] | map | census | forensic [frames] | watch ... | mark <text>]\n");
  }
}
