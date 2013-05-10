// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright(C) 2009 James Haley
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//
// Native implementation of the zone API. This doesn't have a lot of advantage
// over the zone heap during normal play, but it shines when the game is
// under stress, whereas the zone heap chokes doing an O(N) search over the
// block list and wasting time dumping purgables, causing unnecessary disk 
// thrashing.
//
// When running with this heap, there is no limitation to the amount of memory
// allocated except what the system will provide.
//
// Limitations:
// * Purgables are never currently dumped unless the machine runs out of RAM.
// * Instrumentation cannot track the amount of free memory.
// * Heap check is limited to a zone ID check.
// * Core dump function is not supported.
//
//-----------------------------------------------------------------------------

#include "z_zone.h"
#include "i_system.h"
#include "doomstat.h"
#include "m_argv.h"

//=============================================================================
//
// Macros
//

// Uncomment this to see real-time memory allocation statistics.
//#define INSTRUMENTED

// Uncomment this to use memory scrambling on allocations and frees.
// haleyjd 01/27/09: made independent of INSTRUMENTED
//#define ZONESCRAMBLE

// Uncomment this to exhaustively run memory checks
// while the game is running (this is EXTREMELY slow).
// haleyjd 01/27/09: made independent of INSTRUMENTED
//#define CHECKHEAP

// Uncomment this to perform id checks on zone blocks,
// to detect corrupted and illegally freed blocks
//#define ZONEIDCHECK

// Uncomment this to dump the heap to file on exit.
//#define DUMPONEXIT

// Uncomment this to log all memory operations to a file
//#define ZONEFILE

// Uncomment this to enable more verbose - but dangerous - error messages
// that could cause crashes
//#define ZONEVERBOSE

//=============================================================================
//
// Tunables
//

// signature for block header
#define ZONEID  0x931d4a11

// End Tunables

//=============================================================================
//
// Memblock Structure
// 

typedef struct memblock
{
#ifdef ZONEIDCHECK
  unsigned int id;
#endif

  struct memblock *next,**prev;
  size_t size;
  void **user;
  unsigned char tag;

#ifdef INSTRUMENTED
  const char *file;
  int line;
#endif
} memblock_t;

//=============================================================================
//
// Heap Globals
//

// haleyjd 03/08/10: dynamically calculated header size;
// round sizeof(memblock_t) up to nearest 16-byte boundary. This should work
// just about everywhere, and keeps the assumption of a 32-byte header on 
// 32-bit. 64-bit will use a 64-byte header.
static const size_t header_size = (sizeof(memblock_t) + 15) & ~15;

static memblock_t *blockbytag[PU_MAX];   // used for tracking all zone blocks

// ZoneObject class statics
ZoneObject *ZoneObject::objectbytag[PU_MAX]; // like blockbytag but for objects
void       *ZoneObject::newalloc;            // most recent ZoneObject alloc

//=============================================================================
//
// Debug Macros
//
// haleyjd 11/18/09
// These help clean up the #ifdef hell.
//

// Instrumentation macros
#ifdef INSTRUMENTED
#define INSTRUMENT(a) a
#define INSTRUMENT_IF(opt, a, b) if((opt)) (a); else (b)
#else
#define INSTRUMENT(a)
#define INSTRUMENT_IF(opt, a, b)
#endif

// ID Check macros
#ifdef ZONEIDCHECK

#define IDCHECK(a) a
#define IDBOOL(a) (a)

//
// Z_IDCheckNB
// 
// Performs a fatal error condition check contingent on the definition
// of ZONEIDCHECK, in any context where a memblock pointer is not available.
//
static void Z_IDCheckNB(bool err, const char *errmsg,
                        const char *file, int line)
{
   if(err)
      I_FatalError(I_ERR_KILL, "%s\nSource: %s:%d\n", errmsg, file, line);
}

