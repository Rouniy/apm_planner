#include "configuration.h"
#include "AboutDialog.h"
#include "ui_AboutDialog.h"

#define define2string_p(x) #x
#define define2string(x) define2string_p(x)

AboutDialog::AboutDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AboutDialog)
{
    ui->setupUi(this);
    connect(ui->okButton, SIGNAL(clicked()), this, SLOT(accept()));

    ui->appnameLabel->setText(QGC_APPLICATION_NAME);
    ui->versionLabel->setText(tr("Version %1 (%2)")
                              .arg(QGC_APPLICATION_VERSION)
                              .arg(define2string(GIT_COMMIT)));
    ui->linkLabel->setOpenExternalLinks(true);
}

AboutDialog::~AboutDialog()
{
    delete ui;
}
