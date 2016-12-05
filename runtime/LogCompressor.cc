/* Copyright (c) 2016 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <chrono>
#include <vector>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <aio.h>

#include "BufferUtils.h"
#include "Cycles.h"
#include "FastLogger.h"
#include "LogCompressor.h"

// File generated by the FastLogger preprocessor that contains all the
// compression and decompression functions.
#include "BufferStuffer.h"

namespace PerfUtils {

/**
 * Construct a LogCompressor.
 *
 * \param logFile
 *      file path to output compressed logs to.
 */
LogCompressor::LogCompressor(const char *logFile)
        : outputFd(0)
        , aioCb()
        , hasOustandingOperation(false)
        , workerThread()
        , shouldExit(false)
        , mutex()
        , workAdded()
        , hintQueueEmptied()
        , syncRequested(false)
        , outputBuffer(NULL)
        , posixBuffer(NULL)
        , cyclesScanningAndCompressing(0)
        , cyclesAioAndFsync(0)
        , cyclesCompressing(0)
        , padBytesWritten(0)
        , totalBytesRead(0)
        , totalBytesWritten(0)
        , eventsProcessed(0)
    {
        outputFd = open(logFile, FILE_PARAMS);
        memset(&aioCb, 0, sizeof(aioCb));

        //TODO(syang0) Throw exceptions?
        int err = posix_memalign(reinterpret_cast<void**>(&outputBuffer),
                                                            512, BUFFER_SIZE);
        if (err) {
            perror("Couldn't allocate LogCompressor's output buffer");
            std::exit(-1);
        }

        err = posix_memalign(reinterpret_cast<void**>(&posixBuffer),
                                                            512, BUFFER_SIZE);
        if (err) {
            perror("Couldn't allocate LogCompressor's output buffer");
            std::exit(-1);
        }

        workerThread = std::thread(
                            &PerfUtils::LogCompressor::threadMain, this);
    }

/**
 * Log Compressor Destructor
 */
LogCompressor::~LogCompressor() {
    sync();
    exit();

    if (outputBuffer) {
        free(outputBuffer);
        outputBuffer = nullptr;
    }

    if (posixBuffer) {
        free(posixBuffer);
        posixBuffer = nullptr;
    }

    if (outputFd > 0)
        close(outputFd);

    outputFd = 0;
}

void
LogCompressor::waitForAIO()
{
    if (hasOustandingOperation) {
        // burn time waiting for io (could aio_suspend)
        // and could signal back when ready via a register.
        while (aio_error(&aioCb) == EINPROGRESS);
        int err = aio_error(&aioCb);
        ssize_t ret = aio_return(&aioCb);

        if (err != 0) {
            fprintf(stderr, "LogCompressor's POSIX AIO failed with %d: %s\r\n",
                    err, strerror(err));
        } else if (ret < 0) {
            perror("LogCompressor's Posix AIO Write operation failed");
        }
        ++numAioWritesCompleted;
        hasOustandingOperation = false;
    }
}

