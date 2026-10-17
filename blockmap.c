//------------------------------------------------------------------------
// BLOCKMAP : Generate the blockmap
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

#include "system.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <limits.h>
#include <assert.h>

#include "blockmap.h"
#include "level.h"
#include "node.h"
#include "seg.h"
#include "structs.h"
#include "util.h"
#include "wad.h"


#define DEBUG_BLOCKMAP  0

extern int block_AutoSelfRef;

static int block_x, block_y;
static int block_w, block_h;
static int block_count;

static int block_mid_x = 0;
static int block_mid_y = 0;

static uint16_g ** block_lines;

static uint16_g *block_ptrs;
static uint16_g *block_dups;

static int block_compression;
static int block_overflowed;

#define DUMMY_DUP  0xFFFF


//
// GetBlockmapBounds
//
void GetBlockmapBounds(int *x, int *y, int *w, int *h)
{
  *x = block_x; *y = block_y;
  *w = block_w; *h = block_h;
}

//
// CheckLinedefInsideBox
//
int CheckLinedefInsideBox(int xmin, int ymin, int xmax, int ymax,
    int x1, int y1, int x2, int y2)
{
  int count = 2;
  int tmp;

  for (;;)
  {
    if (y1 > ymax)
    {
      if (y2 > ymax)
        return FALSE;
        
      x1 = x1 + (int) ((x2-x1) * (double)(ymax-y1) / (double)(y2-y1));
      y1 = ymax;
      
      count = 2;
      continue;
    }

    if (y1 < ymin)
    {
      if (y2 < ymin)
        return FALSE;
      
      x1 = x1 + (int) ((x2-x1) * (double)(ymin-y1) / (double)(y2-y1));
      y1 = ymin;
      
      count = 2;
      continue;
    }

    if (x1 > xmax)
    {
      if (x2 > xmax)
        return FALSE;
        
      y1 = y1 + (int) ((y2-y1) * (double)(xmax-x1) / (double)(x2-x1));
      x1 = xmax;

      count = 2;
      continue;
    }

    if (x1 < xmin)
    {
      if (x2 < xmin)
        return FALSE;
        
      y1 = y1 + (int) ((y2-y1) * (double)(xmin-x1) / (double)(x2-x1));
      x1 = xmin;

      count = 2;
      continue;
    }

    count--;

    if (count == 0)
      break;

    /* swap end points */
    tmp=x1;  x1=x2;  x2=tmp;
    tmp=y1;  y1=y2;  y2=tmp;
  }

  /* linedef touches block */
  return TRUE;
}


/* ----- create blockmap ------------------------------------ */

#define BK_NUM    0
#define BK_MAX    1
#define BK_XOR    2
#define BK_FIRST  3

#define BK_QUANTUM  32

static void BlockAdd(int blk_num, int line_index)
{
  uint16_g *cur = block_lines[blk_num];

# if DEBUG_BLOCKMAP
  PrintDebug("Block %d has line %d\n", blk_num, line_index);
# endif

  if (blk_num < 0 || blk_num >= block_count)
    InternalError("BlockAdd: bad block number %d", blk_num);
    
  if (! cur)
  {
    // create empty block
    block_lines[blk_num] = cur = UtilCalloc(BK_QUANTUM * 
        sizeof(uint16_g));
    cur[BK_NUM] = 0;
    cur[BK_MAX] = BK_QUANTUM;
    cur[BK_XOR] = 0x1234;
  }

  if (BK_FIRST + cur[BK_NUM] == cur[BK_MAX])
  {
    // no more room, so allocate some more...
    cur[BK_MAX] += BK_QUANTUM;

    block_lines[blk_num] = cur = UtilRealloc(cur, cur[BK_MAX] * 
        sizeof(uint16_g));
  }

  // compute new checksum
  cur[BK_XOR] = ((cur[BK_XOR] << 4) | (cur[BK_XOR] >> 12)) ^ line_index;

  cur[BK_FIRST + cur[BK_NUM]] = UINT16(line_index);
  cur[BK_NUM]++;
}


/* ----- compress-edges step safety ------------------------------ */

