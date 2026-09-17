// ---------------------------------------------------------------------------------------------------------- insight recorder
// Included by RagdollDemoClock.cpp inside its anonymous namespace. Records, per hook event, every change the game makes to the
// memory around the newest dead player (pawn, scene node, bones, animation controller, and heap objects found by a pointer census
// from the pawn, the physics worlds and the ragdoll game systems), so the pause / seek behaviour can be observed instead of guessed.
// Everything here runs on the game main thread except the cross-reference scan, which only reads memory with ReadProcessMemory.
// Output: logs\insight_*.bin (parsed by insight.py) and logs\insight_map_*.txt.

#include "Insight_base.inc"
#include "Insight_census.inc"
#include "Insight_watch.inc"
#include "Insight_bodies.inc"
#include "Insight_rec.inc"
#include "Insight_map.inc"
