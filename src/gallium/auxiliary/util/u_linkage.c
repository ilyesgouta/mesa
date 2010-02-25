#include "util/u_debug.h"
#include "pipe/p_shader_tokens.h"
#include "tgsi/tgsi_parse.h"
#include "tgsi/tgsi_scan.h"
#include "util/u_linkage.h"

/* we must only record the registers that are actually used, not just declared */
static INLINE boolean
util_semantic_set_test_and_set(struct util_semantic_set *set, unsigned value)
{
   unsigned mask = 1 << (value % (sizeof(long) * 8));
   unsigned long *p = &set->masks[value / (sizeof(long) * 8)];
   unsigned long v = *p & mask;
   *p |= mask;
   return !!v;
}

unsigned
util_semantic_set_from_program_file(struct util_semantic_set *set, const struct tgsi_token *tokens, enum tgsi_file_type file)
{
   struct tgsi_shader_info info;
   struct tgsi_parse_context parse;
   unsigned count = 0;
   ubyte *semantic_name;
   ubyte *semantic_index;

   tgsi_scan_shader(tokens, &info);

   if(file == TGSI_FILE_INPUT)
   {
      semantic_name = info.input_semantic_name;
      semantic_index = info.input_semantic_index;
   }
   else if(file == TGSI_FILE_OUTPUT)
   {
      semantic_name = info.output_semantic_name;
      semantic_index = info.output_semantic_index;
   }
   else
      assert(0);

   tgsi_parse_init(&parse, tokens);

   memset(set->masks, 0, sizeof(set->masks));
   while(!tgsi_parse_end_of_tokens(&parse))
   {
      tgsi_parse_token(&parse);

      if(parse.FullToken.Token.Type == TGSI_TOKEN_TYPE_INSTRUCTION)
      {
	 const struct tgsi_full_instruction *finst = &parse.FullToken.FullInstruction;
	 unsigned i;
	 for(i = 0; i < finst->Instruction.NumDstRegs; ++i)
	 {
	    if(finst->Dst[i].Register.File == file)
	    {
	       unsigned idx = finst->Dst[i].Register.Index;
	       if(semantic_name[idx] == TGSI_SEMANTIC_GENERIC)
	       {
		  if(!util_semantic_set_test_and_set(set, semantic_index[idx]))
		     ++count;
	       }
	    }
	 }

	 for(i = 0; i < finst->Instruction.NumSrcRegs; ++i)
	 {
	    if(finst->Src[i].Register.File == file)
	    {
	       unsigned idx = finst->Src[i].Register.Index;
	       if(semantic_name[idx] == TGSI_SEMANTIC_GENERIC)
	       {
		  if(!util_semantic_set_test_and_set(set, semantic_index[idx]))
		     ++count;
	       }
	    }
	 }
      }
   }
   tgsi_parse_free(&parse);

   return count;
}

#define UTIL_SEMANTIC_SET_FOR_EACH(i, set) for(i = 0; i < 256; ++i) if(set->masks[i / (sizeof(long) * 8)] & (1 << (i % (sizeof(long) * 8))))

void
util_semantic_layout_from_set(unsigned char *layout, const struct util_semantic_set *set, unsigned efficient_slots, unsigned num_slots)
{
   int first = -1;
   int last = -1;
   unsigned i;

   memset(layout, 0xff, num_slots);

   UTIL_SEMANTIC_SET_FOR_EACH(i, set)
   {
      if(first < 0)
	 first = i;
      last = i;
   }

   if(last < efficient_slots)
   {
      UTIL_SEMANTIC_SET_FOR_EACH(i, set)
         layout[i] = i;
   }
   else if((last - first) < efficient_slots)
   {
      UTIL_SEMANTIC_SET_FOR_EACH(i, set)
         layout[i - first] = i;
   }
   else
   {
      unsigned idx = 0;
      UTIL_SEMANTIC_SET_FOR_EACH(i, set)
         layout[idx++] = i;
   }
}
