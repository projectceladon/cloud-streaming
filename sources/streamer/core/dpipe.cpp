// Copyright (C) 2022 Intel Corporation

/*
 * Copyright (c) 2013-2015 Chun-Ying Huang
 *
 * This file is part of GamingAnywhere (GA).
 *
 * GA is free software; you can redistribute it and/or modify it
 * under the terms of the 3-clause BSD License as published by the
 * Free Software Foundation: http://directory.fsf.org/wiki/License:BSD_3Clause
 *
 * GA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the 3-clause BSD License along with GA;
 * if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/**
 * @file
 * dpipe implementation: pipe for delivering discrete frames
 */
#include "dpipe.h"

#include <string.h>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

using namespace std;
using namespace std::chrono;

/** Store the mapping between pipe-name and pipe structure */
static std::mutex dpipemap_mutex;
static map<string,dpipe_t*> dpipemap;

/**
 * Create and register a new video pipe.
 *
 * @param id [in] The video channel id
 * @param name [in] The name of the dpipe, must be unique
 * @param nframe [in] Number of frame buffers in the pipe
 * @param maxframesize [in] The maximum frame buffer size
 * @return Pointer to a created dpipe, or NULL on failure
 *
 * Note: dpipe_create() also returns NULL if the requesting name is existed.
 */
dpipe_t *
dpipe_create(int id, const char *name, int nframe, int maxframesize) {
    int i;
    dpipe_t *dpipe;
    // sanity checks
    if(name == NULL || id < 0 || nframe <= 0 || maxframesize <= 0)
        return NULL;
    // existing?
    if((dpipe = dpipe_lookup(name)) != NULL)
        return NULL;
    // allocate the space
    dpipe = new dpipe_t{};
    if(dpipe == NULL)
        return NULL;
    //
    dpipe->channel_id = id;
    if((dpipe->name = strdup(name)) == NULL)
        goto err_create;
    // alloc and init frame buffers
    for(i = 0; i < nframe; i++) {
        dpipe_buffer_t* dbuffer;
        if((dbuffer = (dpipe_buffer_t*) malloc(sizeof(dpipe_buffer_t))) == NULL)
            goto err_create;
        if(ga_malloc(maxframesize, &dbuffer->internal, &dbuffer->offset) < 0) {
            free(dbuffer);
            goto err_create;
        }
        dbuffer->pointer = (void*) (((char*) dbuffer->internal) + dbuffer->offset);
        dbuffer->next = dpipe->in;
        dpipe->in = dbuffer;
        dpipe->in_count++;
    }
    //
    {
        std::lock_guard<std::mutex> lock(dpipemap_mutex);
        dpipemap[dpipe->name] = dpipe;
    }
    ga_logger(Severity::INFO, "dpipe: '%s' initialized, %d frames, framesize = %d\n",
        dpipe->name, dpipe->in_count, maxframesize);
    return dpipe;
    // failure cases
err_create:
    dpipe_destroy(dpipe);
    return NULL;
}

/**
 * Lookup an existing video pipe
 *
 * @param name [in] The name of the pipe
 * @return Pointer to the requested pipe, or NULL if not found.
 */
dpipe_t *
dpipe_lookup(const char *name) {
    map<string,dpipe_t*>::iterator mi;
    dpipe_t *dpipe = NULL;
    std::lock_guard<std::mutex> lock(dpipemap_mutex);
    if((mi = dpipemap.find(name)) != dpipemap.end())
        dpipe = mi->second;
    return dpipe;
}

/**
 * Destroy an existing dpipe
 *
 * @param dpipe [in] Pointer to the dpipe structure
 * @return Currently always return 0
 */
int
dpipe_destroy(dpipe_t *dpipe) {
    dpipe_buffer_t *vbuf, *next;
    if(dpipe == NULL)
        return 0;
    if(dpipe->name) {
        {
            std::lock_guard<std::mutex> lock(dpipemap_mutex);
            dpipemap.erase(dpipe->name);
        }
        free(dpipe->name);
    }
    //
    for(vbuf = dpipe->in; vbuf != NULL; vbuf = next) {
        next = vbuf->next;
        free(vbuf->internal);
        free(vbuf);
    }
    for(vbuf = dpipe->out; vbuf != NULL; vbuf = next) {
        next = vbuf->next;
        free(vbuf->internal);
        free(vbuf);
    }
    //
    delete dpipe;
    return 0;
}

/**
 * Get a free frame buffer from the pipe
 *
 * @param dpipe [in] The pipe to get a free frame
 * @return Pointer to the frame buffer structure
 *
 * Note: Data should be stored in vbuf->pointer, with a maximum size
 * of \a maxframesize given when creating the pipe.
 * This function should always success.
 * In case there is no available free frame buffer, this function
 * returns the eldest frame buffer in the output pool.
 *
 */