// Doom 64 takes a mover's floor height from the sector holding its centre
// point, and only lines found through the blockmap can raise it earlier (when
// the 32 unit wide collision box touches them).  With compress edges, the lines
// between sectors with a small floor difference are dropped from the blockmap,
// so a small "step" sector sitting between two floors that differ by more than
// 24 units can only be climbed if the player's centre lands inside it.  If the
// step is too small the player jumps right over it and is stopped by the 32
// unit rise, or the box cannot fit between the neighbouring walls.  The lines
// of such sectors are kept in the blockmap, so they behave as normal.
//
// A region (a connected outline of one sector) is "too small" when:
//   - its longest dimension is below the player's diameter plus clearance, or
//   - its thinnest width is below one unit and a half (a hair thin sliver), or
//   - the thinness estimate 4*area/perimeter is below that same width
//
// These were calibrated with a set of test maps (stairs of 16 unit risers)
// which were verified in the game.
#define STEP_MIN_LENGTH     48.0   // player diameter (32) + clearance (16)
#define STEP_MIN_WIDTH      1.5
#define STEP_MAX_CLIMB      24     // largest rise the player climbs

static char *block_keep = NULL;   // per linedef: keep out of removal

typedef struct { double x, y; } step_pt_t;

static int StepPtCompare(const void *a, const void *b)
{
  const step_pt_t *p = (const step_pt_t *) a;
  const step_pt_t *q = (const step_pt_t *) b;

  if (p->x < q->x) return -1;
  if (p->x > q->x) return +1;
  if (p->y < q->y) return -1;
  if (p->y > q->y) return +1;
  return 0;
}

static double StepCross(const step_pt_t *o, const step_pt_t *a,
    const step_pt_t *b)
{
  return (a->x - o->x) * (b->y - o->y) - (a->y - o->y) * (b->x - o->x);
}

// Convex hull (monotone chain).  Returns number of hull points, stored in
// 'hull' (needs room for 2*n).  'pts' is sorted in place.
static int StepHull(step_pt_t *pts, int n, step_pt_t *hull)
{
  int i, k = 0, t;

  qsort(pts, n, sizeof(step_pt_t), StepPtCompare);

  for (i=0; i < n; i++)
  {
    while (k >= 2 && StepCross(&hull[k-2], &hull[k-1], &pts[i]) <= 0) k--;
    hull[k++] = pts[i];
  }

  for (i=n-2, t=k+1; i >= 0; i--)
  {
    while (k >= t && StepCross(&hull[k-2], &hull[k-1], &pts[i]) <= 0) k--;
    hull[k++] = pts[i];
  }

  return (k > 1) ? k-1 : k;
}

// Is the outline too small to be climbed reliably ?  (pts are the outline's
// vertices, area and perimeter come from its lines)
static int StepRegionTooSmall(step_pt_t *pts, int n, double area,
    double perim)
{
  step_pt_t *hull;
  int h, i, j;
  double length = 0, width = 1e30;

  if (n < 3)
    return TRUE;

  hull = UtilCalloc(2 * n * sizeof(step_pt_t));
  h = StepHull(pts, n, hull);

  if (h < 3)
  {
    UtilFree(hull);
    return TRUE;
  }

  // longest dimension: largest distance between two hull points
  for (i=0; i < h; i++)
  for (j=i+1; j < h; j++)
  {
    double d = UtilComputeDist(hull[j].x - hull[i].x, hull[j].y - hull[i].y);
    if (d > length) length = d;
  }

  // thinnest width: minimum over the hull edges of the farthest distance
  // of any hull point from that edge
  for (i=0; i < h; i++)
  {
    step_pt_t *a = &hull[i];
    step_pt_t *b = &hull[(i+1) % h];
    double elen = UtilComputeDist(b->x - a->x, b->y - a->y);
    double far_d = 0;

    if (elen <= 0) continue;

    for (j=0; j < h; j++)
    {
      double d = fabs(StepCross(a, b, &hull[j])) / elen;
      if (d > far_d) far_d = d;
    }

    if (far_d < width) width = far_d;
  }

  UtilFree(hull);

  if (length < STEP_MIN_LENGTH - 0.001) return TRUE;
  if (width  < STEP_MIN_WIDTH) return TRUE;

  // the outline may be far thinner than its hull (e.g. a bent strip)
  if (perim > 0 && 4.0 * area / perim < STEP_MIN_WIDTH) return TRUE;

  return FALSE;
}

