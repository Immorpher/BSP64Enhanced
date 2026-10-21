//------------------------------------------------------------------------
// WAD : WAD read/write functions.
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

#ifndef __GLBSP_WAD_H__
#define __GLBSP_WAD_H__

#include "structs.h"
#include "system.h"


struct lump_s;


// wad header

typedef struct wad_s
{
  // kind of wad file
  enum { IWAD, PWAD } kind;

  // number of entries in directory
  int num_entries;

  // offset to start of directory
  int dir_start;

  // current directory entries
  struct lump_s *dir_head;
  struct lump_s *dir_tail;

  // current level
  struct lump_s *current_level;

  // array of level names found
  const char ** level_names;
  int num_level_names;
}
wad_t;


// level information

typedef struct level_s
{
  // various flags
  int flags;

  // the child lump list
  struct lump_s *children;

  // information on overflow
  int soft_limit;
  int hard_limit;
}
level_t;

/* limit flags, to show what went wrong */
#define LIMIT_VERTEXES     0x000001
#define LIMIT_SECTORS      0x000002
#define LIMIT_SIDEDEFS     0x000004
#define LIMIT_LINEDEFS     0x000008

#define LIMIT_SEGS         0x000010
#define LIMIT_SSECTORS     0x000020
#define LIMIT_NODES        0x000040

#define LIMIT_BAD_SIDE     0x001000
#define LIMIT_BMAP_TRUNC   0x002000
#define LIMIT_BLOCKMAP     0x004000


// directory entry

typedef struct lump_s
{
  // link in list
  struct lump_s *next;
  struct lump_s *prev;

  // name of lump
  char *name;

  // offset to start of lump
  int start;
  int new_start;

  // length of lump
  int length;
  int space;

  // various flags
  int flags;
 
  // data of lump
  void *data;

  // level information, usually NULL
  level_t *lev_info;
}
lump_t;

/* this lump should be copied from the input wad */
#define LUMP_COPY_ME       0x0004

/* this lump shouldn't be written to the output wad */
#define LUMP_IGNORE_ME     0x0008

/* this lump needs to be loaded */
#define LUMP_READ_ME       0x0100

/* this lump is new (didn't exist in the original) */
#define LUMP_NEW           0x0200


/* ----- function prototypes --------------------- */

// open the input wad file and read the contents into memory.  When
// 'load_all' is false, lumps other than level info will be marked as
// copyable instead of loaded.
//
// Returns GLBSP_E_OK if all went well, otherwise an error code (in
// which case cur_comms->message has been set and all files/memory
// have been freed).
//
glbsp_ret_e ReadWadFile(const char *filename);

// open the output wad file and write the contents.  Any lumps marked
// as copyable will be copied from the input file instead of from
// memory.  Lumps marked as ignorable will be skipped.
//
// Returns GLBSP_E_OK if all went well, otherwise an error code (in
// which case cur_comms->message has been set -- but no files/memory
// are freed).
//
glbsp_ret_e WriteWadFile(const char *filename);

// close all wad files and free any memory.
void CloseWads(void);

// returns the number of levels found in the wad.
int CountLevels(void);

// find the next level lump in the wad directory, and store the
// reference in 'wad.current_level'.  Call this straight after
// ReadWadFile() to get the first level.  Returns 1 if found,
// otherwise 0 if there are no more levels in the wad.
//
int FindNextLevel(void);

// return the current level name
const char *GetLevelName(void);

// find the level lump with the given name in the current level, and
// return a reference to it.  Returns NULL if no such lump exists.
// Level lumps are always present in memory (i.e. never marked
// copyable).
//
lump_t *FindLevelLump(const char *name);

// tests if the level lump contains nothing but zeros.

// create a new lump in the current level with the given name.  If
// such a lump already exists, it is truncated to zero length.
//
lump_t *CreateLevelLump(const char *name);

// append some raw data to the end of the given level lump (created
// with the above function).
//
void AppendLevelLump(lump_t *lump, const void *data, int length);

// for the current GL lump, add a keyword/value pair into the
// level marker lump.

// Zlib compression support
//void ZLibBeginLump(lump_t *lump);
//void ZLibAppendLump(const void *data, int length);
//void ZLibFinishLump(void);

// mark the fact that this level failed to build.
void MarkSoftFailure(int soft);
void MarkHardFailure(int hard);

// alert the user if any levels failed to build properly.
void ReportFailedLevels(void);


/* ----- conversion macros ----------------------- */


#define UINT8(x)   ((uint8_g) (x))
#define SINT8(x)   ((sint8_g) (x))

#define UINT16(x)  Endian_U16(x)
#define UINT32(x)  Endian_U32(x)

#define SINT16(x)  ((sint16_g) Endian_U16((uint16_g) (x)))
#define SINT32(x)  ((sint32_g) Endian_U32((uint32_g) (x)))


#endif /* __GLBSP_WAD_H__ */