//
// Z_IDCheck
//
// Performs a fatal error condition check contingent on the definition
// of ZONEIDCHECK, and accepts a memblock pointer for provision of additional
// malloc source information available when INSTRUMENTED is also defined.
//
static void Z_IDCheck(bool err, const char *errmsg, 
                      memblock_t *block, const char *file, int line)
{
   if(err)
   {
      I_FatalError(I_ERR_KILL,
                   "%s\nSource: %s:%d\nSource of malloc: %s:%d\n",
                   errmsg, file, line,
#if defined(ZONEVERBOSE) && defined(INSTRUMENTED)
                   block->file, block->line
#else
                   "(not available)", 0
#endif
                  );
   }
}
#else

#define IDCHECK(a)
#define IDBOOL(a) false
#define Z_IDCheckNB(err, errmsg, file, line)
#define Z_IDCheck(err, errmsg, block, file, line)

#endif

// Heap checking macro
#ifdef CHECKHEAP
#define DEBUG_CHECKHEAP() Z_CheckHeap()
#else
#define DEBUG_CHECKHEAP()
#endif

// Zone scrambling macro
#ifdef ZONESCRAMBLE
#define SCRAMBLER(b, s) memset((b), 1 | (gametic & 0xff), (s))
#else
#define SCRAMBLER(b, s)
#endif

//=============================================================================
//
// Instrumentation Statistics
//

// statistics for evaluating performance
INSTRUMENT(size_t memorybytag[PU_MAX]); // haleyjd 04/02/11: track by tag
INSTRUMENT(int printstats = 0);         // killough 8/23/98

// haleyjd 04/02/11: Instrumentation output has been moved to d_main.cpp and
// is now drawn directly to the screen instead of passing through doom_printf.

// haleyjd 06/20/09: removed unused, crashy, and non-useful Z_DumpHistory

//=============================================================================
//
// Zone Log File
//
// haleyjd 09/16/06
//

#ifdef ZONEFILE
static FILE *zonelog;
static bool  logclosed;
#endif

static void Z_OpenLogFile()
{
#ifdef ZONEFILE
   if(!zonelog && !logclosed)
      zonelog = fopen("zonelog.txt", "w");
#endif
}

static void Z_CloseLogFile()
{
#ifdef ZONEFILE
   if(zonelog)
   {
      fputs("Closing zone log", zonelog);
      fclose(zonelog);
      zonelog = NULL;      
   }
   // Do not open a new log after this point.
   logclosed = true;
#endif
}

static void Z_LogPrintf(const char *msg, ...)
{
#ifdef ZONEFILE
   if(!zonelog)
      Z_OpenLogFile();
   if(zonelog)
   {
      va_list ap;
      va_start(ap, msg);
      vfprintf(zonelog, msg, ap);
      va_end(ap);

      // flush after every message
      fflush(zonelog);
   }
#endif
}

static void Z_LogPuts(const char *msg)
{
#ifdef ZONEFILE
   if(!zonelog)
      Z_OpenLogFile();
   if(zonelog)
      fputs(msg, zonelog);
#endif
}

//=============================================================================
//
// Initialization and Shutdown
//

static void Z_Close(void)
{
   Z_CloseLogFile();

#ifdef DUMPONEXIT
   Z_PrintZoneHeap();
#endif
}

void Z_Init(void)
{   
   atexit(Z_Close);            // exit handler

   Z_LogPrintf("Initialized zone heap (using native implementation)\n");
   
   printf("Z_Init: Init zone memory allocation daemon.\n");
}

//=============================================================================
//
// Core Memory Management Routines
//

