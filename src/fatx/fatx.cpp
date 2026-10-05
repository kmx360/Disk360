// SPDX-License-Identifier: GPL-2.0-only

#include "fatx.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#ifdef __BYTE_ORDER__
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
static inline uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t be32(uint32_t v) { return __builtin_bswap32(v); }
#else
static inline uint16_t be16(uint16_t v) { return v; }
static inline uint32_t be32(uint32_t v) { return v; }
#endif
#endif

static void make_dos_timestamp(uint16_t &date, uint16_t &time_val)
{
	time_t now = ::time(nullptr);
	struct tm tm;
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	date = ((tm.tm_year - 80) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday;
	time_val = (tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2);
}

static int strcasecmp_ascii(const char *a, const char *b)
{
	while (*a && *b) {
		char ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
		char cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
		if (ca != cb)
			return ca - cb;
		++a;
		++b;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

// -- FatxFileInfo --

bool FatxFileInfo::is_directory() const { return attrs & FATX_ATTR_DIRECTORY; }
bool FatxFileInfo::is_deleted() const { return name.empty() && first_cluster == 0; }
bool FatxFileInfo::is_empty() const { return name.empty(); }

static FatxFileInfo dirent_to_info(const FatxDirEntry &de)
{
	FatxFileInfo info;
	uint8_t len = de.name_len;
	if (len == 0xE5 || len == 0xFF || len == 0x00)
		len = 0;
	if (len > FATX_NAME_MAX)
		len = FATX_NAME_MAX;
	info.name = std::string(de.name, len);
	info.attrs = de.attrs;
	info.first_cluster = be32(de.first_cluster);
	info.size = be32(de.size);
	info.create_date = be16(de.create_date);
	info.create_time = be16(de.create_time);
	info.write_date = be16(de.write_date);
	info.write_time = be16(de.write_time);
	info.access_date = be16(de.access_date);
	info.access_time = be16(de.access_time);
	return info;
}

static void info_to_dirent(const char *name, uint8_t attrs, uint32_t first_cluster,
                            uint32_t size, FatxDirEntry &de)
{
	memset(&de, 0xFF, sizeof(de));
	size_t len = strlen(name);
	if (len > FATX_NAME_MAX)
		len = FATX_NAME_MAX;
	de.name_len = (uint8_t)len;
	de.attrs = attrs;
	memcpy(de.name, name, len);
	if (len < FATX_NAME_MAX)
		memset(de.name + len, 0xFF, FATX_NAME_MAX - len);
	de.first_cluster = be32(first_cluster);
	de.size = be32(size);
	uint16_t d, t;
	make_dos_timestamp(d, t);
	de.create_date = be16(d);
	de.create_time = be16(t);
	de.write_date = be16(d);
	de.write_time = be16(t);
	de.access_date = be16(d);
	de.access_time = be16(t);
}

// -- FatxFatCache --

FatxFatCache::FatxFatCache(PartitionIoPtr part, uint32_t fat_entry_size, uint32_t num_clusters)
	: m_part(std::move(part))
	, m_fat_entry_size(fat_entry_size)
	, m_fat_sector_base(FATX_FAT_OFFSET / SECTOR_SIZE)
	, m_num_clusters(num_clusters)
{
}

uint8_t *FatxFatCache::load_sector(uint32_t sector)
{
	auto it = m_cache.find(sector);
	if (it != m_cache.end())
		return it->second.data.data();

	CachedSector &cs = m_cache[sector];
	cs.data.resize(SECTOR_SIZE);
	cs.dirty = false;
	m_part->read_sectors(m_fat_sector_base + sector, 1, cs.data.data());
	return cs.data.data();
}

uint32_t FatxFatCache::get(uint32_t cluster)
{
	uint64_t byte_offset = (uint64_t)cluster * m_fat_entry_size;
	uint32_t sector = byte_offset / SECTOR_SIZE;
	uint32_t offset = byte_offset % SECTOR_SIZE;
	uint8_t *data = load_sector(sector);

	if (m_fat_entry_size == 2) {
		uint16_t val;
		memcpy(&val, data + offset, 2);
		return be16(val);
	} else {
		uint32_t val;
		memcpy(&val, data + offset, 4);
		return be32(val);
	}
}

void FatxFatCache::set(uint32_t cluster, uint32_t value)
{
	uint64_t byte_offset = (uint64_t)cluster * m_fat_entry_size;
	uint32_t sector = byte_offset / SECTOR_SIZE;
	uint32_t offset = byte_offset % SECTOR_SIZE;
	uint8_t *data = load_sector(sector);
	m_cache[sector].dirty = true;

	if (m_fat_entry_size == 2) {
		uint16_t val = be16((uint16_t)value);
		memcpy(data + offset, &val, 2);
	} else {
		uint32_t val = be32(value);
		memcpy(data + offset, &val, 4);
	}
}

void FatxFatCache::flush()
{
	for (auto &[sector, cs] : m_cache) {
		if (cs.dirty) {
			m_part->write_sectors(m_fat_sector_base + sector, 1, cs.data.data());
			cs.dirty = false;
		}
	}
}

uint32_t FatxFatCache::fat_end() const
{
	return m_fat_entry_size == 2 ? FATX_FAT_END16 : FATX_FAT_END32;
}

bool FatxFatCache::is_end(uint32_t value) const
{
	return value == 0 || value >= (fat_end() & ~(uint32_t)0x7);
}

uint32_t FatxFatCache::num_clusters() const
{
	return m_num_clusters;
}

// -- FatxFilesystem --

static void compute_fat_layout(uint64_t part_bytes, uint32_t cluster_size,
                                uint32_t &fat_entry_size, uint64_t &data_start, uint32_t &num_clusters)
{
	uint64_t max_clusters = part_bytes / cluster_size + 1;
	fat_entry_size = max_clusters < 0xFFF0 ? 2 : 4;
	uint64_t fat_size = max_clusters * fat_entry_size;
	fat_size = (fat_size + 0xFFF) & ~(uint64_t)0xFFF;
	data_start = FATX_FAT_OFFSET + fat_size;
	num_clusters = (uint32_t)((part_bytes - data_start) / cluster_size);
}

static FatxHeader read_fatx_header(const PartitionIoPtr &part)
{
	SectorBuffer buf(SECTOR_SIZE);
	part->read_sectors(0, 1, buf.data());
	FatxHeader h;
	memcpy(&h, buf.data(), sizeof(h));
	h.magic = be32(h.magic);
	h.volume_id = be32(h.volume_id);
	h.sectors_per_cluster = be32(h.sectors_per_cluster);
	h.root_dir_cluster = be32(h.root_dir_cluster);
	if (h.magic != FATX_MAGIC)
		throw std::runtime_error("Not a FATX partition");
	return h;
}

static uint64_t compute_data_start(uint64_t part_bytes, uint32_t cluster_size)
{
	uint32_t fat_entry_size, num_clusters;
	uint64_t data_start;
	compute_fat_layout(part_bytes, cluster_size, fat_entry_size, data_start, num_clusters);
	return data_start;
}

static uint32_t compute_fat_entry_size(uint64_t part_bytes, uint32_t cluster_size)
{
	uint32_t fat_entry_size, num_clusters;
	uint64_t data_start;
	compute_fat_layout(part_bytes, cluster_size, fat_entry_size, data_start, num_clusters);
	return fat_entry_size;
}

static uint32_t compute_num_clusters(uint64_t part_bytes, uint32_t cluster_size)
{
	uint32_t fat_entry_size, num_clusters;
	uint64_t data_start;
	compute_fat_layout(part_bytes, cluster_size, fat_entry_size, data_start, num_clusters);
	return num_clusters;
}

FatxFilesystem::FatxFilesystem(PartitionIoPtr part)
	: m_part(std::move(part))
	, m_header(read_fatx_header(m_part))
	, m_cluster_size(m_header.sectors_per_cluster * SECTOR_SIZE)
	, m_fat(m_part,
	        compute_fat_entry_size(m_part->length() * SECTOR_SIZE, m_cluster_size),
	        compute_num_clusters(m_part->length() * SECTOR_SIZE, m_cluster_size))
{
	m_data_start_sector = compute_data_start(m_part->length() * SECTOR_SIZE, m_cluster_size) / SECTOR_SIZE;
}

uint32_t FatxFilesystem::root_cluster() const
{
	return m_header.root_dir_cluster;
}

uint64_t FatxFilesystem::cluster_to_sector(uint32_t cluster)
{
	return m_data_start_sector + (uint64_t)(cluster - 1) * m_header.sectors_per_cluster;
}

void FatxFilesystem::read_cluster(uint32_t cluster, void *buf)
{
	m_part->read_sectors(cluster_to_sector(cluster), m_header.sectors_per_cluster, buf);
}

void FatxFilesystem::write_cluster(uint32_t cluster, const void *buf)
{
	m_part->write_sectors(cluster_to_sector(cluster), m_header.sectors_per_cluster, buf);
}

// -- Chain I/O --

SectorBuffer FatxFilesystem::read_chain(uint32_t first_cluster)
{
	SectorBuffer data;
	uint32_t cluster = first_cluster;
	SectorBuffer cbuf(m_cluster_size);

	while (!m_fat.is_end(cluster)) {
		if (cluster >= m_fat.num_clusters())
			throw std::runtime_error("FAT cluster out of range");
		read_cluster(cluster, cbuf.data());
		data.insert(data.end(), cbuf.begin(), cbuf.end());
		cluster = m_fat.get(cluster);
	}
	return data;
}

std::vector<uint32_t> FatxFilesystem::alloc_clusters(uint32_t count)
{
	std::vector<uint32_t> allocated;
	allocated.reserve(count);
	for (uint32_t c = 2; c < m_fat.num_clusters() && allocated.size() < count; ++c) {
		if (m_fat.get(c) == 0)
			allocated.push_back(c);
	}
	if (allocated.size() < count)
		throw std::runtime_error("Not enough free clusters");
	for (uint32_t i = 0; i < allocated.size(); ++i)
		m_fat.set(allocated[i], i + 1 < allocated.size() ? allocated[i + 1] : m_fat.fat_end());
	return allocated;
}

void FatxFilesystem::free_chain(uint32_t first_cluster)
{
	uint32_t cluster = first_cluster;
	while (!m_fat.is_end(cluster)) {
		uint32_t next = m_fat.get(cluster);
		m_fat.set(cluster, 0);
		cluster = next;
	}
}

uint32_t FatxFilesystem::write_chain(const void *data, size_t size)
{
	uint32_t n_clusters = std::max((size_t)1, (size + m_cluster_size - 1) / m_cluster_size);
	auto clusters = alloc_clusters(n_clusters);

	SectorBuffer cbuf(m_cluster_size, 0xFF);
	const uint8_t *src = (const uint8_t *)data;

	for (uint32_t i = 0; i < clusters.size(); ++i) {
		size_t offset = (size_t)i * m_cluster_size;
		size_t chunk = std::min(m_cluster_size, (uint32_t)(size - offset));
		memset(cbuf.data(), 0xFF, m_cluster_size);
		if (offset < size)
			memcpy(cbuf.data(), src + offset, chunk);
		write_cluster(clusters[i], cbuf.data());
	}
	m_fat.flush();
	return clusters[0];
}

// -- Directory I/O --

std::vector<FatxFileInfo> FatxFilesystem::read_dir(uint32_t first_cluster)
{
	auto raw = read_chain(first_cluster);
	std::vector<FatxFileInfo> entries;

	for (size_t i = 0; i + FATX_DIRENT_SIZE <= raw.size(); i += FATX_DIRENT_SIZE) {
		uint8_t name_len = raw[i];
		if (name_len == 0xFF || name_len == 0x00)
			break;
		if (name_len == 0xE5)
			continue;
		FatxDirEntry de;
		memcpy(&de, raw.data() + i, sizeof(de));
		entries.push_back(dirent_to_info(de));
	}
	return entries;
}

SectorBuffer FatxFilesystem::read_dir_raw(uint32_t first_cluster)
{
	return read_chain(first_cluster);
}

void FatxFilesystem::write_dir_raw(uint32_t first_cluster, const SectorBuffer &raw)

{
	uint32_t cluster = first_cluster;
	size_t offset = 0;

	while (offset < raw.size() && !m_fat.is_end(cluster)) {
		SectorBuffer cbuf(m_cluster_size, 0xFF);
		size_t chunk = std::min((size_t)m_cluster_size, raw.size() - offset);
		memcpy(cbuf.data(), raw.data() + offset, chunk);
		write_cluster(cluster, cbuf.data());
		offset += m_cluster_size;
		cluster = m_fat.get(cluster);
	}

	if (offset < raw.size()) {
		uint32_t extra = (raw.size() - offset + m_cluster_size - 1) / m_cluster_size;
		auto new_clusters = alloc_clusters(extra);

		// find last cluster in existing chain
		uint32_t prev = first_cluster;
		while (!m_fat.is_end(m_fat.get(prev)))
			prev = m_fat.get(prev);
		m_fat.set(prev, new_clusters[0]);

		for (auto c : new_clusters) {
			SectorBuffer cbuf(m_cluster_size, 0xFF);
			size_t chunk = std::min((size_t)m_cluster_size, raw.size() - offset);
			if (offset < raw.size())
				memcpy(cbuf.data(), raw.data() + offset, chunk);
			write_cluster(c, cbuf.data());
			offset += m_cluster_size;
		}
		m_fat.flush();
	}
}

void FatxFilesystem::add_dir_entry(uint32_t dir_cluster, const FatxDirEntry &ent)
{
	auto raw = read_dir_raw(dir_cluster);

	for (size_t i = 0; i + FATX_DIRENT_SIZE <= raw.size(); i += FATX_DIRENT_SIZE) {
		uint8_t slot = raw[i];
		if (slot == 0xFF || slot == 0x00 || slot == 0xE5) {
			memcpy(raw.data() + i, &ent, FATX_DIRENT_SIZE);
			write_dir_raw(dir_cluster, raw);
			return;
		}
	}

	// extend
	size_t old_size = raw.size();
	raw.resize(old_size + m_cluster_size, 0xFF);
	memcpy(raw.data() + old_size, &ent, FATX_DIRENT_SIZE);
	write_dir_raw(dir_cluster, raw);
}

bool FatxFilesystem::remove_dir_entry(uint32_t dir_cluster, const char *name, FatxDirEntry *removed)
{
	auto raw = read_dir_raw(dir_cluster);

	for (size_t i = 0; i + FATX_DIRENT_SIZE <= raw.size(); i += FATX_DIRENT_SIZE) {
		uint8_t name_len = raw[i];
		if (name_len == 0xFF || name_len == 0x00)
			break;
		if (name_len == 0xE5)
			continue;
		FatxDirEntry de;
		memcpy(&de, raw.data() + i, sizeof(de));
		std::string entry_name(de.name, name_len > FATX_NAME_MAX ? FATX_NAME_MAX : name_len);
		if (strcasecmp_ascii(entry_name.c_str(), name) == 0) {
			if (removed)
				*removed = de;
			raw[i] = 0xE5;
			write_dir_raw(dir_cluster, raw);
			return true;
		}
	}
	return false;
}

bool FatxFilesystem::rename_dir_entry(uint32_t dir_cluster, const char *old_name, const char *new_name)
{
	auto raw = read_dir_raw(dir_cluster);

	for (size_t i = 0; i + FATX_DIRENT_SIZE <= raw.size(); i += FATX_DIRENT_SIZE) {
		uint8_t name_len = raw[i];
		if (name_len == 0xFF || name_len == 0x00)
			break;
		if (name_len == 0xE5)
			continue;
		FatxDirEntry de;
		memcpy(&de, raw.data() + i, sizeof(de));
		std::string entry_name(de.name, name_len > FATX_NAME_MAX ? FATX_NAME_MAX : name_len);
		if (strcasecmp_ascii(entry_name.c_str(), old_name) == 0) {
			size_t new_len = strlen(new_name);
			if (new_len > FATX_NAME_MAX)
				new_len = FATX_NAME_MAX;
			de.name_len = (uint8_t)new_len;
			memcpy(de.name, new_name, new_len);
			if (new_len < FATX_NAME_MAX)
				memset(de.name + new_len, 0xFF, FATX_NAME_MAX - new_len);
			uint16_t d, t;
			make_dos_timestamp(d, t);
			de.write_date = be16(d);
			de.write_time = be16(t);
			de.access_date = be16(d);
			de.access_time = be16(t);
			memcpy(raw.data() + i, &de, FATX_DIRENT_SIZE);
			write_dir_raw(dir_cluster, raw);
			return true;
		}
	}
	return false;
}

void FatxFilesystem::delete_dir_contents(uint32_t dir_cluster)
{
	auto entries = read_dir(dir_cluster);
	for (auto &ent : entries) {
		if (ent.is_directory())
			delete_dir_contents(ent.first_cluster);
		free_chain(ent.first_cluster);
	}
}

// -- High-level operations --

FatxFileInfo *FatxFilesystem::find_entry(uint32_t dir_cluster, const char *name,
                                          std::vector<FatxFileInfo> &entries)
{
	entries = read_dir(dir_cluster);
	for (auto &e : entries) {
		if (strcasecmp_ascii(e.name.c_str(), name) == 0)
			return &e;
	}
	return nullptr;
}

SectorBuffer FatxFilesystem::read_file(const FatxFileInfo &ent)
{
	auto data = read_chain(ent.first_cluster);
	data.resize(ent.size);
	return data;
}

void FatxFilesystem::import_file(uint32_t dir_cluster, const char *name,
                                  const void *data, size_t size, bool overwrite)
{
	std::vector<FatxFileInfo> entries;
	auto *existing = find_entry(dir_cluster, name, entries);
	if (existing) {
		if (!overwrite)
			throw std::runtime_error("File already exists");
		free_chain(existing->first_cluster);
		FatxDirEntry removed;
		remove_dir_entry(dir_cluster, name, &removed);
	}

	uint32_t first_cluster = write_chain(data, size);
	FatxDirEntry de;
	info_to_dirent(name, FATX_ATTR_ARCHIVE, first_cluster, (uint32_t)size, de);
	add_dir_entry(dir_cluster, de);
}

void FatxFilesystem::mkdir(uint32_t dir_cluster, const char *name)
{
	std::vector<FatxFileInfo> entries;
	if (find_entry(dir_cluster, name, entries))
		throw std::runtime_error("Directory already exists");

	auto clusters = alloc_clusters(1);
	SectorBuffer empty(m_cluster_size, 0xFF);
	write_cluster(clusters[0], empty.data());
	m_fat.flush();

	FatxDirEntry de;
	info_to_dirent(name, FATX_ATTR_DIRECTORY, clusters[0], 0, de);
	add_dir_entry(dir_cluster, de);
}

void FatxFilesystem::remove(uint32_t dir_cluster, const char *name, bool recursive)
{
	FatxDirEntry removed;
	if (!remove_dir_entry(dir_cluster, name, &removed))
		throw std::runtime_error("File not found");

	uint32_t first_cluster = be32(removed.first_cluster);
	if (removed.attrs & FATX_ATTR_DIRECTORY) {
		if (recursive)
			delete_dir_contents(first_cluster);
	}
	free_chain(first_cluster);
	m_fat.flush();
}

void FatxFilesystem::rename(uint32_t dir_cluster, const char *old_name, const char *new_name)
{
	if (!rename_dir_entry(dir_cluster, old_name, new_name))
		throw std::runtime_error("File not found");
}

void FatxFilesystem::flush()
{
	m_fat.flush();
}
