/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * FATX helper provenance:
 * Derived from libfatx in Matt Borgerson's FATX project:
 * https://github.com/mborgerson/fatx
 * Copyright (C) 2015 Matt Borgerson
 *
 * libfatx is GPL-2.0-or-later. The repository's COPYING file contains the
 * GPLv2 text. The upstream FATX project also credits FATX research by
 * Andrew de Quincey, Lucien Murray-Pitts, and Michael Steil.
 */
#include "fatx.h"

#include "qemu/bswap.h"

#define FATX_SIGNATURE 0x58544146

// Structure/layout derived from libfatx (see provenance header above).
#pragma pack(1)
struct fatx_superblock {
    uint32_t signature;
    uint32_t volume_id;
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint16_t unknown1;
    uint8_t padding[4078];
};
#pragma pack()

bool create_fatx_image(const char *filename, unsigned int size)
{
    unsigned int empty_fat = cpu_to_le32(0xfffffff8);
    unsigned char zero = 0;

    FILE *fp = qemu_fopen(filename, "wb");
    if (fp != NULL) {
        struct fatx_superblock superblock;
        memset(&superblock, 0xff, sizeof(struct fatx_superblock));

        superblock.signature = cpu_to_le32(FATX_SIGNATURE);
        superblock.sectors_per_cluster = cpu_to_le32(4);
        superblock.volume_id = (uint32_t)rand();
        superblock.root_cluster = cpu_to_le32(1);
        superblock.unknown1 = 0;

        // Write the fatx superblock.
        fwrite(&superblock, sizeof(superblock), 1, fp);

        // Write the FAT
        fwrite(&empty_fat, sizeof(empty_fat), 1, fp);

        fseek(fp, size - sizeof(unsigned char), SEEK_SET);
        fwrite(&zero, sizeof(unsigned char), 1, fp);

        fflush(fp);
        fclose(fp);

        return true;
    }

    return false;
}
