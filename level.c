//------------------------------------------------------------------------
// LEVEL : Level structure read/write functions.
//------------------------------------------------------------------------
//
//  GL-Friendly Node Builder (C) 2000-2007 Andrew Apted
//
//  Based on 'BSP 2.3' by Colin Reed, Lee Killough and others.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2
//  of the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//------------------------------------------------------------------------
//
//  ZDBSP format support based on code (C) 2002,2003 Randy Heit
// 
//------------------------------------------------------------------------

#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <assert.h>
#include <stdint.h>

#include "analyze.h"
#include "blockmap.h"
#include "level.h"
#include "node.h"
#include "reject.h"
#include "seg.h"
#include "structs.h"
#include "util.h"
#include "wad.h"


#define DEBUG_LOAD      0
#define DEBUG_BSP       0

#define ALLOC_BLKNUM  1024


// per-level variables




#define LEVELARRAY(TYPE, BASEVAR, NUMVAR)  \
    TYPE ** BASEVAR = NULL;  \
    int NUMVAR = 0;


LEVELARRAY(vertex_t,  lev_vertices,   num_vertices)
LEVELARRAY(linedef_t, lev_linedefs,   num_linedefs)
LEVELARRAY(sidedef_t, lev_sidedefs,   num_sidedefs)
LEVELARRAY(sector_t,  lev_sectors,    num_sectors)
LEVELARRAY(thing_t,   lev_things,     num_things)

static LEVELARRAY(seg_t,     segs,       num_segs)
static LEVELARRAY(subsec_t,  subsecs,    num_subsecs)
static LEVELARRAY(node_t,    nodes,      num_nodes)
static LEVELARRAY(wall_tip_t,wall_tips,  num_wall_tips)

int num_normal_vert = 0;
int num_gl_vert = 0;
int num_complete_seg = 0;


/* ----- allocation routines ---------------------------- */

#define ALLIGATOR(TYPE, BASEVAR, NUMVAR)  \
{  \
  if ((NUMVAR % ALLOC_BLKNUM) == 0)  \
  {  \
    BASEVAR = UtilRealloc(BASEVAR, (NUMVAR + ALLOC_BLKNUM) *   \
        sizeof(TYPE *));  \
  }  \
  BASEVAR[NUMVAR] = (TYPE *) UtilCalloc(sizeof(TYPE));  \
  NUMVAR += 1;  \
  return BASEVAR[NUMVAR - 1];  \
}


vertex_t *NewVertex(void)
  ALLIGATOR(vertex_t, lev_vertices, num_vertices)

linedef_t *NewLinedef(void)
  ALLIGATOR(linedef_t, lev_linedefs, num_linedefs)

sidedef_t *NewSidedef(void)
  ALLIGATOR(sidedef_t, lev_sidedefs, num_sidedefs)

sector_t *NewSector(void)
  ALLIGATOR(sector_t, lev_sectors, num_sectors)

thing_t *NewThing(void)
  ALLIGATOR(thing_t, lev_things, num_things)

seg_t *NewSeg(void)
  ALLIGATOR(seg_t, segs, num_segs)

subsec_t *NewSubsec(void)
  ALLIGATOR(subsec_t, subsecs, num_subsecs)

node_t *NewNode(void)
  ALLIGATOR(node_t, nodes, num_nodes)

wall_tip_t *NewWallTip(void)
  ALLIGATOR(wall_tip_t, wall_tips, num_wall_tips)


/* ----- free routines ---------------------------- */

#define FREEMASON(TYPE, BASEVAR, NUMVAR)  \
{  \
  int i;  \
  for (i=0; i < NUMVAR; i++)  \
    UtilFree(BASEVAR[i]);  \
  if (BASEVAR)  \
    UtilFree(BASEVAR);  \
  BASEVAR = NULL; NUMVAR = 0;  \
}


void FreeVertices(void)
  FREEMASON(vertex_t, lev_vertices, num_vertices)

void FreeLinedefs(void)
  FREEMASON(linedef_t, lev_linedefs, num_linedefs)

void FreeSidedefs(void)
  FREEMASON(sidedef_t, lev_sidedefs, num_sidedefs)

void FreeSectors(void)
  FREEMASON(sector_t, lev_sectors, num_sectors)

void FreeThings(void)
  FREEMASON(thing_t, lev_things, num_things)

void FreeSegs(void)
  FREEMASON(seg_t, segs, num_segs)

