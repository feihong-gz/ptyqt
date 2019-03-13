#include "unixptyprocess.h"
#include <QStandardPaths>

#include <termios.h>
#include <errno.h>
#include <utmpx.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <QFileInfo>
#include <QCoreApplication>

UnixPtyProcess::UnixPtyProcess()
    : IPtyProcess()
    , m_readMasterNotify(0)
    , m_writeMasterNotify(0)
    , m_readSlaveNotify(0)
    , m_writeSlaveNotify(0)
{
    m_shellProcess.setWorkingDirectory(QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
}

UnixPtyProcess::~UnixPtyProcess()
{
    kill();
}

bool UnixPtyProcess::startProcess(const QString &shellPath, QStringList environment, qint16 cols, qint16 rows)
{
    if (!isAvailable())
    {
        m_lastError = QString("WinPty Error: winpty-agent.exe or winpty.dll not found!");
        return false;
    }

    if (m_shellProcess.state() == QProcess::Running)
        return false;

    QFileInfo fi(shellPath);
    if (fi.isRelative() || !QFile::exists(shellPath))
    {
        //todo add auto-find executable in PATH env var
        m_lastError = QString("WinPty Error: shell file path must be absolute");
        return false;
    }

    m_shellPath = shellPath;
    m_size = QPair<qint16, qint16>(cols, rows);

    //m_shellProcess.startCli(shellPath, QStringList(), size, environment);

    int rc = 0;

    //open master
    m_shellProcess.m_handleMaster = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (m_shellProcess.m_handleMaster <= 0)
    {
        m_lastError = QString("UnixPty Error: unable to open master -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    //get name of slave
    m_shellProcess.m_handleSlaveName = ptsname(m_shellProcess.m_handleMaster);
    if ( m_shellProcess.m_handleSlaveName.isEmpty())
    {
        m_lastError = QString("UnixPty Error: unable to get slave name -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    //change permission of slave
    rc = grantpt(m_shellProcess.m_handleMaster);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: unable to change perms for slave -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    //unlock slave
    rc = unlockpt(m_shellProcess.m_handleMaster);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: unable to unlock slave -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    //open slave
    m_shellProcess.m_handleSlave = ::open(m_shellProcess.m_handleSlaveName.toLatin1().data(), O_RDWR | O_NOCTTY);
    if (m_shellProcess.m_handleSlave < 0)
    {
        m_lastError = QString("UnixPty Error: unable to open slave -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    rc = fcntl(m_shellProcess.m_handleMaster, F_SETFD, FD_CLOEXEC);
    if (rc == -1)
    {
        m_lastError = QString("UnixPty Error: unable to set flags for master -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    rc = fcntl(m_shellProcess.m_handleSlave, F_SETFD, FD_CLOEXEC);
    if (rc == -1)
    {
        m_lastError = QString("UnixPty Error: unable to set flags for slave -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    struct ::termios ttmode;
    rc = tcgetattr(m_shellProcess.m_handleMaster, &ttmode);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: termios fail -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    ttmode.c_iflag = ICRNL | IXON | IXANY | IMAXBEL | BRKINT;
#if defined(IUTF8)
    ttmode.c_iflag |= IUTF8;
#endif

    ttmode.c_oflag = OPOST | ONLCR;
    ttmode.c_cflag = CREAD | CS8 | HUPCL;
    ttmode.c_lflag = ICANON | ISIG | IEXTEN | ECHO | ECHOE | ECHOK | ECHOKE | ECHOCTL;

    ttmode.c_cc[VEOF] = 4;
    ttmode.c_cc[VEOL] = -1;
    ttmode.c_cc[VEOL2] = -1;
    ttmode.c_cc[VERASE] = 0x7f;
    ttmode.c_cc[VWERASE] = 23;
    ttmode.c_cc[VKILL] = 21;
    ttmode.c_cc[VREPRINT] = 18;
    ttmode.c_cc[VINTR] = 3;
    ttmode.c_cc[VQUIT] = 0x1c;
    ttmode.c_cc[VSUSP] = 26;
    ttmode.c_cc[VSTART] = 17;
    ttmode.c_cc[VSTOP] = 19;
    ttmode.c_cc[VLNEXT] = 22;
    ttmode.c_cc[VDISCARD] = 15;
    ttmode.c_cc[VMIN] = 1;
    ttmode.c_cc[VTIME] = 0;

#if (__APPLE__)
    ttmode.c_cc[VDSUSP] = 25;
    ttmode.c_cc[VSTATUS] = 20;
#endif

    cfsetispeed(&ttmode, B38400);
    cfsetospeed(&ttmode, B38400);


    //set params to master
    rc = tcsetattr(m_shellProcess.m_handleMaster, TCSANOW, &ttmode);
    if (rc != 0)
    {
        m_lastError = QString("UnixPty Error: unabble to set associated params -> %1").arg(strerror(errno));
        kill();
        return false;
    }

    m_readMasterNotify = new QSocketNotifier(m_shellProcess.m_handleMaster, QSocketNotifier::Read, &m_shellProcess);
    m_readMasterNotify->setEnabled(true);
    QObject::connect(m_readMasterNotify, &QSocketNotifier::activated, [this](int socket)
    {
        Q_UNUSED(socket)

        QByteArray buffer;
        int size = 1024;
        QByteArray data;
        do
        {
            char nativeBuffer[size];
            int len = ::read(m_shellProcess.m_handleMaster, nativeBuffer, size);
            data = QByteArray(nativeBuffer, len);
            buffer.append(data);
        } while (data.size() == size);

        m_shellReadBuffer.append(data);
        m_shellProcess.emitReadyRead();
    });

    m_writeMasterNotify = new QSocketNotifier(m_shellProcess.m_handleMaster, QSocketNotifier::Write, &m_shellProcess);
    m_writeMasterNotify->setEnabled(true);
    QObject::connect(m_writeMasterNotify, &QSocketNotifier::activated, [this](int socket)
    {
        Q_UNUSED(socket)
        m_writeMasterNotify->setEnabled(false);
    });

    m_readSlaveNotify = new QSocketNotifier(m_shellProcess.m_handleSlave, QSocketNotifier::Read, &m_shellProcess);
    m_readSlaveNotify->setEnabled(true);
    QObject::connect(m_readSlaveNotify, &QSocketNotifier::activated, [](int socket)
    {
        Q_UNUSED(socket)
        //not used... slave redirected to master
    });

    m_writeSlaveNotify = new QSocketNotifier(m_shellProcess.m_handleSlave, QSocketNotifier::Write, &m_shellProcess);
    m_writeSlaveNotify->setEnabled(true);
    QObject::connect(m_writeSlaveNotify, &QSocketNotifier::activated, [this](int socket)
    {
        Q_UNUSED(socket)
        m_writeSlaveNotify->setEnabled(false);
    });

    QProcessEnvironment envFormat;
    foreach (QString line, environment)
    {
        envFormat.insert(line.split("=").first(), line.split("=").last());
    }
    m_shellProcess.setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_shellProcess.setProcessEnvironment(envFormat);
    m_shellProcess.setReadChannel(QProcess::StandardOutput);
    m_shellProcess.start(m_shellPath, QStringList());
    m_shellProcess.waitForStarted();

    m_pid = m_shellProcess.processId();

    return true;
}

bool UnixPtyProcess::resize(qint16 cols, qint16 rows)
{
    struct winsize winp;
    winp.ws_col = cols;
    winp.ws_row = rows;
    winp.ws_xpixel = 0;
    winp.ws_ypixel = 0;

    bool res =  ( (ioctl(m_shellProcess.m_handleMaster, TIOCSWINSZ, &winp) != -1) && (ioctl(m_shellProcess.m_handleSlave, TIOCSWINSZ, &winp) != -1) );

    if (res)
    {
        m_size = QPair<qint16, qint16>(cols, rows);
    }

    return res;
}

bool UnixPtyProcess::kill()
{
    //close cli
    m_shellProcess.m_handleSlaveName = QString();
    if (m_shellProcess.m_handleSlave >= 0)
    {
        ::close(m_shellProcess.m_handleSlave);
        m_shellProcess.m_handleSlave = -1;
    }
    if (m_shellProcess.m_handleMaster >= 0)
    {
        ::close(m_shellProcess.m_handleMaster);
        m_shellProcess.m_handleMaster = -1;
    }

    //kill child process
    if (m_shellProcess.state() == QProcess::Running)
    {
        m_readMasterNotify->disconnect();
        m_writeMasterNotify->disconnect();
        m_readSlaveNotify->disconnect();
        m_writeSlaveNotify->disconnect();

        m_readMasterNotify->deleteLater();
        m_writeMasterNotify->deleteLater();
        m_readSlaveNotify->deleteLater();
        m_writeSlaveNotify->deleteLater();

        m_shellProcess.terminate();
        m_shellProcess.waitForFinished(1000);

        if (m_shellProcess.state() == QProcess::Running)
        {
            QProcess::startDetached( QString("kill -9 %1").arg( pid() ) );
            m_shellProcess.kill();
            m_shellProcess.waitForFinished(1000);
        }

        return (m_shellProcess.state() == QProcess::NotRunning);
    }
    return false;
}

IPtyProcess::PtyType UnixPtyProcess::type()
{
    return IPtyProcess::UnixPty;
}

#ifdef PTYQT_DEBUG
QString UnixPtyProcess::dumpDebugInfo()
{
    return QString("PID: %1, In: %2, Out: %3, Type: %4, Cols: %5, Rows: %6, IsRunning: %7, Shell: %8, SlaveName: %9")
                .arg(m_pid).arg(m_shellProcess.m_handleMaster).arg(m_shellProcess.m_handleSlave).arg(type())
                .arg(m_size.first).arg(m_size.second).arg(m_shellProcess.state() == QProcess::Running)
                .arg(m_shellPath).arg(m_shellProcess.m_handleSlaveName);
}
#endif

QIODevice *UnixPtyProcess::notifier()
{
    return &m_shellProcess;
}

QByteArray UnixPtyProcess::readAll()
{
    QByteArray tmpBuffer =  m_shellReadBuffer;
    m_shellReadBuffer.clear();
    return tmpBuffer;
}

qint64 UnixPtyProcess::write(const QByteArray &byteArray)
{
    int result = ::write(m_shellProcess.m_handleMaster, byteArray.constData(), byteArray.size());
    Q_UNUSED(result)

    return byteArray.size();
}

bool UnixPtyProcess::isAvailable()
{
    return true;
}


void ShellProcess::setupChildProcess()
{
    //for more info in book 'Advanced Programming in the UNIX Environment, 3rd Edition'
    //at this point we are forked, but not executed
    dup2(m_handleSlave, STDIN_FILENO);
    dup2(m_handleSlave, STDOUT_FILENO);
    dup2(m_handleSlave, STDERR_FILENO);

    //create session for process
    pid_t sid = setsid();

    //setup slave
    ioctl(m_handleSlave, TIOCSCTTY, 0);

    //new group for process
    tcsetpgrp(m_handleSlave, sid);

    struct utmpx utmpxInfo;
    memset(&utmpxInfo, 0, sizeof(utmpxInfo));

    strncpy(utmpxInfo.ut_user, qgetenv("USER"), sizeof(utmpxInfo.ut_user));

    QString device(m_handleSlaveName);
    if (device.startsWith("/dev/"))
        device = device.mid(5);

    const char *d = device.toLatin1().constData();

    strncpy(utmpxInfo.ut_line, d, sizeof(utmpxInfo.ut_line));

    strncpy(utmpxInfo.ut_id, d + strlen(d) - sizeof(utmpxInfo.ut_id), sizeof(utmpxInfo.ut_id));

    struct timeval tv;
    gettimeofday(&tv, 0);
    utmpxInfo.ut_tv.tv_sec = tv.tv_sec;
    utmpxInfo.ut_tv.tv_usec = tv.tv_usec;

    utmpxInfo.ut_type = USER_PROCESS;
    utmpxInfo.ut_pid = getpid();

    utmpxname(_PATH_UTMPX);
    setutxent();
    pututxline(&utmpxInfo);
    endutxent();

#if !defined(Q_OS_UNIX)
    updwtmpx(_PATH_UTMPX, &loginInfo);
#endif
}