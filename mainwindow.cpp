#include <QDir>
#include <QUrl>
#include <QFont>
#include <QMenu>
#include <QDebug>
#include <QScreen>
#include <QByteArray>
#include <QMessageBox>
#include <QJsonObject>
#include <QJsonDocument>

#include "messageevent.h"
#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "gamehelper.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    connect(ui->quitBtn, &QPushButton::clicked, [&]() {
        quitClear();
        qApp->quit();
    });

    connect(ui->minBtn, &QPushButton::clicked, [&]() {
        this->hide();
    });

    setWindowFlags(Qt::WindowMinimizeButtonHint | Qt::FramelessWindowHint);

    bellForMessage = new QSound(":/Resources/message.wav");

    appRedIcon = QIcon(":/Resources/red-icon.icns");
    appBlueIcon = QIcon(":/Resources/blue-icon.icns");

    stateOnPixmap = QPixmap::fromImage(QImage(":/Resources/light_on_16.png"));
    stateOffPixmap = QPixmap::fromImage(QImage(":/Resources/light_off_16.png"));

    ui->states->setPixmap(stateOffPixmap);

    gotAppIconPosition = false;
    connectedService = false;
    trayIconSwitched = false;

    settings = new QSettings(QSettings::IniFormat, QSettings::UserScope, QApplication::organizationName(), QApplication::applicationName());
}

MainWindow::~MainWindow()
{
    quitClear();
}

void MainWindow::quitClear()
{
    if (webSocket != NULL) {
        webSocket->close();
        delete webSocket;
    }

    if (webSocketPingTimer.isActive()) {
        webSocketPingTimer.stop();
    }

    if (messageTipTimer.isActive()) {
        messageTipTimer.stop();
    }

    delete ui;
    delete bellForMessage;
    delete trayIcon;
    delete settings;
}

void MainWindow::start()
{
    trayIcon = new QSystemTrayIcon(this);

    connect(trayIcon, &QSystemTrayIcon::messageClicked, this, &MainWindow::messageClicked);
    connect(trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::iconActivated);

    trayIcon->setIcon(appBlueIcon);
    trayIcon->show();

    messageTipTimer.setInterval(500);
    connect(&messageTipTimer, &QTimer::timeout, this, &MainWindow::onMessageTipTimerTimeout);

    connectServer();
}

void MainWindow::messageClicked()
{
    qDebug() << "message notice clicked";

    resetTrayIcon();

    if (false == gotAppIconPosition) {
        if (qApp->screens().size() == 1) {
            this->move(trayIcon->geometry().x(), 22);
        } else {
            this->move(qApp->primaryScreen()->size().width() - 550, 22);
        }
    }

    this->show();
    this->raise();
}

void MainWindow::iconActivated(QSystemTrayIcon::ActivationReason reason)
{
    Q_UNUSED(reason);

    resetTrayIcon();

    QPoint cursorPoint = QCursor::pos();

    if (gotAppIconPosition
            && this->isVisible()
            && this->isActiveWindow() == false) {
        this->move(cursorPoint.x() - 16, cursorPoint.y() - 11);
        this->raise();
        return;
    }

    if (this->isVisible()) {
        this->hide();
        return;
    }

    gotAppIconPosition = true;

    this->move(cursorPoint.x() - 16, cursorPoint.y() - 11);
    this->show();
    this->raise();
}

void MainWindow::updateMessage(QString message)
{
    qDebug() << "receive http server message:" << message;
    bellForMessage->play();

    QListWidgetItem *item = new QListWidgetItem();
    item->setText(message);
    item->setSizeHint(QSize(470, 25));
    item->setToolTip(message);
    item->setFont(QFont("Monaco", 14));

    ui->listWidget->insertItem(0, item);
    ui->listWidget->setCurrentRow(0);
    ui->infoLabel->setText(QDateTime::currentDateTime().toString("截止:yyyy-MM-dd hh:mm:ss"));

    trayIcon->showMessage(QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss"),
                          message,
                          appBlueIcon,
                          300000);
    trayIcon->setToolTip(message);

    messageTipTimer.start();
}

bool MainWindow::event(QEvent *event)
{
    if (event->type() == MessageEvent::MessageEventType) {
        MessageEvent *msgEvent = static_cast<MessageEvent *>(event);
        updateMessage(msgEvent->getMessage());
        return true;
    }

    return QMainWindow::event(event);
}

void MainWindow::on_listWidget_customContextMenuRequested(const QPoint &pos)
{
    QListWidgetItem* curItem = ui->listWidget->itemAt(pos);
    if (curItem == NULL)  {
        return;
    }

    QAction *deleteMenu = new QAction(tr("delete"), this);
    QAction *clearMenu = new QAction(tr("clear"), this);

    QMenu* popMenu = new QMenu(this);
    popMenu->addAction(deleteMenu);
    popMenu->addAction(clearMenu);
    connect(deleteMenu, &QAction::triggered, this, &MainWindow::deleteMenuSelected);
    connect(clearMenu, &QAction::triggered, this, &MainWindow::clearMenuSelected);
    popMenu->exec(QCursor::pos());
}

