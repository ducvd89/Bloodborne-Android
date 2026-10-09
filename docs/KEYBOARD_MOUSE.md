# Local keyboard and mouse controls

This build includes mouse-to-right-stick camera control. The enabled profile is
saved in `bbport.ini`. Restart the game after editing bindings. Game prompts
continue to show PlayStation buttons. A connected controller also works.

| Action | Keyboard / mouse | PS4 input |
| --- | --- | --- |
| Move | WASD | Left stick |
| Camera | Mouse; IJKL as a fallback | Right stick |
| Attack | Left click; 3 as a fallback | R1 |
| Heavy / charged attack | Right click (hold to charge); 4 | R2 |
| Firearm / left weapon attack | Mouse side button X1 or Left Ctrl | L2 |
| Transform weapon | F | L1 |
| Lock on / reset camera | Middle click or C | R3 |
| Dodge / sprint | Left Shift or mouse side button X2; hold while moving to sprint | Circle |
| Interact / confirm | E or Space | Cross |
| Use selected item | R | Square |
| Heal | Q | Triangle |
| Game menu | Enter | Options |
| Menu navigation | Arrow keys or WASD | D-pad / left stick |
| D-pad shortcuts | 1 up, 2 down, Z left, X right | D-pad |
| Gestures | Tab | Left touchpad click |
| Personal effects | Backspace | Right touchpad click |
| Left stick click | V | L3 |
| Port graphics overlay | Insert | Host shortcut |
| Release / recapture mouse | F10 | Host shortcut |

Use **Play Offline** at the title screen. For menu cancellation, use Shift
(Circle). Enter opens the game menu. Escape closes the port overlay when that is open.

Mouse sensitivity and Y inversion can be changed in the launcher's Controls
section. Default: `mouse_sensitivity=0.10`, `mouse_invert_y=0`. The game still
receives a virtual stick, so camera speed is limited by the game's controller
camera behavior. This is not direct, unrestricted mouse aiming.

The cursor is captured while the game is focused, and released for the port
overlay, character-name text entry, Alt-Tab or F10. Mouse attacks and camera
movement are suppressed while released; unfocused input is neutral. The mouse
is recaptured when returning to the game unless F10 released it manually.

Bindings accept SDL keyboard names and `Mouse Left`, `Mouse Right`,
`Mouse Middle`, `Mouse X1`, `Mouse X2`, separated by commas, for example:

```ini
mouse_look=1
mouse_sensitivity=0.10
mouse_invert_y=0
key.r1=Mouse Left, 3
key.r2=Mouse Right, 4
key.r3=Mouse Middle, C
```

Disable the mouse profile with `mouse_look=0` and use keyboard or controller.
Original graphics and keyboard settings are backed up in
`out/bbport-before-keyboard-mouse.ini`.

Validation: `bash build.sh --test` checks button/trigger translation, keyboard
movement, camera direction/clamping/inversion, repeated pad reads, focus/overlay
suppression and the motion accumulator. Camera feel in gameplay still requires
player testing.

Bloodborne's firearm, lock-on, healing and weapon transformation controls are
described in [PlayStation's official tips](https://blog.playstation.com/2015/03/23/bloodborne-24-tips-for-survival/).
