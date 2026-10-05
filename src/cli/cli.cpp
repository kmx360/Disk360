// SPDX-License-Identifier: GPL-2.0-only

#include "diskio.h"
#include "fatx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace stdfs = std::filesystem;

static const char *fmt_size(uint64_t n, char *buf, size_t buflen)
{
	const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
	double v = (double)n;
	int u = 0;
	while (v >= 1024.0 && u < 5) {
		v /= 1024.0;
		u++;
	}
	if (v == (int)v)
		snprintf(buf, buflen, "%d %s", (int)v, units[u]);
	else
		snprintf(buf, buflen, "%.1f %s", v, units[u]);
	return buf;
}

static FatxPartition *find_partition(std::vector<FatxPartition> &parts, const char *name)
{
	for (auto &p : parts)
		if (p.name == name)
			return &p;
	return nullptr;
}

static std::vector<std::string> split_path(const char *path)
{
	std::vector<std::string> parts;
	std::string p(path);
	size_t start = 0;
	for (size_t i = 0; i <= p.size(); ++i) {
		if (i == p.size() || p[i] == '/') {
			if (i > start)
				parts.push_back(p.substr(start, i - start));
			start = i + 1;
		}
	}
	return parts;
}

static uint32_t resolve_dir(FatxFilesystem &fs, const char *path)
{
	uint32_t cluster = fs.root_cluster();
	for (auto &part : split_path(path)) {
		std::vector<FatxFileInfo> entries;
		auto *ent = fs.find_entry(cluster, part.c_str(), entries);
		if (!ent) {
			fprintf(stderr, "Path not found: %s\n", path);
			exit(1);
		}
		if (!ent->is_directory()) {
			fprintf(stderr, "Not a directory: %s\n", part.c_str());
			exit(1);
		}
		cluster = ent->first_cluster;
	}
	return cluster;
}

static void resolve_parent(FatxFilesystem &fs, const char *path,
                            uint32_t &parent_cluster, std::string &basename)
{
	auto parts = split_path(path);
	if (parts.empty()) {
		fprintf(stderr, "Empty path\n");
		exit(1);
	}
	basename = parts.back();
	parts.pop_back();

	parent_cluster = fs.root_cluster();
	for (auto &part : parts) {
		std::vector<FatxFileInfo> entries;
		auto *ent = fs.find_entry(parent_cluster, part.c_str(), entries);
		if (!ent || !ent->is_directory()) {
			fprintf(stderr, "Directory not found: %s\n", part.c_str());
			exit(1);
		}
		parent_cluster = ent->first_cluster;
	}
}

// -- Commands --

static void cmd_disks()
{
	auto disks = enumerate_disks();
	if (disks.empty()) {
		printf("No disks found.\n");
		return;
	}
	char buf[64];
	for (auto &d : disks) {
		printf("%-20s  %-20s  %-20s  %s\n",
			d.path.c_str(), d.model.c_str(), d.serial.c_str(),
			fmt_size(d.total_sectors * SECTOR_SIZE, buf, sizeof(buf)));
	}
}

static void cmd_info(const DiskIoPtr &disk)
{
	char buf[64];
	uint64_t sectors = disk->total_sectors();
	printf("Disk size:  %s (%#llx sectors)\n",
		fmt_size(sectors * SECTOR_SIZE, buf, sizeof(buf)),
		(unsigned long long)sectors);
	printf("\n");

	auto parts = discover_partitions(disk);
	if (parts.empty()) {
		printf("No FATX partitions found.\n");
		return;
	}

	for (auto &p : parts) {
		printf("  [%s]\n", p.name.c_str());
		printf("  Offset:   sector %#llx (%s)\n",
			(unsigned long long)p.part->start(),
			fmt_size(p.part->start() * SECTOR_SIZE, buf, sizeof(buf)));
		printf("  Size:     %s (%#llx sectors)\n",
			fmt_size(p.part->length() * SECTOR_SIZE, buf, sizeof(buf)),
			(unsigned long long)p.part->length());
		printf("\n");
	}
}

static void cmd_ls(FatxFilesystem &fs, const char *path)
{
	uint32_t cluster = (path && path[0]) ? resolve_dir(fs, path) : fs.root_cluster();
	auto entries = fs.read_dir(cluster);

	for (auto &ent : entries) {
		const char *kind = ent.is_directory() ? "D" : " ";
		printf("%s %10u  %s\n", kind, ent.size, ent.name.c_str());
	}
}

