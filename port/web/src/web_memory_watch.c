/*
WEB_MEMORY_WATCH.C

port/linux/src/memory_watch.c's interface for the browser. WebAssembly has
no page protection, so writes cannot fault. The file layer announces the
writes it makes into guest memory (memory_watch_prepare_write, which is how
textures stream in from the cache files), and those bump the generation of
the pages they touch. Writes the game's own code makes to a cached texture
are not seen; see docs/wasm-spike.md.
*/

#include "platform.h"

#define WATCH_PAGE_SIZE 0x1000UL
#define WATCH_PAGE_COUNT (PLATFORM_CONTIGUOUS_SIZE / WATCH_PAGE_SIZE)

static unsigned long page_generation[WATCH_PAGE_COUNT];
static volatile unsigned long current_generation = 1;

static void touch(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, generation;

	if (!size || !platform_is_contiguous((void *)address))
		return;
	first = (address - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
	last = (address + size - 1 - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	generation = __sync_add_and_fetch(&current_generation, 1);
	for (page = first; page <= last; page++)
		page_generation[page] = generation;
}

void memory_watch_initialize(void)
{
}

void memory_watch_protect(unsigned long address, unsigned long size)
{
	(void)address;
	(void)size;
}

unsigned long memory_watch_generation(unsigned long address, unsigned long size)
{
	unsigned long first, last, page, newest = 0;

	if (!size || !platform_is_contiguous((void *)address))
		return 0;
	first = (address - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
	last = (address + size - 1 - PLATFORM_CONTIGUOUS_BASE) / WATCH_PAGE_SIZE;
	if (last >= WATCH_PAGE_COUNT)
		last = WATCH_PAGE_COUNT - 1;
	for (page = first; page <= last; page++)
		if (page_generation[page] > newest)
			newest = page_generation[page];
	return newest;
}

unsigned long memory_watch_serial(void)
{
	return current_generation;
}

void memory_watch_prepare_write(void *address, unsigned long size)
{
	touch((unsigned long)address, size);
}

void memory_watch_forget(void *address, unsigned long size)
{
	touch((unsigned long)address, size);
}
