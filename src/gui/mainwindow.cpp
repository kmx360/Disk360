// SPDX-License-Identifier: GPL-2.0-only

#include "mainwindow.h"

#include <QApplication>
#include <QFileDialog>
#include <QHeaderView>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace stdfs = std::filesystem;

static QString fmtSize(uint64_t n)
{
	const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
	double v = (double)n;
	int u = 0;
	while (v >= 1024.0 && u < 5) {
		v /= 1024.0;
		u++;
	}
	if (v == (int)v)
		return QString("%1 %2").arg((int)v).arg(units[u]);
	return QString("%1 %2").arg(v, 0, 'f', 1).arg(units[u]);
}

MainWindow::MainWindow(QWidget *parent)
	: QMainWindow(parent)
{
	setWindowTitle("Disk360");
	resize(900, 600);

	m_model = new QStandardItemModel(this);
	m_model->setHorizontalHeaderLabels({"Name", "Size", "Modified"});

	m_tree = new QTreeView;
	m_tree->setModel(m_model);
	m_tree->setAlternatingRowColors(true);
	m_tree->setContextMenuPolicy(Qt::CustomContextMenu);
	m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);

	connect(m_tree, &QTreeView::expanded, this, &MainWindow::onExpand);
	connect(m_tree, &QTreeView::clicked, this, &MainWindow::onClick);
	connect(m_tree, &QTreeView::customContextMenuRequested, this, &MainWindow::onContextMenu);

	m_detail = new QLabel("Open a disk image to browse.");
	m_detail->setWordWrap(true);
	m_detail->setAlignment(Qt::AlignTop | Qt::AlignLeft);
	m_detail->setMinimumWidth(250);
	m_detail->setStyleSheet("padding: 8px;");

	auto *splitter = new QSplitter;
	splitter->addWidget(m_tree);
	splitter->addWidget(m_detail);
	splitter->setStretchFactor(0, 3);
	splitter->setStretchFactor(1, 1);
	setCentralWidget(splitter);

	auto *fileMenu = menuBar()->addMenu("&File");
	fileMenu->addAction("&Open Image...", QKeySequence("Ctrl+O"), this, &MainWindow::onOpenImage);
	fileMenu->addAction("Open &Device...", QKeySequence("Ctrl+D"), this, &MainWindow::onOpenDevice);
	fileMenu->addSeparator();
	fileMenu->addAction("&Quit", QKeySequence("Ctrl+Q"), this, &QWidget::close);

	auto *editMenu = menuBar()->addMenu("&Edit");
	editMenu->addAction("&Extract...", QKeySequence("Ctrl+E"), this, &MainWindow::extractSelected);
	editMenu->addAction("&Import...", QKeySequence("Ctrl+I"), this, &MainWindow::importFiles);
	editMenu->addSeparator();
	editMenu->addAction("&New Directory...", QKeySequence("Ctrl+N"), this, &MainWindow::newDirectory);
	editMenu->addAction("&Rename...", QKeySequence("F2"), this, &MainWindow::renameSelected);
	editMenu->addAction("&Delete", QKeySequence("Delete"), this, &MainWindow::deleteSelected);

	statusBar()->showMessage("Ready");
}

// -- Open --

void MainWindow::onOpenImage()
{
	auto path = QFileDialog::getOpenFileName(this, "Open Disk Image", "", "All Files (*)");
	if (path.isEmpty())
		return;
	loadDisk(path.toUtf8().constData());
}

void MainWindow::onOpenDevice()
{
	auto disks = enumerate_disks();
	if (disks.empty()) {
		QMessageBox::warning(this, "No Devices",
			"No disk devices found.\nUse Open Image to open a disk image file.");
		return;
	}

	QStringList labels;
	for (auto &d : disks)
		labels << QString("%1 - %2 (%3)")
			.arg(QString::fromStdString(d.path))
			.arg(QString::fromStdString(d.model))
			.arg(fmtSize(d.total_sectors * SECTOR_SIZE));

	bool ok;
	auto label = QInputDialog::getItem(this, "Open Device", "Select a disk:", labels, 0, false, &ok);
	if (!ok)
		return;

	int idx = labels.indexOf(label);
	loadDisk(disks[idx].path.c_str());
}