static int StepFind(int *parent, int i)
{
  while (parent[i] != i)
  {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}

// work out which linedefs must stay in the blockmap (block_keep)
static void PrepareStepSafety(void)
{
  int i, s;
  int *parent;       // union find over vertices
  int *line_count;   // lines per sector
  int **sec_lines;   // [sector] -> line indices (side entries, 2 per line)

  block_keep = UtilCalloc((num_linedefs + 1) * sizeof(char));

  if (num_linedefs == 0 || num_sectors == 0)
    return;

  parent     = UtilCalloc((num_vertices + 1) * sizeof(int));
  line_count = UtilCalloc((num_sectors + 1) * sizeof(int));
  sec_lines  = UtilCalloc((num_sectors + 1) * sizeof(int *));

  for (i=0; i < num_linedefs; i++)
  {
    linedef_t *L = LookupLinedef(i);

    if (L->zero_len) continue;
    if (L->right && L->right->sector) line_count[L->right->sector->index]++;
    if (L->left  && L->left->sector)  line_count[L->left->sector->index]++;
  }

  for (s=0; s < num_sectors; s++)
    if (line_count[s] > 0)
      sec_lines[s] = UtilCalloc(line_count[s] * sizeof(int));

  for (s=0; s < num_sectors; s++)
    line_count[s] = 0;

  for (i=0; i < num_linedefs; i++)
  {
    linedef_t *L = LookupLinedef(i);

    if (L->zero_len) continue;

    // encode as line*2 + side
    if (L->right && L->right->sector)
    {
      int sn = L->right->sector->index;
      sec_lines[sn][line_count[sn]++] = i*2;
    }
    if (L->left && L->left->sector)
    {
      int sn = L->left->sector->index;
      sec_lines[sn][line_count[sn]++] = i*2 + 1;
    }
  }

  for (s=0; s < num_sectors; s++)
  {
    int n = line_count[s];
    int j;
    int *comp;
    sector_t *sec = LookupSector(s);

    if (n == 0) continue;

    comp = UtilCalloc(n * sizeof(int));

    for (i=0; i < num_vertices; i++) parent[i] = i;

    // join the end points of this sector's lines
    for (j=0; j < n; j++)
    {
      linedef_t *L = LookupLinedef(sec_lines[s][j] >> 1);
      int a = L->start->index, b = L->end->index;

      if (a < 0 || a >= num_vertices || b < 0 || b >= num_vertices)
        continue;

      parent[StepFind(parent, a)] = StepFind(parent, b);
    }

    for (j=0; j < n; j++)
    {
      linedef_t *L = LookupLinedef(sec_lines[s][j] >> 1);
      int a = L->start->index;

      comp[j] = (a >= 0 && a < num_vertices) ? StepFind(parent, a) : -1;
    }

    // evaluate each outline (connected group of lines)
    for (j=0; j < n; j++)
    {
      int root = comp[j];
      int k, m, np = 0;
      double area2 = 0, perim = 0;
      int fmin, fmax;
      step_pt_t *pts;

      if (root < 0) continue;

      // only handle each outline once (at its first line)
      for (k=0; k < j; k++)
        if (comp[k] == root) break;
      if (k < j) continue;

      for (k=0, m=0; k < n; k++)
        if (comp[k] == root) m++;

      pts = UtilCalloc(2 * m * sizeof(step_pt_t));

      fmin = fmax = sec->floor_h;

      for (k=j; k < n; k++)
      {
        int ent, side;
        linedef_t *L;
        double x1, y1, x2, y2;
        sector_t *other;

        if (comp[k] != root) continue;

        ent  = sec_lines[s][k];
        side = ent & 1;
        L    = LookupLinedef(ent >> 1);

        x1 = L->start->x;  y1 = L->start->y;
        x2 = L->end->x;    y2 = L->end->y;

        if (side) { double t; t=x1; x1=x2; x2=t; t=y1; y1=y2; y2=t; }

        // sector on the right of each edge: outer outlines run clockwise
        area2 += x1 * y2 - x2 * y1;
        perim += UtilComputeDist(x2 - x1, y2 - y1);

        pts[np].x = x1; pts[np].y = y1; np++;
        pts[np].x = x2; pts[np].y = y2; np++;

        other = side ? (L->right ? L->right->sector : NULL)
                     : (L->left  ? L->left->sector  : NULL);

        if (other && other != sec)
        {
          if (other->floor_h < fmin) fmin = other->floor_h;
          if (other->floor_h > fmax) fmax = other->floor_h;
        }
      }

      // holes run the other way round: they are no stepping surface
      if (area2 < 0 && (fmax - fmin) > STEP_MAX_CLIMB &&
          StepRegionTooSmall(pts, np, -area2 / 2.0, perim))
      {
        for (k=j; k < n; k++)
          if (comp[k] == root)
            block_keep[sec_lines[s][k] >> 1] = 1;
      }

      UtilFree(pts);
    }

    UtilFree(comp);
  }

  for (s=0; s < num_sectors; s++)
    if (sec_lines[s]) UtilFree(sec_lines[s]);

  UtilFree(sec_lines);
  UtilFree(line_count);
  UtilFree(parent);
}

// Decides whether a line is left out of the blockmap.
//   0 = keep it
//   1 = always omit it (explicit flags, or identical sectors)
//   2 = omit it only because of compress edges (a small floor difference)
static int BlockLineRemoval(linedef_t *L)
{
  sector_t *sec1, *sec2;

  if (cur_info->no_block_rem)
    return 0;

  if((L->flags & LINEFLAG_HIDE_AUTOMAP) && (L->flags & LINEFLAG_SHOW_AUTOMAP)) return 1; //dma: if both "show" and "hide" (on automap), then skip this line.

  if(L->specials[1] & LINEFLAG_NO_BLOCKMAP) return 1; //immorpher: unused flag to hide line from blockmap

  if ((L->flags & LINEFLAG_TWO_SIDED) && (L->tag == 0) && (L->specials[2] == 0) && (L->specials[3] == 0) && !(L->flags & LINEFLAG_IMPASSABLE) && !(L->flags & LINEFLAG_BLOCK_MONSTERS) && !(L->flags & LINEFLAG_BLOCK_PROJ) && !(L->specials[1] & LINEFLAG_BLOCK_PLAYER)) //immorpher: does the line have the right properties to be considered to be automatically excluded?
  {
    sec1 = L->right->sector;
    sec2 = L->left->sector;

    if(sec1->tag == sec2->tag) { // immorpher: do not compress if sectors might move due to different tags
      if((sec1->floor_h == sec2->floor_h) && (sec1->ceil_h == sec2->ceil_h)) return 1; //immorpher: exclude sectors of same size and type from collisions
      if(cur_info->comp_edge && (abs(sec1->floor_h - sec2->floor_h) <= 16) && ((sec1->ceil_h - sec2->floor_h) >= 64) && ((sec2->ceil_h - sec1->floor_h) >= 64)) return 2; //immorpher: if compress edges enabled allow for more leeway in compressing edges
    }
  }

  return 0;
}

static int BlockLineIsOmitted(linedef_t *L)
{
  int r = BlockLineRemoval(L);

  if (r == 2 && block_keep && block_keep[L->index])
    return FALSE;

  return (r != 0);
}


/* ----- compress-edges hang down safety ------------------------- */

// A ceiling that hangs lower than the player fits under (64 units) blocks
// a player who is standing on a ledge, because the line in front of the hang
// down raises the ceiling check.  Players who fall off the ledge and are
// lower are not stopped by it.  When the lines of such a hang down are left out
// of the blockmap, a player can push their 19 unit radius box into it.  They
// normally fall off the ledge first, unless the ledge line is so close that
// the player's centre cannot get past it (no room to fall).  The lines of a
// hang down are kept when a ledge line lies this close to them and runs
// alongside them.
#define HANG_PLAYER_HEIGHT   64
#define HANG_PLAYER_RADIUS   19.0
#define HANG_MARGIN          0.5     // reach of the ledge contact test

static double HangPointSegDist(double px, double py, double ax, double ay,
    double bx, double by)
{
  double dx = bx - ax, dy = by - ay;
  double len2 = dx*dx + dy*dy;
  double t = (len2 > 0) ? ((px-ax)*dx + (py-ay)*dy) / len2 : 0;

  if (t < 0) t = 0;
  if (t > 1) t = 1;

  return UtilComputeDist(px - (ax + t*dx), py - (ay + t*dy));
}

static int HangSegCross(double ax, double ay, double bx, double by,
    double cx, double cy, double dx, double dy)
{
  double d1 = (bx-ax)*(cy-ay) - (by-ay)*(cx-ax);
  double d2 = (bx-ax)*(dy-ay) - (by-ay)*(dx-ax);
  double d3 = (dx-cx)*(ay-cy) - (dy-cy)*(ax-cx);
  double d4 = (dx-cx)*(by-cy) - (dy-cy)*(bx-cx);

  return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) &&
         ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

// distance between two segments
static double HangSegDist(const linedef_t *L, const linedef_t *E)
{
  double lx1 = L->start->x, ly1 = L->start->y, lx2 = L->end->x, ly2 = L->end->y;
  double ex1 = E->start->x, ey1 = E->start->y, ex2 = E->end->x, ey2 = E->end->y;
  double d, best;

  if (HangSegCross(lx1, ly1, lx2, ly2, ex1, ey1, ex2, ey2))
    return 0;

  best = HangPointSegDist(lx1, ly1, ex1, ey1, ex2, ey2);
  d = HangPointSegDist(lx2, ly2, ex1, ey1, ex2, ey2);  if (d < best) best = d;
  d = HangPointSegDist(ex1, ey1, lx1, ly1, lx2, ly2);  if (d < best) best = d;
  d = HangPointSegDist(ex2, ey2, lx1, ly1, lx2, ly2);  if (d < best) best = d;

  return best;
}

// does the ledge line E run alongside the hang down line L, close enough that
// the player standing at the ledge cannot clear the hang down ?
static int HangLedgeIsClose(const linedef_t *L, const linedef_t *E)
{
  double lx = L->end->x - L->start->x, ly = L->end->y - L->start->y;
  double ex = E->end->x - E->start->x, ey = E->end->y - E->start->y;
  double llen = UtilComputeDist(lx, ly);
  double elen = UtilComputeDist(ex, ey);
  double t1, t2, lo, hi;

  if (llen <= 0 || elen <= 0)
    return FALSE;

  // roughly parallel (within 45 degrees)
  if (fabs(lx*ey - ly*ex) / (llen * elen) > 0.7072)
    return FALSE;

  // must overlap along the hang down line, not merely touch at a corner
  t1 = ((E->start->x - L->start->x)*lx + (E->start->y - L->start->y)*ly) / llen;
  t2 = ((E->end->x   - L->start->x)*lx + (E->end->y   - L->start->y)*ly) / llen;

  lo = (t1 < t2) ? t1 : t2;
  hi = (t1 < t2) ? t2 : t1;

  if (lo < 0)    lo = 0;
  if (hi > llen) hi = llen;

  if (hi - lo <= 1.0)
    return FALSE;

  return HangSegDist(L, E) <= HANG_PLAYER_RADIUS + HANG_MARGIN;
}

static void PrepareHangSafety(void)
{
  int i, j;

  for (i=0; i < num_linedefs; i++)
  {
    linedef_t *L = LookupLinedef(i);
    int low_ceil;

    if (L->zero_len || block_keep[i])
      continue;

    if (BlockLineRemoval(L) != 2)
      continue;

    // only a ceiling step can hang down over a player
    if (L->right->sector->ceil_h == L->left->sector->ceil_h)
      continue;

    low_ceil = (L->right->sector->ceil_h < L->left->sector->ceil_h) ?
        L->right->sector->ceil_h : L->left->sector->ceil_h;

    for (j=0; j < num_linedefs; j++)
    {
      linedef_t *E = LookupLinedef(j);
      int ledge;

      if (j == i || E->zero_len || ! E->right || ! E->left)
        continue;

      if (BlockLineIsOmitted(E))
        continue;

      // height the player stands at when touching this line
      ledge = (E->right->sector->floor_h > E->left->sector->floor_h) ?
          E->right->sector->floor_h : E->left->sector->floor_h;

      if (low_ceil - ledge >= HANG_PLAYER_HEIGHT)
        continue;

      if (HangLedgeIsClose(L, E))
      {
        block_keep[i] = 1;
        break;
      }
    }
  }
}

//dma: 2-11-25: allow for ignoring certain lines from blockmap addition
static void BlockAddLine(linedef_t *L)
{
  if (BlockLineIsOmitted(L))
    return;

  int x1 = (int) L->start->x;
  int y1 = (int) L->start->y;
  int x2 = (int) L->end->x;
  int y2 = (int) L->end->y;

  int bx1 = (MIN(x1,x2) - block_x) / 128;
  int by1 = (MIN(y1,y2) - block_y) / 128;
  int bx2 = (MAX(x1,x2) - block_x) / 128;
  int by2 = (MAX(y1,y2) - block_y) / 128;

  int bx, by;
  int line_index = L->index;

# if DEBUG_BLOCKMAP
  PrintDebug("BlockAddLine: %d (%d,%d) -> (%d,%d)\n", line_index, 
      x1, y1, x2, y2);
# endif

  // handle truncated blockmaps
  if (bx1 < 0) bx1 = 0;
  if (by1 < 0) by1 = 0;
  if (bx2 >= block_w) bx2 = block_w - 1;
  if (by2 >= block_h) by2 = block_h - 1;

  if (bx2 < bx1 || by2 < by1)
    return;

  // handle simple case #1: completely horizontal
  if (by1 == by2)
  {
    for (bx=bx1; bx <= bx2; bx++)
    {
      int blk_num = by1 * block_w + bx;
      BlockAdd(blk_num, line_index);
    }
    return;
  }

  // handle simple case #2: completely vertical
  if (bx1 == bx2)
  {
    for (by=by1; by <= by2; by++)
    {
      int blk_num = by * block_w + bx1;
      BlockAdd(blk_num, line_index);
    }
    return;
  }

  // handle the rest (diagonals)

  for (by=by1; by <= by2; by++)
  for (bx=bx1; bx <= bx2; bx++)
  {
    int blk_num = by * block_w + bx;
  
    int minx = block_x + bx * 128;
    int miny = block_y + by * 128;
    int maxx = minx + 127;
    int maxy = miny + 127;

    if (CheckLinedefInsideBox(minx, miny, maxx, maxy, x1, y1, x2, y2))
    {
      BlockAdd(blk_num, line_index);
    }
  }
}

static void CreateBlockmap(void)
{
  int i;

  block_lines = UtilCalloc(block_count * sizeof(uint16_g *));

  if (cur_info->comp_edge && ! cur_info->no_block_rem)
  {
    PrepareStepSafety();
    PrepareHangSafety();
  }

  DisplayTicker();

  for (i=0; i < num_linedefs; i++)
  {
    linedef_t *L = LookupLinedef(i);

    // ignore zero-length lines
    if (L->zero_len)
      continue;

    BlockAddLine(L);
  }

  if (block_keep)
  {
    UtilFree(block_keep);
    block_keep = NULL;
  }
}


static int BlockCompare(const void *p1, const void *p2)
{
  int blk_num1 = ((const uint16_g *) p1)[0];
  int blk_num2 = ((const uint16_g *) p2)[0];

  const uint16_g *A = block_lines[blk_num1];
  const uint16_g *B = block_lines[blk_num2];

  if (A == B)
    return 0;

  if (A == NULL) return -1;
  if (B == NULL) return +1;

  if (A[BK_NUM] != B[BK_NUM])
  {
    return A[BK_NUM] - B[BK_NUM];
  }
 
  if (A[BK_XOR] != B[BK_XOR])
  {
    return A[BK_XOR] - B[BK_XOR];
  }
 
  return memcmp(A+BK_FIRST, B+BK_FIRST, A[BK_NUM] * sizeof(uint16_g));
}

static void CompressBlockmap(void)
{
  int i;
  int cur_offset;
# if DEBUG_BLOCKMAP
  int dup_count=0;
# endif

  int orig_size, new_size;

  block_ptrs = UtilCalloc(block_count * sizeof(uint16_g));
  block_dups = UtilCalloc(block_count * sizeof(uint16_g));

  DisplayTicker();

  // sort duplicate-detecting array.  After the sort, all duplicates
  // will be next to each other.  The duplicate array gives the order
  // of the blocklists in the BLOCKMAP lump.
  
  for (i=0; i < block_count; i++)
    block_dups[i] = i;

  qsort(block_dups, block_count, sizeof(uint16_g), BlockCompare);

  // scan duplicate array and build up offset array

  cur_offset = 4 + block_count + 2; //dma: ?

  orig_size = 4 + block_count;
  new_size  = cur_offset;

  DisplayTicker();

  for (i=0; i < block_count; i++)
  {
    int blk_num = block_dups[i];
    int count;

    // empty block ?
    if (block_lines[blk_num] == NULL)
    {
      block_ptrs[blk_num] = 4 + block_count;
      block_dups[i] = DUMMY_DUP;

      orig_size += 2;
      continue;
    }

    //dma: ?
    count = 2 + block_lines[blk_num][BK_NUM];
    //count = block_lines[blk_num][BK_NUM];

    // duplicate ?  Only the very last one of a sequence of duplicates
    // will update the current offset value.

    if (i+1 < block_count && 
        BlockCompare(block_dups + i, block_dups + i+1) == 0)
    {
      block_ptrs[blk_num] = cur_offset;
      block_dups[i] = DUMMY_DUP;

      // free the memory of the duplicated block
      UtilFree(block_lines[blk_num]);
      block_lines[blk_num] = NULL;
      
# if DEBUG_BLOCKMAP
      dup_count++;
# endif

      orig_size += count;
      continue;
    }

    // OK, this block is either the last of a series of duplicates, or
    // just a singleton.

    block_ptrs[blk_num] = cur_offset;

    cur_offset += count;

    orig_size += count;
    new_size  += count;
  }

  if (cur_offset > 65535)
  {
    MarkSoftFailure(LIMIT_BLOCKMAP);
    block_overflowed = TRUE;
    return;
  }

# if DEBUG_BLOCKMAP
  PrintDebug("Blockmap: Last ptr = %d  duplicates = %d\n", 
      cur_offset, dup_count);
# endif

  block_compression = (orig_size - new_size) * 100 / orig_size;

  // there's a tiny chance of new_size > orig_size
  if (block_compression < 0)
    block_compression = 0;
}


static void WriteBlockmap(void)
{
  int i;
  
  raw_blockmap_header_t header;

  lump_t *lump = CreateLevelLump("BLOCKMAP");

  uint16_g null_block[2] = { 0x0000, 0xFFFF };
  uint16_g m_zero = 0x0000;
  uint16_g m_neg1 = 0xFFFF;
  
  // leave empty if the blockmap overflowed
  if (block_overflowed)
    return;

  // fill in header
  header.x_origin = UINT16(block_x);
  header.y_origin = UINT16(block_y);
  header.x_blocks = UINT16(block_w);
  header.y_blocks = UINT16(block_h);
  
  AppendLevelLump(lump, &header, sizeof(header));

  // handle pointers
  for (i=0; i < block_count; i++)
  {
    uint16_g ptr = UINT16(block_ptrs[i]);

    if (ptr == 0)
      InternalError("WriteBlockmap: offset %d not set.", i);

    AppendLevelLump(lump, &ptr, sizeof(uint16_g));
  }

  // add the null block which _all_ empty blocks will use
  AppendLevelLump(lump, null_block, sizeof(null_block));

  // handle each block list
  for (i=0; i < block_count; i++)
  {
    int blk_num = block_dups[i];
    uint16_g *blk;

    // ignore duplicate or empty blocks
    if (blk_num == DUMMY_DUP)
      continue;

    blk = block_lines[blk_num];

    if (blk == NULL)
      InternalError("WriteBlockmap: block %d is NULL !", i);

    //dma: 2-14-2025: remove unnecessary header/footer? doesn't seem to work on remaster
    AppendLevelLump(lump, &m_zero, sizeof(uint16_g));
    AppendLevelLump(lump, blk + BK_FIRST, blk[BK_NUM] * sizeof(uint16_g));
    AppendLevelLump(lump, &m_neg1, sizeof(uint16_g));
  }
}


static void FreeBlockmap(void)
{
  int i;

  for (i=0; i < block_count; i++)
  {
    if (block_lines[i])
      UtilFree(block_lines[i]);
  }

  UtilFree(block_lines);
  UtilFree(block_ptrs);
  UtilFree(block_dups);
}


/* ----- top level funcs ------------------------------------ */

static void FindBlockmapLimits(bbox_t *bbox)
{
  int i;

  int mid_x = 0;
  int mid_y = 0;

  bbox->minx = bbox->miny = SHRT_MAX;
  bbox->maxx = bbox->maxy = SHRT_MIN;

  for (i=0; i < num_linedefs; i++)
  {
    linedef_t *L = LookupLinedef(i);

    if (! L->zero_len)
    {
      float_g x1 = L->start->x;
      float_g y1 = L->start->y;
      float_g x2 = L->end->x;
      float_g y2 = L->end->y;

      int lx = (int)floor(MIN(x1, x2));
      int ly = (int)floor(MIN(y1, y2));
      int hx = (int)ceil(MAX(x1, x2));
      int hy = (int)ceil(MAX(y1, y2));

      if (lx < bbox->minx) bbox->minx = lx;
      if (ly < bbox->miny) bbox->miny = ly;
      if (hx > bbox->maxx) bbox->maxx = hx;
      if (hy > bbox->maxy) bbox->maxy = hy;

      // compute middle of cluster (roughly, so we don't overflow)
      mid_x += (lx + hx) / 32;
      mid_y += (ly + hy) / 32;
    }
  }

  if (num_linedefs > 0)
  {
    block_mid_x = (mid_x / num_linedefs) * 16;
    block_mid_y = (mid_y / num_linedefs) * 16;
  }

# if DEBUG_BLOCKMAP
  PrintDebug("Blockmap lines centered at (%d,%d)\n", block_mid_x, block_mid_y);
# endif
}

//
// TruncateBlockmap
//
static void TruncateBlockmap(void)
{
  while (block_w * block_h > cur_info->block_limit)
  {
    block_w -= block_w / 8;
    block_h -= block_h / 8;
  }

  block_count = block_w * block_h;

  PrintMiniWarn("Blockmap TOO LARGE!  Truncated to %dx%d blocks\n",
      block_w, block_h);

  MarkSoftFailure(LIMIT_BMAP_TRUNC);

  /* center the truncated blockmap */
  block_x = block_mid_x - block_w * 64;
  block_y = block_mid_y - block_h * 64;

# if DEBUG_BLOCKMAP
  PrintDebug("New blockmap origin: (%d,%d)\n", block_x, block_y);
# endif
}

//
// InitBlockmap
//
void InitBlockmap(void)
{
  bbox_t map_bbox;

  /* find limits of linedefs, and store as map limits */
  FindBlockmapLimits(&map_bbox);

  PrintVerbose("Map goes from (%d,%d) to (%d,%d)\n",
      map_bbox.minx, map_bbox.miny, map_bbox.maxx, map_bbox.maxy);

  block_x = map_bbox.minx - (map_bbox.minx & 0x7);
  block_y = map_bbox.miny - (map_bbox.miny & 0x7);

  block_w = ((map_bbox.maxx - block_x) / 128) + 1;
  block_h = ((map_bbox.maxy - block_y) / 128) + 1;
  block_count = block_w * block_h;

}

//
// PutBlockmap
//
void PutBlockmap(void)
{
  block_overflowed = FALSE;

  // truncate blockmap if too large.  We're limiting the number of
  // blocks to around 16000 (user changeable), this leaves about 48K
  // of shorts for the actual line lists.
 
  if (block_count > cur_info->block_limit)
    TruncateBlockmap();

  // initial phase: create internal blockmap containing the index of
  // all lines in each block.
  
  CreateBlockmap();

  // -AJA- second phase: compress the blockmap.  We do this by sorting
  //       the blocks, which is a typical way to detect duplicates in
  //       a large list.

  CompressBlockmap();
 
  // final phase: write it out in the correct format

  WriteBlockmap();

  if (block_overflowed)
    PrintVerbose("Blockmap overflowed (lump will be empty)\n");
  else
    PrintVerbose("Completed blockmap, size %dx%d (compression: %d%%)\n",
        block_w, block_h, block_compression);

  FreeBlockmap();
}

