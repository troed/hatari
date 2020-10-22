
#ifndef HATARI_SDL_H
#define HATARI_SDL_H

//RETRO HACK
//#warning This is just an SDL wrapper for the retro core.

// Since Hatari makes use of SDL for video, defines are used to null out everything
// except DrawRects, which we intercept to instead render onto our framebuffer.
// The latter is then returned in the libretro run loop.

extern int Reset_Cold(void);
extern int Reset_Warm(void);

#include <unistd.h>
#include <time.h>

#include <SDL_types.h>
#include <SDL_render.h>
#include "SDL_keyboard.h"
#include "SDL_video.h"
#include "SDL_joystick.h"

#define SDL_GetTicks  GetTicks 
#include "SDL_types.h"

#define RGB565(r, g, b)  (((r) << (5+6)) | ((g) << 6) | (b))
#define SDL_MapRGB(a, r, g, b) RGB565( (r)>>3, (g)>>3, (b)>>3)
extern long GetTicks(void);

//extern void retro_fillrect(SDL_Surface * surf,SDL_Rect *rect,unsigned int col);
extern void retro_updaterects(SDL_Surface * surf, int num, SDL_Rect *rects);
extern SDL_Surface *retro_creatergbsurface(Uint32 f,int w,int h,int d,Uint32 rmask,Uint32 gmask,Uint32 bmask,Uint32 amask);
extern SDL_Surface *prepare_texture(int w,int h,int b);
//extern int SDL_SaveBMP(SDL_Surface *surface,const char *file);

typedef struct SDL_Event{
Uint8 type;
} SDL_Event;

//SOME SDL_FUNC WRAPPER
//GLOBALS
#define SDL_ShowCursor(a) 0
//#define SDL_GRAB_OFF 0
//#define SDL_GRAB_ON 1
#define SDL_WM_GrabInput(a)
#define SDL_WM_IconifyWindow()
#define SDL_WM_SetCaption(...)
#define SDL_WM_SetIcon(...)
#define SDL_HWSURFACE 0
#define SDL_FULLSCREEN 1
//#define SDL_SWSURFACE 2 // defined in SDL_surface.h to 0
#define SDL_HWPALETTE 4
#define SDL_INIT_NOPARACHUTE 1
#define SDL_DISABLE 0
#define SDL_Quit(...)
#define SDL_InitSubSystem(...) 1
#define SDL_Init(...) 1
//TIME
#if defined(WIIU) || defined(VITA)
#define SDL_Delay(a) //we are awake
#else
#define SDL_Delay(a) usleep((a)*1000)
#endif
//SURFACE
#define SDL_getenv(a) NULL
//#define SDL_CreateRenderer(...) 1;return true // Hatari checks non-NULL
// SDL 2.0 path, needs to return our SDL surface
#define SDL_CreateRGBSurface(f, w, h, d, rmask, gmask, bmask, amask) retro_creatergbsurface(f,w,h,d,rmask,gmask,bmask,amask)
//#define SDL_CreateTexture(...) NULL
//#define SDL_CreateWindow(...) 1 // Hatari checks non-NULL
#define SDL_CreateWindow(t,x,y,w,h,f) SDL_CreateWindow(t,x,y,w,h,f|SDL_WINDOW_HIDDEN)
//#define SDL_DestroyTexture(a)
//#define SDL_DestroyWindow(a)
#define SDL_FillRect(s,r,c)
#define SDL_Flip(a)
#define SDL_FreeSurface(a)
#define SDL_GetDesktopDisplayMode(...) 0
#define SDL_GetWindowFlags(a) SDL_WINDOW_MAXIMIZED
#define SDL_GetWindowSurface(a) NULL
//#define SDL_LoadBMP(a) // defined in SDL_surface
#define SDL_LockSurface(a) 0
//#define SDL_MUSTLOCK(a) 0 // defined in SDL_surface
#define SDL_RenderClear(a)
#define SDL_RenderCopy(...)
#define SDL_RenderPresent(...)
#define SDL_RenderSetLogicalSize(...)
#define SDL_RenderSetScale(...)
//#define SDL_SetColors(a, b, c, d)
#define SDL_SetColorKey(...)
#define SDL_SetHintWithPriority(...)
#define SDL_SetRenderDrawColor(...)
#define SDL_SetVideoMode(w, h, b, f) prepare_texture((w),(h),(b)) // SDL 1.2 path, needs to return our SDL surface
#define SDL_SetWindowIcon(...)
#define SDL_SetWindowSize(...)
#define SDL_UnlockSurface(a) 0
#define SDL_UpdateRect(s, x, y, w, h) retro_updaterect(s,x,y,w,h)
#define SDL_UpdateRects(s, n, r) retro_updaterects(s,n,r)
#define SDL_UpdateTexture(...)
#define SDL_UpdateWindowSurfaceRects(...)

//KEY
#define SDL_GetError() "RetroWrapper"
#define SDL_GetModState() 0
#define SDL_GetKeyName(...) "RetroWrapper"
//SOUND
#define SDL_CloseAudio();
#define SDL_LockAudio();
#define SDL_UnlockAudio();
#define SDL_PauseAudio(a);

//MOUSE
#define SDL_GetMouseState(X,Y) GuiGetMouseState((X),(Y))

//PS3 HACK
#if defined(__CELLOS_LV2__) 
#include <unistd.h> //stat() is defined here
#define S_ISDIR(x) (x & CELL_FS_S_IFDIR)
#define F_OK 0

#include "sys/sys_time.h"
#include "sys/timer.h"
#define usleep  sys_timer_usleep
#endif


#endif