dpipe_buffer_t *
dpipe_get(dpipe_t *dpipe) {
    dpipe_buffer_t *vbuf = NULL;
    //
    std::lock_guard<std::mutex> lock(dpipe->io_mutex);
    if(dpipe->in != NULL) {
        // quick path: has available frame buffers
        if((vbuf = dpipe->in) != NULL) {
            dpipe->in = vbuf->next;
            vbuf->next = NULL;
            dpipe->in_count--;
        }
    } else {
        // no available buffers: drop the eldest frame buffer from output pool
        if((vbuf = dpipe->out) != NULL) {
            dpipe->out = vbuf->next;
            vbuf->next = NULL;
            if(dpipe->out == NULL) {
                dpipe->out_tail = NULL;
            }
            dpipe->out_count--;
        }
    }
    //
    return vbuf;
}

/**
 * Put a frame back to the free frame buffer (input pool)
 *
 * @param dpipe [in] The involved pipe
 * @param buffer [in] Pointer to the buffer to be released
 */
void
dpipe_put(dpipe_t *dpipe, dpipe_buffer_t *buffer) {
    std::lock_guard<std::mutex> lock(dpipe->io_mutex);
    buffer->next = dpipe->in;
    dpipe->in = buffer;
    dpipe->in_count++;
    return;
}

static nanoseconds timespec2dur(const timespec& ts)
{
    auto dur = seconds{ts.tv_sec} + nanoseconds{ts.tv_nsec};
    return duration_cast<nanoseconds>(dur);
}

static time_point<system_clock, nanoseconds> timespec2tp(const timespec& ts)
{
    return time_point<system_clock, nanoseconds>{
        duration_cast<system_clock::duration>(timespec2dur(ts)) };
}

/**
 * Load a frame from the output pool of the pipe
 *
 * @param dpipe [in] Pointer to the pipe to load a buffer
 * @param abstime [in] Wait for a frame until \a abstime, pass NULL to wait indefinitely
 * @return Pointer to the loaded buffer
 *
 * This function returns the first frame buffer in the output pool.
 * If \a abstime is NULL, this function blocks until a frame buffer
 * is available in the output pool.
 * If \a abstime is given, it returns NULL on timed out.
 */
dpipe_buffer_t *
dpipe_load(dpipe_t *dpipe, const struct timespec *abstime) {
    dpipe_buffer_t *vbuf = NULL;
    int failed = 0;
    //
    std::unique_lock<std::mutex> lock(dpipe->io_mutex);
again:
    if(dpipe->out != NULL) {
        vbuf = dpipe->out;
        dpipe->out = vbuf->next;
        vbuf->next = NULL;
        if(dpipe->out == NULL)
            dpipe->out_tail = NULL;
        dpipe->out_count--;
    } else if(abstime == NULL) {
        // no frame buffered
        dpipe->cond.wait(lock);
        goto again;
    } else if(failed == 0) {
        dpipe->cond.wait_until(lock, timespec2tp(*abstime));
        failed = 1;
        goto again;
    }
    //
    return vbuf;
}

/**
 * Load a frame from the output pool of the pipe without wait
 *
 * @param dpipe [in] Pointer to the pipe to load a buffer
 * @return Pointer to the loaded buffer
 *
 * This function returns the first frame buffer in the output pool.
 * If there is not a frame in the output pool, it returns NULL immediately.
 */
dpipe_buffer_t *
dpipe_load_nowait(dpipe_t *dpipe) {
    dpipe_buffer_t *vbuf = NULL;
    //
    std::lock_guard<std::mutex> lock(dpipe->io_mutex);
    if(dpipe->out != NULL) {
        vbuf = dpipe->out;
        dpipe->out = vbuf->next;
        vbuf->next = NULL;
        if(dpipe->out == NULL)
            dpipe->out_tail = NULL;
        dpipe->out_count--;
    }
    //
    return vbuf;
}

/**
 * Store a frame into the output pool of the pipe.
 * This function also notifies the receiver that is attempting to load a buffer.
 *
 * @param dpipe [in] The involved pipe
 * @param buffer [in] Pointer to the buffer to be stored.
 */
void
dpipe_store(dpipe_t *dpipe, dpipe_buffer_t *buffer) {
    std::lock_guard<std::mutex> lock(dpipe->io_mutex);
    // put at the end
    if(dpipe->out_tail != NULL) {
        dpipe->out_tail->next = buffer;
        dpipe->out_tail = buffer;
    } else {
        dpipe->out = dpipe->out_tail = buffer;
    }
    buffer->next = NULL;
    dpipe->out_count++;
    //
    dpipe->cond.notify_one();
    return;
}

