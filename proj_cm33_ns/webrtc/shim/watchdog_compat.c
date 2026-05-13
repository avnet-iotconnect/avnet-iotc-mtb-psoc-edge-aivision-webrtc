/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Avnet
 * Authors: Nikola Markovic <nikola.markovic@avnet.com> et al.
 */

// fork-aivision: upstream-derived transport/ICE files expect a platform
// watchdog hook named vPetWatchdog(). The current PSE84 tree has no provider,
// so keep a weak no-op compatibility symbol until the real watchdog path is
// wired or these long-running loops are reworked.
__attribute__((weak)) void vPetWatchdog(void) {
}

