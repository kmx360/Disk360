// SPDX-License-Identifier: GPL-2.0-only

#pragma once

#include "diskio.h"
#include "fatx.h"

#include <QMainWindow>
#include <QStandardItemModel>
#include <QTreeView>
#include <QLabel>

#include <memory>
#include <string>
#include <vector>

enum ItemRole {
	RoleFileSize = Qt::UserRole + 1,
	RoleCluster,
	RoleIsDir,
	RoleLoaded,
	RolePartIndex,
};

class MainWindow : public QMainWindow {
	Q_OBJECT
public:
	MainWindow(QWidget *parent = nullptr);
	void loadDisk(const char *path);

private slots:
	void onOpenImage();
	void onOpenDevice();
	void onExpand(const QModelIndex &index);
	void onClick(const QModelIndex &index);
	void onContextMenu(const QPoint &pos);

	void extractSelected();
	void importFiles();
	void newDirectory();
	void renameSelected();
	void deleteSelected();

private:
	struct PartitionState {
		std::string name;
		PartitionIoPtr part;
		std::unique_ptr<FatxFilesystem> fs;
	};

	QStandardItemModel *m_model;
	QTreeView *m_tree;
	QLabel *m_detail;

	DiskIoPtr m_disk;
	std::vector<PartitionState> m_partitions;

	FatxFilesystem *getFs(const QModelIndex &index);
	QStandardItem *getDirItem(const QModelIndex &index);
	QString getItemPath(QStandardItem *item);
	void refreshDir(QStandardItem *dirItem);
	void populateDir(QStandardItem *parent, FatxFilesystem *fs, int partIndex);
	void extractDir(FatxFilesystem *fs, uint32_t cluster, const QString &dest);
	void importPath(FatxFilesystem *fs, uint32_t cluster, const QString &hostPath);
};