void MainWindow::loadDisk(const char *path)
{
	m_partitions.clear();
	m_model->removeRows(0, m_model->rowCount());

	try {
		m_disk = std::make_shared<DiskIo>(path);
	} catch (std::exception &e) {
		QMessageBox::critical(this, "Error", QString("Failed to open:\n%1").arg(e.what()));
		return;
	}

	auto parts = discover_partitions(m_disk);
	if (parts.empty()) {
		QMessageBox::information(this, "No Partitions", "No FATX partitions found on this disk.");
		return;
	}

	auto dirIcon = style()->standardIcon(QStyle::SP_DirIcon);

	for (auto &p : parts) {
		int idx = (int)m_partitions.size();
		std::unique_ptr<FatxFilesystem> fs;
		try {
			fs = std::make_unique<FatxFilesystem>(p.part);
		} catch (std::exception &e) {
			statusBar()->showMessage(QString("Skipping %1: %2")
				.arg(QString::fromStdString(p.name)).arg(e.what()));
			continue;
		}

		auto *nameItem = new QStandardItem(dirIcon, QString::fromStdString(p.name));
		nameItem->setEditable(false);
		nameItem->setData(true, RoleIsDir);
		nameItem->setData(fs->root_cluster(), RoleCluster);
		nameItem->setData(false, RoleLoaded);
		nameItem->setData(idx, RolePartIndex);

		auto *sizeItem = new QStandardItem();
		sizeItem->setEditable(false);
		auto *dateItem = new QStandardItem();
		dateItem->setEditable(false);

		nameItem->appendRow({new QStandardItem()});
		m_model->appendRow({nameItem, sizeItem, dateItem});

		m_partitions.push_back({p.name, std::move(p.part), std::move(fs)});
	}

	m_tree->expandAll();
	setWindowTitle(QString("Disk360 - %1").arg(path));

	QStringList partNames;
	for (auto &ps : m_partitions)
		partNames << QString::fromStdString(ps.name);

	m_detail->setText(QString("Disk: %1\nSize: %2\nPartitions: %3")
		.arg(path)
		.arg(fmtSize(m_disk->total_sectors() * SECTOR_SIZE))
		.arg(partNames.join(", ")));

	statusBar()->showMessage(QString("Opened %1 - %2 partition(s)")
		.arg(path).arg(m_partitions.size()));
}

// -- Helpers --

FatxFilesystem *MainWindow::getFs(const QModelIndex &index)
{
	auto *item = m_model->itemFromIndex(index);
	while (item) {
		QVariant v = item->data(RolePartIndex);
		if (v.isValid()) {
			int idx = v.toInt();
			if (idx >= 0 && idx < (int)m_partitions.size())
				return m_partitions[idx].fs.get();
		}
		item = item->parent();
	}
	return nullptr;
}

QStandardItem *MainWindow::getDirItem(const QModelIndex &index)
{
	auto *item = m_model->itemFromIndex(index.siblingAtColumn(0));
	if (!item)
		return nullptr;
	if (item->data(RoleIsDir).toBool())
		return item;
	return item->parent();
}

QString MainWindow::getItemPath(QStandardItem *item)
{
	QStringList parts;
	while (item) {
		auto *parent = item->parent();
		if (parent)
			parts.prepend(item->text());
		item = parent;
	}
	return parts.join("/");
}

void MainWindow::refreshDir(QStandardItem *dirItem)
{
	auto *fs = getFs(dirItem->index());
	if (!fs)
		return;
	int partIndex = dirItem->data(RolePartIndex).toInt();
	uint32_t cluster = dirItem->data(RoleCluster).toUInt();
	dirItem->removeRows(0, dirItem->rowCount());
	try {
		populateDir(dirItem, fs, partIndex);
		dirItem->setData(true, RoleLoaded);
	} catch (std::exception &e) {
		statusBar()->showMessage(QString("Error refreshing: %1").arg(e.what()));
	}
}

void MainWindow::populateDir(QStandardItem *parent, FatxFilesystem *fs, int partIndex)
{
	auto dirIcon = style()->standardIcon(QStyle::SP_DirIcon);
	auto fileIcon = style()->standardIcon(QStyle::SP_FileIcon);
	uint32_t cluster = parent->data(RoleCluster).toUInt();

	auto entries = fs->read_dir(cluster);
	for (auto &ent : entries) {
		auto *nameItem = new QStandardItem(
			ent.is_directory() ? dirIcon : fileIcon,
			QString::fromStdString(ent.name));
		nameItem->setEditable(false);
		nameItem->setData(ent.is_directory(), RoleIsDir);
		nameItem->setData(ent.first_cluster, RoleCluster);
		nameItem->setData(partIndex, RolePartIndex);
		nameItem->setData((qulonglong)ent.size, RoleFileSize);

		auto *sizeItem = new QStandardItem(
			ent.is_directory() ? "" : fmtSize(ent.size));
		sizeItem->setEditable(false);
		if (!ent.is_directory())
			sizeItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);

		auto *dateItem = new QStandardItem();
		dateItem->setEditable(false);

		if (ent.is_directory()) {
			nameItem->setData(false, RoleLoaded);
			nameItem->appendRow({new QStandardItem()});
		}

		parent->appendRow({nameItem, sizeItem, dateItem});
	}
}

// -- Events --

