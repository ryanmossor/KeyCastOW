# KeyCastOW

Keystroke visualizer for Windows. This fork makes some changes inspired by [Screenkey](https://gitlab.com/screenkey/screenkey) for Linux. 

![Settings dialog](./settings.png)

## Changes From Original

Keystrokes are now displayed using only a single display label, rather than certain keys/mouse actions creating new display labels. 
  - Keystrokes will always be appended to the current display label unless the keyboard combination to toggle capturing (default: <kbd>Win + Shift + K</kbd>) is pressed, in which case the label will be overwritten with a &#10006; or &#10004; to indicate the status of the program.

Pressing <kbd>Backspace</kbd> will now delete the last character in the display label if the last character is not a special character (e.g., <kbd>Tab</kbd>, <kbd>Enter</kbd>, <kbd>F1</kbd>) or key combination (e.g., <kbd>Ctrl + a</kbd>).
  - If the last character in the display label is a special character or key combination, the Unicode symbol for <kbd>Backspace</kbd> will be displayed instead of deleting the character.

<kbd>Backspace</kbd>, <kbd>Tab</kbd>, <kbd>Enter</kbd>, and <kbd>Space</kbd> have been replaced with their respective Unicode symbols.

Modified settings:
  - The display label now uses a single value for "linger time," rather than separate values for keystroke delay and linger time.
  - Combination characters are now hard-coded to `<+>` (e.g., `<Ctrl+a>`)
  - Display time (previously "linger time") now has a maximum value of 60,000ms (60 seconds)
  - Fade duration now has a maximum value of 2,000ms (2 seconds)
  - Background opacity is now a percentage value rather than a 0-255 value

Removed settings:
  - Display border
  - Display corner rounding
  - Text opacity
  - Label preview
  - Maximum lines (only one display label is used)
  - Label spacing
  - Branding
  - Combination characters

## Known Issues

I didn't want the display label spanning my entire screen, so I made a code change that leaves some space on the left side of my screen. Unfortunately, this empty space is relative to the left edge of the screen, so positioning the display label anywhere other than near the right edge of the screen will shorten the maximum length of the display label. At some point, I may try making the width of the display label configurable.

## Build

`msbuild /p:platform=win32 /p:Configuration=Release`

## License

MIT License
