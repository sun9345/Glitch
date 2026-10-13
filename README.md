# Glitch

Windows tool for glitching images. Effects can be stacked in any order (repeats allowed).

- **GUI** (`glitch_gui.cpp`) — preview, undo/redo, drag & drop, per-effect settings
- **Console** (`glitch.cpp`) — pick a file and a sequence of operations, write `name_glitch.jpg` / `.png`

Supported formats: JPG, PNG, BMP, TGA (via [stb](https://github.com/nothings/stb)).

## Effects

- RGB split
- Horizontal / vertical slicing
- Contrast
- Pixel sorting
- Pixel smear
- RGB channel multiplication
- RGB channel displacement
- Invert colors
- JPEG compression corruption
- Pixel displacement *(needs a second image as a map)*
- Subtract blending *(needs a second image)*

In the GUI, open two files or drop them together to use a displacement map. In the console, type two paths on one line (quote paths with spaces).

## Build

From the `Glitch` project folder (`cd Glitch` after clone):

```
Build (MSVC):  cl /std:c++17 /O2 /EHsc glitch_gui.cpp
Build (MinGW): g++ -std=c++17 -O2 -mwindows glitch_gui.cpp -o glitch_gui -lgdi32 -lcomdlg32 -lshell32 -luser32
```

C++17, Windows. No extra libraries: `stb_image.h` and `stb_image_write.h` are already in the repo.

## License

MIT. `stb_image` / `stb_image_write` are by Sean Barrett (MIT or public domain). Details are in `LICENSE`.
