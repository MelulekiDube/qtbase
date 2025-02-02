/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Copyright (C) 2021 Alex Trotsenko <alex1973tr@gmail.com>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qwindowspipewriter_p.h"
#include <qcoreapplication.h>
#include <QMutexLocker>

QT_BEGIN_NAMESPACE

QWindowsPipeWriter::QWindowsPipeWriter(HANDLE pipeWriteEnd, QObject *parent)
    : QObject(parent),
      handle(pipeWriteEnd),
      eventHandle(CreateEvent(NULL, FALSE, FALSE, NULL)),
      syncHandle(CreateEvent(NULL, TRUE, FALSE, NULL)),
      waitObject(NULL),
      pendingBytesWrittenValue(0),
      lastError(ERROR_SUCCESS),
      stopped(true),
      writeSequenceStarted(false),
      bytesWrittenPending(false),
      winEventActPosted(false)
{
    ZeroMemory(&overlapped, sizeof(OVERLAPPED));
    overlapped.hEvent = eventHandle;
    waitObject = CreateThreadpoolWait(waitCallback, this, NULL);
    if (waitObject == NULL)
        qErrnoWarning("QWindowsPipeWriter: CreateThreadpollWait failed.");
}

QWindowsPipeWriter::~QWindowsPipeWriter()
{
    stop();
    CloseThreadpoolWait(waitObject);
    CloseHandle(eventHandle);
    CloseHandle(syncHandle);
}

/*!
    Assigns the handle to this writer. The handle must be valid.
    Call this function if data was buffered before getting the handle.
 */
void QWindowsPipeWriter::setHandle(HANDLE hPipeWriteEnd)
{
    Q_ASSERT(!stopped);

    handle = hPipeWriteEnd;
    QMutexLocker locker(&mutex);
    startAsyncWriteLocked(&locker);
}

/*!
    Stops the asynchronous write sequence.
    If the write sequence is running then the I/O operation is canceled.
 */
void QWindowsPipeWriter::stop()
{
    if (stopped)
        return;

    mutex.lock();
    stopped = true;
    if (writeSequenceStarted) {
        // Trying to disable callback before canceling the operation.
        // Callback invocation is unnecessary here.
        SetThreadpoolWait(waitObject, NULL, NULL);
        if (!CancelIoEx(handle, &overlapped)) {
            const DWORD dwError = GetLastError();
            if (dwError != ERROR_NOT_FOUND) {
                qErrnoWarning(dwError, "QWindowsPipeWriter: CancelIoEx on handle %p failed.",
                              handle);
            }
        }
        writeSequenceStarted = false;
    }
    mutex.unlock();

    WaitForThreadpoolWaitCallbacks(waitObject, TRUE);
}

/*!
    Returns the number of bytes that are waiting to be written.
 */
qint64 QWindowsPipeWriter::bytesToWrite() const
{
    QMutexLocker locker(&mutex);
    return writeBuffer.size() + pendingBytesWrittenValue;
}

/*!
    Writes a shallow copy of \a ba to the internal buffer.
 */
void QWindowsPipeWriter::write(const QByteArray &ba)
{
    writeImpl(ba);
}

/*!
    Writes data to the internal buffer.
 */
void QWindowsPipeWriter::write(const char *data, qint64 size)
{
    writeImpl(data, size);
}

template <typename... Args>
inline void QWindowsPipeWriter::writeImpl(Args... args)
{
    QMutexLocker locker(&mutex);

    if (lastError != ERROR_SUCCESS)
        return;

    writeBuffer.append(args...);

    if (writeSequenceStarted)
        return;

    stopped = false;

    // If we don't have an assigned handle yet, defer writing until
    // setHandle() is called.
    if (handle != INVALID_HANDLE_VALUE)
        startAsyncWriteLocked(&locker);
}

/*!
    Starts a new write sequence.
 */
