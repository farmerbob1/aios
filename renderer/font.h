/* ChaosGL Font — Claude Mono 8x16 bitmap (Phase 5) */

#pragma once

#include "../include/types.h"

#define CLAUDE_MONO_WIDTH   8
#define CLAUDE_MONO_HEIGHT  16

/* Bitmap glyph data: [ascii_code][row], MSB = leftmost pixel.
 * Only ASCII 32-126 have visible glyphs; others are blank. */
extern const uint8_t claude_mono_8x16[128][16];
