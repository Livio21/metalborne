# Keyboard and mouse controls

The SDL window collects keyboard and relative mouse input on its event thread.
The runtime maps that input to Bloodborne's PS4 controller interface. The game
continues to display PlayStation button prompts.

## Default layout

| Action | Keyboard or mouse | PS4 input |
|---|---|---|
| Move | W / A / S / D | Left stick |
| Walk | Hold Option / Alt while moving | Half-strength left stick |
| Camera | Move mouse while captured | Right stick |
| Attack | Left mouse | R1 |
| Strong / charged attack | Shift + left mouse; hold to charge | R2 |
| Firearm / parry | Right mouse | L2 |
| Dodge / backstep; hold while moving to sprint | Space | Circle |
| Interact / confirm | E or Return | Cross |
| Transform weapon | Q or mouse side button 1 | L1 |
| Heal | R or mouse side button 2 | Triangle |
| Use selected item | F | Square |
| Lock on / release lock | C or middle mouse | R3 |
| Blood bullets | 1 | D-pad up |
| Change right-hand weapon | 2 or wheel up | D-pad right |
| Change selected item | 3 or wheel down | D-pad down |
| Change left-hand weapon | 4 | D-pad left |
| Menu navigation | Arrow keys or I / J / K / L | D-pad |
| Game menu | Escape | Options |
| Left / right touchpad click | Tab / Backspace | Touchpad click |
| Left stick click | Z | L3 |
| Capture / release mouse | F8 | Host shortcut |

Use Space to cancel/back out of game menus. Escape sends Options, so it does
not replace Circle in every screen. Some Mac keyboards require Fn + F8 to send
F8 rather than a media key.

Mouse capture releases when the window loses focus, the host settings overlay
opens, or the game's native text-entry dialog opens. Keyboard game input is
also suspended during those dialogs. Capture resumes afterwards if enabled.
Mouse buttons and the wheel are game inputs only while the mouse is captured.

## Input selection and sensitivity

Set these environment variables before launching:

| Variable | Values and default |
|---|---|
| `BB_INPUT_MODE` | `auto` (default): use a connected controller, otherwise KBM. `kbm`: force KBM even with a controller connected. `gamepad`: controller only. `legacy`: controller or the original keyboard fallback. |
| `BB_MOUSE_CAPTURE` | `1` (default): capture while active. `0`: start released; F8 enables capture. |
| `BB_MOUSE_SENSITIVITY` | Number greater than 0 and at most 20; default `2.0`. Higher values turn faster. |
| `BB_MOUSE_INVERT_Y` | `1`: invert vertical camera movement. Default `0`. |

For the current macOS workspace:

```bash
BB_INPUT_MODE=kbm BB_MOUSE_SENSITIVITY=2.0 bash macos/run.sh
```

Mouse movement is converted to right-stick velocity. Bloodborne's own stick
dead zone, acceleration and maximum turn speed still apply; this is not raw
camera rotation. Camera feel and combat behaviour need checking in gameplay.
Bindings are currently defined in `src/runtime_pad.c`; arbitrary user
rebinding is not implemented.

## Original keyboard layout

`BB_INPUT_MODE=legacy` restores WASD movement, arrows for camera, Space for
Cross, left Shift for Circle, E for Square, Q for Triangle, 1/3 for L1/R1,
R/F for L2/R2, Z/C for L3/R3, Return for Options, IJKL for the D-pad and
Tab/Backspace for the two touchpad clicks. Relative mouse look is disabled.
