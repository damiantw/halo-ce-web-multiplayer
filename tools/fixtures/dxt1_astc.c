/* tools/test_dxt1_astc.py: DXT1 blocks (8 bytes each) on stdin; for each,
on stdout, its ASTC 4x4 block (16 bytes) from port/linux/src/dxt1_astc.h,
then the game's own DXT1 decode of it (16 texels of 32-bit ARGB, little
endian), from xbox_textures.c's dxt_color_block (dxt_decode.inc, which the
test extracts) */

#include <stdio.h>
#include <string.h>

typedef int BOOL;
#define TRUE 1
#define FALSE 0

#include "dxt_decode.inc"
#include "dxt1_astc.h"

int main(void)
{
	unsigned char source[8], block[16], texels[64];
	unsigned long colors[16];
	int index;

	while (fread(source, 1, sizeof(source), stdin) == sizeof(source))
	{
		dxt1_astc_block(source, block);
		dxt_color_block(source, TRUE, colors);
		for (index = 0; index < 16; index++)
		{
			texels[index * 4] = (unsigned char)colors[index];
			texels[index * 4 + 1] = (unsigned char)(colors[index] >> 8);
			texels[index * 4 + 2] = (unsigned char)(colors[index] >> 16);
			texels[index * 4 + 3] = (unsigned char)(colors[index] >> 24);
		}
		if (fwrite(block, 1, sizeof(block), stdout) != sizeof(block) ||
			fwrite(texels, 1, sizeof(texels), stdout) != sizeof(texels))
			return 1;
	}
	return 0;
}
