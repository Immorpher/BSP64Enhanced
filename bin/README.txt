# BSP64 Enhanced 1.1

This is a nodebuilder for Doom 64 which has blockmap optimization, sidedef compression, and reject editing features. It is a fork of Diema's DMA-BSP64 (https://github.com/darkhaven3/dma-bsp64) which adds features from ZokumBSP (https://doomwiki.org/wiki/ZokumBSP) to Kaiser's D64BSP (https://github.com/MP2E/d64bsp), which is based on glBSP (https://doomwiki.org/wiki/GlBSP), which is further based on the original BSP (https://doomwiki.org/wiki/BSP_(node_builder)). 

## Installation

Copy "BSP64Enhanced.cfg" and "BSP64Enhanced.exe" into the "Compilers\Nodebuilders" folder of your Doom Builder 64 installation. Load up Doom Builder 64 then in the "Tools" menu go to the "Nodebuilder". Here you can change your configurations to "BSP64 Enhanced". There will be two options for node building which will be described in the following section.

## Usage

In the Doom 64 map format, the "BLOCKMAP" and "SIDEDEFS" lumps are limited to 65535 bytes and 65535 side definitions respectively. BSP64 Enhanced will automatically compress these two lumps without any significant gameplay changes. This allows mappers to make larger maps (than without compression) without needing to manually edit their maps. Lines which lack tags, specials, flags which are involved with collisions, and which border sectors of identical floor and ceiling heights, will automatically excluded from the blockmap. Duplicate sidedefs which contain the same information will be merged, unless they are referenced by a linedef which has a tag, scroll flags or have switch display flags. 

BSP64 Enhanced adds a few extra mapping features and a second more-aggressive blockmap compression mode as described in the following sections.

### Normal Mode

Lines can be manually omitted from the blockmap by using the "No Blockmap" (0x10000000) flag, OR by combining the "Always show on automap" AND "Never shown on automap" linedef flags. Besides reducing the blockmap size, this can allow monsters to fall down ledges and climb up small stairs that they may not normally be able to. Or it can allow for players to walk through thin sectors or reach items from ledges.

Lines of the same tag can be forced to merge their sidedefs, with the "Merge Sides" (0x20000000) flag. This forced merger can allow texture scrolling at faster speeds and multiple switches to be activated at once.

Sectors can be removed from the "REJECT" lump with the special 999 (Blind To Monsters). It can be used to break a monster's line of sight from or to a sector, making them effectively blind to things outside of their sector or at a specific sector.

### Compress Edges Mode

In addition to all of the features in the normal mode, the compress edges mode will more aggressively omit lines from the blockmap. Specifically non-special lines that border sectors that are only 32 units high (or lower) and the ceiling gap is 64 units high (or greater) will be omitted. Consequences of this allows enemies to fall down from these ledges and transverse stairs more easily too. The player can approach some of these edges a bit closer than normal too.

Special lines are lines that contain a special (actions, macros, and switch properties), have a non-zero tag, border sectors with differing tags, or use any flags which specifically block movement (one-sided, block projectiles, block monsters, and ect...). These special lines are excluded from edge compression, thus any of these properties can be added to any line (or a tag to a sector) where you may not want enemies or players being able to approach closely to.

### Command-line Commands

These additional options can be added to the nodebuilder configurations of the "BSP64Enhanced.cfg" file or they can be used when running the nodebuilder from command-line.

General Options:
+  -q  -quiet         Quieter output, no level stats
+  -f  -fast          Reuse original nodes to build faster
+  -w  -warn          Show extra warning messages
+  -n  -normal        Forces the normal nodes to be rebuilt
+  -xr -noreject      Don't clobber the REJECT map
+  -e  -compedge      Compress edges by blockmap removal
+  -nb -noblockrem    Don't remove lines from blockmap

Advanced Options:
+  -v1 .. -v5         Version of GL-Nodes to use (1,2,3 or 5)
+  -m  -mergevert     Merge duplicate vertices
+  -y  -windowfx      Handle the 'One-Sided Window' trick
+  -u  -prunesec      Remove unused sectors
+  -b  -maxblock ###  Sets the BLOCKMAP truncation limit
+  -c  -factor ###    Sets the cost assigned to SEG splits
+  -xn -nonormal      Don't add (if missing) the normal nodes
+  -xp -noprog        Don't show progress indicator
+  -xu -noprune       Never prune linedefs or sidedefs
+  -s  -skipselfref   Ignore self referencing lines