static void print_tree(FatxFilesystem &fs, uint32_t cluster,
                        const std::string &prefix, int &dirs, int &files)
{
	auto entries = fs.read_dir(cluster);
	for (size_t i = 0; i < entries.size(); ++i) {
		auto &ent = entries[i];
		bool last = (i == entries.size() - 1);
		const char *connector = last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 "
		                             : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";

		if (ent.is_directory()) {
			dirs++;
			printf("%s%s%s\n", prefix.c_str(), connector, ent.name.c_str());
			std::string ext = prefix + (last ? "    " : "\xe2\x94\x82   ");
			print_tree(fs, ent.first_cluster, ext, dirs, files);
		} else {
			files++;
			char buf[64];
			printf("%s%s%s  [%s]\n", prefix.c_str(), connector, ent.name.c_str(),
				fmt_size(ent.size, buf, sizeof(buf)));
		}
	}
}

static void cmd_tree(FatxFilesystem &fs, const char *path)
{
	uint32_t cluster;
	if (path && path[0]) {
		cluster = resolve_dir(fs, path);
		printf("%s\n", path);
	} else {
		cluster = fs.root_cluster();
		printf(".\n");
	}

	int dirs = 0, files = 0;
	print_tree(fs, cluster, "", dirs, files);
	printf("\n%d directories, %d files\n", dirs, files);
}

static void extract_recursive(FatxFilesystem &fs, uint32_t cluster, const stdfs::path &dest)
{
	stdfs::create_directories(dest);
	auto entries = fs.read_dir(cluster);
	for (auto &ent : entries) {
		stdfs::path child = dest / ent.name;
		if (ent.is_directory()) {
			extract_recursive(fs, ent.first_cluster, child);
		} else {
			auto data = fs.read_file(ent);
			std::ofstream f(child, std::ios::binary);
			f.write((const char *)data.data(), data.size());
			printf("  %s  (%zu bytes)\n", child.c_str(), data.size());
		}
	}
}

static void cmd_extract(FatxFilesystem &fs, const char *path, const char *output)
{
	if (!path || !path[0]) {
		fprintf(stderr, "extract requires a path\n");
		exit(1);
	}

	uint32_t parent_cluster;
	std::string basename;
	resolve_parent(fs, path, parent_cluster, basename);

	std::vector<FatxFileInfo> entries;
	auto *ent = fs.find_entry(parent_cluster, basename.c_str(), entries);
	if (!ent) {
		fprintf(stderr, "Path not found: %s\n", path);
		exit(1);
	}

	if (ent->is_directory()) {
		stdfs::path out_dir = output ? output : basename.c_str();
		extract_recursive(fs, ent->first_cluster, out_dir);
	} else {
		const char *out_path = output ? output : basename.c_str();
		auto data = fs.read_file(*ent);
		std::ofstream f(out_path, std::ios::binary);
		f.write((const char *)data.data(), data.size());
		printf("Extracted %zu bytes to %s\n", data.size(), out_path);
	}
}

static void import_recursive(FatxFilesystem &fs, uint32_t dir_cluster, const stdfs::path &host_path)
{
	for (auto &entry : stdfs::directory_iterator(host_path)) {
		std::string name = entry.path().filename().string();
		if (entry.is_directory()) {
			fs.mkdir(dir_cluster, name.c_str());
			std::vector<FatxFileInfo> entries;
			auto *ent = fs.find_entry(dir_cluster, name.c_str(), entries);
			import_recursive(fs, ent->first_cluster, entry.path());
		} else {
			auto data = std::ifstream(entry.path(), std::ios::binary | std::ios::ate);
			size_t size = data.tellg();
			data.seekg(0);
			std::vector<uint8_t> buf(size);
			data.read((char *)buf.data(), size);
			fs.import_file(dir_cluster, name.c_str(), buf.data(), buf.size());
			printf("  %s  (%zu bytes)\n", entry.path().c_str(), size);
		}
	}
}

static void cmd_import(FatxFilesystem &fs, const char *dest_path, const char *host_path)
{
	if (!host_path) {
		fprintf(stderr, "import requires a host path\n");
		exit(1);
	}

	uint32_t dir_cluster;
	if (dest_path && dest_path[0])
		dir_cluster = resolve_dir(fs, dest_path);
	else
		dir_cluster = fs.root_cluster();

	stdfs::path hp(host_path);
	if (stdfs::is_directory(hp)) {
		std::string name = hp.filename().string();
		fs.mkdir(dir_cluster, name.c_str());
		std::vector<FatxFileInfo> entries;
		auto *ent = fs.find_entry(dir_cluster, name.c_str(), entries);
		import_recursive(fs, ent->first_cluster, hp);
	} else {
		std::string name = hp.filename().string();
		auto data = std::ifstream(hp, std::ios::binary | std::ios::ate);
		if (!data) {
			fprintf(stderr, "Cannot open: %s\n", host_path);
			exit(1);
		}
		size_t size = data.tellg();
		data.seekg(0);
		std::vector<uint8_t> buf(size);
		data.read((char *)buf.data(), size);
		fs.import_file(dir_cluster, name.c_str(), buf.data(), buf.size());
		printf("Imported %zu bytes as %s\n", size, name.c_str());
	}
	fs.flush();
}

