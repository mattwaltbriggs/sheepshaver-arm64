#include "config-macosx-x86_64.h"

/* arm64 macOS launchd rejects binaries whose __PAGEZERO segment is made
   writable, and the default 64-bit __PAGEZERO reservation blocks SheepShaver's
   fixed low-address mappings. Keep Mac addresses away from process address
   zero by using direct addressing. Pick the host base dynamically because
   fixed compile-time offsets collide with arm64 ASLR and the shared cache. */
#undef NATMEM_OFFSET
#define NATMEM_OFFSET 0
#define NATMEM_OFFSET_VARIABLE 1

#undef PAGEZERO_HACK

#undef ENABLE_DYNGEN
#define ENABLE_DYNGEN 0

/* Keep the decoded block cache on for arm64 builds. The PPC core source is
   compiled with conservative optimization on arm64 to avoid Release-only
   optimizer issues while preserving usable boot speed. */
#undef PPC_DECODE_CACHE
#define PPC_DECODE_CACHE 1

#undef HAVE_LINKER_SCRIPT
