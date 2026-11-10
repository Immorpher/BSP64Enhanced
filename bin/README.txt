# BSP64 Enhanced 1.17

This is a multi-threaded nodebuilder for Doom 64 which has blockmap optimization, sidedef compression, and reject optimization features. It originates from Diema's DMA-BSP64 (https://github.com/darkhaven3/dma-bsp64) which adds features from ZokumBSP (https://doomwiki.org/wiki/ZokumBSP) to Kaiser's D64BSP (https://github.com/MP2E/d64bsp), which is based on glBSP (https://doomwiki.org/wiki/GlBSP), which is further based on the original BSP (https://doomwiki.org/wiki/BSP_(node_builder)).

## Installation

BSP64 Enhanced is designed to work with any Doom Builder 64. Copy "BSP64Enhanced.cfg" and "BSP64Enhanced.exe" into the "Compilers\Nodebuilders" folder of your Doom Builder 64 installation. Load up Doom Builder 64 then in the "Tools" menu go to the "Nodebuilder". Here you can change your configurations to "BSP64 Enhanced". There will be two options for node building which will be described in the following section.

## Usage

The `BLOCKMAP` and `SIDEDEFS` lumps are limited to 65535 bytes and 65535 side definitions respectively. BSP64 Enhanced will automatically compress these two lumps without any significant gameplay changes. This allows mappers to make larger maps (than without compression) without needing to manually edit their maps. Lines which lack tags, specials, flags which are involved with collisions, and which border sectors of identical floor and ceiling heights, will automatically excluded from the blockmap. Duplicate sidedefs which contain the same information will be merged, unless they are referenced by a linedef which has a tag, scroll flags or have switch display flags. Sidedefs whose upper, lower and middle textures are all "BLANK" or "?" (missing) are also merged when only their X and Y offsets differ, since the offsets of a texture which is not drawn cannot be seen. 

`REJECT` lump is built from real line of sight between sectors. A sector pair is hidden only when no straight line can run between them without crossing a one-sided line. This can result in performance improvements when maps have many idle enemies.

The `VERTEXES`, `SEGS`, `SSECTORS`, `NODES` and `LEAFS` lumps are kept as small as possible automatically as BSP tree is built with partition cost presets that avoid seg splits much more strongly than the classic glBSP cost, and that also charge every sector which a partition would cut in two (each cut sector needs at least one more subsector, node and LEAFS list). Maps with up to 4000 linedefs try four presets and keep the one giving the smallest lumps; larger maps use the best all-round preset directly, so build times stay short. Using `-c` (split cost) or `-f` (fast) skips the preset search and uses the classic cost. `-xt` does the same without changing anything else.

BSP64 Enhanced adds a few extra mapping features and a second more-aggressive blockmap compression mode as described in the following sections.

### Normal Mode

Lines can be manually omitted from the blockmap by using the "No Blockmap" (0x10000000) flag, OR by combining the "Always show on automap" AND "Never shown on automap" linedef flags. Besides reducing the blockmap size, this can allow monsters to fall down ledges and climb up small stairs that they may not normally be able to. Or it can allow for players to walk through thin sectors or reach items from ledges.

Lines of the same tag can be forced to merge their sidedefs, with the "Merge Sides" (0x20000000) flag. This forced merger can allow texture scrolling at faster speeds and multiple switches to be activated at once. Be careful when manually merging sidedefs across multiple sectors as sidedefs link to specific sectors.

Sectors can be removed from the `REJECT` lump with the special 999 (Blind To Monsters). Such a sector is hidden from every other sector and sees none of them, which makes monsters blind to things in or at that sector.

### Compress Edges Mode

In addition to all of the features in the normal mode, the compress edges mode will more aggressively omit lines from the blockmap. Specifically non-special lines that border sectors that have floors within 16 units high (or lower) and the ceiling gap from the floor is at least 64 units high (or greater) will be omitted. Small steps and gaps are preserved in the blockmap for traversal and collision reasons. Consequences of this allows enemies to traverse stairs and steps more easily. There are rare cases where a player might be able to approach a wall too closely. If this is undesirable a sector can be tagged or the line definitions can be made into special lines to exclude them from blockmap compression.

Special lines are line definitions that contain a special (actions, macros, and switch properties), have a non-zero tag, border sectors with differing tags, or use any flags which specifically block movement (one-sided, block projectiles, block monsters, and ect...). These special lines are excluded from edge compression, thus any of these properties can be added to any line (or a tag to a sector) where you may not want enemies or players being able to approach closely to.

### Command-line Commands

These additional options can be added to the nodebuilder configurations of the "BSP64Enhanced.cfg" file or they can be used when running the nodebuilder from command-line.

General Options:
+  -q  -quiet         Quieter output, no level stats
+  -f  -fast          Reuse original nodes to build faster
+  -w  -warn          Show extra warning messages
+  -xr -noreject      Don't clobber the REJECT map
+  -e  -compedge      Compress edges by blockmap removal
+  -nb -noblockrem    Don't remove lines from blockmap

Advanced Options:
+  -m  -mergevert     Merge duplicate vertices
+  -b  -maxblock ###  Sets the BLOCKMAP truncation limit
+  -c  -factor ###    Sets the cost assigned to SEG splits
+  -xp -noprog        Don't show progress indicator
+  -xu -noprune       Never prune linedefs or sidedefs
+  -s  -skipselfref   Ignore self referencing lines
+  -xt -notrials      Build the tree once with the classic glBSP cost
+  -xo -noorigin      Skip the blockmap origin search