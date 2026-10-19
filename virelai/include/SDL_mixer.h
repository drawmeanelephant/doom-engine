/*
 * virelai/include/SDL_mixer.h — stub for i_sound.c's FEATURE_SOUND include.
 *
 * doomgeneric's i_sound.c does `#include <SDL_mixer.h>` when FEATURE_SOUND
 * is defined (non-DJGPP). It uses nothing from the header — the SDL mixer
 * calls all live in i_sdlsound.c, which this port does not compile — so an
 * empty translation unit satisfies it. This port's mixer is
 * virelai/vl_sound.c on the sys_audio stream ABI.
 */

#ifndef VIRELAI_STUB_SDL_MIXER_H
#define VIRELAI_STUB_SDL_MIXER_H

#endif
