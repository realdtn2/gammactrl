# Gamma Control (gammactrl)

A graphical utility to adjust display gamma on KDE Plasma Wayland sessions.

Wayland does not natively support legacy X11 gamma adjustment tools like `xgamma`. This tool works around that limitation by copying your active ICC color profile (or generating a neutral one), patching the VCGT (Video Card Gamma Table) tag with your desired gamma multiplier, and applying it seamlessly using `kscreen-doctor`.

Note: HDR is not supported yet.

## Prerequisites

To build and run this application, you will need the following packages installed on your system. The exact package names may vary slightly depending on your Linux distribution.

### Build Dependencies

- A C compiler (`gcc` or `clang`)
- `cmake`
- `pkg-config`
- GTK4 development headers (e.g., `libgtk-4-dev` on Debian/Ubuntu, or `gtk4-devel` on Arch/Fedora)

### Runtime Dependencies

- `kscreen-doctor` (typically included with KDE Plasma via the `libkscreen` package)

## Building and Installing

1. Make the scripts executable:
   ```bash
   chmod +x build.sh install.sh
   ```
2. Build
   ```bash
   ./build.sh
   ```
3. Build and install:
   ```bash
   ./install.sh
   ```

Alternatively, you can run the CMake commands manually:

```bash
cmake -B build
cmake --build build
cmake --install build --prefix ~/.local
```

## Usage

After installation, launch `gammactrl` in your terminal or search for "Gamma Control" in your application launcher.

Use the slider to dynamically brighten or darken your display.

The application uses a default baseline. This should be fine unless the baseline does not align with your actual screen default gamma. To set a custom baseline for a monitor, use the **Resync Baseline** button.