void FreeSubsecs(void)
  FREEMASON(subsec_t, subsecs, num_subsecs)

void FreeNodes(void)
  FREEMASON(node_t, nodes, num_nodes)

void FreeWallTips(void)
  FREEMASON(wall_tip_t, wall_tips, num_wall_tips)


/* ----- lookup routines ------------------------------ */

#define LOOKERUPPER(BASEVAR, NUMVAR, NAMESTR)  \
{  \
  if (index < 0 || index >= NUMVAR)  \
    FatalError("No such %s number #%d", NAMESTR, index);  \
    \
  return BASEVAR[index];  \
}

vertex_t *LookupVertex(int index)
  LOOKERUPPER(lev_vertices, num_vertices, "vertex")

linedef_t *LookupLinedef(int index)
  LOOKERUPPER(lev_linedefs, num_linedefs, "linedef")
  
sidedef_t *LookupSidedef(int index)
  LOOKERUPPER(lev_sidedefs, num_sidedefs, "sidedef")
  
sector_t *LookupSector(int index)
  LOOKERUPPER(lev_sectors, num_sectors, "sector")
  
subsec_t *LookupSubsec(int index)
  LOOKERUPPER(subsecs, num_subsecs, "subsector")
  

/* ----- reading routines ------------------------------ */


//
// GetVertices
//
void GetVertices(void)
{
  int i, count=-1;
  raw_vertex_t *raw;
  lump_t *lump = FindLevelLump("VERTEXES");

  if (lump)
    count = lump->length / sizeof(raw_vertex_t);

  DisplayTicker();

# if DEBUG_LOAD
  PrintDebug("GetVertices: num = %d\n", count);
# endif

  if (!lump || count == 0)
    FatalError("Couldn't find any Vertices");

  raw = (raw_vertex_t *) lump->data;

  for (i=0; i < count; i++, raw++)
  {
    vertex_t *vert = NewVertex();

    vert->x = (float_g) SINT32(raw->x) / 65536.0;
    vert->y = (float_g) SINT32(raw->y) / 65536.0;

    vert->index = i;
  }

  num_normal_vert = num_vertices;
  num_gl_vert = 0;
  num_complete_seg = 0;
}

//
// GetSectors
//
void GetSectors(void)
{
  int i, count=-1;
  raw_sector_t *raw;
  lump_t *lump = FindLevelLump("SECTORS");

  if (lump)
    count = lump->length / sizeof(raw_sector_t);

  if (!lump || count == 0)
    FatalError("Couldn't find any Sectors");

  DisplayTicker();

# if DEBUG_LOAD
  PrintDebug("GetSectors: num = %d\n", count);
# endif

  raw = (raw_sector_t *) lump->data;

  for (i=0; i < count; i++, raw++)
  {
    sector_t *sector = NewSector();

    sector->floor_h = SINT16(raw->floor_h);
    sector->ceil_h  = SINT16(raw->ceil_h);

//    memcpy(sector->floor_tex, raw->floor_tex, sizeof(sector->floor_tex));
 //   memcpy(sector->ceil_tex,  raw->ceil_tex,  sizeof(sector->ceil_tex));

	sector->floor_index = UINT16(raw->floor_index);
	sector->ceil_index = UINT16(raw->ceiling_index);

	sector->colors[0] = UINT16(raw->clr_ceiling);
	sector->colors[1] = UINT16(raw->clr_floor);
	sector->colors[2] = UINT16(raw->clr_thing);
	sector->colors[3] = UINT16(raw->clr_upper);
	sector->colors[4] = UINT16(raw->clr_lower);

    //sector->light = UINT16(raw->light);
    sector->special = UINT16(raw->special);
    sector->tag = UINT16(raw->tag);
	sector->flag = UINT16(raw->flag);

    sector->coalesce = (sector->tag >= 900 && sector->tag < 1000) ?
        TRUE : FALSE;

    /* sector indices never change */
    sector->index = i;

    sector->warned_facing = -1;

    /* Note: rej_* fields are handled completely in reject.c */
  }
}