void QWindowsPipeWriter::startAsyncWriteLocked(QMutexLocker<QMutex> *locker)
{
    while (!writeBuffer.isEmpty()) {
        // WriteFile() returns true, if the write operation completes synchronously.
        // We don't need to call GetOverlappedResult() additionally, because
        // 'numberOfBytesWritten' is valid in this case.
        DWORD numberOfBytesWritten;
        DWORD errorCode = ERROR_SUCCESS;
        if (!WriteFile(handle, writeBuffer.readPointer(), writeBuffer.nextDataBlockSize(),
                       &numberOfBytesWritten, &overlapped)) {
            errorCode = GetLastError();
            if (errorCode == ERROR_IO_PENDING) {
                // Operation has been queued and will complete in the future.
                writeSequenceStarted = true;
                SetThreadpoolWait(waitObject, eventHandle, NULL);
                break;
            }
        }

        if (!writeCompleted(errorCode, numberOfBytesWritten))
            break;
    }

    // Do not post the event, if the write operation will be completed asynchronously.
    if (!bytesWrittenPending)
        return;

    if (!winEventActPosted) {
        winEventActPosted = true;
        locker->unlock();
        QCoreApplication::postEvent(this, new QEvent(QEvent::WinEventAct));
    } else {
        locker->unlock();
    }

    // We set the event only after unlocking to avoid additional context
    // switches due to the released thread immediately running into the lock.
    SetEvent(syncHandle);
}

/*!
    \internal
    Thread pool callback procedure.
 */
void QWindowsPipeWriter::waitCallback(PTP_CALLBACK_INSTANCE instance, PVOID context,
                                      PTP_WAIT wait, TP_WAIT_RESULT waitResult)
{
    Q_UNUSED(instance);
    Q_UNUSED(wait);
    Q_UNUSED(waitResult);
    QWindowsPipeWriter *pipeWriter = reinterpret_cast<QWindowsPipeWriter *>(context);

    // Get the result of the asynchronous operation.
    DWORD numberOfBytesTransfered = 0;
    DWORD errorCode = ERROR_SUCCESS;
    if (!GetOverlappedResult(pipeWriter->handle, &pipeWriter->overlapped,
                             &numberOfBytesTransfered, FALSE))
        errorCode = GetLastError();

    QMutexLocker locker(&pipeWriter->mutex);

    // After the writer was stopped, the only reason why this function can be called is the
    // completion of a cancellation. No signals should be emitted, and no new write sequence
    // should be started in this case.
    if (pipeWriter->stopped)
        return;

    pipeWriter->writeSequenceStarted = false;

    if (pipeWriter->writeCompleted(errorCode, numberOfBytesTransfered)) {
        pipeWriter->startAsyncWriteLocked(&locker);
    } else {
        // The write operation failed, so we must unblock the main thread,
        // which can wait for the event. We set the event only after unlocking
        // to avoid additional context switches due to the released thread
        // immediately running into the lock.
        locker.unlock();
        SetEvent(pipeWriter->syncHandle);
    }
}

/*!
    Will be called whenever the write operation completes. Returns \c true if
    no error occurred; otherwise returns \c false.
 */
bool QWindowsPipeWriter::writeCompleted(DWORD errorCode, DWORD numberOfBytesWritten)
{
    if (errorCode == ERROR_SUCCESS) {
        bytesWrittenPending = true;
        pendingBytesWrittenValue += numberOfBytesWritten;
        writeBuffer.free(numberOfBytesWritten);
        return true;
    }

    lastError = errorCode;
    writeBuffer.clear();
    switch (errorCode) {
    case ERROR_PIPE_NOT_CONNECTED: // the other end has closed the pipe
    case ERROR_OPERATION_ABORTED: // the operation was canceled
    case ERROR_NO_DATA: // the pipe is being closed
        break;
    default:
        qErrnoWarning(errorCode, "QWindowsPipeWriter: write failed.");
        break;
    }
    return false;
}

/*!
    Receives notification that the write operation has completed.
 */
bool QWindowsPipeWriter::event(QEvent *e)
{
    if (e->type() == QEvent::WinEventAct) {
        consumePendingAndEmit(true);
        return true;
    }
    return QObject::event(e);
}

/*!
    Updates the state and emits pending signals in the main thread.
    Returns \c true, if bytesWritten() was emitted.
 */
bool QWindowsPipeWriter::consumePendingAndEmit(bool allowWinActPosting)
{
    ResetEvent(syncHandle);
    QMutexLocker locker(&mutex);

    // Enable QEvent::WinEventAct posting.
    if (allowWinActPosting)
        winEventActPosted = false;

    if (!bytesWrittenPending)
        return false;

    // Reset the state even if we don't emit bytesWritten().
    // It's a defined behavior to not re-emit this signal recursively.
    bytesWrittenPending = false;
    qint64 numberOfBytesWritten = pendingBytesWrittenValue;
    pendingBytesWrittenValue = 0;

    locker.unlock();

    // Disable any further processing, if the pipe was stopped.
    if (stopped)
        return false;

    emit bytesWritten(numberOfBytesWritten);
    return true;
}

QT_END_NAMESPACE
