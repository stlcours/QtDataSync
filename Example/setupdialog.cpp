#include "setupdialog.h"
#include "ui_setupdialog.h"

#include <QSettings>
#include <wsauthenticator.h>

bool SetupDialog::setup(QWidget *parent)
{
	SetupDialog dialog(parent);

	QSettings settings;
	dialog.ui->storageDirComboBox->addItems(settings.value(QStringLiteral("localDirs"), QStringList("./qtdatasync_localstore")).toStringList());

	if(dialog.exec() == QDialog::Accepted) {
		QtDataSync::Setup()
				.setLocalDir(dialog.ui->storageDirComboBox->currentText())
				.create();

		auto auth = QtDataSync::Setup::authenticatorForSetup<QtDataSync::WsAuthenticator>(&dialog);
		auth->setRemoteUrl(dialog.ui->remoteURLLineEdit->text());
		auth->reconnect();

		QStringList items;
		for(auto i = 0; i < dialog.ui->storageDirComboBox->count(); i++)
			items.append(dialog.ui->storageDirComboBox->itemText(i));
		if(!items.contains(dialog.ui->storageDirComboBox->currentText()))
			items.append(dialog.ui->storageDirComboBox->currentText());
		settings.setValue(QStringLiteral("localDirs"), items);

		return true;
	} else
		return false;
}

SetupDialog::SetupDialog(QWidget *parent) :
	QDialog(parent),
	ui(new Ui::SetupDialog)
{
	ui->setupUi(this);
}

SetupDialog::~SetupDialog()
{
	delete ui;
}