//
// Z_Malloc
//
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
void *(Z_Malloc)(size_t size, int tag, void **user, const char *file, int line)
{
   register memblock_t *block;
   byte *ret;

   DEBUG_CHECKHEAP();

   Z_IDCheckNB(IDBOOL(tag >= PU_PURGELEVEL && !user),
               "Z_Malloc: an owner is required for purgable blocks", 
               file, line);

   if(!size)
      return user ? *user = NULL : NULL;          // malloc(0) returns NULL
   
   if(!(block = (memblock_t *)(malloc(size + header_size))))
   {
      if(blockbytag[PU_CACHE])
      {
         Z_FreeTags(PU_CACHE, PU_CACHE);
         block = (memblock_t *)(malloc(size + header_size));
      }
   }

   if(!block)
   {
      I_FatalError(I_ERR_KILL, "Z_Malloc: Failure trying to allocate %u bytes\n"
                               "Source: %s:%d\n", (unsigned int)size, file, line);
   }
   
   block->size = size;
   
   if((block->next = blockbytag[tag]))
      block->next->prev = &block->next;
   blockbytag[tag] = block;
   block->prev = &blockbytag[tag];
           
   INSTRUMENT(memorybytag[tag] += block->size);
   INSTRUMENT(block->file = file);
   INSTRUMENT(block->line = line);
         
   IDCHECK(block->id = ZONEID); // signature required in block header
   
   block->tag  = tag;           // tag
   block->user = user;          // user
   
   ret = ((byte *) block + header_size);
   if(user)                     // if there is a user
      *user = ret;              // set user to point to new block
   
   // scramble memory -- weed out any bugs
   SCRAMBLER(ret, size);

   Z_LogPrintf("* %p = Z_Malloc(size=%lu, tag=%d, user=%p, source=%s:%d)\n", 
               ret, size, tag, user, file, line);

   return ret;
}

//
// Z_Free
//
void (Z_Free)(void *p, const char *file, int line)
{
   DEBUG_CHECKHEAP();

   if(p)
   {
      memblock_t *block = (memblock_t *)((byte *) p - header_size);

      Z_IDCheck(IDBOOL(block->id != ZONEID),
                "Z_Free: freed a pointer without ZONEID", block, file, line);

      // haleyjd: permanent blocks are never freed even if the code tries.
      if(block->tag == PU_PERMANENT)
         return;
      
      IDCHECK(block->id = 0); // Nullify id so another free fails

      // haleyjd 01/20/09: check invalid tags
      // catches double frees and possible selective heap corruption
      if(block->tag == PU_FREE || block->tag >= PU_MAX)
      {
         I_FatalError(I_ERR_KILL,
                      "Z_Free: freed a pointer with invalid tag %d\n"
                      "Source: %s:%d\n"
#if defined(ZONEVERBOSE) && defined(INSTRUMENTED)
                      "Source of malloc: %s:%d\n"
                      , block->tag, file, line, block->file, block->line
#else
                      , block->tag, file, line
#endif
                     );
      }
      INSTRUMENT(memorybytag[block->tag] -= block->size);
      block->tag = PU_FREE;       // Mark block freed

      // scramble memory -- weed out any bugs
      SCRAMBLER(p, block->size);

      if(block->user)            // Nullify user if one exists
         *block->user = NULL;

      if((*block->prev = block->next))
         block->next->prev = block->prev;
         
      free(block);
         
      Z_LogPrintf("* Z_Free(p=%p, file=%s:%d)\n", p, file, line);
   }
}

//
// Z_FreeTags
//
void (Z_FreeTags)(int lowtag, int hightag, const char *file, int line)
{
   memblock_t *block;

   // haleyjd 03/30/2011: delete ZoneObjects of the same tags as well
   ZoneObject::FreeTags(lowtag, hightag);
   
   if(lowtag <= PU_FREE)
      lowtag = PU_FREE+1;

   if(hightag > PU_CACHE)
      hightag = PU_CACHE;
   
   for(; lowtag <= hightag; ++lowtag)
   {
      for(block = blockbytag[lowtag], blockbytag[lowtag] = NULL; block;)
      {
         memblock_t *next = block->next;

         Z_IDCheck(IDBOOL(block->id != ZONEID),
                   "Z_FreeTags: Changed a tag without ZONEID", 
                   block, file, line);

         (Z_Free)((byte *)block + header_size, file, line);
         block = next;               // Advance to next block
      }
   }

   Z_LogPrintf("* Z_FreeTags(lowtag=%d, hightag=%d, file=%s:%d)\n",
               lowtag, hightag, file, line);
}

