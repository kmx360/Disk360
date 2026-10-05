// SPDX-License-Identifier: GPL-2.0-only

#include "diskio.h"

#include <stdexcept>

PartitionIo::PartitionIo(DiskIoPtr disk, uint64_t start, uint64_t length)
	: m_disk(std::move(disk))
	, m_start(start)
	, m_length(length)
{
}

uint64_t PartitionIo::start() const
{
	return m_start;
}

uint64_t PartitionIo::length() const
{
	return m_length;
}

void PartitionIo::read_sectors(uint64_t start, uint64_t count, void *buf)
{
	if (start + count > m_length || start + count < start)
		throw std::out_of_range("partition read out of bounds");
	m_disk->read_sectors(m_start + start, count, buf);
}

void PartitionIo::write_sectors(uint64_t start, uint64_t count, const void *buf)
{
	if (start + count > m_length || start + count < start)
		throw std::out_of_range("partition write out of bounds");
	m_disk->write_sectors(m_start + start, count, buf);
}