//
// GetThings
//
void GetThings(void)
{
  int i, count=-1;
  raw_thing_t *raw;
  lump_t *lump = FindLevelLump("THINGS");

  if (lump)
    count = lump->length / sizeof(raw_thing_t);

  if (!lump || count == 0)
  {
    // Note: no error if no things exist, even though technically a map
    // will be unplayable without the player starts.
    PrintWarn("Couldn't find any Things!\n");
    return;
  }

  DisplayTicker();

# if DEBUG_LOAD
  PrintDebug("GetThings: num = %d\n", count);
# endif

  raw = (raw_thing_t *) lump->data;

  for (i=0; i < count; i++, raw++)
  {
    thing_t *thing = NewThing();

    thing->x = SINT16(raw->x);
    thing->y = SINT16(raw->y);

    thing->type = UINT16(raw->type);
    thing->options = UINT16(raw->options);
	thing->tid = UINT16(raw->tid);

    thing->index = i;
  }
}

//
// GetSidedefs
//
void GetSidedefs(void)
{
  int i, count=-1;
  raw_sidedef_t *raw;
  lump_t *lump = FindLevelLump("SIDEDEFS");

  if (lump)
    count = lump->length / sizeof(raw_sidedef_t);

  if (!lump || count == 0)
    FatalError("Couldn't find any Sidedefs");

  DisplayTicker();

# if DEBUG_LOAD
  PrintDebug("GetSidedefs: num = %d\n", count);
# endif

  raw = (raw_sidedef_t *) lump->data;

  for (i=0; i < count; i++, raw++)
  {
    sidedef_t *side = NewSidedef();

    side->sector = (SINT16(raw->sector) == -1) ? NULL :
        LookupSector(UINT16(raw->sector));

    if (side->sector)
      side->sector->ref_count++;

    side->x_offset = SINT16(raw->x_offset);
    side->y_offset = SINT16(raw->y_offset);

    //memcpy(side->upper_tex, raw->upper_tex, sizeof(side->upper_tex));
    //memcpy(side->lower_tex, raw->lower_tex, sizeof(side->lower_tex));
    //memcpy(side->mid_tex,   raw->mid_tex,   sizeof(side->mid_tex));

	side->upper_index = UINT16(raw->upper_index);
	side->lower_index = UINT16(raw->lower_index);
	side->mid_index = UINT16(raw->mid_index);

    /* sidedef indices never change */
    side->index = i;
  }
}

// immorpher: the engine swaps the texture of a sidedef when a line that has a
// special (an action type, or a macro) and the switch texture flags is used.
// Such a sidedef must stay unique, otherwise every line that was merged into
// it would change texture as well.  (Note that line->type is not set for Doom 64
// lines, which meant that on_special never got set before.)
static int LineChangesSidedefTexture(const linedef_t *line)
{
  int has_special = (line->specials[2] != 0) ||
      (line->specials[3] & 0x01);  // action type, or macro flag

  return has_special &&
      (line->flags & (LINEFLAG_DECAL_UPPER | LINEFLAG_DECAL_LOWER));
}

// immorpher: decide if the sidedef of a line may be merged with other sidedefs.
// A sidedef can be referenced by more than one line, so the result must not depend
// on the order the lines are read in: if any of the lines has the "Merge Sides"
// flag the sidedef is merged using the tag of the first such line, otherwise it is
// never merged when any of the lines are scrollers/switches/tagged lines.
static void SetSidedefMergeInfo(sidedef_t *side, const linedef_t *line)
{
  if (line->specials[1] & LINEFLAG_COMB_SIDEDEF)
  {
    // immorpher: force the merger of sidedefs if flagged
    if (side->merge_side != 1)
    {
      side->merge_side = 1;
      side->tag = line->tag;
    }
  }
  else if (side->merge_side == 0 &&
      (line->tag != 0 || line->specials[0] & LINEFLAG_SCROLL_LEFT || line->specials[0] & LINEFLAG_SCROLL_RIGHT || line->specials[0] & LINEFLAG_SCROLL_UP || line->specials[0] & LINEFLAG_SCROLL_DOWN || line->flags & LINEFLAG_DISPLAY_UPPER || line->specials[0] & LINEFLAG_DISPLAY_LOWER))
  {
    // immorpher: do not merge scrollers/switches/tagged lines
    side->merge_side = 2;
  }
}

static INLINE_G sidedef_t *SafeLookupSidedef(uint16_g num)
{
  if (num == 0xFFFF)
    return NULL;

  if ((int)num >= num_sidedefs && (sint16_g)(num) < 0)
    return NULL;

  return LookupSidedef(num);
}