//
// Z_ChangeTag
//
void (Z_ChangeTag)(void *ptr, int tag, const char *file, int line)
{
   memblock_t *block;
   
   DEBUG_CHECKHEAP();
   
   if(!ptr)
   {
      I_FatalError(I_ERR_KILL, 
                   "Z_ChangeTag: can't change a NULL pointer at %s:%d\n",
                   file, line);
   }
   
   block = (memblock_t *)((byte *) ptr - header_size);

   Z_IDCheck(IDBOOL(block->id != ZONEID),
             "Z_ChangeTag: Changed a tag without ZONEID", block, file, line);

   Z_IDCheck(IDBOOL(tag >= PU_PURGELEVEL && !block->user),
             "Z_ChangeTag: an owner is required for purgable blocks",
             block, file, line);

   // haleyjd: permanent blocks are not re-tagged even if the code tries.
   if(block->tag == PU_PERMANENT)
      return;

   if((*block->prev = block->next))
      block->next->prev = block->prev;
   if((block->next = blockbytag[tag]))
      block->next->prev = &block->next;
   block->prev = &blockbytag[tag];
   blockbytag[tag] = block;

   INSTRUMENT(memorybytag[block->tag] -= block->size);
   INSTRUMENT(memorybytag[tag] += block->size);

   block->tag = tag;

   Z_LogPrintf("* Z_ChangeTag(p=%p, tag=%d, file=%s:%d)\n",
               ptr, tag, file, line);
}

//
// Z_Realloc
//
// For the native heap, this can easily behave as a real realloc, and not
// just an ignorant copy-and-free.
//
void *(Z_Realloc)(void *ptr, size_t n, int tag, void **user,
                  const char *file, int line)
{
   void *p;
   memblock_t *block, *newblock, *origblock;

   // if not allocated at all, defer to Z_Malloc
   if(!ptr)
      return (Z_Malloc)(n, tag, user, file, line);

   // size == 0 is a special case that cannot be handled below
   if(n == 0)
   {
      (Z_Free)(ptr, file, line);
      return NULL;
   }

   DEBUG_CHECKHEAP();

   block = origblock = (memblock_t *)((byte *)ptr - header_size);

   Z_IDCheck(IDBOOL(block->id != ZONEID),
             "Z_Realloc: Reallocated a block without ZONEID\n", 
             block, file, line);

   // haleyjd: realloc cannot change the tag of a permanent block
   if(block->tag == PU_PERMANENT)
      tag = PU_PERMANENT;

   // nullify current user, if any
   if(block->user)
      *(block->user) = NULL;

   // detach from list before reallocation
   if((*block->prev = block->next))
      block->next->prev = block->prev;

   block->next = NULL;
   block->prev = NULL;

   INSTRUMENT(memorybytag[block->tag] -= block->size);

   if(!(newblock = (memblock_t *)(realloc(block, n + header_size))))
   {
      // haleyjd 07/09/10: Note that unlinking the block above makes this safe 
      // even if the current block is PU_CACHE; Z_FreeTags won't find it.
      if(blockbytag[PU_CACHE])
      {
         Z_FreeTags(PU_CACHE, PU_CACHE);
         newblock = (memblock_t *)(realloc(block, n + header_size));
      }
   }

   if(!(block = newblock))
   {
      if(origblock->size >= n)
      {
         block = origblock; // restore original block if size was equal or smaller
         n = block->size;   // keep same size in this event
      }
      else
      {
         I_FatalError(I_ERR_KILL, "Z_Realloc: Failure trying to allocate %u bytes\n"
                                  "Source: %s:%d\n", (unsigned int)n, file, line);
      }
   }

   block->size = n;
   block->tag  = tag;

   p = (byte *)block + header_size;

   // set new user, if any
   block->user = user;
   if(user)
      *user = p;

   // reattach to list at possibly new address, new tag
   if((block->next = blockbytag[tag]))
      block->next->prev = &block->next;
   blockbytag[tag] = block;
   block->prev = &blockbytag[tag];

   INSTRUMENT(memorybytag[tag] += block->size);
   INSTRUMENT(block->file = file);
   INSTRUMENT(block->line = line);

   Z_LogPrintf("* %p = Z_Realloc(ptr=%p, n=%lu, tag=%d, user=%p, source=%s:%d)\n", 
               p, ptr, n, tag, user, file, line);

   return p;
}