void
LogCompressor::threadMain() {
    //TODO(syang0) These should be abstracted away
    uint32_t lastFmtId = 0;
    uint64_t lastTimestamp = 0;

    // Index of the last StagingBuffer checked for uncompressed log messages
    size_t lastStagingBufferChecked = 0;


    // Each iteration of this loop scans for uncompressed log messages in the
    // thread buffers, compresses as much as possible, and outputs it to a file.
    while (!shouldExit) {
        // Main buffer to put compressed log messages into
        char *out = outputBuffer;
        char *endOfBuffer = outputBuffer + BUFFER_SIZE;

        {
            uint64_t start = Cycles::rdtsc();
            std::unique_lock<std::mutex> lock(FastLogger::bufferMutex);
            size_t i = lastStagingBufferChecked;

            if (FastLogger::threadBuffers.empty())
                break;

            // Indicates whether a compression operation failed or not due
            // to insufficient space in the outputBuffer
            bool outputBufferFull = false;

            // Indicates whether uncompressed log messages were found through
            // an iteration through all the staging buffers.
            bool workFound = false;

            // Scan through the threadBuffers looking for log messages to
            // compress while the output buffer is not full.
            while (!shouldExit && !outputBufferFull) {
                uint64_t readableBytes = 0;
                FastLogger::StagingBuffer *sb = FastLogger::threadBuffers[i];

                // Thread killed itself, remove it from the list.
                if (sb == nullptr) {
                    FastLogger::threadBuffers.erase(
                            FastLogger::threadBuffers.begin() + i);

                    if (i == FastLogger::threadBuffers.size()) {
                        if (lastStagingBufferChecked == i)
                            lastStagingBufferChecked = 0;

                        i = 0;
                    }
                    continue;
                }

                char *peekPosition = sb->peek(&readableBytes);

                // If there's work, unlock to perform it
                if (readableBytes > 0) {
                    uint64_t start = Cycles::rdtsc();
                    workFound = true;
                    lock.unlock();

                    uint64_t readableBytesStart = readableBytes;
                    //TODO(syang0) This should be abstracted away, me thinks.
                    while (readableBytes > 0) {
                        assert(readableBytes >= sizeof(BufferUtils::RecordEntry));

                        BufferUtils::RecordEntry *re = reinterpret_cast<
                                    BufferUtils::RecordEntry*>(peekPosition);
                        assert(re->entrySize <= readableBytes);

                        if (re->entrySize + re->argMetaBytes > endOfBuffer - out) {
                            // don't have enough space in the output to save
                            // the uncompressed form (worst case),
                            // save our place and back out
                            lastStagingBufferChecked = i;
                            outputBufferFull = true;
                            break;
                        }

                        ++eventsProcessed;

                        // Compress metadata here.
                        BufferUtils::compressMetadata(re, &out, lastTimestamp, lastFmtId);
                        lastFmtId = re->fmtId;
                        lastTimestamp = re->timestamp;

                        //TODO(syang0) This should be analogs with above
                        size_t bytesOut = compressFnArray[re->fmtId](re, out);
                        out += bytesOut;

                        readableBytes -= re->entrySize;
                        peekPosition += re->entrySize;
                        sb->consume(re->entrySize);
                    }
                    totalBytesRead += readableBytesStart - readableBytes;

                    cyclesCompressing += Cycles::rdtsc() - start;
                    lock.lock();
                }

                i = (i + 1) % FastLogger::threadBuffers.size();

                // Completed a pass through the buffers
                if (i == lastStagingBufferChecked) {
                    // If no work was found in the last pass, stop.
                    if (!workFound) {
                        break;
                    }

                    workFound = false;
                }
            }

            cyclesScanningAndCompressing += Cycles::rdtsc() - start;
        }

        // Nothing was compressed
        if (out == outputBuffer) {
            std::unique_lock<std::mutex> lock(mutex);

            // If a sync was requested, we should make at least 1 more
            // pass to make sure we got everything up to the sync point.
            if (syncRequested) {
                syncRequested = false;
                continue;
            }

            hintQueueEmptied.notify_one();
            workAdded.wait_for(lock, std::chrono::microseconds(1));
            continue;
        }

        // Determine how many pad bytes we will need if O_DIRECT is used
        ssize_t bytesToWrite = out - outputBuffer;
        if (FILE_PARAMS & O_DIRECT) {
            ssize_t bytesOver = bytesToWrite%512;

            if (bytesOver != 0) {
                bytesToWrite = bytesToWrite + 512 - bytesOver;
                padBytesWritten += (512 - bytesOver);
            }
        }

        if (bytesToWrite) {
            uint64_t start = Cycles::rdtsc();
            if (USE_AIO) {
                waitForAIO();
                aioCb.aio_fildes = outputFd;
                aioCb.aio_buf = outputBuffer;
                aioCb.aio_nbytes = bytesToWrite;
                totalBytesWritten += bytesToWrite;

                if (aio_write(&aioCb) == -1) {
                    fprintf(stderr, "Error at aio_write(): %s\n",
                            strerror(errno));
                }

                hasOustandingOperation = true;

                // Swap buffers
                char *tmp = outputBuffer;
                outputBuffer = posixBuffer;
                posixBuffer = tmp;
            } else {
                if (bytesToWrite != write(outputFd, outputBuffer, bytesToWrite))
                    perror("Error dumping log");
            }

            // TODO(syang0) Currently, the cyclesAioAndFsync metric is
            // incorrect if we use POSIX AIO since it only measures the
            // time to submit the work and (if applicable) the amount of
            // time spent waiting for a previous incomplete AIO to finish.
            // We could get a better time metric if we spawned a thread to
            // do synchronous IO on our behalf.
            cyclesAioAndFsync += (Cycles::rdtsc() - start);
        }
    }

    if (hasOustandingOperation) {
        uint64_t start = Cycles::rdtsc();
        waitForAIO();
        cyclesAioAndFsync += (Cycles::rdtsc() - start);
    }

    // Output the stats after this thread exits
    printf("\r\nLogger Compressor Thread Exiting, printing stats\r\n");
    printStats();
}

