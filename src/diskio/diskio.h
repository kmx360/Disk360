// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#define SECTOR_SIZE 0x200

static inline void *sector_alloc(size_t size)
{
	size = (size + SECTOR_SIZE - 1) & ~(SECTOR_SIZE - 1);
#ifdef _WIN32
	return _aligned_malloc(size, SECTOR_SIZE);
#else
	return aligned_alloc(SECTOR_SIZE, size);
#endif
}

static inline void sector_free(void *p)
{
#ifdef _WIN32
	_aligned_free(p);
#else
	free(p);
#endif
}

template <typename T>
struct SectorAllocator {
	using value_type = T;
	SectorAllocator() = default;
	template <typename U> SectorAllocator(const SectorAllocator<U> &) {}
	T *allocate(size_t n) {
		void *p = sector_alloc(n * sizeof(T));
		if (!p) throw std::bad_alloc();
		return static_cast<T *>(p);
	}
	void deallocate(T *p, size_t) { sector_free(p); }
};
template <typename T, typename U>
bool operator==(const SectorAllocator<T> &, const SectorAllocator<U> &) { return true; }

using SectorBuffer = std::vector<uint8_t, SectorAllocator<uint8_t>>;

struct DiskInfo {
	std::string path;
	std::string model;
	std::string serial;
	uint64_t total_sectors;
};

class DiskIo {
private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
public:
	DiskIo(const char *path);
	~DiskIo();
	DiskIo(DiskIo &&other) noexcept;
	DiskIo &operator=(DiskIo &&other) noexcept;
	uint64_t total_sectors();
	void read_sectors(uint64_t start, uint64_t count, void *buf);
	void write_sectors(uint64_t start, uint64_t count, const void *buf);
};

using DiskIoPtr = std::shared_ptr<DiskIo>;

class PartitionIo {
private:
	DiskIoPtr m_disk;
	uint64_t m_start;
	uint64_t m_length;
public:
	PartitionIo(DiskIoPtr disk, uint64_t start, uint64_t length);
	uint64_t start() const;
	uint64_t length() const;
	void read_sectors(uint64_t start, uint64_t count, void *buf);
	void write_sectors(uint64_t start, uint64_t count, const void *buf);
};

using PartitionIoPtr = std::shared_ptr<PartitionIo>;

std::vector<DiskInfo> enumerate_disks();
