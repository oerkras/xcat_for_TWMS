// ARCHIVED 2026-07-30 — NOT linked into TwmsFly.dll.
// GRAP / BlackCat forbid INLINE HOOK (E9 / .text patch). See:
//   docs/features/security/GRAP与枫星对齐.md §4.1
// Historical MovePath.Flush E9 source: fly_flush_hook.cpp.archived
//   (from Dumps/runtime/MovePathFlushHook + former fly merge; do not link)
// Do NOT add this file (or the .archived twin) back to build.bat.

#error "REMOVED: MovePath.Flush E9 violates GRAP INLINE HOOK ban. Use data-plane fly only."