//
// Z_Calloc
//
void *(Z_Calloc)(size_t n1, size_t n2, int tag, void **user,
                 const char *file, int line)
{
   return (n1*=n2) ? memset((Z_Malloc)(n1, tag, user, file, line), 0, n1) : NULL;
}

//
// Z_Strdup
//
char *(Z_Strdup)(const char *s, int tag, void **user,
                 const char *file, int line)
{
   return strcpy((char *)((Z_Malloc)(strlen(s)+1, tag, user, file, line)), s);
}

//=============================================================================
//
// Heap Verification
//

//
// Z_CheckHeap
//
void (Z_CheckHeap)(const char *file, int line)
{
#ifdef ZONEIDCHECK
   memblock_t *block;
   int lowtag;

   for(lowtag = PU_FREE+1; lowtag < PU_MAX; ++lowtag)
   {
      for(block = blockbytag[lowtag]; block; block = block->next)
      {
         Z_IDCheck(IDBOOL(block->id != ZONEID),
                   "Z_CheckHeap: Block found without ZONEID", 
                   block, file, line);
      }
   }
#endif

#ifndef CHECKHEAP
   Z_LogPrintf("* Z_CheckHeap(file=%s:%d)\n", file, line);
#endif
}

//
// Z_CheckTag
//
// haleyjd: a function to return the allocation tag of a block.
// This is needed by W_CacheLumpNum so that it does not
// inadvertently lower the cache level of lump allocations and
// cause code which expects them to be static to lose them
//
int (Z_CheckTag)(void *ptr, const char *file, int line)
{
   memblock_t *block = (memblock_t *)((byte *) ptr - header_size);

   DEBUG_CHECKHEAP();

   Z_IDCheck(IDBOOL(block->id != ZONEID),
             "Z_CheckTag: block doesn't have ZONEID", block, file, line);
   
   return block->tag;
}

//
// Z_PrintZoneHeap
//
void Z_PrintZoneHeap(void)
{
   memblock_t *block;
   int lowtag;
   FILE *outfile;

   const char *fmtstr =
#if defined(ZONEIDCHECK) && defined(INSTRUMENTED)
      "%p: { %8X : %p : %p : %8u : %p : %d : %s : %d }\n"
#elif defined(INSTRUMENTED)
      "%p: { %p : %p : %8u : %p : %d : %s : %d }\n"
#elif defined(ZONEIDCHECK)
      "%p: { %8X : %p : %p : %8u : %p : %d }\n"
#else
      "%p: { %p : %p : %8u : %p : %d }\n"
#endif
      ;

   outfile = fopen("heap.txt", "w");
   if(!outfile)
      return;

   for(lowtag = PU_FREE; lowtag < PU_MAX; ++lowtag)
   {
      for(block = blockbytag[lowtag]; block; block = block->next)
      {
         fprintf(outfile, fmtstr, block,
#if defined(ZONEIDCHECK)
                 block->id, 
#endif
                 block->next, block->prev, block->size,
                 block->user, block->tag
#if defined(INSTRUMENTED)
#if defined(ZONEVERBOSE)
                 , block->file, block->line
#else
                 , "not printed", 0
#endif
#endif
                 );
         // warnings
#if defined(ZONEIDCHECK)
         if(block->tag != PU_FREE && block->id != ZONEID)
            fputs("\tWARNING: block does not have ZONEID\n", outfile);
#endif
         if(!block->user && block->tag >= PU_PURGELEVEL)
            fputs("\tWARNING: purgable block with no user\n", outfile);
         if(block->tag >= PU_MAX)
            fputs("\tWARNING: invalid cache level\n", outfile);
         
         fflush(outfile);
      }
   }

   fclose(outfile);
}

