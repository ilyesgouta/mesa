#ifndef U_LINKAGE_H_
#define U_LINKAGE_H_

#include "pipe/p_compiler.h"

struct util_semantic_set
{
   unsigned long masks[256 / 8 / sizeof(unsigned long)];
};

static INLINE bool
util_semantic_set_contains(struct util_semantic_set *set, unsigned char value)
{
   return !!(set->masks[value / (sizeof(long) * 8)] & (1 << (value / (sizeof(long) * 8))));
}

unsigned util_semantic_set_from_program_file(struct util_semantic_set *set, const struct tgsi_token *tokens, enum tgsi_file_type file);

/* efficient_slots is the number of slots such that hardware performance is
 * the same for using that amount, with holes, or less slots but with less
 * holes.
 *
 * num_slots is the size of the layout array and hardware limit instead.
 *
 * efficient_slots == 0 or efficient_solts == num_slots are typical settings.
 */
void util_semantic_layout_from_set(unsigned char *layout, const struct util_semantic_set *set, unsigned efficient_slots, unsigned num_slots);

static INLINE void
util_semantic_table_from_layout(unsigned char *table, unsigned char *layout, unsigned char first_slot_value, unsigned char num_slots)
{
   memset(table, 0xff, sizeof(table));

   for(int i = 0; i < num_slots; ++i)
      table[layout[i]] = first_slot_value + i;
}

#endif /* U_LINKAGE_H_ */
