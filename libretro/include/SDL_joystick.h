#ifndef HATARI_JOYSTICK_SDL_H
#define HATARI_JOYSTICK_SDL_H

//JOY
#define SDL_Joystick int
#define SDL_NumJoysticks() 0
#define SDL_JoystickOpen(i) NULL
#define SDL_JoystickName(i) "RetroWrapper"
#define SDL_JoystickClose
#define SDL_JoystickGetAxis(...) 0
#define SDL_JoystickGetButton(...) 0
#define SDL_JoystickNumAxes(...) 0
#define SDL_JoystickNumButtons(a) 16

#define SDL_HAT_CENTERED    0x00
#define SDL_HAT_UP          0x01
#define SDL_HAT_RIGHT       0x02
#define SDL_HAT_DOWN        0x04
#define SDL_HAT_LEFT        0x08
#define SDL_HAT_RIGHTUP     (SDL_HAT_RIGHT|SDL_HAT_UP)
#define SDL_HAT_RIGHTDOWN   (SDL_HAT_RIGHT|SDL_HAT_DOWN)
#define SDL_HAT_LEFTUP      (SDL_HAT_LEFT|SDL_HAT_UP)
#define SDL_HAT_LEFTDOWN    (SDL_HAT_LEFT|SDL_HAT_DOWN)

#endif // HATARI_JOYSTICK_SDL_H