void MainWindow::onExpand(const QModelIndex &index)
{
	auto *item = m_model->itemFromIndex(index);
	if (!item || !item->data(RoleIsDir).toBool() || item->data(RoleLoaded).toBool())
		return;

	auto *fs = getFs(index);
	if (!fs)
		return;

	int partIndex = item->data(RolePartIndex).toInt();
	item->removeRows(0, item->rowCount());
	try {
		populateDir(item, fs, partIndex);
		item->setData(true, RoleLoaded);
	} catch (std::exception &e) {
		statusBar()->showMessage(QString("Error reading directory: %1").arg(e.what()));
	}
}

void MainWindow::onClick(const QModelIndex &index)
{
	auto *item = m_model->itemFromIndex(index.siblingAtColumn(0));
	if (!item)
		return;

	bool isDir = item->data(RoleIsDir).toBool();
	uint32_t cluster = item->data(RoleCluster).toUInt();
	QString name = item->text();

	if (isDir) {
		m_detail->setText(QString("Directory: %1\nCluster: %2")
			.arg(name).arg(cluster));
	} else {
		uint64_t size = item->data(RoleFileSize).toULongLong();
		m_detail->setText(QString("File: %1\nSize: %2 (%3 bytes)\nCluster: %4")
			.arg(name).arg(fmtSize(size)).arg(size).arg(cluster));
	}
}

void MainWindow::onContextMenu(const QPoint &pos)
{
	auto index = m_tree->indexAt(pos);
	if (!index.isValid())
		return;

	auto *item = m_model->itemFromIndex(index.siblingAtColumn(0));
	if (!item)
		return;

	bool isDir = item->data(RoleIsDir).toBool();

	QMenu menu(this);
	menu.addAction("Extract...", this, &MainWindow::extractSelected);
	menu.addSeparator();
	if (isDir) {
		menu.addAction("Import...", this, &MainWindow::importFiles);
		menu.addAction("New Directory...", this, &MainWindow::newDirectory);
		menu.addSeparator();
	}
	menu.addAction("Rename...", this, &MainWindow::renameSelected);
	menu.addAction("Delete", this, &MainWindow::deleteSelected);
	menu.exec(m_tree->viewport()->mapToGlobal(pos));
}

// -- Extract --

void MainWindow::extractDir(FatxFilesystem *fs, uint32_t cluster, const QString &dest)
{
	stdfs::create_directories(dest.toStdString());
	auto entries = fs->read_dir(cluster);
	for (auto &ent : entries) {
		QString child = dest + "/" + QString::fromStdString(ent.name);
		if (ent.is_directory()) {
			extractDir(fs, ent.first_cluster, child);
		} else {
			auto data = fs->read_file(ent);
			std::ofstream f(child.toStdString(), std::ios::binary);
			f.write((const char *)data.data(), data.size());
		}
	}
}

void MainWindow::extractSelected()
{
	auto index = m_tree->currentIndex();
	if (!index.isValid())
		return;

	auto *item = m_model->itemFromIndex(index.siblingAtColumn(0));
	if (!item)
		return;

	auto *fs = getFs(index);
	if (!fs)
		return;

	bool isDir = item->data(RoleIsDir).toBool();
	uint32_t cluster = item->data(RoleCluster).toUInt();
	QString name = item->text();

	if (isDir) {
		auto outDir = QFileDialog::getExistingDirectory(this, "Extract Directory To");
		if (outDir.isEmpty())
			return;

		QString dest = outDir + "/" + name;
		try {
			extractDir(fs, cluster, dest);
			statusBar()->showMessage(QString("Extracted to %1").arg(dest));
		} catch (std::exception &e) {
			QMessageBox::critical(this, "Error", QString("Extract failed:\n%1").arg(e.what()));
		}
	} else {
		auto outPath = QFileDialog::getSaveFileName(this, "Save File As", name);
		if (outPath.isEmpty())
			return;

		try {
			// Find the entry to get the size
			auto *parent = item->parent();
			if (!parent)
				return;
			uint32_t parentCluster = parent->data(RoleCluster).toUInt();
			std::vector<FatxFileInfo> entries;
			auto *ent = fs->find_entry(parentCluster, name.toUtf8().constData(), entries);
			if (!ent)
				throw std::runtime_error("File not found");

			auto data = fs->read_file(*ent);
			std::ofstream f(outPath.toStdString(), std::ios::binary);
			f.write((const char *)data.data(), data.size());
			statusBar()->showMessage(QString("Extracted %1 bytes to %2")
				.arg(data.size()).arg(outPath));
		} catch (std::exception &e) {
			QMessageBox::critical(this, "Error", QString("Extract failed:\n%1").arg(e.what()));
		}
	}
}

// -- Import --

