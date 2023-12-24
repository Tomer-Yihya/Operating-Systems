#include "os.h"

uint64_t get_vpn_part(uint64_t vpn, int part_index)
{
	int bit_offset = 9 * (4 - part_index); // offset of lsb of current part. part 0 goes back the most, part 4 is in the front.
	uint64_t bitmask = 0x1FFULL << bit_offset; // mask to get 9 bits for current part
	return (vpn & bitmask) >> bit_offset;
}

void page_table_update(uint64_t pt, uint64_t vpn, uint64_t ppn)
{
	uint64_t cur_level_phys_addr = pt << 12;

	for (int i = 0; i < 4; ++i)
	{
		uint64_t cur_vpn_part = get_vpn_part(vpn, i);
		uint64_t* cur_level_ptr = (uint64_t*) phys_to_virt(cur_level_phys_addr);
		uint64_t cur_pte = cur_level_ptr[cur_vpn_part];
		uint64_t valid = cur_pte & 1;

		uint64_t next_addr;
		if (valid)
		{
			next_addr = cur_pte & ~1;
		}
		else
		{
			if (ppn == NO_MAPPING) // we were asked to invalidate an entry which is already invalid so nothing needs to be done
			{
				return;
			}

			else // if we are adding a new mapping and reached invalid before the end we need to add levels
			{
				next_addr = alloc_page_frame() << 12;
				cur_level_ptr[cur_vpn_part] = next_addr | 1;
			}
		}

		cur_level_phys_addr = next_addr;
	}

	uint64_t* last_level_ptr = (uint64_t*) phys_to_virt(cur_level_phys_addr);
	uint64_t last_vpn_part = get_vpn_part(vpn, 4);
	if (ppn == NO_MAPPING)
	{
		last_level_ptr[last_vpn_part] = 0; // invalidate entry
	}
	else
	{
		last_level_ptr[last_vpn_part] = (ppn << 12) | 1; // set entry to ppn with the valid bit on
	}
}

uint64_t page_table_query(uint64_t pt, uint64_t vpn)
{
	uint64_t cur_level_phys_addr = pt << 12;

	for (int i = 0; i < 5; ++i)
	{
		uint64_t cur_vpn_part = get_vpn_part(vpn, i);

		uint64_t* cur_level_ptr = (uint64_t*) phys_to_virt(cur_level_phys_addr);
		uint64_t cur_pte = cur_level_ptr[cur_vpn_part];
		uint64_t valid = cur_pte & 1;

		if (!valid)
		{
			return NO_MAPPING;
		}

		uint64_t found_addr = cur_pte & ~1; // turn off valid bit

		if (i == 4) // last level - address represents query result
		{
			return found_addr >> 12; // get physical page number
		}
		else // on levels 1-4 the found address represents the address of the next level
		{
			cur_level_phys_addr = found_addr;
		}
	}

	return NO_MAPPING; // should never be reached
}
