/*
 * blackbox — separate-translation-unit build.
 *
 * blackbox is a single-header library: the implementation lives in blackbox.h under
 * BLACKBOX_IMPLEMENTATION. Unity/embedded builds define that in the app's TU and never compile this
 * file. This wrapper exists only for builds that prefer a standalone object (e.g. the native test):
 * compile it with the same -DBLACKBOX_PERSIST / -DBLACKBOX_CLOCK as the rest of the program.
 *
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */
#define BLACKBOX_IMPLEMENTATION
#include "blackbox.h"
