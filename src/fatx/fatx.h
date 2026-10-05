// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include "diskio.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#define FATX_MAGIC 0x58544146
#define FATX_FAT_OFFSET 0x1000
#define FATX_DIRENT_SIZE 64
#define FATX_NAME_MAX 42
#define FATX_FAT_END16 0xFFFF
#define FATX_FAT_END32 0xFFFFFFFF

#define FATX_ATTR_READONLY  0x01
#define FATX_ATTR_HIDDEN    0x02
#define FATX_ATTR_SYSTEM    0x04
#define FATX_ATTR_DIRECTORY 0x10
#define FATX_ATTR_ARCHIVE   0x20

struct __attribute__((packed)) FatxHeader {
	uint32_t magic;
	uint32_t volume_id;
	uint32_t sectors_per_cluster;
	uint32_t root_dir_cluster;
};

struct __attribute__((packed)) FatxDirEntry {
	uint8_t name_len;
	uint8_t attrs;
	char name[FATX_NAME_MAX];
	uint32_t first_cluster;
	uint32_t size;
	uint16_t create_date;
	uint16_t create_time;
	uint16_t write_date;
	uint16_t write_time;
	uint16_t access_date;
	uint16_t access_time;
};

static_assert(sizeof(FatxDirEntry) == FATX_DIRENT_SIZE);

struct FatxFileInfo {
	std::string name;
	uint8_t attrs;
	uint32_t first_cluster;
	uint32_t size;
	uint16_t create_date;
	uint16_t create_time;
	uint16_t write_date;
	uint16_t write_time;
	uint16_t access_date;
	uint16_t access_time;

	bool is_directory() const;
	bool is_deleted() const;
	bool is_empty() const;
};

struct FatxPartition {
	std::string name;
	PartitionIoPtr part;
};

std::vector<FatxPartition> discover_partitions(DiskIoPtr disk);

class FatxFatCache {
private:
	PartitionIoPtr m_part;
	uint32_t m_fat_entry_size;
	uint32_t m_fat_sector_base;
	uint32_t m_num_clusters;

	struct CachedSector {
		SectorBuffer data;
		bool dirty;
	};
	std::unordered_map<uint32_t, CachedSector> m_cache;

	uint8_t *load_sector(uint32_t sector);
public:
	FatxFatCache(PartitionIoPtr part, uint32_t fat_entry_size, uint32_t num_clusters);

	uint32_t get(uint32_t cluster);
	void set(uint32_t cluster, uint32_t value);
	void flush();

	uint32_t fat_end() const;
	bool is_end(uint32_t value) const;
	uint32_t num_clusters() const;
};

class FatxFilesystem {
private:
	PartitionIoPtr m_part;
	FatxHeader m_header;
	uint32_t m_cluster_size;
	uint64_t m_data_start_sector;
	FatxFatCache m_fat;

	uint64_t cluster_to_sector(uint32_t cluster);
	void read_cluster(uint32_t cluster, void *buf);
	void write_cluster(uint32_t cluster, const void *buf);

	SectorBuffer read_chain(uint32_t first_cluster);
	uint32_t write_chain(const void *data, size_t size);
	void free_chain(uint32_t first_cluster);
	std::vector<uint32_t> alloc_clusters(uint32_t count);

	SectorBuffer read_dir_raw(uint32_t first_cluster);
	void write_dir_raw(uint32_t first_cluster, const SectorBuffer &raw);
	void add_dir_entry(uint32_t dir_cluster, const FatxDirEntry &ent);
	bool remove_dir_entry(uint32_t dir_cluster, const char *name, FatxDirEntry *removed);
	bool rename_dir_entry(uint32_t dir_cluster, const char *old_name, const char *new_name);
	void delete_dir_contents(uint32_t dir_cluster);

public:
	FatxFilesystem(PartitionIoPtr part);

	uint32_t root_cluster() const;
	std::vector<FatxFileInfo> read_dir(uint32_t first_cluster);
	FatxFileInfo *find_entry(uint32_t dir_cluster, const char *name, std::vector<FatxFileInfo> &entries);

	SectorBuffer read_file(const FatxFileInfo &ent);
	void import_file(uint32_t dir_cluster, const char *name, const void *data, size_t size, bool overwrite = false);
	void mkdir(uint32_t dir_cluster, const char *name);
	void remove(uint32_t dir_cluster, const char *name, bool recursive = false);
	void rename(uint32_t dir_cluster, const char *old_name, const char *new_name);
	void flush();
};