void MainWindow::importPath(FatxFilesystem *fs, uint32_t cluster, const QString &hostPath)
{
	stdfs::path hp(hostPath.toStdString());
	std::string name = hp.filename().string();

	if (stdfs::is_directory(hp)) {
		fs->mkdir(cluster, name.c_str());
		std::vector<FatxFileInfo> entries;
		auto *ent = fs->find_entry(cluster, name.c_str(), entries);
		if (!ent)
			return;
		for (auto &entry : stdfs::directory_iterator(hp)) {
			importPath(fs, ent->first_cluster, QString::fromStdString(entry.path().string()));
		}
	} else {
		std::ifstream f(hp, std::ios::binary | std::ios::ate);
		if (!f)
			return;
		size_t size = f.tellg();
		f.seekg(0);
		std::vector<uint8_t> buf(size);
		f.read((char *)buf.data(), size);
		fs->import_file(cluster, name.c_str(), buf.data(), buf.size(), true);
	}
}

void MainWindow::importFiles()
{
	auto index = m_tree->currentIndex();
	if (!index.isValid())
		return;

	auto *dirItem = getDirItem(index);
	if (!dirItem)
		return;

	auto *fs = getFs(index);
	if (!fs)
		return;

	auto paths = QFileDialog::getOpenFileNames(this, "Import Files");
	if (paths.isEmpty())
		return;

	uint32_t cluster = dirItem->data(RoleCluster).toUInt();

	try {
		for (auto &path : paths)
			importPath(fs, cluster, path);
		fs->flush();
		refreshDir(dirItem);
		statusBar()->showMessage(QString("Imported %1 item(s)").arg(paths.size()));
	} catch (std::exception &e) {
		refreshDir(dirItem);
		QMessageBox::critical(this, "Error", QString("Import failed:\n%1").arg(e.what()));
	}
}

// -- New Directory --

void MainWindow::newDirectory()
{
	auto index = m_tree->currentIndex();
	if (!index.isValid())
		return;

	auto *dirItem = getDirItem(index);
	if (!dirItem)
		return;

	auto *fs = getFs(index);
	if (!fs)
		return;

	bool ok;
	auto name = QInputDialog::getText(this, "New Directory", "Name:", QLineEdit::Normal, "", &ok);
	if (!ok || name.isEmpty())
		return;

	uint32_t cluster = dirItem->data(RoleCluster).toUInt();
	try {
		fs->mkdir(cluster, name.toUtf8().constData());
		fs->flush();
		refreshDir(dirItem);
		statusBar()->showMessage(QString("Created directory: %1").arg(name));
	} catch (std::exception &e) {
		QMessageBox::critical(this, "Error", QString("Failed to create directory:\n%1").arg(e.what()));
	}
}

// -- Rename --

void MainWindow::renameSelected()
{
	auto index = m_tree->currentIndex();
	if (!index.isValid())
		return;

	auto *item = m_model->itemFromIndex(index.siblingAtColumn(0));
	if (!item)
		return;

	auto *parent = item->parent();
	if (!parent)
		return;

	auto *fs = getFs(index);
	if (!fs)
		return;

	QString oldName = item->text();
	bool ok;
	auto newName = QInputDialog::getText(this, "Rename", "New name:",
		QLineEdit::Normal, oldName, &ok);
	if (!ok || newName.isEmpty() || newName == oldName)
		return;

	uint32_t parentCluster = parent->data(RoleCluster).toUInt();
	try {
		fs->rename(parentCluster, oldName.toUtf8().constData(), newName.toUtf8().constData());
		refreshDir(parent);
		statusBar()->showMessage(QString("Renamed: %1 -> %2").arg(oldName, newName));
	} catch (std::exception &e) {
		QMessageBox::critical(this, "Error", QString("Rename failed:\n%1").arg(e.what()));
	}
}

// -- Delete --

void MainWindow::deleteSelected()
{
	auto index = m_tree->currentIndex();
	if (!index.isValid())
		return;

	auto *item = m_model->itemFromIndex(index.siblingAtColumn(0));
	if (!item)
		return;

	auto *parent = item->parent();
	if (!parent)
		return;

	auto *fs = getFs(index);
	if (!fs)
		return;

	bool isDir = item->data(RoleIsDir).toBool();
	QString name = item->text();

	QString msg = QString("Delete %1 \"%2\"?")
		.arg(isDir ? "directory" : "file", name);
	if (isDir)
		msg += "\n\nThis will delete all contents recursively.";

	if (QMessageBox::question(this, "Confirm Delete", msg) != QMessageBox::Yes)
		return;

	uint32_t parentCluster = parent->data(RoleCluster).toUInt();
	try {
		fs->remove(parentCluster, name.toUtf8().constData(), isDir);
		refreshDir(parent);
		statusBar()->showMessage(QString("Deleted: %1").arg(name));
	} catch (std::exception &e) {
		QMessageBox::critical(this, "Error", QString("Delete failed:\n%1").arg(e.what()));
	}
}
