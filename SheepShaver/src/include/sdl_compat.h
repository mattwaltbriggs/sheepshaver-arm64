#ifndef SDL_COMPAT_H
#define SDL_COMPAT_H

#if __has_include(<SDL/SDL.h>)
#include <SDL/SDL.h>
#elif __has_include(<SDL.h>)
#include <SDL.h>
#elif __has_include(<SDL2/SDL.h>)
#include <SDL2/SDL.h>
#else
#error "SDL headers not found"
#endif

#if __has_include(<SDL/SDL_audio.h>)
#include <SDL/SDL_audio.h>
#elif __has_include(<SDL_audio.h>)
#include <SDL_audio.h>
#elif __has_include(<SDL2/SDL_audio.h>)
#include <SDL2/SDL_audio.h>
#endif

#if __has_include(<SDL/SDL_mutex.h>)
#include <SDL/SDL_mutex.h>
#elif __has_include(<SDL_mutex.h>)
#include <SDL_mutex.h>
#elif __has_include(<SDL2/SDL_mutex.h>)
#include <SDL2/SDL_mutex.h>
#endif

#if __has_include(<SDL/SDL_thread.h>)
#include <SDL/SDL_thread.h>
#elif __has_include(<SDL_thread.h>)
#include <SDL_thread.h>
#elif __has_include(<SDL2/SDL_thread.h>)
#include <SDL2/SDL_thread.h>
#endif

#if __has_include(<SDL/SDL_syswm.h>)
#include <SDL/SDL_syswm.h>
#elif __has_include(<SDL_syswm.h>)
#include <SDL_syswm.h>
#elif __has_include(<SDL2/SDL_syswm.h>)
#include <SDL2/SDL_syswm.h>
#endif

#endif