//
// GetLinedefs
//
void GetLinedefs(void)
{
  int i, count=-1;
  raw_linedef_t *raw;
  lump_t *lump = FindLevelLump("LINEDEFS");

  if (lump)
    count = lump->length / sizeof(raw_linedef_t);

  if (!lump || count == 0)
    FatalError("Couldn't find any Linedefs");

  DisplayTicker();

# if DEBUG_LOAD
  PrintDebug("GetLinedefs: num = %d\n", count);
# endif

  raw = (raw_linedef_t *) lump->data;

  for (i=0; i < count; i++, raw++)
  {
    linedef_t *line;

    vertex_t *start = LookupVertex(UINT16(raw->start));
    vertex_t *end   = LookupVertex(UINT16(raw->end));

    start->ref_count++;
    end->ref_count++;

    line = NewLinedef();

    line->start = start;
    line->end   = end;

    /* check for zero-length line */
    line->zero_len = (fabs(start->x - end->x) < DIST_EPSILON) && 
        (fabs(start->y - end->y) < DIST_EPSILON);

    line->flags = UINT16(raw->flags);
	line->specials[0] = UINT8(raw->renderflags);
	line->specials[1] = UINT8(raw->textureflags);
	line->specials[2] = UINT8(raw->type);
	line->specials[3] = UINT8(raw->usetype);

    //line->type = UINT16(raw->type);
    line->tag  = UINT16(raw->tag);

    line->two_sided = (line->flags & LINEFLAG_TWO_SIDED) ? TRUE : FALSE;
    line->is_precious = (line->tag >= 900 && line->tag < 1000) ? 
        TRUE : FALSE;

    line->right = SafeLookupSidedef(UINT16(raw->sidedef1));
    line->left  = SafeLookupSidedef(UINT16(raw->sidedef2));

    if (line->right)
    {
      line->right->ref_count++;
      line->right->on_special |= LineChangesSidedefTexture(line) ? 1 : 0;
	  SetSidedefMergeInfo(line->right, line);
    }

    if (line->left)
    {
      line->left->ref_count++;
      line->left->on_special |= LineChangesSidedefTexture(line) ? 1 : 0;
	  SetSidedefMergeInfo(line->left, line);
    }

    line->self_ref = (line->left && line->right &&
        (line->left->sector == line->right->sector));

    line->index = i;
  }
}

static INLINE_G int TransformSegDist(const seg_t *seg)
{
  float_g sx = seg->side ? seg->linedef->end->x : seg->linedef->start->x;
  float_g sy = seg->side ? seg->linedef->end->y : seg->linedef->start->y;

  return (int) ceil(UtilComputeDist(seg->start->x - sx, seg->start->y - sy));
}

static INLINE_G int TransformAngle(angle_g angle)
{
  int result;
  
  result = (int)(angle * 65536.0 / 360.0);
  
  if (result < 0)
    result += 65536;

  return (result & 0xFFFF);
}

static int SegCompare(const void *p1, const void *p2)
{
  const seg_t *A = ((const seg_t **) p1)[0];
  const seg_t *B = ((const seg_t **) p2)[0];

  if (A->index < 0)
    InternalError("Seg %p never reached a subsector !", A);

  if (B->index < 0)
    InternalError("Seg %p never reached a subsector !", B);

  return (A->index - B->index);
}


/* ----- writing routines ------------------------------ */


static int real_vert_num = 0;

// number of vertices that the Doom 64 engine can index
#ifndef VERTEX_SOFT_LIMIT
#define VERTEX_SOFT_LIMIT  32768   // signed 16-bit index (original engine)
#endif
#ifndef VERTEX_HARD_LIMIT
#define VERTEX_HARD_LIMIT  65536   // unsigned 16-bit index (file format)
#endif

