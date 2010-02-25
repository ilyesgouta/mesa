#ifndef U_SEMANTICS_H_
#define U_SEMANTICS_H_

#include "pipe/p_compiler.h"
#include "pipe/p_shader_tokens.h"

/* same as SM3 values */
#define TGSI_SEMANTIC_BYTE_POSITION 0
#define TGSI_SEMANTIC_BYTE_PSIZE (4 << 4)
#define TGSI_SEMANTIC_BYTE_COLOR0 (10 << 4)
#define TGSI_SEMANTIC_BYTE_COLOR1 (TGSI_SEMANTIC_BYTE_COLOR0 + 1)
#define TGSI_SEMANTIC_BYTE_FOG (11 << 4)
#define TGSI_SEMANTIC_BYTE_BCOLOR0 (14 << 4)
#define TGSI_SEMANTIC_BYTE_BCOLOR1 (TGSI_SEMANTIC_BYTE_BCOLOR0 + 1)
#define TGSI_SEMANTIC_BYTE_TGSI (15 << 4)

static INLINE unsigned char
pipe_semantic_to_byte(unsigned name, unsigned index)
{
   switch (name)
   {
   case TGSI_SEMANTIC_POSITION:
      return TGSI_SEMANTIC_BYTE_POSITION;
   case TGSI_SEMANTIC_PSIZE:
      return TGSI_SEMANTIC_BYTE_PSIZE;
   case TGSI_SEMANTIC_FOG:
      return TGSI_SEMANTIC_BYTE_FOG;
   case TGSI_SEMANTIC_COLOR:
      return TGSI_SEMANTIC_BYTE_COLOR0 + index;
   case TGSI_SEMANTIC_GENERIC:
      ++index;
      if(index >= TGSI_SEMANTIC_BYTE_PSIZE)
      {
	 ++index;
	 if(index >= TGSI_SEMANTIC_BYTE_COLOR0)
	 {
	    index += 2;
	    if(index >= TGSI_SEMANTIC_BYTE_FOG)
	       ++index;
	 }
      }
      return index;
   case TGSI_SEMANTIC_BCOLOR:
      return TGSI_SEMANTIC_BYTE_BCOLOR0 + index;
   default:
      return TGSI_SEMANTIC_BYTE_TGSI + name;
   }
}

/* this fits BCOLOR in the SM3 range, but is not reversible */
static INLINE unsigned char
pipe_semantic_to_byte_sm3(unsigned name, unsigned index)
{
   if(name == TGSI_SEMANTIC_BCOLOR)
      return TGSI_SEMANTIC_BYTE_BCOLOR0 - 1 - index;
   return pipe_semantic_to_byte(name, index);
}

static INLINE unsigned
pipe_semantic_name_from_byte(unsigned char value)
{
   switch (value)
   {
   case TGSI_SEMANTIC_BYTE_POSITION:
      return TGSI_SEMANTIC_POSITION;
   case TGSI_SEMANTIC_BYTE_PSIZE:
      return TGSI_SEMANTIC_PSIZE;
   case TGSI_SEMANTIC_BYTE_FOG:
      return TGSI_SEMANTIC_FOG;
   case TGSI_SEMANTIC_BYTE_COLOR0:
   case TGSI_SEMANTIC_BYTE_COLOR1:
      return TGSI_SEMANTIC_COLOR;
   case TGSI_SEMANTIC_BYTE_BCOLOR0:
   case TGSI_SEMANTIC_BYTE_BCOLOR1:
      return TGSI_SEMANTIC_BCOLOR;
   default:
      if(value < TGSI_SEMANTIC_BYTE_TGSI)
	 return TGSI_SEMANTIC_GENERIC;
      else
	 return value - TGSI_SEMANTIC_BYTE_TGSI;
   }
}

static INLINE unsigned
pipe_semantic_index_from_byte(unsigned char value)
{
   if(value == TGSI_SEMANTIC_BYTE_POSITION)
      return 0;

   if(value <= TGSI_SEMANTIC_BYTE_PSIZE)
   {
      if(value < TGSI_SEMANTIC_BYTE_PSIZE)
	 return value - 1;
      else
	 return 0;
   }

   if(value < (TGSI_SEMANTIC_BYTE_COLOR0 + 2))
   {
      if(value < TGSI_SEMANTIC_BYTE_COLOR0)
	 return value - 2;
      else
	 return value - TGSI_SEMANTIC_BYTE_COLOR0;
   }

   if(value <= TGSI_SEMANTIC_BYTE_FOG)
   {
      if(value < TGSI_SEMANTIC_BYTE_FOG)
	 return value - 4;
      else
	 return 0;
   }

   if(value < TGSI_SEMANTIC_BYTE_BCOLOR0)
      return value - 5;

   if(value == (TGSI_SEMANTIC_BYTE_BCOLOR1))
      return 1;

   return 0;
}

#endif /* U_SEMANTICS_H_ */