//
// Z_DumpCore
//
// haleyjd 03/18/07: Write the zone heap to file
//
void Z_DumpCore()
{
   static const char *namefortag[PU_MAX] =
   {
      "PU_FREE", 
      "PU_STATIC",
      "PU_PERMANENT",
      "PU_SOUND",
      "PU_MUSIC",
      "PU_RENDERER",
      "PU_VALLOC",
      "PU_AUTO",
      "PU_LEVEL",
      "PU_CACHE",
   };

   int tag;
   memblock_t *block;
   uint32_t dirofs = 12;
   uint32_t dirlen;
   uint32_t numentries = 0;

   for(tag = PU_FREE+1; tag < PU_MAX; tag++)
   {
      for(block = blockbytag[tag]; block; block = block->next)
         ++numentries;
   }

   dirlen = numentries * 64; // crazy PAK format...

   FILE *f = fopen("coredump.pak", "wb");
   if(!f)
      return;
   fwrite("PACK",  4,              1, f);
   fwrite(&dirofs, sizeof(dirofs), 1, f);
   fwrite(&dirlen, sizeof(dirlen), 1, f);

   uint32_t offs = 12 + 64 * numentries;
   for(tag = PU_FREE+1; tag < PU_MAX; tag++)
   {
      for(block = blockbytag[tag]; block; block = block->next)
      {
         char     name[56];
         uint32_t filepos = offs;
         uint32_t filelen = (uint32_t)(block->size);

         memset(name, 0, sizeof(name));
         sprintf(name, "/%s/%p", 
                 block->tag < PU_MAX ? namefortag[block->tag] : "UNKNOWN",
                 block);
         fwrite(name,     sizeof(name),    1, f);
         fwrite(&filepos, sizeof(filepos), 1, f);
         fwrite(&filelen, sizeof(filelen), 1, f);

         offs += filelen;
      }
   }

   for(tag = PU_FREE+1; tag < PU_MAX; tag++)
   {
      for(block = blockbytag[tag]; block; block = block->next)
         fwrite(((byte *)block + header_size), block->size, 1, f);
   }

   fclose(f);
}

//=============================================================================
//
// System Allocator Functions
//
// Guaranteed access to system malloc and free, regardless of the heap in use.
//

//
// Z_SysMalloc
//
// Similar to I_AllocLow in the original source, this function gives explicit
// access to the C heap. There are allocations which are a detriment to the zone
// system, such as large video buffers, which should be handled through this
// function instead.
//
// Care must be taken, of course, to not mix zone and C heap allocations.
//
void *Z_SysMalloc(size_t size)
{
   void *ret;
   
   if(!(ret = malloc(size)))
   {
      I_FatalError(I_ERR_KILL,
                   "Z_SysMalloc: failed on allocation of %u bytes\n", 
                   (unsigned int)size);
   }

   return ret;
}

//
// Z_SysCalloc
//
// Convenience routine to match above.
//
void *Z_SysCalloc(size_t n1, size_t n2)
{
   void *ret;

   if(!(ret = calloc(n1, n2)))
   {
      I_FatalError(I_ERR_KILL,
                   "Z_SysCalloc: failed on allocation of %u bytes\n", 
                   (unsigned int)(n1*n2));
   }

   return ret;
}

//
// Z_SysRealloc
//
// Turns out I need this in the sound code to avoid possible multithreaded
// race conditions.
//
void *Z_SysRealloc(void *ptr, size_t size)
{
   void *ret;

   if(!(ret = realloc(ptr, size)))
   {
      I_FatalError(I_ERR_KILL,
                   "Z_SysRealloc: failed on allocation of %u bytes\n", 
                   (unsigned int)size);
   }

   return ret;
}

//
// Z_SysFree
//
// For use with Z_SysMalloc.
//
void Z_SysFree(void *p)
{
   if(p)
      free(p);
}

//=============================================================================
//
// Zone Alloca
//
// haleyjd 12/06/06
//

//
// Z_FreeAlloca
//
// haleyjd 12/06/06: Frees all blocks allocated with Z_Alloca.
//
void Z_FreeAlloca(void)
{
   memblock_t *block = blockbytag[PU_AUTO];

   if(!block)
      return;
   
   Z_LogPuts("* Freeing alloca blocks\n");

   blockbytag[PU_AUTO] = NULL;

   while(block)
   {
      memblock_t *next = block->next;

      Z_IDCheckNB(IDBOOL(block->id != ZONEID),
                  "Z_FreeAlloca: Freed a tag without ZONEID", 
                  __FILE__, __LINE__);

      Z_Free((byte *)block + header_size);
      block = next;               // Advance to next block
   }
}

