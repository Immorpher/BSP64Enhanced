//------------------------------------------------------------------------
// REJECT : Generate the reject table (exact line-of-sight, multi-threaded)
//------------------------------------------------------------------------
//
//  GL-Friendly Node Builder (C) 2000-2007 Andrew Apted
//
//  Based on 'BSP 2.3' by Colin Reed, Lee Killough and others.
//
//  The line-of-sight algorithm is adapted from ZenNode / ZokumBSP:
//    ZenNode (C) 1995-2004 Marc Rousseau
//    ZokumBSP (C) 2016-2024 Kim Roar Foldoy Hauge
//  Both are GPL v2 or later, as is this file.
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
//  How it works
//  ------------
//  A sector pair is hidden (bit set in REJECT) only when no straight line
//  can run between them without crossing a one-sided line.  Every
//  two-sided line between different sectors is treated as see-through no
//  matter what its floor/ceiling heights are, because sectors can move in
//  Doom 64 (tagged sectors, macros, manual doors).  A moving sector can
//  therefore only make sight easier than the table assumes, never harder,
//  so the table is always safe.
//
//  The work is split by source line over pthreads (the result does not
//  depend on the thread count).  All CPU cores are used by default; set
//  the environment variable BSP64_THREADS to use a different number.
//
//  Geometry is done with long double: Doom 64 coordinates are 16.16 fixed
//  point, so a cross product needs up to 64 significant bits to be exact.
//
//  Special 999 sectors ("Blind To Monsters"): they are treated like any
//  other sector while sight is traced, then every pair involving a 999
//  sector (other than the sector with itself) is hidden afterwards.  They
//  no longer cut line of sight between the sectors on either side.
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

#include <pthread.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "reject.h"
#include "level.h"
#include "node.h"
#include "seg.h"
#include "structs.h"
#include "util.h"
#include "wad.h"


#define SPECIAL_BLIND  999   // sector special: hide from / to everything


#ifndef REJECT_NUM
#define REJECT_NUM long double
#endif

typedef REJECT_NUM num_t;

#define VIS_UNKNOWN  0
#define VIS_VISIBLE  1
#define VIS_HIDDEN   2

#define BLOCK_SIZE   128


// atomic helpers (the table and the line cache are shared between threads)

#if !defined(__GNUC__)
#error "reject.c needs GCC style atomic builtins"
#endif

#define LOAD8(p)        __atomic_load_n((p), __ATOMIC_RELAXED)
#define STORE8(p,v)     __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define OR8(p,v)        __atomic_fetch_or((p), (v), __ATOMIC_RELAXED)


// ----- structures ------------------------------------------------------

typedef struct { num_t x, y; } pt_t;

typedef struct
{
  int id;                 // index into solids[]
  const pt_t *start, *end;
}
solid_t;

typedef struct
{
  const pt_t *start, *end;
  int left_sec, right_sec;
  num_t DX, DY;
  num_t lo, hi;
  pt_t *loPoint, *hiPoint;
}
trans_t;

typedef struct { solid_t **lines; int loIndex, hiIndex; } lineset_t;
typedef struct { int noPoints, lastPoint; const pt_t **point; } poly_t;

typedef struct
{
  trans_t *src, *tgt;
  lineset_t solidSet;
  poly_t upperPoly, lowerPoly;
}
world_t;

// everything that is written to while testing one line pair.  Each
// thread owns one of these.
typedef struct
{
  int loRow, hiRow;
  int *bound_lo, *bound_hi;
  unsigned char *checkLine;
  unsigned char *ignore;
  solid_t **testLines;
  const pt_t **polyPoints;
  num_t X, Y, DX, DY;
  pt_t p[4];
}
ctx_t;


// ----- shared, read-only while the line pairs are tested ---------------

static int num_solid;
static solid_t *solids;

static int num_trans;
static trans_t *transes;

static int rej_sectors;

static unsigned char *rej_table;       // rej_sectors * rej_sectors bytes
static unsigned char *line_vis;        // 2 bits per line pair

// block grid holding the solid lines
static num_t grid_x0, grid_y0;
static int grid_cols, grid_rows;
static int *cell_start;                // grid_cols*grid_rows + 1
static int *cell_items;                // solid ids


#define TABLE(a,b)  (rej_table[(size_t)(a) * (size_t)rej_sectors + (size_t)(b)])


static void MarkVisibility(int s1, int s2, unsigned char vis)
{
  if (LOAD8(&TABLE(s1, s2)) == VIS_UNKNOWN)
    STORE8(&TABLE(s1, s2), vis);

  if (LOAD8(&TABLE(s2, s1)) == VIS_UNKNOWN)
    STORE8(&TABLE(s2, s1), vis);
}


// ----- block grid -------------------------------------------------------

static int CellOf(num_t v)
{
  return (int) floorl((long double) (v / BLOCK_SIZE));
}

