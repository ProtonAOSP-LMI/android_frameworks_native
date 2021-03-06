/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "PipeRelay.h"

#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>

#include <utils/Thread.h>

namespace android {
namespace lshal {

static constexpr struct timeval READ_TIMEOUT { .tv_sec = 1, .tv_usec = 0 };

static std::string getThreadName(std::string interfaceName, const std::string &instanceName) {
    auto dot = interfaceName.rfind(".");
    if (dot != std::string::npos) interfaceName = interfaceName.substr(dot + 1);
    return "RelayThread_" + interfaceName + "_" + instanceName;
}

struct PipeRelay::RelayThread : public Thread {
    explicit RelayThread(int fd, std::ostream &os, const NullableOStream<std::ostream> &err,
                         const std::string &fqName);

    bool threadLoop() override;
    void setFinished();

private:
    int mFd;
    std::ostream &mOutStream;
    NullableOStream<std::ostream> mErrStream;

    // If we were to use requestExit() and exitPending() instead, threadLoop()
    // may not run at all by the time ~PipeRelay is called (i.e. debug() has
    // returned from HAL). By using our own flag, we ensure that select() and
    // read() are executed until data are drained.
    std::atomic_bool mFinished;

    std::string mFqName;

    DISALLOW_COPY_AND_ASSIGN(RelayThread);
};

////////////////////////////////////////////////////////////////////////////////

PipeRelay::RelayThread::RelayThread(int fd, std::ostream &os,
                                    const NullableOStream<std::ostream> &err,
                                    const std::string &fqName)
      : mFd(fd), mOutStream(os), mErrStream(err), mFinished(false), mFqName(fqName) {}

bool PipeRelay::RelayThread::threadLoop() {
    char buffer[1024];

    fd_set set;
    FD_ZERO(&set);
    FD_SET(mFd, &set);

    struct timeval timeout = READ_TIMEOUT;

    int res = TEMP_FAILURE_RETRY(select(mFd + 1, &set, nullptr, nullptr, &timeout));
    if (res < 0) {
        mErrStream << "debug " << mFqName << ": select() failed";
        return false;
    }

    if (res == 0 || !FD_ISSET(mFd, &set)) {
        if (mFinished) {
            mErrStream << "debug " << mFqName
                       << ": timeout reading from pipe, output may be truncated.";
            return false;
        }
        // timeout, but debug() has not returned, so wait for HAL to finish.
        return true;
    }

    // FD_ISSET(mFd, &set) == true. Data available, start reading
    ssize_t n = TEMP_FAILURE_RETRY(read(mFd, buffer, sizeof(buffer)));

    if (n < 0) {
        mErrStream << "debug " << mFqName << ": read() failed";
    }

    if (n <= 0) {
        return false;
    }

    mOutStream.write(buffer, n);

    return true;
}

void PipeRelay::RelayThread::setFinished() {
    mFinished = true;
}

////////////////////////////////////////////////////////////////////////////////

PipeRelay::PipeRelay(std::ostream &os, const NullableOStream<std::ostream> &err,
                     const std::string &interfaceName, const std::string &instanceName)
      : mInitCheck(NO_INIT) {
    int res = pipe(mFds);

    if (res < 0) {
        mInitCheck = -errno;
        return;
    }

    mThread = new RelayThread(mFds[0], os, err, interfaceName + "/" + instanceName);
    mInitCheck = mThread->run(getThreadName(interfaceName, instanceName).c_str());
}

void PipeRelay::CloseFd(int *fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

PipeRelay::~PipeRelay() {
    CloseFd(&mFds[1]);

    if (mThread != nullptr) {
        mThread->setFinished();
        mThread->join();
        mThread.clear();
    }

    CloseFd(&mFds[0]);
}

status_t PipeRelay::initCheck() const {
    return mInitCheck;
}

int PipeRelay::fd() const {
    return mFds[1];
}

}  // namespace lshal
}  // namespace android
