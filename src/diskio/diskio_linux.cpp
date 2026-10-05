// SPDX-License-Identifier: GPL-2.0-only

#include "diskio.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include <fcntl.h>
#include <libudev.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <unistd.h>

static std::string safe_str(const char *s)
{
	return s ? s : "";
}

struct DiskIo::Impl {
	int fd;

	Impl(const char *path)
	{
		fd = open(path, O_RDONLY | O_CLOEXEC | O_EXCL | O_DIRECT);
		if (fd < 0)
			throw std::system_error(errno, std::generic_category(), path);
	}

	~Impl()
	{
		close(fd);
	}
};

DiskIo::DiskIo(const char *path) : m_impl(std::make_unique<Impl>(path)) {}
DiskIo::~DiskIo() = default;
DiskIo::DiskIo(DiskIo &&other) noexcept = default;
DiskIo &DiskIo::operator=(DiskIo &&other) noexcept = default;

uint64_t DiskIo::total_sectors()
{
	uint64_t blksize;
	if (ioctl(m_impl->fd, BLKGETSIZE64, &blksize) < 0)
		throw std::system_error(errno, std::generic_category(), "BLKGETSIZE64");
	return blksize / SECTOR_SIZE;
}

void DiskIo::read_sectors(uint64_t start, uint64_t count, void *buf)
{
	if (pread(m_impl->fd, buf, count * SECTOR_SIZE, start * SECTOR_SIZE) < 0)
		throw std::system_error(errno, std::generic_category(), "pread");
}

void DiskIo::write_sectors(uint64_t start, uint64_t count, const void *buf)
{
	if (pwrite(m_impl->fd, buf, count * SECTOR_SIZE, start * SECTOR_SIZE) < 0)
		throw std::system_error(errno, std::generic_category(), "pwrite");
}

std::vector<DiskInfo> enumerate_disks()
{
	std::vector<DiskInfo> result;

	struct udev *udev = udev_new();
	if (!udev)
		throw std::system_error(errno, std::generic_category(), "udev_new");

	struct udev_enumerate *en = udev_enumerate_new(udev);
	if (!en) {
		udev_unref(udev);
		throw std::system_error(errno, std::generic_category(), "udev_enumerate_new");
	}

	int r;
	if ((r = udev_enumerate_add_match_subsystem(en, "block")) < 0 ||
	    (r = udev_enumerate_add_match_property(en, "DEVTYPE", "disk")) < 0 ||
	    (r = udev_enumerate_scan_devices(en)) < 0) {
		udev_enumerate_unref(en);
		udev_unref(udev);
		throw std::system_error(-r, std::generic_category(), "udev enumerate");
	}

	for (auto *entry = udev_enumerate_get_list_entry(en);
	     entry;
	     entry = udev_list_entry_get_next(entry)) {
		struct udev_device *dev = udev_device_new_from_syspath(
			udev, udev_list_entry_get_name(entry));
		if (!dev)
			continue;

		if (!udev_device_get_parent(dev)) {
			udev_device_unref(dev);
			continue;
		}

		const char *devnode = udev_device_get_devnode(dev);
		if (!devnode) {
			udev_device_unref(dev);
			continue;
		}

		DiskInfo info;
		info.path = devnode;
		info.model = safe_str(udev_device_get_property_value(dev, "ID_MODEL"));
		info.serial = safe_str(udev_device_get_property_value(dev, "ID_SERIAL_SHORT"));

		const char *size_str = udev_device_get_sysattr_value(dev, "size");
		info.total_sectors = size_str ? strtoull(size_str, nullptr, 10) : 0;

		udev_device_unref(dev);
		result.push_back(std::move(info));
	}

	udev_enumerate_unref(en);
	udev_unref(udev);
	return result;
}