void MainWindow::deleteMenuSelected()
{
    QListWidgetItem *item = ui->listWidget->currentItem();
    if (item == NULL) {
        return;
    }

    ui->listWidget->removeItemWidget(item);
    delete item;

    if (ui->listWidget->count() == 0) {
        ui->infoLabel->setText("");
    }
}

void MainWindow::clearMenuSelected()
{
    int result = QMessageBox::warning(this,
                                      "Warning",
                                      "Are you sure to clear logs ?",
                                      QMessageBox::Yes | QMessageBox::No,
                                      QMessageBox::No);

    if (result != QMessageBox::Yes) {
        return;
    }

    QListWidgetItem *item = ui->listWidget->currentItem();
    if (item == NULL) {
        return;
    }

    ui->listWidget->clear();
    ui->infoLabel->setText("");
}

void MainWindow::connectServer()
{
    QString macAddress = GameHelper::getInstance().getMacAddress();
    qDebug() << "address:" << macAddress;

    if (macAddress.length() == 0) {
        return;
    }

    QString origin;
    origin.append("rumbladeApp:").append(macAddress);

    webUrl = QUrl(settings->value("url").toString().append("&uuid=ctips:").append(macAddress));

    qDebug() << "start connect websocket " << webUrl.toString();

    webSocket = new QWebSocket(origin);
    connect(webSocket, &QWebSocket::connected, this, &MainWindow::onWebSocketConnected);
    connect(webSocket, &QWebSocket::disconnected, this, &MainWindow::onWebSocketDisconnected);
    connect(webSocket, &QWebSocket::textMessageReceived, this, &MainWindow::onWebSocketMessageReceived);
    connect(webSocket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this, &MainWindow::onWebSocketError);
    connect(webSocket, &QWebSocket::stateChanged, this, &MainWindow::onWebSocketStateChanged);
    connect(webSocket, &QWebSocket::pong, this, &MainWindow::onWebSocketPong);

    webSocket->open(webUrl);

    webSocketPingTimer.setInterval(15000);
    webSocketPingTimer.start();
    connect(&webSocketPingTimer, &QTimer::timeout, this, &MainWindow::onWebSocketTimerTimeout);
}

void MainWindow::onWebSocketConnected()
{
    qDebug() << "webSocket connected";

    ui->states->setPixmap(stateOnPixmap);

    connectedService = true;
    webSocketLastPongTime = QDateTime::currentDateTime();
}

void MainWindow::onWebSocketDisconnected()
{
    qDebug() << "webSocket disconnected";

    ui->states->setPixmap(stateOffPixmap);

    connectedService = false;
}

void MainWindow::onWebSocketMessageReceived(const QString &message)
{
    qDebug() << "webSocket message received:" << message;

    QJsonParseError jsonErr;
    QJsonDocument responseDoc = QJsonDocument::fromJson(message.toUtf8(), &jsonErr);
    if (jsonErr.error != QJsonParseError::NoError
            || responseDoc.isEmpty()
            || responseDoc.isNull()) {
        qDebug() << "parse webSocket message failed:" << message;
        return;
    }

    QJsonObject responseObj = responseDoc.object();
    if (responseObj.contains("noticeData")) {
        QJsonObject noticeData = responseObj.value("noticeData").toObject();
        updateMessage(noticeData.value("message").toString());
        return;
    }
}

void MainWindow::onWebSocketPong(quint64 elapsedTime, const QByteArray &payload)
{
    qDebug() << "websocket got pong:" << elapsedTime << QString(payload);

    webSocketLastPongTime = QDateTime::currentDateTime();
}

void MainWindow::onWebSocketClose()
{
    qDebug() << "to close websocket";
    webSocket->close();
}

void MainWindow::onWebSocketError(QAbstractSocket::SocketError error)
{
    qDebug() << "websocket error:" << error;
}

void MainWindow::onWebSocketStateChanged(QAbstractSocket::SocketState state)
{
    qDebug() << "websocket state changed:" << state;
}

void MainWindow::onWebSocketTimerTimeout()
{
    qDebug() << "websocket timer timeout";

    if (connectedService && webSocketLastPongTime.addSecs(30) > QDateTime::currentDateTime()) {
        webSocket->ping(QByteArray().append("PING"));
    } else {
        qDebug() << "websocket reconnect for timeout";
        webSocket->close();
        webSocket->open(webUrl);
    }
}

void MainWindow::onMessageTipTimerTimeout()
{
    if (trayIconSwitched == false) {
        trayIconSwitched = true;
        trayIcon->setIcon(appRedIcon);
    } else {
        trayIconSwitched = false;
        trayIcon->setIcon(appBlueIcon);
    }
}

void MainWindow::resetTrayIcon()
{
    trayIconSwitched = false;

    trayIcon->setIcon(appBlueIcon);
    trayIcon->setToolTip("");

    messageTipTimer.stop();
}