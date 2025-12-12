# DIM-BSP64

This is a nodebuilder for Doom 64 which has blockmap optimization and sidedef compression features. It is a fork of Diema's DMA-BSP64 (https://github.com/darkhaven3/dma-bsp64) which adds features from ZokumBSP (https://doomwiki.org/wiki/ZokumBSP) to Kaiser's D64BSP (https://github.com/MP2E/d64bsp), which is based on glBSP (https://doomwiki.org/wiki/GlBSP), which is further based on the original BSP (https://doomwiki.org/wiki/BSP_(node_builder)). 

## Installation

Copy "dim-bsp64.cfg" and "dim-bsp64.exe" into the "Compilers\Nodebuilders" folder of your Doom Builder 64 installation. Load up Doom Builder 64 then in the "Tools" menu go to the "Nodebuilder". Here you can change your configurations to "DIM-BSP64". 

## Usage

Lines can be omitted from the blockmap by using the "No Blockmap" (0x10000000) flag, OR by combining the "Always show on automap" AND "Never shown on automap" linedef flags. This can allow monsters to fall down and climb up small sectors that they may not normally be able to or it can allow for players to walk through thin sectors.

There is also the ability identical sidedefs into a single sidedef to save space. Typically lines that scroll or have switches will be excluded. The "Merge Sides" (0x20000000) flag can be used to force the merger of sidedefs associated with lines of the same tag. This forced merger can allow scrolling at faster speeds and multiple switches to be activated at once.

By using the special 999 (Blind To Monsters) on a sector, it can be used to break a monster's line of sight from or to a sector, making them effectively blind, but they can still hear unless flagged not to (Ambush).