void PutVertices(void)
{
  int count, i;
  lump_t *lump;

  DisplayTicker();

  lump = CreateLevelLump("VERTEXES");

  for (i=0, count=0; i < num_vertices; i++)
  {
    raw_vertex_t raw;
    vertex_t *vert = lev_vertices[i];

	if(vert->index & IS_GL_VERTEX)
      continue;

    raw.x = SINT32((int)(vert->x * 65536.0));
    raw.y = SINT32((int)(vert->y * 65536.0));

    AppendLevelLump(lump, &raw, sizeof(raw));

    count++;
  }

  real_vert_num = count;

  {
    int gl_count = 0;

    for (i=0; i < num_vertices; i++)
    {
      raw_vertex_t raw;
      vertex_t *vert = lev_vertices[i];

      if (! (vert->index & IS_GL_VERTEX))
        continue;

      // vertices left over after CompactBspTree() are dropped
      if (! vert->used)
      {
        vert->index = 0;   // never referenced by anything written
        continue;
      }

      raw.x = SINT32((int)(vert->x * 65536.0));
      raw.y = SINT32((int)(vert->y * 65536.0));
      vert->index = real_vert_num + gl_count;
      gl_count++;

      AppendLevelLump(lump, &raw, sizeof(raw));
    }
    count = real_vert_num + gl_count;
  }

  // Vertex numbers are int16 in the Doom 64 engine (the original reads
  // them as signed shorts), so only indices 0..32767 are valid there.
  // The file format itself has 16 bits for the number.
  if (count > VERTEX_HARD_LIMIT)
    MarkHardFailure(LIMIT_VERTEXES);
  else if (count > VERTEX_SOFT_LIMIT)
    MarkSoftFailure(LIMIT_VERTEXES);
}

void PutLeafs(void)
{
	lump_t *lump;
	int i = 0;
	int j = 0;
	int idx = 0;

	DisplayTicker();

	lump = CreateLevelLump("LEAFS");

	for(i = 0; i < num_subsecs; i++)
	{
		subsec_t *sub = subsecs[i];
		seg_t *seg;
		AppendLevelLump(lump, &sub->seg_count, 2);

		seg = sub->seg_list;

		for(j = 0; j < sub->seg_count; j++)
		{
			vertex_t *vert;
			static const int miniseg = -1;
			int v;

			vert = seg->start;
			v = vert->index;

			//printf("%i : %i %i\n", seg->index, seg->start->index, seg->end->index);

			AppendLevelLump(lump, &v, 2);
			if(seg->linedef && !(vert->index & IS_GL_VERTEX))
			{
				AppendLevelLump(lump, &idx, 2);
				idx++;
			}
			else
				AppendLevelLump(lump, &miniseg, 2);

			seg = seg->next;
		}
	}
}

void PutSectors(void)
{
  int i;
  lump_t *lump = CreateLevelLump("SECTORS");

  DisplayTicker();

  for (i=0; i < num_sectors; i++)
  {
    raw_sector_t raw;
    sector_t *sector = lev_sectors[i];

    raw.floor_h = SINT16(sector->floor_h);
    raw.ceil_h  = SINT16(sector->ceil_h);

	raw.floor_index = UINT16(sector->floor_index);
	raw.ceiling_index = UINT16(sector->ceil_index);

	raw.clr_ceiling = UINT16(sector->colors[0]);
	raw.clr_floor = UINT16(sector->colors[1]);
	raw.clr_thing = UINT16(sector->colors[2]);
	raw.clr_upper = UINT16(sector->colors[3]);
	raw.clr_lower = UINT16(sector->colors[4]);

    //sector->light = UINT16(raw->light);
    raw.special = UINT16(sector->special);
    raw.tag = UINT16(sector->tag);
	raw.flag = UINT16(sector->flag);


    AppendLevelLump(lump, &raw, sizeof(raw));
  }

  if (num_sectors > 65534)
    MarkHardFailure(LIMIT_SECTORS);
  else if (num_sectors > 32767)
    MarkSoftFailure(LIMIT_SECTORS);
}

void PutSidedefs(void)
{
  int i;
  lump_t *lump = CreateLevelLump("SIDEDEFS");

  DisplayTicker();

  for (i=0; i < num_sidedefs; i++)
  {
    raw_sidedef_t raw;
    sidedef_t *side = lev_sidedefs[i];

    raw.sector = (side->sector == NULL) ? SINT16(-1) :
        UINT16(side->sector->index);

    raw.x_offset = SINT16(side->x_offset);
    raw.y_offset = SINT16(side->y_offset);

//    memcpy(raw.upper_tex, side->upper_tex, sizeof(raw.upper_tex));
 //   memcpy(raw.lower_tex, side->lower_tex, sizeof(raw.lower_tex));
 //   memcpy(raw.mid_tex,   side->mid_tex,   sizeof(raw.mid_tex));

	raw.upper_index = UINT16(side->upper_index);
	raw.lower_index = UINT16(side->lower_index);
	raw.mid_index = UINT16(side->mid_index);
 
    AppendLevelLump(lump, &raw, sizeof(raw));
  }

  if (num_sidedefs > 65534)
    MarkHardFailure(LIMIT_SIDEDEFS);
  else if (num_sidedefs > 32767)
    MarkSoftFailure(LIMIT_SIDEDEFS);
}

