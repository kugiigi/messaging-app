/*
 * Copyright (C) 2012 Canonical, Ltd.
 *
 * This file is part of messaging-app.
 *
 * messaging-app is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * messaging-app is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "messagingapplication.h"

#include <QDir>
#include <QUrl>
#include <QUrlQuery>
#include <QDebug>
#include <QStringList>
#include <QQuickItem>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickView>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusConnectionInterface>
#include <QLibrary>
#include "config.h"
#include <QQmlEngine>
#include <QMimeDatabase>
#include <QVersitReader>

using namespace QtVersit;
#define Pair QPair<QString,QString>

static void printUsage(const QStringList& arguments)
{
    qDebug() << "usage:"
             << arguments.at(0).toUtf8().constData()
             << "[message:///PHONE_NUMBER]"
             << "[--fullscreen]"
             << "[--help]"
             << "[-testability]";
}

//this is necessary to work on desktop
//On desktop use: export MESSAGING_APP_ICON_THEME=ubuntu-mobile
static void installIconPath()
{
    QByteArray iconTheme = qgetenv("MESSAGING_APP_ICON_THEME");
    if (!iconTheme.isEmpty()) {
        QIcon::setThemeName(iconTheme);
    }
}

MessagingApplication::MessagingApplication(int &argc, char **argv)
    : QGuiApplication(argc, argv), m_view(0), m_applicationIsReady(false)
{
    setApplicationName("MessagingApp");
}

bool MessagingApplication::setup()
{
    installIconPath();
    static QList<QString> validSchemes;
    bool fullScreen = false;

    if (validSchemes.isEmpty()) {
        validSchemes << "message";
    }

    QStringList arguments = this->arguments();

    if (arguments.contains("--help")) {
        printUsage(arguments);
        return false;
    }

    if (arguments.contains("--fullscreen")) {
        arguments.removeAll("--fullscreen");
        fullScreen = true;
    }

    // The testability driver is only loaded by QApplication but not by QGuiApplication.
    // However, QApplication depends on QWidget which would add some unneeded overhead => Let's load the testability driver on our own.
    if (arguments.contains("-testability") || qgetenv("QT_LOAD_TESTABILITY") == "1") {
        arguments.removeAll("-testability");
        QLibrary testLib(QLatin1String("qttestability"));
        if (testLib.load()) {
            typedef void (*TasInitialize)(void);
            TasInitialize initFunction = (TasInitialize)testLib.resolve("qt_testability_init");
            if (initFunction) {
                initFunction();
            } else {
                qCritical("Library qttestability resolve failed!");
            }
        } else {
            qCritical("Library qttestability load failed!");
        }
    }

    /* Ubuntu APP Manager gathers info on the list of running applications from the .desktop
       file specified on the command line with the desktop_file_hint switch, and will also pass a stage hint
       So app will be launched like this:

       /usr/bin/messaging-app --desktop_file_hint=/usr/share/applications/messaging-app.desktop
                          --stage_hint=main_stage

       So remove whatever --arg still there before continue parsing
    */
    for (int i = arguments.count() - 1; i >=0; --i) {
        if (arguments[i].startsWith("--")) {
            arguments.removeAt(i);
        }
    }

    if (arguments.size() == 2) {
        QUrl uri(arguments.at(1));
        if (validSchemes.contains(uri.scheme())) {
            m_arg = arguments.at(1);
        }
    }

    m_view = new QQuickView();
    QObject::connect(m_view, SIGNAL(statusChanged(QQuickView::Status)), this, SLOT(onViewStatusChanged(QQuickView::Status)));
    QObject::connect(m_view->engine(), SIGNAL(quit()), SLOT(quit()));
    m_view->setResizeMode(QQuickView::SizeRootObjectToView);
    m_view->setTitle("Messaging");
    m_view->rootContext()->setContextProperty("application", this);
    m_view->rootContext()->setContextProperty("i18nDirectory", I18N_DIRECTORY);
    m_view->engine()->setBaseUrl(QUrl::fromLocalFile(messagingAppDirectory()));

    // check if there is a contacts backend override
    QString contactsBackend = qgetenv("QTCONTACTS_MANAGER_OVERRIDE");
    if (!contactsBackend.isEmpty()) {
        qDebug() << "Overriding the contacts backend, using:" << contactsBackend;
        m_view->rootContext()->setContextProperty("QTCONTACTS_MANAGER_OVERRIDE", contactsBackend);
    }

    QString pluginPath = ubuntuPhonePluginPath();
    if (!pluginPath.isNull()) {
        m_view->engine()->addImportPath(pluginPath);
    }

    m_view->setSource(QUrl::fromLocalFile("messaging-app.qml"));
    if (fullScreen) {
        m_view->showFullScreen();
    } else {
        m_view->show();
    }

    return true;
}

MessagingApplication::~MessagingApplication()
{
    if (m_view) {
        delete m_view;
    }
}

void MessagingApplication::onViewStatusChanged(QQuickView::Status status)
{
    if (status != QQuickView::Ready) {
        return;
    }
    onApplicationReady();
}

void MessagingApplication::onApplicationReady()
{
    m_applicationIsReady = true;
    parseArgument(m_arg);
    m_arg.clear();
}

void MessagingApplication::parseArgument(const QString &arg)
{
    if (arg.isEmpty()) {
        return;
    }

    QString text;
    QUrl url(arg);
    QString scheme = url.scheme();
    // Remove the first "/"
    QString value = url.path().right(url.path().length() -1);
    QUrlQuery query(url);
    Q_FOREACH(const Pair &item, query.queryItems()) {
        if (item.first == "text") {
            text = item.second;
            break;
        }
    }

    QQuickItem *mainView = m_view->rootObject();
    if (!mainView) {
        return;
    }

    if (scheme == "message") {
        if (!value.isEmpty()) {
            QMetaObject::invokeMethod(mainView, "startChat", Q_ARG(QVariant, value), Q_ARG(QVariant, text));
        } else {
            QMetaObject::invokeMethod(mainView, "startNewMessage");
        }
    }
}

void MessagingApplication::activateWindow()
{
    if (m_view) {
        m_view->raise();
        m_view->requestActivate();
    }
}

QString MessagingApplication::readTextFile(const QString &fileName) {
    QString text;
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    text = QString(file.readAll());
    file.close();
    return text;
}

QString MessagingApplication::fileMimeType(const QString &fileName) {
    QMimeDatabase db;
    QMimeType type = db.mimeTypeForFile(fileName);
    return type.name();
}

QString MessagingApplication::contactNameFromVCard(const QString &fileName) {
    QFile file(fileName);
    QString formattedName, structuredName, nickname;
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    QVersitReader reader(file.readAll());
    reader.startReading();
    reader.waitForFinished();
    if (reader.results().count() > 0) {
        // read only the first contact
        QVersitDocument firstVcard = reader.results()[0];
        Q_FOREACH(const QVersitProperty & prop, firstVcard.properties()) {
            if (prop.name() == "N") {
                structuredName = prop.value();
            } else if (prop.name() == "FN") {
                formattedName = prop.value();
            } else if (prop.name() == "NICKNAME") {
                nickname = prop.value();
            }
        }
        if (!formattedName.isEmpty()) {
            return formattedName;
        } else if (!structuredName.isEmpty()) {
            return structuredName.split(";")[1];
        } else if (!nickname.isEmpty()) {
            return nickname;
        }
    }
    return QString();
}

