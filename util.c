//------------------------------------------------------------------------
// UTILITY : general purpose functions
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

#include "util.h"

#ifdef WIN32
#include <windows.h>
#else
#include <time.h>
#endif


//
// UtilCalloc
//
// Allocate memory with error checking.  Zeros the memory.
//
void *UtilCalloc(int size)
{
  void *ret = calloc(1, size);
  
  if (!ret)
    FatalError("Out of memory (cannot allocate %d bytes)", size);

  return ret;
}

//
// UtilRealloc
//
// Reallocate memory with error checking.
//
void *UtilRealloc(void *old, int size)
{
  void *ret = realloc(old, size);

  if (!ret)
    FatalError("Out of memory (cannot reallocate %d bytes)", size);

  return ret;
}

//
// UtilFree
//
// Free the memory with error checking.
//
void UtilFree(void *data)
{
  if (data == NULL)
    InternalError("Trying to free a NULL pointer");
  
  free(data);
}

//
// UtilStrDup
//
// Duplicate a string with error checking.
//
char *UtilStrDup(const char *str)
{
  char *result;
  int len = (int)strlen(str);

  result = UtilCalloc(len+1);

  if (len > 0)
    memcpy(result, str, len);
  
  result[len] = 0;

  return result;
}

//
// UtilStrNDup
//
// Duplicate a limited length string.
//
char *UtilStrNDup(const char *str, int size)
{
  char *result;
  int len;

  for (len=0; len < size && str[len]; len++)
  { }

  result = UtilCalloc(len+1);

  if (len > 0)
    memcpy(result, str, len);
  
  result[len] = 0;

  return result;
}

char *UtilFormat(const char *str, ...)
{
  /* Algorithm: keep doubling the allocated buffer size
   * until the output fits. Based on code by Darren Salt.
   */
  char *buf = NULL;
  int buf_size = 128;
  
  for (;;)
  {
    va_list args;
    int out_len;

    buf_size *= 2;

    buf = realloc(buf, buf_size);
    if (!buf)
      FatalError("Out of memory (formatting string)");

    va_start(args, str);
    out_len = _vsnprintf(buf, buf_size, str, args);
    va_end(args);

    // old versions of vsnprintf() simply return -1 when
    // the output doesn't fit.
    if (out_len < 0 || out_len >= buf_size)
      continue;

    return buf;
  }
}

int UtilStrCaseCmp(const char *A, const char *B)
{
  for (; *A || *B; A++, B++)
  {
    // this test also catches end-of-string conditions
    if (toupper(*A) != toupper(*B))
      return (toupper(*A) - toupper(*B));
  }

  // strings are equal
  return 0;
}


//
// UtilRoundPOW2
//
// Rounds the value _up_ to the nearest power of two.
//
int UtilRoundPOW2(int x)
{
  int tmp;

  if (x <= 2)
    return x;

  x--;
  
  for (tmp=x / 2; tmp; tmp /= 2)
    x |= tmp;
  
  return (x + 1);
}


//
// UtilComputeAngle
//
// Translate (dx, dy) into an angle value (degrees)
//
angle_g UtilComputeAngle(float_g dx, float_g dy)
{
  double angle;

  if (dx == 0)
    return (dy > 0) ? 90.0 : 270.0;

  angle = atan2((double) dy, (double) dx) * 180.0 / M_PI;

  if (angle < 0) 
    angle += 360.0;

  return angle;
}


//------------------------------------------------------------------------
//  Adler-32 CHECKSUM Code
//------------------------------------------------------------------------

