// SPDX-License-Identifier: GPL-2.0-only

#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
	QApplication app(argc, argv);
	MainWindow w;
	if (argc > 1)
		w.loadDisk(argv[1]);
	w.show();
	return app.exec();
}