//
// Z_Alloca
//
// haleyjd 12/06/06:
// Implements a portable garbage-collected alloca on the zone heap.
//
void *(Z_Alloca)(size_t n, const char *file, int line)
{
   void *ptr;

   if(n == 0)
      return NULL;

   // allocate it
   ptr = (Z_Calloc)(n, 1, PU_AUTO, NULL, file, line);

   Z_LogPrintf("* %p = Z_Alloca(n = %lu, file = %s, line = %d)\n", 
               ptr, n, file, line);

   return ptr;
}

//
// Z_Realloca
//
// haleyjd 07/08/10: realloc for automatic allocations.
//
void *(Z_Realloca)(void *ptr, size_t n, const char *file, int line)
{
   void *ret;

   if(ptr)
   {
      // get zone block
      memblock_t *block = (memblock_t *)((byte *)ptr - header_size);

      Z_IDCheck(IDBOOL(block->id != ZONEID),
         "Z_Realloca: block found without ZONEID", block, file, line);

      if(block->tag != PU_AUTO)
         I_FatalError(I_ERR_KILL, "Z_Realloca: strange block tag %d\n", block->tag);
   }
   
   ret = (Z_Realloc)(ptr, n, PU_AUTO, NULL, file, line);

   Z_LogPrintf("* %p = Z_Realloca(ptr = %p, n = %lu, file = %s, line = %d)\n", 
               ret, ptr, n, file, line);

   return ret;
}

//
// Z_Strdupa
//
// haleyjd 05/07/08: strdup that uses alloca, for convenience.
//
char *(Z_Strdupa)(const char *s, const char *file, int line)
{      
   return strcpy((char *)((Z_Alloca)(strlen(s)+1, file, line)), s);
}

//=============================================================================
//
// ZoneObject class methods
//

//
// ZoneObject::operator new
//
// Heap allocation new for ZoneObject, which calls Z_Calloc.
//
void *ZoneObject::operator new (size_t size)
{
   return (newalloc = Z_Calloc(1, size, PU_STATIC, NULL));
}

//
// ZoneObject::operator new
//
// Overload supporting full zone allocation semantics.
//
void *ZoneObject::operator new(size_t size, int tag, void **user)
{
   return (newalloc = Z_Calloc(1, size, tag, user));
}

//
// ZoneObject Constructor
//
// If the ZoneObject::newalloc static is set, it will be picked up by the 
// subsequent constructor call and stored in the object that was allocated.
//
ZoneObject::ZoneObject() 
   : zonealloc(NULL), zonenext(NULL), zoneprev(NULL)
{
   if(newalloc)
   {
      zonealloc = newalloc;
      newalloc  = NULL;
      addToTagList(getZoneTag());
   }
}

//
// ZoneObject::removeFromTagList
//
// Private method. Removes the object from any tag list it may be in.
//
void ZoneObject::removeFromTagList()
{
   if(zoneprev && (*zoneprev = zonenext))
      zonenext->zoneprev = zoneprev;

   zonenext = NULL;
   zoneprev = NULL;
}

//
// ZoneObject::addToTagList
//
// Private method. Adds the object into the indicated tag list.
//
void ZoneObject::addToTagList(int tag)
{
   if((zonenext = objectbytag[tag]))
      zonenext->zoneprev = &zonenext;
   objectbytag[tag] = this;
   zoneprev = &objectbytag[tag];
}

//
// ZoneObject::changeTag
//
// If the object was allocated on the zone heap, the allocation tag will be
// changed.
//
void ZoneObject::changeTag(int tag)
{
   if(zonealloc) // If not a zone object, this is a no-op
   {
      int curtag = getZoneTag();

      // not actually changing?
      if(tag == curtag)
         return;

      // remove from current tag list, if in one
      removeFromTagList();

      // put it in the new list
      addToTagList(tag);

      // change the actual allocation tag
      Z_ChangeTag(zonealloc, tag);
   }
}

//
// ZoneObject Destructor
//
// If the object was allocated on the zone heap, it will be removed from its
// block list.
//
ZoneObject::~ZoneObject()
{
   if(zonealloc)
   {
      removeFromTagList();
      zonealloc = NULL;
   }
}

