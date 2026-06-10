# minih264 overlay

`minih264e.h` here is a project-owned overlay of the upstream single-header
encoder vendored via `deps/minih264.mtb` (upstream commit
`b0baea7a80ef9d12da97301dd1099b8791b5ba43`,
https://github.com/lieff/minih264).

The upstream copy in `mtb_shared` is fully `CY_IGNORE`d in `proj_cm55/Makefile`;
this directory is placed on the include path instead, so `#include
"minih264e.h"` (from `source/encoder_task.c`, which defines
`MINIH264_IMPLEMENTATION`) resolves here. The library is header-only, so this
one file is the entire library.

## Divergence from upstream

Two patches, both tagged `Patch for fork-aivision WebRTC pilot` in-file:

1. **Bare-metal ARM byte order.** Upstream only defines `__BYTE_ORDER` for
   Linux/ARMCC. The arm-none-eabi (GCC) bare-metal build hits neither branch,
   so we add a little-endian `__arm__ && !__linux__` case.

2. **ACLE intrinsics for arm-none-eabi.** GCC 14 for arm-none-eabi provides
   `__usad8/__usada8/__sadd16/__ssub16/__clz` as built-ins via `<arm_acle.h>`.
   Upstream's hand-rolled inline-asm versions collide with those declarations,
   so for `__arm__` we include `<arm_acle.h>` and drop the hand-rolled copies.

## Updating

To move to a newer upstream: bump `deps/minih264.mtb`, re-fetch, then re-apply
the two patches above on top of the new `minih264e.h` and drop the result here.
Diff this file against `mtb_shared/minih264/<commit>/minih264e.h` to review.