void PutLinedefs(void)
{
  int i;
  lump_t *lump = CreateLevelLump("LINEDEFS");

  DisplayTicker();

  for (i=0; i < num_linedefs; i++)
  {
    raw_linedef_t raw;
    linedef_t *line = lev_linedefs[i];

    raw.start = UINT16(line->start->index);
    raw.end   = UINT16(line->end->index);

    raw.flags = UINT16(line->flags);

	raw.renderflags = UINT8(line->specials[0]);
	raw.textureflags = UINT8(line->specials[1]);
	raw.type = UINT8(line->specials[2]);
	raw.usetype = UINT8(line->specials[3]);

    //raw.type  = UINT16(line->type);
    raw.tag   = UINT16(line->tag);

    raw.sidedef1 = line->right ? UINT16(line->right->index) : 0xFFFF;
    raw.sidedef2 = line->left  ? UINT16(line->left->index)  : 0xFFFF;

    AppendLevelLump(lump, &raw, sizeof(raw));
  }

  if (num_linedefs > 65534)
    MarkHardFailure(LIMIT_LINEDEFS);
  else if (num_linedefs > 32767)
    MarkSoftFailure(LIMIT_LINEDEFS);
}

static INLINE_G uint16_g VertexIndex16Bit(const vertex_t *v)
{
  if (v->index & IS_GL_VERTEX)
    return (uint16_g) ((v->index & ~IS_GL_VERTEX) | 0x8000U);

  return (uint16_g) v->index;
}

void PutSegs(void)
{
  int i, count;
  lump_t *lump = CreateLevelLump("SEGS");

  DisplayTicker();

  // sort segs into ascending index
  qsort(segs, num_segs, sizeof(seg_t *), SegCompare);

  for (i=0, count=0; i < num_segs; i++)
  {
    raw_seg_t raw;
    seg_t *seg = segs[i];

    // ignore minisegs and degenerate segs
    if (! seg->linedef || seg->degenerate)
      continue;

    raw.start   = UINT16(VertexIndex16Bit(seg->start));
    raw.end     = UINT16(VertexIndex16Bit(seg->end));
    raw.angle   = UINT16(TransformAngle(seg->p_angle));
    raw.linedef = UINT16(seg->linedef->index);
    raw.flip    = UINT16(seg->side);
    raw.dist    = UINT16(TransformSegDist(seg));

    AppendLevelLump(lump, &raw, sizeof(raw));

    count++;

#   if DEBUG_BSP
    PrintDebug("PUT SEG: %04X  Vert %04X->%04X  Line %04X %s  "
        "Angle %04X  (%1.1f,%1.1f) -> (%1.1f,%1.1f)\n", seg->index,
        UINT16(raw.start), UINT16(raw.end), UINT16(raw.linedef), 
        seg->side ? "L" : "R", UINT16(raw.angle), 
        seg->start->x, seg->start->y, seg->end->x, seg->end->y);
#   endif
  }

  if (count != num_complete_seg)
    InternalError("PutSegs miscounted (%d != %d)", count,
      num_complete_seg);

  if (count > 65534)
    MarkHardFailure(LIMIT_SEGS);
  else if (count > 32767)
    MarkSoftFailure(LIMIT_SEGS);
}

void PutSubsecs(void)
{
  int i;
  lump_t *lump;

  DisplayTicker();

  lump = CreateLevelLump("SSECTORS");

  for (i=0; i < num_subsecs; i++)
  {
    raw_subsec_t raw;
    subsec_t *sub = subsecs[i];

    raw.first = UINT16(sub->seg_list->index);
    raw.num   = UINT16(sub->seg_count);

    AppendLevelLump(lump, &raw, sizeof(raw));

#   if DEBUG_BSP
    PrintDebug("PUT SUBSEC %04X  First %04X  Num %04X\n",
      sub->index, UINT16(raw.first), UINT16(raw.num));
#   endif
  }

  if (num_subsecs > 32767)
    MarkHardFailure(LIMIT_SSECTORS);
}

static int node_cur_index;

