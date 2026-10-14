/** @file Version.h
 *  @brief FRIZZ's version, as the loop transfer's HELLO reports it (LoopTransfer.h,
 *  docs/LOOP_PROTOCOL.md 7.1): major, minor, patch, pre-release (0 for a release, N for
 *  -beta.N). It can't come from the git tag at build time, as a build's md5 names it
 *  (CLAUDE.md): the commit before a tag sets it, and tools/builds.py and flash.py --slot 10/13
 *  refuse a v* tag's build whose kFrizzVersion isn't its tag. A commit without a tag carries the
 *  next version's. Keep the line's shape: the tools read it.
 */
#pragma once
#include <stdint.h>

static const uint8_t kFrizzVersion[4] = {1, 5, 0, 1};
