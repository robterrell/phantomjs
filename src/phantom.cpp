/*
  This file is part of the PhantomJS project from Ofi Labs.

  Copyright (C) 2011 Ariya Hidayat <ariya.hidayat@gmail.com>
  Copyright (C) 2010 Ariya Hidayat <ariya.hidayat@gmail.com>

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the <organization> nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
  (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "phantom.h"

#include <iostream>

#include <QtGui>
#include <QtWebKit>
#include <QDir>
#include <QFileInfo>
#include <QFile>

#include "consts.h"
#include "utils.h"
#include "webpage.h"

// public:
Phantom::Phantom(QObject *parent)
    : QObject(parent)
    , m_terminated(false)
    , m_returnValue(0)
    , m_converter(0)
    , m_netAccessMan(0)
{
    m_page = new WebPage(this);
    m_pages.append(m_page);

    QString proxyHost;
    QString cookieFile;
    int proxyPort = 1080;
    bool autoLoadImages = true;
    bool pluginsEnabled = false;
    bool diskCacheEnabled = false;
    bool ignoreSslErrors = false;
    bool localAccessRemote = false;

    // second argument: script name
    QStringList args = QApplication::arguments();

    // Skip the first argument, i.e. the application executable (phantomjs).
    args.removeFirst();

    // Handle all command-line options.
    QStringListIterator argIterator(args);
    while (argIterator.hasNext()) {
        const QString &arg = argIterator.next();
        if (arg == "--version") {
            m_terminated = true;
            std::cout << PHANTOMJS_VERSION_STRING << " (development)" << std::endl;
            return;
        }
        if (arg == "--load-images=yes") {
            autoLoadImages = true;
            continue;
        }
        if (arg == "--load-images=no") {
            autoLoadImages = false;
            continue;
        }
        if (arg == "--load-plugins=yes") {
            pluginsEnabled = true;
            continue;
        }
        if (arg == "--load-plugins=no") {
            pluginsEnabled = false;
            continue;
        }
        if (arg == "--disk-cache=yes") {
            diskCacheEnabled = true;
            continue;
        }
        if (arg == "--disk-cache=no") {
            diskCacheEnabled = false;
            continue;
        }
        if (arg == "--ignore-ssl-errors=yes") {
            ignoreSslErrors = true;
            continue;
        }
        if (arg == "--ignore-ssl-errors=no") {
            ignoreSslErrors = false;
            continue;
        }
        if (arg == "--local-access-remote=no") {
            localAccessRemote = false;
            continue;
        }
        if (arg == "--local-access-remote=yes") {
            localAccessRemote = true;
            continue;
        }
        if (arg.startsWith("--proxy=")) {
            proxyHost = arg.mid(8).trimmed();
            if (proxyHost.lastIndexOf(':') > 0) {
                bool ok = true;
                int port = proxyHost.mid(proxyHost.lastIndexOf(':') + 1).toInt(&ok);
                if (ok) {
                    proxyHost = proxyHost.left(proxyHost.lastIndexOf(':')).trimmed();
                    proxyPort = port;
                }
            }
            continue;
        }
        if (arg.startsWith("--cookies=")) {
            cookieFile = arg.mid(10).trimmed();
            continue;
        }
        if (arg.startsWith("--")) {
            std::cerr << "Unknown option '" << qPrintable(arg) << "'" << std::endl;
            m_terminated = true;
            return;
        } else {
            m_scriptFile = arg;
            break;
        }
    }

    if (m_scriptFile.isEmpty()) {
        Utils::showUsage();
        return;
    }

    if (proxyHost.isEmpty()) {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
    } else {
        QNetworkProxy proxy(QNetworkProxy::HttpProxy, proxyHost, proxyPort);
        QNetworkProxy::setApplicationProxy(proxy);
    }

    // The remaining arguments are available for the script.
    while (argIterator.hasNext()) {
        const QString &arg = argIterator.next();
        m_args += arg;
    }

    // Provide WebPage with a non-standard Network Access Manager
    m_netAccessMan = new NetworkAccessManager(this, diskCacheEnabled, cookieFile, ignoreSslErrors);
    m_page->setNetworkAccessManager(m_netAccessMan);

    connect(m_page, SIGNAL(javaScriptConsoleMessageSent(QString, int, QString)),
            SLOT(printConsoleMessage(QString, int, QString)));

    m_defaultPageSettings[PAGE_SETTINGS_LOAD_IMAGES] = QVariant::fromValue(autoLoadImages);
    m_defaultPageSettings[PAGE_SETTINGS_LOAD_PLUGINS] = QVariant::fromValue(pluginsEnabled);
    m_defaultPageSettings[PAGE_SETTINGS_USER_AGENT] = QVariant::fromValue(m_page->userAgent());
    m_defaultPageSettings[PAGE_SETTINGS_LOCAL_ACCESS_REMOTE] = QVariant::fromValue(localAccessRemote);
    m_page->applySettings(m_defaultPageSettings);

    setLibraryPath(QFileInfo(m_scriptFile).dir().absolutePath());

    m_page->mainFrame()->addToJavaScriptWindowObject("phantom", this);
    m_page->mainFrame()->addToJavaScriptWindowObject("fs", &m_filesystem);

    QFile file(":/bootstrap.js");
    if (!file.open(QFile::ReadOnly)) {
        std::cerr << "Can not bootstrap!" << std::endl;
        exit(1);
    }
    QString bootstrapper = QString::fromUtf8(file.readAll());
    file.close();
    if (bootstrapper.isEmpty()) {
        std::cerr << "Can not bootstrap!" << std::endl;
        exit(1);
    }
    m_page->mainFrame()->evaluateJavaScript(bootstrapper);
}

QStringList Phantom::args() const
{
    return m_args;
}

QVariantMap Phantom::defaultPageSettings() const
{
    return m_defaultPageSettings;
}

bool Phantom::execute()
{
    if (m_terminated)
        return false;

    if (m_scriptFile.isEmpty())
        return false;

    if (!Utils::injectJsInFrame(m_scriptFile, QDir::currentPath(), m_page->mainFrame(), true)) {
        m_returnValue = -1;
        return false;
    }

    return !m_terminated;
}

int Phantom::returnValue() const
{
    return m_returnValue;
}

QString Phantom::libraryPath() const
{
   return m_page->libraryPath();
}

void Phantom::setLibraryPath(const QString &libraryPath)
{
   m_page->setLibraryPath(libraryPath);
}

QString Phantom::scriptName() const
{
    return QFileInfo(m_scriptFile).fileName();
}

QVariantMap Phantom::version() const
{
    QVariantMap result;
    result["major"] = PHANTOMJS_VERSION_MAJOR;
    result["minor"] = PHANTOMJS_VERSION_MINOR;
    result["patch"] = PHANTOMJS_VERSION_PATCH;
    return result;
}

// public slots:
QObject *Phantom::createWebPage()
{
    WebPage *page = new WebPage(this);
    m_pages.append(page);
    page->applySettings(m_defaultPageSettings);
    page->setNetworkAccessManager(m_netAccessMan);
    page->setLibraryPath(QFileInfo(m_scriptFile).dir().absolutePath());
    return page;
}

bool Phantom::injectJs(const QString &jsFilePath) {
    return Utils::injectJsInFrame(jsFilePath, libraryPath(), m_page->mainFrame());
}

void Phantom::exit(int code)
{
    m_terminated = true;
    m_returnValue = code;
    qDeleteAll(m_pages);
    m_pages.clear();
    m_page = 0;
    QApplication::instance()->exit(code);
}

void Phantom::_destroy(QObject *page) {
    m_pages.removeOne((WebPage*)page);
    delete page;
}

// private slots:
void Phantom::printConsoleMessage(const QString &message, int lineNumber, const QString &source)
{
    QString msg = message;
    if (!source.isEmpty())
        msg = source + ":" + QString::number(lineNumber) + " " + msg;
    std::cout << qPrintable(msg) << std::endl;
}


QString Phantom::getStdinLine()
{
    std::string instr;
    while(true) {
        int ch = std::cin.get();
        if (ch == '\n' || ch == EOF || ch == '\r' )
            break;
        instr.push_back(static_cast<char>(ch));
    }
    return QString(instr.c_str());
}