//
// ZoneObject::operator delete
//
// Calls Z_Free
//
void ZoneObject::operator delete (void *p)
{
   Z_Free(p);
}

//
// ZoneObject::operator delete
//
// This overload only exists because of a rule in C++ regarding 
// exceptions during initialization.
//
void ZoneObject::operator delete (void *p, int, void **)
{
   Z_Free(p);
}

//
// ZoneObject::FreeTags
//
// Called from Z_FreeTags, this does the same thing it does except this runs
// down the objectbytag chains instead of the blockbytag chains and invokes
// operator delete on each object. By virtue of the virtual base class
// destructor, this will fully delete descendent objects.
//
// Caveat, however! Objects that use volatile tags must NOT contain other
// objects under a tag in the same equivalence class, unless you do not
// try to explicitly free that object in the containing object's destructor!
// You can do either explicit or implicit deallocation, but you must not mix
// them. Otherwise, problems will arise with arbitrary order of destruction.
//
void ZoneObject::FreeTags(int lowtag, int hightag)
{
   ZoneObject *obj;

   if(lowtag <= PU_FREE)
      lowtag = PU_FREE+1;

   if(hightag > PU_CACHE)
      hightag = PU_CACHE;
   
   for(; lowtag <= hightag; ++lowtag)
   {
      for(obj = objectbytag[lowtag], objectbytag[lowtag] = NULL; obj;)
      {
         ZoneObject *next = obj->zonenext;
         delete obj;
         obj = next;               // Advance to next object
      }
   }
}

//
// ZoneObject::getZoneTag
//
// Get the object's allocation tag.
// Returns PU_FREE if the object is not heap-allocated.
//
int ZoneObject::getZoneTag() const
{
   int tag = PU_FREE;

   if(zonealloc)
   {
      memblock_t *block = (memblock_t *)((byte *)zonealloc - header_size);
      tag = block->tag;
   }

   return tag;
}

//
// ZoneObject::getZoneSize
//
// If the ZoneObject is actually allocated on the zone heap, this will return
// the size of its block on the heap. This is interesting because it provides
// a polymorphic sizeof that returns the actual allocated size of an object
// and not a size only relative to the immediate type of the object through a
// given pointer. Borland VCL has a similar method in its TObject::InstanceSize,
// but it's implemented through black magic inline-asm hackery in Delphi. I
// think I've won that one easily as far as elegance goes :P
//
// Returns 0 if the object is not a zone allocation. You'll need to use some
// other method of getting an object's size in that case.
//
size_t ZoneObject::getZoneSize() const
{
   size_t retsize = 0;

   if(zonealloc)
   {
      memblock_t *block = (memblock_t *)((byte *)zonealloc - header_size);
      retsize = block->size;
   }

   return retsize;
}

//-----------------------------------------------------------------------------
//
// $Log: z_zone.c,v $
// Revision 1.13  1998/05/12  06:11:55  killough
// Improve memory-related error messages
//
// Revision 1.12  1998/05/03  22:37:45  killough
// beautification
//
// Revision 1.11  1998/04/27  01:49:39  killough
// Add history of malloc/free and scrambler (INSTRUMENTED only)
//
// Revision 1.10  1998/03/28  18:10:33  killough
// Add memory scrambler for debugging
//
// Revision 1.9  1998/03/23  03:43:56  killough
// Make Z_CheckHeap() more diagnostic
//
// Revision 1.8  1998/03/02  11:40:02  killough
// Put #ifdef CHECKHEAP around slow heap checks (debug)
//
// Revision 1.7  1998/02/02  13:27:45  killough
// Additional debug info turned on with #defines
//
// Revision 1.6  1998/01/26  19:25:15  phares
// First rev with no ^Ms
//
// Revision 1.5  1998/01/26  07:15:43  phares
// Added rcsid
//
// Revision 1.4  1998/01/26  06:12:30  killough
// Fix memory usage problems and improve debug stat display
//
// Revision 1.3  1998/01/22  05:57:20  killough
// Allow use of virtual memory when physical memory runs out
//
// ???
//
//-----------------------------------------------------------------------------