static int ClampInt(int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

// call 'visit' for every grid cell touched by the segment (conservative)
// mode 0: count, mode 1: store
static void RasterSolid(const solid_t *s, int mode, int *fill)
{
  num_t x0 = s->start->x - grid_x0, y0 = s->start->y - grid_y0;
  num_t x1 = s->end->x   - grid_x0, y1 = s->end->y   - grid_y0;

  num_t dx = x1 - x0, dy = y1 - y0;

  num_t ymin = (y0 < y1) ? y0 : y1;
  num_t ymax = (y0 < y1) ? y1 : y0;

  int ry0 = ClampInt(CellOf(ymin), 0, grid_rows - 1);
  int ry1 = ClampInt(CellOf(ymax), 0, grid_rows - 1);
  int ry;

  for (ry = ry0; ry <= ry1; ry++)
  {
    num_t ylo = ymin > ry * (num_t) BLOCK_SIZE ? ymin : ry * (num_t) BLOCK_SIZE;
    num_t yhi = ymax < (ry + 1) * (num_t) BLOCK_SIZE ? ymax : (ry + 1) * (num_t) BLOCK_SIZE;
    num_t xa, xb, xmin, xmax;
    int cx0, cx1, cx;

    if (dy == 0)
    {
      xa = x0; xb = x1;
    }
    else
    {
      xa = x0 + (ylo - y0) * dx / dy;
      xb = x0 + (yhi - y0) * dx / dy;
    }

    xmin = (xa < xb) ? xa : xb;
    xmax = (xa < xb) ? xb : xa;

    cx0 = ClampInt(CellOf(xmin), 0, grid_cols - 1);
    cx1 = ClampInt(CellOf(xmax), 0, grid_cols - 1);

    for (cx = cx0; cx <= cx1; cx++)
    {
      int cell = ry * grid_cols + cx;

      if (mode == 0)
        cell_start[cell + 1]++;
      else
        cell_items[fill[cell]++] = s->id;
    }
  }
}

static void BuildGrid(const pt_t *pts, int num_pts)
{
  int i, total;
  int *fill;
  num_t minx = 0, miny = 0, maxx = 0, maxy = 0;

  for (i = 0; i < num_pts; i++)
  {
    if (i == 0 || pts[i].x < minx) minx = pts[i].x;
    if (i == 0 || pts[i].x > maxx) maxx = pts[i].x;
    if (i == 0 || pts[i].y < miny) miny = pts[i].y;
    if (i == 0 || pts[i].y > maxy) maxy = pts[i].y;
  }

  grid_x0 = minx - 8;
  grid_y0 = miny - 8;
  grid_cols = CellOf(maxx - grid_x0) + 1;
  grid_rows = CellOf(maxy - grid_y0) + 1;

  cell_start = calloc((size_t) grid_cols * grid_rows + 1, sizeof(int));
  if (! cell_start) FatalError("Out of memory (reject grid)");

  for (i = 0; i < num_solid; i++)
    RasterSolid(&solids[i], 0, NULL);

  for (i = 0; i < grid_cols * grid_rows; i++)
    cell_start[i + 1] += cell_start[i];

  total = cell_start[grid_cols * grid_rows];

  cell_items = calloc(total > 0 ? total : 1, sizeof(int));
  fill = calloc((size_t) grid_cols * grid_rows + 1, sizeof(int));
  if (! cell_items || ! fill) FatalError("Out of memory (reject grid)");

  memcpy(fill, cell_start, ((size_t) grid_cols * grid_rows) * sizeof(int));

  for (i = 0; i < num_solid; i++)
    RasterSolid(&solids[i], 1, fill);

  free(fill);
}


// ----- region marking (per thread) --------------------------------------

static void MarkEdge(ctx_t *c, const pt_t *p1, const pt_t *p2)
{
  num_t x0 = p1->x - grid_x0, y0 = p1->y - grid_y0;
  num_t x1 = p2->x - grid_x0, y1 = p2->y - grid_y0;

  num_t dx = x1 - x0, dy = y1 - y0;

  num_t ymin = (y0 < y1) ? y0 : y1;
  num_t ymax = (y0 < y1) ? y1 : y0;

  int ry0 = ClampInt(CellOf(ymin), 0, grid_rows - 1);
  int ry1 = ClampInt(CellOf(ymax), 0, grid_rows - 1);
  int ry;

  if (ry0 < c->loRow) c->loRow = ry0;
  if (ry1 > c->hiRow) c->hiRow = ry1;

  for (ry = ry0; ry <= ry1; ry++)
  {
    num_t ylo = ymin > ry * (num_t) BLOCK_SIZE ? ymin : ry * (num_t) BLOCK_SIZE;
    num_t yhi = ymax < (ry + 1) * (num_t) BLOCK_SIZE ? ymax : (ry + 1) * (num_t) BLOCK_SIZE;
    num_t xa, xb, xmin, xmax;
    int cx0, cx1;

    if (dy == 0)
    {
      xa = x0; xb = x1;
    }
    else
    {
      xa = x0 + (ylo - y0) * dx / dy;
      xb = x0 + (yhi - y0) * dx / dy;
    }

    xmin = (xa < xb) ? xa : xb;
    xmax = (xa < xb) ? xb : xa;

    cx0 = ClampInt(CellOf(xmin), 0, grid_cols - 1);
    cx1 = ClampInt(CellOf(xmax), 0, grid_cols - 1);

    if (cx0 < c->bound_lo[ry]) c->bound_lo[ry] = cx0;
    if (cx1 > c->bound_hi[ry]) c->bound_hi[ry] = cx1;
  }
}

static void MarkBlockMap(ctx_t *c, world_t *w)
{
  c->loRow = grid_rows;
  c->hiRow = -1;

  MarkEdge(c, w->src->start, w->src->end);
  MarkEdge(c, w->tgt->start, w->tgt->end);
  MarkEdge(c, w->src->start, w->tgt->end);
  MarkEdge(c, w->tgt->start, w->src->end);
}

static int FindInterveningLines(ctx_t *c, lineset_t *set)
{
  int row, col, lineCount = 0;

  memset(c->checkLine, 1, num_solid);

  for (row = c->loRow; row <= c->hiRow; row++)
  {
    for (col = c->bound_lo[row]; col <= c->bound_hi[row]; col++)
    {
      int cell = row * grid_cols + col;
      int k;

      for (k = cell_start[cell]; k < cell_start[cell + 1]; k++)
      {
        int id = cell_items[k];

        set->lines[lineCount] = &solids[id];
        lineCount += c->checkLine[id] ? 1 : 0;
        c->checkLine[id] = 0;
      }
    }

    c->bound_lo[row] = grid_cols;
    c->bound_hi[row] = -1;
  }

  set->loIndex = 0;
  set->hiIndex = lineCount - 1;
  set->lines[lineCount] = NULL;

  return (lineCount > 0);
}


// ----- line pair geometry -----------------------------------------------

#define SWAPPT(a,b)  do { const pt_t *t_ = (a); (a) = (b); (b) = t_; } while (0)

// returns 0: lines can't be compared (collinear), -1: lines cross each
// other (so they can obviously see each other), 1: ok
static int AdjustLinePair(trans_t *src, trans_t *tgt, int *bisects)
{
  num_t y1 = src->DX * (tgt->start->y - src->start->y) - src->DY * (tgt->start->x - src->start->x);
  num_t y2 = src->DX * (tgt->end->y   - src->start->y) - src->DY * (tgt->end->x   - src->start->x);
  num_t x1, x2;

  // collinear: ignore
  if (y1 == 0 && y2 == 0) return 0;

  // make sure src doesn't bisect tgt
  if ((y1 > 0 && y2 < 0) || (y1 < 0 && y2 > 0))
  {
    trans_t tmp = *src; *src = *tgt; *tgt = tmp;

    y1 = src->DX * (tgt->start->y - src->start->y) - src->DY * (tgt->start->x - src->start->x);
    y2 = src->DX * (tgt->end->y   - src->start->y) - src->DY * (tgt->end->x   - src->start->x);

    // the two lines actually intersect
    if ((y1 > 0 && y2 < 0) || (y1 < 0 && y2 > 0))
      return -1;
  }

  // make sure tgt ends up on the left of src
  if (y1 <= 0 && y2 <= 0)
  {
    SWAPPT(src->start, src->end);
    src->DX = -src->DX;
    src->DY = -src->DY;
    y1 = -y1;
    y2 = -y2;
  }

  // parallel lines
  if (y2 == y1)
  {
    x1 = src->DX * (tgt->start->x - src->start->x) + src->DY * (tgt->start->y - src->start->y);
    x2 = src->DX * (tgt->end->x   - src->start->x) + src->DY * (tgt->end->y   - src->start->y);

    if (x1 < x2)
    {
      SWAPPT(tgt->start, tgt->end);
      tgt->DX = -tgt->DX;
      tgt->DY = -tgt->DY;
    }
    return 1;
  }

  // look at src from tgt's point of view
  x1 = tgt->DX * (src->start->y - tgt->start->y) - tgt->DY * (src->start->x - tgt->start->x);
  x2 = tgt->DX * (src->end->y   - tgt->start->y) - tgt->DY * (src->end->x   - tgt->start->x);

  if ((x1 < 0 && x2 > 0) || (x1 > 0 && x2 < 0))
  {
    *bisects = 1;

    if (y1 > y2)
    {
      SWAPPT(tgt->start, tgt->end);
      tgt->DX = -tgt->DX;
      tgt->DY = -tgt->DY;
    }
  }
  else if (x1 <= 0 && x2 <= 0)
  {
    SWAPPT(tgt->start, tgt->end);
    tgt->DX = -tgt->DX;
    tgt->DY = -tgt->DY;
  }

  return 1;
}

static void GetBounds(const pt_t *ss, const pt_t *se, const pt_t *ts, const pt_t *te,
    num_t *loY, num_t *hiY, num_t *loX, num_t *hiX)
{
  if (ss->y < se->y)
  {
    if (ts->y < te->y)
    {
      *loY = (ss->y < ts->y) ? ss->y : ts->y;
      *hiY = (se->y > te->y) ? se->y : te->y;
    }
    else
    {
      *loY = (ss->y < te->y) ? ss->y : te->y;
      *hiY = (se->y > ts->y) ? se->y : ts->y;
    }
  }
  else
  {
    if (ts->y < te->y)
    {
      *loY = (se->y < ts->y) ? se->y : ts->y;
      *hiY = (ss->y > te->y) ? ss->y : te->y;
    }
    else
    {
      *loY = (se->y < te->y) ? se->y : te->y;
      *hiY = (ss->y > ts->y) ? ss->y : ts->y;
    }
  }

  if (ss->x < se->x)
  {
    if (ts->x < te->x)
    {
      *loX = (ss->x < ts->x) ? ss->x : ts->x;
      *hiX = (se->x > te->x) ? se->x : te->x;
    }
    else
    {
      *loX = (ss->x < te->x) ? ss->x : te->x;
      *hiX = (se->x > ts->x) ? se->x : ts->x;
    }
  }
  else
  {
    if (ts->x < te->x)
    {
      *loX = (se->x < ts->x) ? se->x : ts->x;
      *hiX = (ss->x > te->x) ? ss->x : te->x;
    }
    else
    {
      *loX = (se->x < te->x) ? se->x : te->x;
      *hiX = (ss->x > ts->x) ? ss->x : ts->x;
    }
  }
}

#define IGN(c,l)  ((c)->ignore[(l)->id])

static int TrimSetBounds(ctx_t *c, lineset_t *set)
{
  if (set->loIndex >= set->hiIndex) return 0;

  while (IGN(c, set->lines[set->loIndex]))
  {
    set->loIndex++;
    if (set->loIndex >= set->hiIndex) return 0;
  }

  while (IGN(c, set->lines[set->hiIndex]))
    set->hiIndex--;

  return 1;
}

static void RotatePoint(ctx_t *c, pt_t *p, num_t x, num_t y)
{
  p->x = c->DX * (x - c->X) + c->DY * (y - c->Y);
  p->y = c->DX * (y - c->Y) - c->DY * (x - c->X);
}

// returns -1 when a single line blocks the view completely
static int TrimLines(ctx_t *c, const trans_t *src, const trans_t *tgt, lineset_t *set)
{
  num_t loY, hiY, loX, hiX;
  pt_t p1, p2, p3;
  num_t minX, maxX, minY;
  int linesLeft = 0;
  int checkBlock;
  int i;

  GetBounds(src->start, src->end, tgt->start, tgt->end, &loY, &hiY, &loX, &hiX);

  c->X  = src->start->x;
  c->Y  = src->start->y;
  c->DX = tgt->end->x - src->start->x;
  c->DY = tgt->end->y - src->start->y;

  RotatePoint(c, &p1, src->end->x, src->end->y);
  RotatePoint(c, &p2, tgt->start->x, tgt->start->y);
  RotatePoint(c, &p3, tgt->end->x, tgt->end->y);

  minX = (p1.x < 0) ? 0 : p1.x;
  maxX = (p2.x < p3.x) ? p2.x : p3.x;
  minY = (p1.y < p2.y) ? p1.y : p2.y;

  checkBlock = (minX <= maxX) && (c->DX != 0 || c->DY != 0);

  for (i = set->loIndex; i <= set->hiIndex; i++)
  {
    solid_t *line = set->lines[i];

    IGN(c, line) = 1;

    // eliminate lines completely outside the axis aligned bounding box
    if (line->start->y <= loY)
    {
      if (line->end->y <= loY) continue;
    }
    else if (line->start->y >= hiY)
    {
      if (line->end->y >= hiY) continue;
    }

    if (line->start->x >= hiX)
    {
      if (line->end->x >= hiX) continue;
    }
    else if (line->start->x <= loX)
    {
      if (line->end->x <= loX) continue;
    }

    // stop if a single line obstructs the view completely
    if (checkBlock)
    {
      pt_t start, end;

      start.y = c->DX * (line->start->y - c->Y) - c->DY * (line->start->x - c->X);

      if (start.y >= 0 || start.y <= minY)
      {
        end.y = c->DX * (line->end->y - c->Y) - c->DY * (line->end->x - c->X);

        if ((end.y <= minY && start.y >= 0) || (end.y >= 0 && start.y <= minY))
        {
          start.x = c->DX * (line->start->x - c->X) + c->DY * (line->start->y - c->Y);

          if (start.x >= minX && start.x <= maxX)
          {
            end.x = c->DX * (line->end->x - c->X) + c->DY * (line->end->y - c->Y);

            if (end.x >= minX && end.x <= maxX)
              return -1;
          }
        }
        else if ((end.y >= 0 && start.y >= 0) || (end.y <= minY && start.y <= minY))
        {
          continue;
        }
      }
    }

    IGN(c, line) = 0;
    linesLeft++;
  }

  if (linesLeft == 0) return 0;

  if ((src->DX != 0 && src->DY != 0) || (tgt->DX != 0 && tgt->DY != 0))
  {
    // eliminate lines that touch the src/tgt lines but are not in view
    for (i = set->loIndex; i <= set->hiIndex; i++)
    {
      solid_t *line = set->lines[i];
      num_t y = 1;

      if (IGN(c, line)) continue;

      if (line->start == src->start || line->start == src->end)
        y = src->DX * (line->end->y - src->start->y) - src->DY * (line->end->x - src->start->x);
      else if (line->end == src->start || line->end == src->end)
        y = src->DX * (line->start->y - src->start->y) - src->DY * (line->start->x - src->start->x);
      else if (line->start == tgt->start || line->start == tgt->end)
        y = tgt->DX * (line->end->y - tgt->start->y) - tgt->DY * (line->end->x - tgt->start->x);
      else if (line->end == tgt->start || line->end == tgt->end)
        y = tgt->DX * (line->start->y - tgt->start->y) - tgt->DY * (line->start->x - tgt->start->x);

      if (y <= 0)
      {
        IGN(c, line) = 1;
        linesLeft--;
      }
    }
  }

  TrimSetBounds(c, set);

  return linesLeft;
}

// which side of the poly-line is the line on?
//    1 above (not completely below), 0 intersects, -1 below, -2 can't tell
static int Intersects(const pt_t *p1, const pt_t *p2, const pt_t *t1, const pt_t *t2)
{
  num_t DX, DY, y1, y2;

  DX = p2->x - p1->x;
  DY = p2->y - p1->y;

  y1 = DX * (t1->y - p1->y) - DY * (t1->x - p1->x);
  y2 = DX * (t2->y - p1->y) - DY * (t2->x - p1->x);

  if (y1 > 0 && y2 > 0) return 1;
  if (y1 <= 0 && y2 <= 0) return -1;

  DX = t2->x - t1->x;
  DY = t2->y - t1->y;

  y1 = DX * (p1->y - t1->y) - DY * (p1->x - t1->x);
  y2 = DX * (p2->y - t1->y) - DY * (p2->x - t1->x);

  if (y1 > 0 && y2 > 0) return -2;
  if (y1 < 0 && y2 < 0) return -2;

  return 0;
}

static int FindSide(const solid_t *line, const poly_t *poly)
{
  int i, completelyBelow = 1;

  for (i = 0; i < poly->noPoints - 1; i++)
  {
    const pt_t *p1 = poly->point[i];
    const pt_t *p2 = poly->point[i + 1];

    switch (Intersects(p1, p2, line->start, line->end))
    {
      case -1: break;
      case  0: return 0;
      case -2:
      case  1: completelyBelow = 0;
    }
  }

  return completelyBelow ? -1 : 1;
}

static void AddToPolyLine(poly_t *poly, const solid_t *line)
{
  num_t DX, DY, y1 = 0, y2;
  int i, j, ptsRemoved, toCopy;

  // find new index start from the 'left'
  for (i = 0; i < poly->noPoints - 1; i++)
  {
    const pt_t *p1 = poly->point[i];
    const pt_t *p2 = poly->point[i + 1];

    DX = p2->x - p1->x;
    DY = p2->y - p1->y;

    y1 = DX * (line->start->y - p1->y) - DY * (line->start->x - p1->x);
    y2 = DX * (line->end->y   - p1->y) - DY * (line->end->x   - p1->x);

    if ((y1 > 0) != (y2 > 0)) break;
  }
  i += 1;

  // find new index start from the 'right'
  for (j = poly->noPoints - 1; j > i; j--)
  {
    const pt_t *p1 = poly->point[j - 1];
    const pt_t *p2 = poly->point[j];
    num_t ya, yb;

    DX = p2->x - p1->x;
    DY = p2->y - p1->y;

    ya = DX * (line->start->y - p1->y) - DY * (line->start->x - p1->x);
    yb = DX * (line->end->y   - p1->y) - DY * (line->end->x   - p1->x);

    if ((ya > 0) != (yb > 0)) break;
  }

  ptsRemoved = j - i;
  toCopy = poly->noPoints - j;

  if (toCopy > 0)
    memmove(&poly->point[i + 1], &poly->point[j], sizeof(pt_t *) * toCopy);

  poly->noPoints += 1 - ptsRemoved;

  poly->point[i] = (y1 > 0) ? line->start : line->end;
  poly->lastPoint = i;
}

static int PolyLinesCross(const poly_t *upper, const poly_t *lower)
{
  int foundAbove = 0, ambiguous = 0;
  int last = 0, max = upper->noPoints - 1;
  int i, j;

  if (upper->lastPoint != -1)
  {
    max  = 2;
    last = upper->lastPoint - 1;
  }

  for (i = 0; i < max; i++)
  {
    const pt_t *p1 = upper->point[last + i];
    const pt_t *p2 = upper->point[last + i + 1];

    for (j = 0; j < lower->noPoints - 1; j++)
    {
      const pt_t *p3 = lower->point[j];
      const pt_t *p4 = lower->point[j + 1];

      switch (Intersects(p1, p2, p3, p4))
      {
        case  1: foundAbove = 1; break;
        case  0: return 1;
        case -2: ambiguous = 1; break;
      }
    }
  }

  if (foundAbove) return 0;

  if (ambiguous)
  {
    const pt_t *p1 = upper->point[0];
    const pt_t *p2 = upper->point[upper->noPoints - 1];
    num_t DX = p2->x - p1->x;
    num_t DY = p2->y - p1->y;

    for (i = 1; i < lower->noPoints - 1; i++)
    {
      const pt_t *tp = lower->point[i];

      if (DX * (tp->y - p1->y) - DY * (tp->x - p1->x) < 0)
        return 1;
    }
  }

  return 0;
}

static int CorrectForNewStart(poly_t *poly)
{
  const pt_t *p0 = poly->point[0];
  int i;

  for (i = poly->noPoints - 1; i > 1; i--)
  {
    const pt_t *p1 = poly->point[i];
    const pt_t *p2 = poly->point[i - 1];
    num_t dx = p1->x - p0->x;
    num_t dy = p1->y - p0->y;
    num_t y  = dx * (p2->y - p0->y) - dy * (p2->x - p0->x);

    if (y < 0)
    {
      poly->point[i - 1] = p0;
      poly->point     += i - 1;
      poly->noPoints  -= i - 1;
      poly->lastPoint -= i - 1;
      return 1;
    }
  }

  return 0;
}

static int CorrectForNewEnd(poly_t *poly)
{
  const pt_t *p0 = poly->point[poly->noPoints - 1];
  int i;

  for (i = 0; i < poly->noPoints - 2; i++)
  {
    const pt_t *p1 = poly->point[i];
    const pt_t *p2 = poly->point[i + 1];
    num_t dx = p0->x - p1->x;
    num_t dy = p0->y - p1->y;
    num_t y  = dx * (p2->y - p1->y) - dy * (p2->x - p1->x);

    if (y < 0)
    {
      poly->point[i + 1] = p0;
      poly->noPoints    -= poly->noPoints - i - 2;
      return 1;
    }
  }

  return 0;
}

static int AdjustEndPoints(trans_t *left, trans_t *right, poly_t *upper, poly_t *lower)
{
  const pt_t *test;
  num_t dx, dy, y;
  int changed = 0;

  if (upper->lastPoint == -1) return 1;

  test = upper->point[upper->lastPoint];

  dx = test->x - left->hiPoint->x;
  dy = test->y - left->hiPoint->y;
  y  = dx * (right->hiPoint->y - left->hiPoint->y) -
       dy * (right->hiPoint->x - left->hiPoint->x);

  if (y > 0)
  {
    num_t num = (right->start->y - left->hiPoint->y) * dx -
                (right->start->x - left->hiPoint->x) * dy;
    num_t det = right->DX * dy - right->DY * dx;
    num_t t = num / det;

    if (t <= right->lo) return 0;

    if (t < right->hi)
    {
      right->hi = t;
      right->hiPoint->x = right->start->x + t * right->DX;
      right->hiPoint->y = right->start->y + t * right->DY;
      changed |= CorrectForNewStart(upper);
    }
  }

  dx = test->x - right->loPoint->x;
  dy = test->y - right->loPoint->y;
  y  = dx * (left->loPoint->y - right->loPoint->y) -
       dy * (left->loPoint->x - right->loPoint->x);

  if (y < 0)
  {
    num_t num = (left->start->y - right->loPoint->y) * dx -
                (left->start->x - right->loPoint->x) * dy;
    num_t det = left->DX * dy - left->DY * dx;
    num_t t = num / det;

    if (t >= left->hi) return 0;

    if (t > left->lo)
    {
      left->lo = t;
      left->loPoint->x = left->start->x + t * left->DX;
      left->loPoint->y = left->start->y + t * left->DY;
      changed |= CorrectForNewEnd(upper);
    }
  }

  return (changed && PolyLinesCross(upper, lower)) ? 0 : 1;
}

static int FindPolyLines(ctx_t *c, world_t *w)
{
  poly_t *upperPoly = &w->upperPoly;
  poly_t *lowerPoly = &w->lowerPoly;
  lineset_t *set = &w->solidSet;

  for (;;)
  {
    int done = 1;
    int stray = 0;
    int i;

    for (i = set->loIndex; i <= set->hiIndex; i++)
    {
      solid_t *line = set->lines[i];

      if (IGN(c, line)) continue;

      switch (FindSide(line, lowerPoly))
      {
        case 1:  // completely above the lower/right poly-line
          switch (FindSide(line, upperPoly))
          {
            case 1:  // between the two poly-lines
              stray = 1;
              break;

            case 0:  // intersects the upper/left poly-line
              if (stray) done = 0;

              AddToPolyLine(upperPoly, line);

              if (lowerPoly->noPoints > 2 && PolyLinesCross(upperPoly, lowerPoly))
                return 0;

              if (! AdjustEndPoints(w->src, w->tgt, upperPoly, lowerPoly))
                return 0;

              /* FALLTHROUGH */
            case -1:  // completely above the upper/left poly-line
              IGN(c, line) = 1;
              break;
          }
          break;

        case 0:  // intersects the lower/right poly-line
          if (stray) done = 0;

          AddToPolyLine(lowerPoly, line);

          if (PolyLinesCross(lowerPoly, upperPoly))
            return 0;

          if (! AdjustEndPoints(w->tgt, w->src, lowerPoly, upperPoly))
            return 0;

          /* FALLTHROUGH */
        case -1:  // completely below the lower/right poly-line
          IGN(c, line) = 1;
          break;
      }
    }

    if (done) break;

    TrimSetBounds(c, set);
  }

  return 1;
}

static void InitializeWorld(ctx_t *c, world_t *w, trans_t *src, trans_t *tgt)
{
  poly_t *lowerPoly, *upperPoly;

  w->src = src;
  w->tgt = tgt;

  w->solidSet.lines   = c->testLines;
  w->solidSet.loIndex = 0;
  w->solidSet.hiIndex = -1;

  c->p[0] = *src->start;
  c->p[1] = *src->end;
  c->p[2] = *tgt->start;
  c->p[3] = *tgt->end;

  src->loPoint = &c->p[0];  src->lo = 0;
  src->hiPoint = &c->p[1];  src->hi = 1;
  tgt->loPoint = &c->p[2];  tgt->lo = 0;
  tgt->hiPoint = &c->p[3];  tgt->hi = 1;

  lowerPoly = &w->lowerPoly;
  lowerPoly->point     = c->polyPoints;
  lowerPoly->noPoints  = 2;
  lowerPoly->lastPoint = -1;
  lowerPoly->point[0]  = src->hiPoint;
  lowerPoly->point[1]  = tgt->loPoint;

  upperPoly = &w->upperPoly;
  upperPoly->point     = &c->polyPoints[num_solid + 2];
  upperPoly->noPoints  = 2;
  upperPoly->lastPoint = -1;
  upperPoly->point[0]  = tgt->hiPoint;
  upperPoly->point[1]  = src->loPoint;
}

// true if there is an unobstructed line between src and tgt
static int CheckLOS(ctx_t *c, trans_t *src, trans_t *tgt)
{
  world_t w;

  InitializeWorld(c, &w, src, tgt);

  MarkBlockMap(c, &w);

  if (FindInterveningLines(c, &w.solidSet))
  {
    const pt_t *common = NULL;
    int i;

    // if src & tgt touch, look for a quick way out: no lines through
    // the common point
    if (((common = src->end) == tgt->start) || ((common = src->start) == tgt->end))
    {
      int blocked = 0;

      for (i = w.solidSet.loIndex; i <= w.solidSet.hiIndex; i++)
      {
        solid_t *line = w.solidSet.lines[i];

        if (line->start == common || line->end == common)
        {
          blocked = 1;
          break;
        }
      }

      if (! blocked) return 1;
    }

    switch (TrimLines(c, w.src, w.tgt, &w.solidSet))
    {
      case -1:  // a single line completely blocks the view
        return 0;

      case 0:   // no intervening lines left
        break;

      default:
        if (! FindPolyLines(c, &w)) return 0;
        break;
    }
  }

  return 1;
}

static int DivideRegion(ctx_t *c, trans_t *src, trans_t *tgt)
{
  num_t num = tgt->DX * (src->start->y - tgt->start->y) - tgt->DY * (src->start->x - tgt->start->x);
  num_t det = src->DX * tgt->DY - src->DY * tgt->DX;
  num_t t;
  pt_t cross;
  trans_t newSrc;
  int vis;

  if (det == 0)
    return CheckLOS(c, src, tgt);

  t = num / det;

  if (t <= 0 || t >= 1)
    return CheckLOS(c, src, tgt);

  cross.x = src->start->x + t * src->DX;
  cross.y = src->start->y + t * src->DY;

  // too close to an end point to split
  if ((cross.x == src->start->x && cross.y == src->start->y) ||
      (cross.x == src->end->x   && cross.y == src->end->y))
    return CheckLOS(c, src, tgt);

  newSrc = *src;
  newSrc.end = &cross;

  vis = CheckLOS(c, &newSrc, tgt);

  if (! vis)
  {
    newSrc.start = &cross;
    newSrc.end   = src->end;

    SWAPPT(tgt->start, tgt->end);
    tgt->DX = -tgt->DX;
    tgt->DY = -tgt->DY;

    vis = CheckLOS(c, &newSrc, tgt);
  }

  return vis;
}


// ----- line pair cache --------------------------------------------------

static size_t LinePairOffset(int a, int b)
{
  size_t row = (a < b) ? a : b;
  size_t col = (a < b) ? b : a;

  return row * (2 * (size_t) num_trans - 1 - row) / 2 + (col - row - 1);
}

static int GetLineVisibility(int a, int b)
{
  size_t off = LinePairOffset(a, b);
  unsigned char data = LOAD8(&line_vis[off / 4]);

  return 0x03 & (data >> (2 * (off % 4)));
}

static void SetLineVisibility(int a, int b, int vis)
{
  size_t off = LinePairOffset(a, b);

  // entries only ever change from VIS_UNKNOWN, so OR is enough
  OR8(&line_vis[off / 4], (unsigned char) (vis << (2 * (off % 4))));
}

static int DontBother(const trans_t *s, const trans_t *t)
{
  return (LOAD8(&TABLE(s->left_sec,  t->left_sec))  != VIS_UNKNOWN) &&
         (LOAD8(&TABLE(s->left_sec,  t->right_sec)) != VIS_UNKNOWN) &&
         (LOAD8(&TABLE(s->right_sec, t->left_sec))  != VIS_UNKNOWN) &&
         (LOAD8(&TABLE(s->right_sec, t->right_sec)) != VIS_UNKNOWN);
}

static int TestLinePair(ctx_t *c, int ia, int ib)
{
  const trans_t *srcLine = &transes[ia];
  const trans_t *tgtLine = &transes[ib];
  trans_t src, tgt;
  int vis, bisect = 0, adj;

  if (GetLineVisibility(ia, ib) != VIS_UNKNOWN || DontBother(srcLine, tgtLine))
    return 0;

  src = *srcLine;
  tgt = *tgtLine;

  adj = AdjustLinePair(&src, &tgt, &bisect);

  if (adj < 0)
    vis = 1;        // the lines cross: they can see each other
  else if (adj == 0)
    vis = 0;
  else
    vis = bisect ? DivideRegion(c, &src, &tgt) : CheckLOS(c, &src, &tgt);

  SetLineVisibility(ia, ib, vis ? VIS_VISIBLE : VIS_HIDDEN);

  return vis;
}

static void MarkPairVisible(const trans_t *s, const trans_t *t)
{
  MarkVisibility(s->left_sec,  t->left_sec,  VIS_VISIBLE);
  MarkVisibility(s->left_sec,  t->right_sec, VIS_VISIBLE);
  MarkVisibility(s->right_sec, t->left_sec,  VIS_VISIBLE);
  MarkVisibility(s->right_sec, t->right_sec, VIS_VISIBLE);
}


// ----- per thread scratch -----------------------------------------------

static void ThreadInit(ctx_t *c)
{
  int i;

  memset(c, 0, sizeof(*c));

  c->bound_lo   = calloc(grid_rows, sizeof(int));
  c->bound_hi   = calloc(grid_rows, sizeof(int));
  c->checkLine  = calloc(num_solid + 1, 1);
  c->ignore     = calloc(num_solid + 1, 1);
  c->testLines  = calloc(num_solid + 2, sizeof(solid_t *));
  c->polyPoints = calloc(2 * (num_solid + 2), sizeof(pt_t *));

  if (! c->bound_lo || ! c->bound_hi || ! c->checkLine || ! c->ignore ||
      ! c->testLines || ! c->polyPoints)
    FatalError("Out of memory (reject)");

  for (i = 0; i < grid_rows; i++)
  {
    c->bound_lo[i] = grid_cols;
    c->bound_hi[i] = -1;
  }
}

static void ThreadFree(ctx_t *c)
{
  free(c->bound_lo);
  free(c->bound_hi);
  free(c->checkLine);
  free(c->ignore);
  free(c->testLines);
  free(c->polyPoints);
}


// ----- worker threads -----------------------------------------------------

#define MAX_THREADS  64

static int *work_lines;           // line_map: the order to test the lines in
static int work_count;
static int work_next;             // next index to hand out (atomic)
static int work_tick_step;

static int DefaultThreads(void)
{
  const char *env;
  int n = 0;

  env = getenv("BSP64_THREADS");
  if (env) n = atoi(env);

  if (n <= 0)
  {
#if defined(_WIN32)
    env = getenv("NUMBER_OF_PROCESSORS");
    if (env) n = atoi(env);
#elif defined(_SC_NPROCESSORS_ONLN)
    n = (int) sysconf(_SC_NPROCESSORS_ONLN);
#endif
  }

  if (n < 1) n = 1;
  if (n > MAX_THREADS) n = MAX_THREADS;

  return n;
}

// Each worker takes the next source line, tests it against every line
// after it, and repeats until all lines are done.
static void *Worker(void *arg)
{
  int id = (int) (size_t) arg;
  int next_tick = work_tick_step;
  ctx_t ctx;

  ThreadInit(&ctx);

  for (;;)
  {
    int ii = __atomic_fetch_add(&work_next, 1, __ATOMIC_RELAXED);
    int a, jj;

    if (ii >= work_count) break;

    a = work_lines[ii];

    for (jj = work_count - 1; jj > ii; jj--)
    {
      int b = work_lines[jj];

      if (TestLinePair(&ctx, a, b))
        MarkPairVisible(&transes[a], &transes[b]);
    }

    // only the calling thread updates the progress display
    if (id == 0 && ii >= next_tick)
    {
      DisplayTicker();
      next_tick = ii + work_tick_step;
    }
  }

  ThreadFree(&ctx);

  return NULL;
}


// ----- building the input lists -------------------------------------------

typedef struct { double x, y; int slot; } pt_sort_t;

static int ComparePts(const void *a, const void *b)
{
  const pt_sort_t *A = a, *B = b;

  if (A->x < B->x) return -1;
  if (A->x > B->x) return +1;
  if (A->y < B->y) return -1;
  if (A->y > B->y) return +1;
  return A->slot - B->slot;
}

typedef struct { int sec; int count; } sec_info_t;

static int CompareSectors(const void *a, const void *b)
{
  const sec_info_t *A = a, *B = b;

  if (A->count != B->count) return B->count - A->count;
  return A->sec - B->sec;
}

//
// Union-find over sectors joined by see-through lines.  Sectors in
// different components can never see each other.
//
static int Find(int *parent, int i)
{
  while (parent[i] != i)
  {
    parent[i] = parent[parent[i]];
    i = parent[i];
  }
  return i;
}


// the work: fills 'rej_table' (rej_sectors x rej_sectors)
static void BuildTable(void)
{
  int i, j;
  int nlines = num_linedefs;

  pt_t *pts = NULL;
  int num_pts = 0;
  const pt_t **end_pt;          // two per linedef
  pt_sort_t *sorted;

  int *parent;
  sec_info_t *order;
  int *sec_lines_count;
  int *line_map = NULL;
  int line_map_size = 0;
  unsigned char *in_map;
  int **sec_lines;
  int *sec_fill;

  int max_sector_lines;

  // ---- unique end points (shared vertices must compare by pointer)

  end_pt = calloc(2 * (size_t) nlines + 2, sizeof(pt_t *));
  sorted = calloc(2 * (size_t) nlines + 2, sizeof(pt_sort_t));
  pts    = calloc(2 * (size_t) nlines + 2, sizeof(pt_t));

  if (! end_pt || ! sorted || ! pts) FatalError("Out of memory (reject)");

  for (i = 0; i < nlines; i++)
  {
    linedef_t *L = LookupLinedef(i);

    sorted[2*i  ].x = L->start->x; sorted[2*i  ].y = L->start->y; sorted[2*i  ].slot = 2*i;
    sorted[2*i+1].x = L->end->x;   sorted[2*i+1].y = L->end->y;   sorted[2*i+1].slot = 2*i+1;
  }

  qsort(sorted, 2 * (size_t) nlines, sizeof(pt_sort_t), ComparePts);

  for (i = 0; i < 2 * nlines; i++)
  {
    if (i == 0 || sorted[i].x != sorted[i-1].x || sorted[i].y != sorted[i-1].y)
    {
      pts[num_pts].x = (num_t) sorted[i].x;
      pts[num_pts].y = (num_t) sorted[i].y;
      num_pts++;
    }
    end_pt[sorted[i].slot] = &pts[num_pts - 1];
  }

  free(sorted);

  // ---- classify lines

  solids = calloc(nlines + 1, sizeof(solid_t));
  transes = calloc(nlines + 1, sizeof(trans_t));
  if (! solids || ! transes) FatalError("Out of memory (reject)");

  num_solid = 0;
  num_trans = 0;

  for (i = 0; i < nlines; i++)
  {
    linedef_t *L = LookupLinedef(i);
    const pt_t *s = end_pt[2*i];
    const pt_t *e = end_pt[2*i+1];

    if (s == e) continue;         // zero length

    // like the engine, a line lets sight through when it has a back
    // sidedef, whatever its flags say.
    if (L->right && L->left && L->right->sector && L->left->sector)
    {
      trans_t *T;

      if (L->right->sector == L->left->sector) continue;   // same sector

      T = &transes[num_trans++];
      T->start = s;
      T->end   = e;
      T->right_sec = L->right->sector->index;
      T->left_sec  = L->left->sector->index;
      T->DX = e->x - s->x;
      T->DY = e->y - s->y;
    }
    else if (L->right || L->left)
    {
      solid_t *S = &solids[num_solid];

      S->id = num_solid++;
      S->start = s;
      S->end   = e;
    }
  }

  // ---- table with the easy cases

  rej_table = calloc((size_t) rej_sectors * rej_sectors + 8, 1);
  if (! rej_table) FatalError("Out of memory (reject table)");

  for (i = 0; i < rej_sectors; i++)
    TABLE(i, i) = VIS_VISIBLE;

  parent = calloc(rej_sectors + 1, sizeof(int));
  for (i = 0; i < rej_sectors; i++) parent[i] = i;

  sec_lines_count = calloc(rej_sectors + 1, sizeof(int));
  sec_fill        = calloc(rej_sectors + 1, sizeof(int));
  sec_lines       = calloc(rej_sectors + 1, sizeof(int *));
  if (! parent || ! sec_lines_count || ! sec_fill || ! sec_lines)
    FatalError("Out of memory (reject)");

  for (i = 0; i < num_trans; i++)
  {
    int a = Find(parent, transes[i].left_sec);
    int b = Find(parent, transes[i].right_sec);

    if (a != b) parent[a] = b;

    sec_lines_count[transes[i].left_sec]++;
    sec_lines_count[transes[i].right_sec]++;
  }

  // different components can never see each other
  {
    int *comp = calloc(rej_sectors + 1, sizeof(int));
    if (! comp) FatalError("Out of memory (reject)");

    for (i = 0; i < rej_sectors; i++) comp[i] = Find(parent, i);

    for (i = 0; i < rej_sectors; i++)
      for (j = 0; j < i; j++)
        if (comp[i] != comp[j])
        {
          TABLE(i, j) = VIS_HIDDEN;
          TABLE(j, i) = VIS_HIDDEN;
        }

    free(comp);
  }

  // sectors without see-through lines are hidden from everything else
  for (i = 0; i < rej_sectors; i++)
    if (sec_lines_count[i] == 0)
      for (j = 0; j < rej_sectors; j++)
        MarkVisibility(i, j, VIS_HIDDEN);

  // each sector can see its immediate neighbours
  for (i = 0; i < num_trans; i++)
    MarkVisibility(transes[i].left_sec, transes[i].right_sec, VIS_VISIBLE);

  PrintVerbose("Reject: %d see-through lines, %d solid lines, %d sectors\n",
      num_trans, num_solid, rej_sectors);

  if (num_trans > 1 && num_solid >= 0)
  {
    int progress_step;
    long long total = (long long) num_trans * (num_trans - 1) / 2;

    // ---- lines per sector, then order the lines
    //      (sectors with most lines first; only a speed hint)

    max_sector_lines = 0;
    for (i = 0; i < rej_sectors; i++)
    {
      sec_lines[i] = calloc(sec_lines_count[i] + 1, sizeof(int));
      if (! sec_lines[i]) FatalError("Out of memory (reject)");
      if (sec_lines_count[i] > max_sector_lines) max_sector_lines = sec_lines_count[i];
    }

    for (i = 0; i < num_trans; i++)
    {
      sec_lines[transes[i].left_sec ][sec_fill[transes[i].left_sec ]++] = i;
      sec_lines[transes[i].right_sec][sec_fill[transes[i].right_sec]++] = i;
    }

    order = calloc(rej_sectors + 1, sizeof(sec_info_t));
    in_map = calloc(num_trans + 1, 1);
    line_map = calloc(num_trans + 1, sizeof(int));
    if (! order || ! in_map || ! line_map) FatalError("Out of memory (reject)");

    for (i = 0; i < rej_sectors; i++)
    {
      order[i].sec = i;
      order[i].count = sec_lines_count[i];
    }

    qsort(order, rej_sectors, sizeof(sec_info_t), CompareSectors);

    for (i = 0; i < rej_sectors; i++)
    {
      int s = order[i].sec;

      for (j = 0; j < sec_lines_count[s]; j++)
      {
        int ln = sec_lines[s][j];

        if (! in_map[ln])
        {
          in_map[ln] = 1;
          line_map[line_map_size++] = ln;
        }
      }
    }

    free(in_map);
    free(order);

    // ---- solid line grid and line pair cache

    BuildGrid(pts, num_pts);

    line_vis = calloc((size_t) (total / 4 + 2), 1);
    if (! line_vis) FatalError("Out of memory (reject line cache)");

    progress_step = line_map_size / 50 + 1;

    // ---- the hard part: test all the line pairs

    {
      pthread_t threads[MAX_THREADS];
      int started = 0;
      int num_threads = DefaultThreads();
      int t;

      work_next = 0;
      work_count = line_map_size;
      work_lines = line_map;
      work_tick_step = progress_step;

      if (num_threads > line_map_size) num_threads = line_map_size;
      if (num_threads < 1) num_threads = 1;

      // the calling thread is worker 0
      for (t = 1; t < num_threads; t++)
      {
        if (pthread_create(&threads[started], NULL, Worker, (void *) (size_t) t) != 0)
          break;

        started++;
      }

      Worker((void *) (size_t) 0);

      for (t = 0; t < started; t++)
        pthread_join(threads[t], NULL);
    }

    for (i = 0; i < rej_sectors; i++) free(sec_lines[i]);

    free(line_map);
    free(line_vis);
    line_vis = NULL;

    free(cell_start);
    free(cell_items);
    cell_start = cell_items = NULL;
  }
  else
  {
    for (i = 0; i < rej_sectors; i++) free(sec_lines[i]);
  }

  free(parent);
  free(sec_lines_count);
  free(sec_fill);
  free(sec_lines);
  free(end_pt);
  free(pts);
  free(solids);
  free(transes);
  solids = NULL;
  transes = NULL;
}


//
// PutReject
//
// Builds a reject table from real line of sight between sectors, see
// the notes at the top of this file.
//
void PutReject(void)
{
  size_t reject_size;
  unsigned char *matrix;
  lump_t *lump;
  int view, target;

  DisplayTicker();

  rej_sectors = num_sectors;

  reject_size = ((size_t) rej_sectors * rej_sectors + 7) / 8;
  matrix = calloc(reject_size + 1, 1);
  if (! matrix) FatalError("Out of memory (reject lump)");

  BuildTable();

  // special 999 sectors are hidden from everything, and see nothing
  for (view = 0; view < rej_sectors; view++)
  {
    sector_t *vs = LookupSector(view);

    if (vs->special != SPECIAL_BLIND) continue;

    for (target = 0; target < rej_sectors; target++)
    {
      if (target == view) continue;

      TABLE(view, target)  = VIS_HIDDEN;
      TABLE(target, view)  = VIS_HIDDEN;
    }
  }

  // pairs that are not known to be visible are hidden
  for (view = 0; view < rej_sectors; view++)
  for (target = 0; target < rej_sectors; target++)
  {
    size_t p;

    if (view == target) continue;
    if (TABLE(view, target) == VIS_VISIBLE) continue;

    p = (size_t) view * rej_sectors + target;
    matrix[p >> 3] |= (1 << (p & 7));
  }

  lump = CreateLevelLump("REJECT");

  AppendLevelLump(lump, matrix, (int) reject_size);

  PrintVerbose("Added line-of-sight reject lump\n");

  free(matrix);
  free(rej_table);
  rej_table = NULL;
}
