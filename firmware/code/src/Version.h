/** @file Version.h
 *  @brief FRIZZ's version, as the loop transfer's HELLO reports it (LoopTransfer.h,
 *  docs/LOOP_PROTOCOL.md 7.1): major, minor, patch, pre-release (0 for a release, N for
 *  -beta.N). It can't come from the git tag at build time, as a build's md5 names it
 *  (CLAUDE.md): the commit before a tag sets it, and tools/builds.py and flash.py --slot 10/11
 *  refuse a v* tag's build whose kFrizzVersion isn't its tag. A commit without a tag may carry
 *  the next version's, or still the last tag's until a commit sets the next (CLAUDE.md,
 *  Releases): a build between tags tells itself from that tag's by IDENT's CRC, not by HELLO.
 *  Keep the line's shape: the tools read it.
 */
#pragma once
#include <stdint.h>

static const uint8_t kFrizzVersion[4] = {1, 5, 0, 3};
