# DIM-BSP64

This is a nodebuilder for Doom 64 which has blockmap optimization and sidedef compression features. It is a fork of Diema's DMA-BSP64 which adds features from ZokumBSP (https://doomwiki.org/wiki/ZokumBSP) to Kaiser's BSP64, which is based on glBSP (https://doomwiki.org/wiki/GlBSP), which is further based on the original BSP (https://doomwiki.org/wiki/BSP_(node_builder)). 

## Usage

Lines can be omitted from the blockmap by providing a negative tag, OR by combining the "Always show on automap" AND "Never shown on automap" linedef flags. It will also compress identical sidedefs into a single sidedef.

## Building

### Installing MSYS2

First you will need the command-line software building package for Windows called MSYS2 here: https://www.msys2.org/

For this example, MSYS2 is installed into this directory: "C:\msys64"

After installation, msys64 will open up automatically, otherwise go to "C:\msys64" and run "msys2.exe".

In the "msys64" command window, check for updates with the command "pacman -Syu".
Enter "Y" to proceed with updates if it finds updates to apply. The command window may close after updates, run "msys2.exe" again if this happens. Keep checking for updates with "pacman -Syu" until all updates are done it will say "there is nothing to do".

### Installing the Compiler

In the MSYS2 folder, "C:\msys64\" in this example, open "mingw32.exe".

Install make function with the command "pacman -S make".

Install the GCC toolchain with "pacman -S mingw-w64-i686-toolchain", choose option "3) mingw-w64-i686-gcc", and enter "Y" to proceed with the installation.

Check the GCC version with "gcc --version" and if it shows the GCC version, then the installation has worked. Otherwise perhaps the wrong option was chosen in the previous step.

### Compiling DIM-BSP64

Clone this repository to a subdirectory of the "msys64" folder, which in this case is "C:\msys64\dim-bsp64".

In the "mingw32.exe" command window, move to the DIM-BSP64 directory, which in this case is done with the "cd 'C:\msys64\dim-bsp64'" command.

Compile DIM-BSP64 with the command "make".

In the "bin" folder of the DIM-BSP64 directory you will now have the nodebuilder executables.
