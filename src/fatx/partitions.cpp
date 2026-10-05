// SPDX-License-Identifier: GPL-2.0-only

#include "fatx.h"

#include <cstring>

#ifdef __BYTE_ORDER__
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
#else
static inline uint32_t be32(uint32_t v) { return v; }
#endif
#endif

#define XDK_PART_MAGIC 0x00020000

struct __attribute__((packed)) XdkPartTable {
	uint32_t magic;
	uint32_t unknown;
	uint32_t content_start;
	uint32_t content_length;
	uint32_t dash_start;
	uint32_t dash_length;
};

static bool probe_fatx(const DiskIoPtr &disk, uint64_t sector)
{
	SectorBuffer buf(SECTOR_SIZE);
	try {
		disk->read_sectors(sector, 1, buf.data());
	} catch (...) {
		return false;
	}
	uint32_t magic;
	memcpy(&magic, buf.data(), 4);
	return be32(magic) == FATX_MAGIC;
}

static void try_add(const DiskIoPtr &disk, std::vector<FatxPartition> &out,
                     const char *name, uint64_t start_sector, uint64_t length_sectors)
{
	if (!probe_fatx(disk, start_sector))
		return;
	out.push_back({name, std::make_shared<PartitionIo>(disk, start_sector, length_sectors)});
}

std::vector<FatxPartition> discover_partitions(DiskIoPtr disk)
{
	std::vector<FatxPartition> result;
	uint64_t disk_sectors = disk->total_sectors();

	SectorBuffer buf(SECTOR_SIZE);
	disk->read_sectors(0, 1, buf.data());
	XdkPartTable xdk;
	memcpy(&xdk, buf.data(), sizeof(xdk));
	xdk.magic = be32(xdk.magic);
	xdk.content_start = be32(xdk.content_start);
	xdk.content_length = be32(xdk.content_length);
	xdk.dash_start = be32(xdk.dash_start);
	xdk.dash_length = be32(xdk.dash_length);

	if (xdk.magic == XDK_PART_MAGIC) {
		try_add(disk, result, "content", xdk.content_start, xdk.content_length);
		try_add(disk, result, "dash", xdk.dash_start, xdk.dash_length);
		return result;
	}

	struct { const char *name; uint64_t offset; uint64_t size; } retail[] = {
		{ "sysext",       0x10C080000,   0xCE30000 },
		{ "sysext2",      0x118EB0000,   0x8000000 },
		{ "xbox1-compat", 0x120EB0000,   0x10000000 },
		{ "content",      0x130EB0000,   0 },
	};

	for (auto &p : retail) {
		uint64_t start = p.offset / SECTOR_SIZE;
		if (start >= disk_sectors)
			continue;
		uint64_t length = p.size ? p.size / SECTOR_SIZE : disk_sectors - start;
		try_add(disk, result, p.name, start, length);
	}

	return result;
}
