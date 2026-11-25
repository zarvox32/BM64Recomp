#ifndef __PATCH_SPEECH_H__
#define __PATCH_SPEECH_H__

#include "patch_helpers.h"

// Accessibility speech output, implemented natively in src/game/speech.cpp.
// Also exported to mods under the same names.

// Speaks a NUL-terminated ASCII/UTF-8 string. A nonzero interrupt cuts off
// current speech and drops anything still queued.
DECLARE_FUNC(void, recomp_speak, const char* text, s32 interrupt);
// Stops current speech and drops anything queued.
DECLARE_FUNC(void, recomp_speech_stop);
// Returns 1 if speech output is currently active, 0 otherwise. Use it to
// skip building announcement strings when nobody will hear them.
DECLARE_FUNC(s32, recomp_speech_is_active);

#endif