/**
 * Print out various statistics related to the LogCompressor to stdout.
 */
void LogCompressor::printStats() {
    // Leaks abstraction, but basically flush so we get all the time
    uint64_t start = Cycles::rdtsc();
    fdatasync(outputFd);
    uint64_t stop = Cycles::rdtsc();
    cyclesAioAndFsync += (stop - start);

    double outputTime = Cycles::toSeconds(cyclesAioAndFsync);
//    double lookingForWork = Cycles::toSeconds(cyclesSearchingForWork);
    double compressTime = Cycles::toSeconds(cyclesCompressing);
    double workTime = outputTime + compressTime;

    double totalBytesWrittenDouble = static_cast<double>(totalBytesWritten);
    double totalBytesReadDouble = static_cast<double>(totalBytesRead);
    double padBytesWrittenDouble = static_cast<double>(padBytesWritten);
    double numEventsProcessedDouble = static_cast<double>(eventsProcessed);

    printf("Wrote %lu events (%0.2lf MB) in %0.3lf seconds "
            "(%0.3lf seconds spent compressing)\r\n",
            eventsProcessed,
            totalBytesWrittenDouble/1.0e6,
            outputTime,
            compressTime);

    printf("There were %u file flushes and the final sync time was %lf sec\r\n",
            numAioWritesCompleted, Cycles::toSeconds(stop - start));

    printf("On average, that's\r\n"
            "\t%0.2lf MB/s or %0.2lf ns/byte w/ processing\r\n"
            "\t%0.2lf MB/s or %0.2lf ns/byte raw output\r\n"
            "\t%0.2lf MB per flush with %0.1lf bytes/event\r\n",
            (totalBytesWrittenDouble/1.0e6)/(workTime),
            (workTime*1.0e9)/totalBytesWrittenDouble,
            (totalBytesWrittenDouble/1.0e6)/outputTime,
            (outputTime)*1.0e9/totalBytesWrittenDouble,
            (totalBytesWrittenDouble/1.0e6)/numAioWritesCompleted,
            totalBytesWrittenDouble*1.0/numEventsProcessedDouble);

    printf("\t%0.2lf ns/event in total\r\n"
            "\t%0.2lf ns/event compressing\r\n",
            (outputTime + compressTime)*1.0e9/numEventsProcessedDouble,
            compressTime*1.0e9/numEventsProcessedDouble);

    printf("The compression ratio was %0.2lf-%0.2lfx "
            "(%lu bytes in, %lu bytes out, %lu pad bytes)\n",
                    1.0*totalBytesReadDouble/(totalBytesWrittenDouble
                                                    + padBytesWrittenDouble),
                    1.0*totalBytesReadDouble/totalBytesWrittenDouble,
                    totalBytesRead,
                    totalBytesWritten,
                    padBytesWritten);
}

/**
 * Blocks until the LogCompressor is unable to find anymore work in its pass
 * through the thread local staging buffers. Note that since access to the
 * buffers is not synchronized, it's possible that some log messages enqueued
 * after this invocation will be missed.
 */
void
LogCompressor::sync()
{
    std::unique_lock<std::mutex> lock(mutex);
    syncRequested = true;
    workAdded.notify_all();
    hintQueueEmptied.wait(lock);
}

/**
 * Stops the log compressor thread as soon as possible. Note that this will
 * not ensure that all log messages are persisted before the exit. If the
 * behavior is desired, one must invoke stop all logging, invoke sync() and
 * then exit().
 */
void
LogCompressor::exit()
{
    {
        std::unique_lock<std::mutex> lock(mutex);
        shouldExit = true;
        workAdded.notify_all();
    }
    workerThread.join();
}
} // Namespace

