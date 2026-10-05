// SPDX-License-Identifier: GPL-2.0-only

#include "diskio.h"

#include <system_error>

#include <windows.h>
#include <setupapi.h>
#include <ntddstor.h>
#include <ntddscsi.h>
#include <winioctl.h>

static void throw_win32(const char *context)
{
	throw std::system_error(GetLastError(), std::system_category(), context);
}

struct DiskIo::Impl {
	HANDLE handle;

	Impl(const char *path)
	{
		handle = CreateFileA(path, GENERIC_READ, 0,
			nullptr, OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
		if (handle == INVALID_HANDLE_VALUE)
			throw_win32(path);
	}

	~Impl()
	{
		CloseHandle(handle);
	}
};

DiskIo::DiskIo(const char *path) : m_impl(std::make_unique<Impl>(path)) {}
DiskIo::~DiskIo() = default;
DiskIo::DiskIo(DiskIo &&other) noexcept = default;
DiskIo &DiskIo::operator=(DiskIo &&other) noexcept = default;

uint64_t DiskIo::total_sectors()
{
	DISK_GEOMETRY_EX geom;
	DWORD returned;
	if (!DeviceIoControl(m_impl->handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
	                     nullptr, 0, &geom, sizeof(geom), &returned, nullptr))
		throw_win32("IOCTL_DISK_GET_DRIVE_GEOMETRY_EX");
	return geom.DiskSize.QuadPart / SECTOR_SIZE;
}

void DiskIo::read_sectors(uint64_t start, uint64_t count, void *buf)
{
	LARGE_INTEGER offset;
	offset.QuadPart = start * SECTOR_SIZE;
	OVERLAPPED ov = {};
	ov.Offset = offset.LowPart;
	ov.OffsetHigh = offset.HighPart;
	DWORD bytes_read;
	DWORD to_read = (DWORD)(count * SECTOR_SIZE);
	if (!ReadFile(m_impl->handle, buf, to_read, &bytes_read, &ov))
		throw_win32("ReadFile");
}

void DiskIo::write_sectors(uint64_t start, uint64_t count, const void *buf)
{
	LARGE_INTEGER offset;
	offset.QuadPart = start * SECTOR_SIZE;
	OVERLAPPED ov = {};
	ov.Offset = offset.LowPart;
	ov.OffsetHigh = offset.HighPart;
	DWORD bytes_written;
	DWORD to_write = (DWORD)(count * SECTOR_SIZE);
	if (!WriteFile(m_impl->handle, buf, to_write, &bytes_written, &ov))
		throw_win32("WriteFile");
}

static std::string trim(const char *s, size_t len)
{
	while (len > 0 && s[len - 1] == ' ')
		--len;
	return std::string(s, len);
}

struct DiskQueryResult {
	std::string model;
	std::string serial;
};

static bool query_disk_info(HANDLE h, DiskQueryResult &out)
{
	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageDeviceProperty;
	query.QueryType = PropertyStandardQuery;

	alignas(STORAGE_DEVICE_DESCRIPTOR) char buf[4096];
	DWORD returned;
	if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
	                     &query, sizeof(query), buf, sizeof(buf), &returned, nullptr))
		return false;

	auto *desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR *>(buf);
	if (desc->ProductIdOffset && desc->ProductIdOffset < returned)
		out.model = trim(buf + desc->ProductIdOffset,
			strlen(buf + desc->ProductIdOffset));
	if (desc->SerialNumberOffset && desc->SerialNumberOffset < returned)
		out.serial = trim(buf + desc->SerialNumberOffset,
			strlen(buf + desc->SerialNumberOffset));
	return true;
}

std::vector<DiskInfo> enumerate_disks()
{
	std::vector<DiskInfo> result;

	HDEVINFO devinfo = SetupDiGetClassDevsA(
		&GUID_DEVINTERFACE_DISK, nullptr, nullptr,
		DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (devinfo == INVALID_HANDLE_VALUE)
		return result;

	SP_DEVICE_INTERFACE_DATA iface_data = {};
	iface_data.cbSize = sizeof(iface_data);

	for (DWORD idx = 0;
	     SetupDiEnumDeviceInterfaces(devinfo, nullptr, &GUID_DEVINTERFACE_DISK, idx, &iface_data);
	     ++idx) {
		DWORD required_size = 0;
		SetupDiGetDeviceInterfaceDetailA(devinfo, &iface_data, nullptr, 0, &required_size, nullptr);
		if (!required_size)
			continue;

		std::vector<char> detail_buf(required_size);
		auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_A *>(detail_buf.data());
		detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

		if (!SetupDiGetDeviceInterfaceDetailA(devinfo, &iface_data, detail, required_size, nullptr, nullptr))
			continue;

		HANDLE h = CreateFileA(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, OPEN_EXISTING, 0, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			continue;

		DiskInfo info;
		info.path = detail->DevicePath;

		DISK_GEOMETRY_EX geom;
		DWORD returned;
		if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
		                    nullptr, 0, &geom, sizeof(geom), &returned, nullptr))
			info.total_sectors = geom.DiskSize.QuadPart / SECTOR_SIZE;
		else
			info.total_sectors = 0;

		DiskQueryResult qr;
		if (query_disk_info(h, qr)) {
			info.model = std::move(qr.model);
			info.serial = std::move(qr.serial);
		}

		CloseHandle(h);
		result.push_back(std::move(info));
	}

	SetupDiDestroyDeviceInfoList(devinfo);
	return result;
}