static void PutOneNode(node_t *node, lump_t *lump)
{
  raw_node_t raw;

  if (node->r.node)
    PutOneNode(node->r.node, lump);

  if (node->l.node)
    PutOneNode(node->l.node, lump);

  node->index = node_cur_index++;

  raw.x  = SINT16(node->x);
  raw.y  = SINT16(node->y);
  raw.dx = SINT16(node->dx / (node->too_long ? 2 : 1));
  raw.dy = SINT16(node->dy / (node->too_long ? 2 : 1));

  raw.b1.minx = SINT16(node->r.bounds.minx);
  raw.b1.miny = SINT16(node->r.bounds.miny);
  raw.b1.maxx = SINT16(node->r.bounds.maxx);
  raw.b1.maxy = SINT16(node->r.bounds.maxy);

  raw.b2.minx = SINT16(node->l.bounds.minx);
  raw.b2.miny = SINT16(node->l.bounds.miny);
  raw.b2.maxx = SINT16(node->l.bounds.maxx);
  raw.b2.maxy = SINT16(node->l.bounds.maxy);

  if (node->r.node)
    raw.right = UINT16(node->r.node->index);
  else if (node->r.subsec)
    raw.right = UINT16(node->r.subsec->index | 0x8000);
  else
    InternalError("Bad right child in node %d", node->index);

  if (node->l.node)
    raw.left = UINT16(node->l.node->index);
  else if (node->l.subsec)
    raw.left = UINT16(node->l.subsec->index | 0x8000);
  else
    InternalError("Bad left child in node %d", node->index);

  AppendLevelLump(lump, &raw, sizeof(raw));

# if DEBUG_BSP
  PrintDebug("PUT NODE %04X  Left %04X  Right %04X  "
    "(%d,%d) -> (%d,%d)\n", node->index, UINT16(raw.left),
    UINT16(raw.right), node->x, node->y,
    node->x + node->dx, node->y + node->dy);
# endif
}

void PutNodes(char *name, node_t *root)
{
  lump_t *lump;

  DisplayTicker();

  lump = CreateLevelLump(name);

  node_cur_index = 0;

  if (root)
  {
    PutOneNode(root, lump);
  }

  if (node_cur_index != num_nodes)
    InternalError("PutNodes miscounted (%d != %d)",
      node_cur_index, num_nodes);

  if (node_cur_index > 32767)
    MarkHardFailure(LIMIT_NODES);
}

/* ----- whole-level routines --------------------------- */

//
// LoadLevel
//
void LoadLevel(void)
{
  char *message;

  const char *level_name = GetLevelName();

  message = UtilFormat("Building nodes on %s", level_name);
 
  DisplaySetBarText(1, message);

  PrintVerbose("\n\n");
  PrintMsg("%s\n", message);
  PrintVerbose("\n");

  UtilFree(message);

  GetVertices();
  GetSectors();
  GetSidedefs();

  GetLinedefs();
  GetThings();

  PrintVerbose("Loaded %d vertices, %d sectors, %d sides, %d lines, %d things\n", 
      num_vertices, num_sectors, num_sidedefs, num_linedefs, num_things);

  // NOTE: order here is critical

  if (!cur_info->no_prune) // disable packing of sidedefs with no prune
  {
    MergeFlaggedLineSidedefs();
    DetectDuplicateSidedefs();
  }

  if (cur_info->merge_vert)
    DetectDuplicateVertices();

  if (!cur_info->no_prune)
    PruneLinedefs();

  // always prune vertices (ignore -noprune), otherwise all the
  // unused vertices from seg splits would keep accumulating.
  PruneVertices();

  if (!cur_info->no_prune)
    PruneSidedefs();

  CalculateWallTips();

  DetectOverlappingLines();
}

//
// FreeLevel
//
void FreeLevel(void)
{
  FreeVertices();
  FreeSidedefs();
  FreeLinedefs();
  FreeSectors();
  FreeThings();
  FreeSegs();
  FreeSubsecs();
  FreeNodes();
  FreeWallTips();
}

//
// SaveLevel
//
void SaveLevel(node_t *root_node)
{
  PutVertices();
  PutLeafs();

  NormaliseBspTree(root_node);

  PutSectors();
  PutSidedefs();
  PutLinedefs();
  PutSegs();
  PutSubsecs();
  PutNodes("NODES", root_node);

  PutBlockmap();

  if (!cur_info->no_reject || !FindLevelLump("REJECT"))
    PutReject();
}