static void cmd_mkdir(FatxFilesystem &fs, const char *path)
{
	if (!path || !path[0]) {
		fprintf(stderr, "mkdir requires a path\n");
		exit(1);
	}

	uint32_t parent_cluster;
	std::string basename;
	resolve_parent(fs, path, parent_cluster, basename);
	fs.mkdir(parent_cluster, basename.c_str());
	fs.flush();
}

static void cmd_rm(FatxFilesystem &fs, const char *path, bool recursive)
{
	if (!path || !path[0]) {
		fprintf(stderr, "rm requires a path\n");
		exit(1);
	}

	uint32_t parent_cluster;
	std::string basename;
	resolve_parent(fs, path, parent_cluster, basename);
	fs.remove(parent_cluster, basename.c_str(), recursive);
}

static void cmd_mv(FatxFilesystem &fs, const char *path, const char *new_name)
{
	if (!path || !path[0] || !new_name) {
		fprintf(stderr, "mv requires a path and new name\n");
		exit(1);
	}

	uint32_t parent_cluster;
	std::string basename;
	resolve_parent(fs, path, parent_cluster, basename);
	fs.rename(parent_cluster, basename.c_str(), new_name);
}

// -- Main --

static void usage()
{
	fprintf(stderr,
		"Usage: disk360-cli [-d <disk>] <command> [options]\n"
		"\n"
		"Commands:\n"
		"  disks                              List available disks\n"
		"  info                               Show disk and partition info\n"
		"  ls      [-p part] [path]           List directory\n"
		"  tree    [-p part] [path]           Recursive file listing\n"
		"  extract [-p part] <path> [-o out]  Extract file or directory\n"
		"  import  [-p part] [dest] <host>    Import file or directory\n"
		"  mkdir   [-p part] <path>           Create directory\n"
		"  rm      [-p part] [-r] <path>      Delete file or directory\n"
		"  mv      [-p part] <path> <name>    Rename file or directory\n"
		"\n"
		"Options:\n"
		"  -d <disk>       Disk device or image path\n"
		"  -p <partition>  Partition name (default: content)\n"
		"  -o <output>     Output file/directory\n"
		"  -r              Recursive delete\n"
	);
	exit(1);
}

int main(int argc, char *argv[])
{
	const char *disk_path = nullptr;
	const char *part_name = "content";
	const char *output = nullptr;
	const char *command = nullptr;
	bool recursive = false;
	std::vector<const char *> positional;

	int i = 1;
	while (i < argc) {
		if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
			disk_path = argv[++i];
		} else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
			part_name = argv[++i];
		} else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			output = argv[++i];
		} else if (strcmp(argv[i], "-r") == 0) {
			recursive = true;
		} else if (!command) {
			command = argv[i];
		} else {
			positional.push_back(argv[i]);
		}
		i++;
	}

	if (!command)
		usage();

	if (strcmp(command, "disks") == 0) {
		cmd_disks();
		return 0;
	}

	if (!disk_path)
		usage();

	auto disk = std::make_shared<DiskIo>(disk_path);

	if (strcmp(command, "info") == 0) {
		cmd_info(disk);
		return 0;
	}

	auto parts = discover_partitions(disk);
	auto *p = find_partition(parts, part_name);
	if (!p) {
		fprintf(stderr, "Partition '%s' not found\n", part_name);
		return 1;
	}

	FatxFilesystem fs(p->part);
	const char *arg0 = positional.size() > 0 ? positional[0] : nullptr;
	const char *arg1 = positional.size() > 1 ? positional[1] : nullptr;

	if (strcmp(command, "ls") == 0)
		cmd_ls(fs, arg0);
	else if (strcmp(command, "tree") == 0)
		cmd_tree(fs, arg0);
	else if (strcmp(command, "extract") == 0)
		cmd_extract(fs, arg0, output);
	else if (strcmp(command, "import") == 0)
		cmd_import(fs, arg1 ? arg0 : nullptr, arg1 ? arg1 : arg0);
	else if (strcmp(command, "mkdir") == 0)
		cmd_mkdir(fs, arg0);
	else if (strcmp(command, "rm") == 0)
		cmd_rm(fs, arg0, recursive);
	else if (strcmp(command, "mv") == 0)
		cmd_mv(fs, arg0, arg1);
	else
		usage();

	return 0;
}
