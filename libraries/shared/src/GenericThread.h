//
//  GenericThread.h
//  shared
//
//  Created by Brad Hefta-Gaub on 8/12/13.
//  Copyright (c) 2013 High Fidelity, Inc. All rights reserved.
//
//  Generic Threaded or non-threaded processing class.
//

#ifndef __shared__GenericThread__
#define __shared__GenericThread__

#include <QtCore/QObject>
#include <QMutex>
#include <QThread>

/// A basic generic "thread" class. Handles a single thread of control within the application. Can operate in non-threaded
/// mode but caller must regularly call threadRoutine() method.
class GenericThread : public QObject {
    Q_OBJECT
public:
    GenericThread();
    virtual ~GenericThread();

    /// Call to start the thread.
    /// \param bool isThreaded true by default. false for non-threaded mode and caller must call threadRoutine() regularly.
    void initialize(bool isThreaded = true);

    /// Call to stop the thread
    void terminate();

    /// Override this function to do whatever your class actually does, return false to exit thread early.
    virtual bool process() = 0;

    bool isThreaded() const { return _isThreaded; }

public slots:
    /// If you're running in non-threaded mode, you must call this regularly
    void threadRoutine();

signals:
    void finished();

protected:

    /// Locks all the resources of the thread.
    void lock() { _mutex.lock(); }

    /// Unlocks all the resources of the thread.
    void unlock() { _mutex.unlock(); }

    bool isStillRunning() const { return !_stopThread; }

private:
    QMutex _mutex;

    bool _stopThread;
    bool _isThreaded;
    QThread* _thread;
};

#endif // __shared__GenericThread__
