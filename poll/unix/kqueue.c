/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "apr_arch_poll_private.h"

#ifdef POLLSET_USES_KQUEUE

static apr_int16_t get_kqueue_revent(apr_int16_t event, apr_int16_t flags)
{
    apr_int16_t rv = 0;

    if (event == EVFILT_READ)
        rv |= APR_POLLIN;
    if (event == EVFILT_WRITE)
        rv |= APR_POLLOUT;
    if (flags & EV_EOF)
        rv |= APR_POLLHUP;
    if (flags & EV_ERROR)
        rv |= APR_POLLERR;

    return rv;
}

struct apr_pollset_t
{
    apr_pool_t *pool;
    apr_uint32_t nelts;
    apr_uint32_t nalloc;
    int kqueue_fd;
    struct kevent kevent;
    struct kevent *ke_set;
    apr_pollfd_t *result_set;
    apr_uint32_t flags;
    /* Pipe descriptors used for wakeup */
    apr_file_t *wakeup_pipe[2];
#if APR_HAS_THREADS
    /* A thread mutex to protect operations on the rings */
    apr_thread_mutex_t *ring_lock;
#endif
    /* A ring containing all of the pollfd_t that are active */
    APR_RING_HEAD(pfd_query_ring_t, pfd_elem_t) query_ring;
    /* A ring of pollfd_t that have been used, and then _remove'd */
    APR_RING_HEAD(pfd_free_ring_t, pfd_elem_t) free_ring;
    /* A ring of pollfd_t where rings that have been _remove'd but
       might still be inside a _poll */
    APR_RING_HEAD(pfd_dead_ring_t, pfd_elem_t) dead_ring;
};

static apr_status_t backend_cleanup(void *p_)
{
    apr_pollset_t *pollset = (apr_pollset_t *) p_;
    close(pollset->kqueue_fd);
    if (pollset->flags & APR_POLLSET_WAKEABLE) {
        /* Close both sides of the wakeup pipe */
        if (pollset->wakeup_pipe[0]) {
            apr_file_close(pollset->wakeup_pipe[0]);
            pollset->wakeup_pipe[0] = NULL;
        }
        if (pollset->wakeup_pipe[1]) {
            apr_file_close(pollset->wakeup_pipe[1]);
            pollset->wakeup_pipe[1] = NULL;
        }
    }
    return APR_SUCCESS;
}

/* Create a dummy wakeup pipe for interrupting the poller
 */
static apr_status_t create_wakeup_pipe(apr_pollset_t *pollset)
{
    apr_status_t rv;
    apr_pollfd_t fd;

    if ((rv = apr_file_pipe_create(&pollset->wakeup_pipe[0],
                                   &pollset->wakeup_pipe[1],
                                   pollset->pool)) != APR_SUCCESS)
        return rv;
    fd.reqevents = APR_POLLIN;
    fd.desc_type = APR_POLL_FILE;
    fd.desc.f = pollset->wakeup_pipe[0];
    /* Add the pipe to the pollset
     */
    return apr_pollset_add(pollset, &fd);
}

/* Read and discard what's ever in the wakeup pipe.
 */
static void drain_wakeup_pipe(apr_pollset_t *pollset)
{
    char rb[512];
    apr_size_t nr = sizeof(rb);

    while (apr_file_read(pollset->wakeup_pipe[0], rb, &nr) == APR_SUCCESS) {
        /* Although we write just one byte to the other end of the pipe
         * during wakeup, multiple treads could call the wakeup.
         * So simply drain out from the input side of the pipe all
         * the data.
         */
        if (nr != sizeof(rb))
            break;
    }
}

APR_DECLARE(apr_status_t) apr_pollset_create(apr_pollset_t **pollset,
                                             apr_uint32_t size,
                                             apr_pool_t *p,
                                             apr_uint32_t flags)
{
    apr_status_t rv = APR_SUCCESS;
    *pollset = apr_palloc(p, sizeof(**pollset));
#if APR_HAS_THREADS
    if (flags & APR_POLLSET_THREADSAFE &&
        ((rv = apr_thread_mutex_create(&(*pollset)->ring_lock,
                                       APR_THREAD_MUTEX_DEFAULT,
                                       p)) != APR_SUCCESS)) {
        *pollset = NULL;
        return rv;
    }
#else
    if (flags & APR_POLLSET_THREADSAFE) {
        *pollset = NULL;
        return APR_ENOTIMPL;
    }
#endif
    if (flags & APR_POLLSET_WAKEABLE) {
        /* Add room for wakeup descriptor */
        size++;
    }

    (*pollset)->nelts = 0;
    (*pollset)->nalloc = size;
    (*pollset)->flags = flags;
    (*pollset)->pool = p;

    (*pollset)->ke_set =
        (struct kevent *) apr_palloc(p, size * sizeof(struct kevent));

    memset((*pollset)->ke_set, 0, size * sizeof(struct kevent));

    (*pollset)->kqueue_fd = kqueue();

    if ((*pollset)->kqueue_fd == -1) {
        return apr_get_netos_error();
    }

    (*pollset)->result_set = apr_palloc(p, size * sizeof(apr_pollfd_t));

    APR_RING_INIT(&(*pollset)->query_ring, pfd_elem_t, link);
    APR_RING_INIT(&(*pollset)->free_ring, pfd_elem_t, link);
    APR_RING_INIT(&(*pollset)->dead_ring, pfd_elem_t, link);
    if (flags & APR_POLLSET_WAKEABLE) {
        /* Create wakeup pipe */
        if ((rv = create_wakeup_pipe(*pollset)) != APR_SUCCESS) {
            close((*pollset)->kqueue_fd);
            *pollset = NULL;
            return rv;
        }
    }
    apr_pool_cleanup_register(p, (void *) (*pollset), backend_cleanup,
                              apr_pool_cleanup_null);

    return rv;
}

APR_DECLARE(apr_status_t) apr_pollset_destroy(apr_pollset_t * pollset)
{
    return apr_pool_cleanup_run(pollset->pool, pollset, backend_cleanup);
}

APR_DECLARE(apr_status_t) apr_pollset_add(apr_pollset_t *pollset,
                                          const apr_pollfd_t *descriptor)
{
    apr_os_sock_t fd;
    pfd_elem_t *elem;
    apr_status_t rv = APR_SUCCESS;

    pollset_lock_rings();

    if (!APR_RING_EMPTY(&(pollset->free_ring), pfd_elem_t, link)) {
        elem = APR_RING_FIRST(&(pollset->free_ring));
        APR_RING_REMOVE(elem, link);
    }
    else {
        elem = (pfd_elem_t *) apr_palloc(pollset->pool, sizeof(pfd_elem_t));
        APR_RING_ELEM_INIT(elem, link);
    }
    elem->pfd = *descriptor;

    if (descriptor->desc_type == APR_POLL_SOCKET) {
        fd = descriptor->desc.s->socketdes;
    }
    else {
        fd = descriptor->desc.f->filedes;
    }

    if (descriptor->reqevents & APR_POLLIN) {
        EV_SET(&pollset->kevent, fd, EVFILT_READ, EV_ADD, 0, 0, elem);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = apr_get_netos_error();
        }
    }

    if (descriptor->reqevents & APR_POLLOUT && rv == APR_SUCCESS) {
        EV_SET(&pollset->kevent, fd, EVFILT_WRITE, EV_ADD, 0, 0, elem);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = apr_get_netos_error();
        }
    }

    if (rv == APR_SUCCESS) {
        pollset->nelts++;
        APR_RING_INSERT_TAIL(&(pollset->query_ring), elem, pfd_elem_t, link);
    }
    else {
        APR_RING_INSERT_TAIL(&(pollset->free_ring), elem, pfd_elem_t, link);
    }

    pollset_unlock_rings();

    return rv;
}

APR_DECLARE(apr_status_t) apr_pollset_remove(apr_pollset_t *pollset,
                                             const apr_pollfd_t *descriptor)
{
    pfd_elem_t *ep;
    apr_status_t rv = APR_SUCCESS;
    apr_os_sock_t fd;

    pollset_lock_rings();

    if (descriptor->desc_type == APR_POLL_SOCKET) {
        fd = descriptor->desc.s->socketdes;
    }
    else {
        fd = descriptor->desc.f->filedes;
    }

    if (descriptor->reqevents & APR_POLLIN) {
        EV_SET(&pollset->kevent, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = APR_NOTFOUND;
        }
    }

    if (descriptor->reqevents & APR_POLLOUT && rv == APR_SUCCESS) {
        EV_SET(&pollset->kevent, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

        if (kevent(pollset->kqueue_fd, &pollset->kevent, 1, NULL, 0,
                   NULL) == -1) {
            rv = APR_NOTFOUND;
        }
    }

    if (!APR_RING_EMPTY(&(pollset->query_ring), pfd_elem_t, link)) {
        for (ep = APR_RING_FIRST(&(pollset->query_ring));
             ep != APR_RING_SENTINEL(&(pollset->query_ring),
                                     pfd_elem_t, link);
             ep = APR_RING_NEXT(ep, link)) {

            if (descriptor->desc.s == ep->pfd.desc.s) {
                APR_RING_REMOVE(ep, link);
                APR_RING_INSERT_TAIL(&(pollset->dead_ring),
                                     ep, pfd_elem_t, link);
                break;
            }
        }
    }

    pollset_unlock_rings();

    return rv;
}

APR_DECLARE(apr_status_t) apr_pollset_poll(apr_pollset_t *pollset,
                                           apr_interval_time_t timeout,
                                           apr_int32_t *num,
                                           const apr_pollfd_t **descriptors)
{
    int ret, i, j;
    struct timespec tv, *tvptr;
    apr_status_t rv = APR_SUCCESS;
    apr_pollfd_t fd;

    if (timeout < 0) {
        tvptr = NULL;
    }
    else {
        tv.tv_sec = (long) apr_time_sec(timeout);
        tv.tv_nsec = (long) apr_time_usec(timeout) * 1000;
        tvptr = &tv;
    }

    ret = kevent(pollset->kqueue_fd, NULL, 0, pollset->ke_set, pollset->nalloc,
                tvptr);
    (*num) = ret;
    if (ret < 0) {
        rv = apr_get_netos_error();
    }
    else if (ret == 0) {
        rv = APR_TIMEUP;
    }
    else {
        for (i = 0, j = 0; i < ret; i++) {
            fd = (((pfd_elem_t*)(pollset->ke_set[i].udata))->pfd);
            if ((pollset->flags & APR_POLLSET_WAKEABLE) &&
                fd.desc_type == APR_POLL_FILE &&
                fd.desc.f == pollset->wakeup_pipe[0]) {
                drain_wakeup_pipe(pollset);
                rv = APR_EINTR;
            }
            else {
                pollset->result_set[j] = fd;
                pollset->result_set[j].rtnevents =
                        get_kqueue_revent(pollset->ke_set[i].filter,
                                          pollset->ke_set[i].flags);
                j++;
            }
        }
        if ((*num) = j)
            rv = APR_SUCCESS;
        if (descriptors) {
            *descriptors = pollset->result_set;
        }
    }


    pollset_lock_rings();

    /* Shift all PFDs in the Dead Ring to be Free Ring */
    APR_RING_CONCAT(&(pollset->free_ring), &(pollset->dead_ring), pfd_elem_t, link);

    pollset_unlock_rings();

    return rv;
}

APR_DECLARE(apr_status_t) apr_pollset_wakeup(apr_pollset_t *pollset)
{
    if (pollset->flags & APR_POLLSET_WAKEABLE)
        return apr_file_putc(1, pollset->wakeup_pipe[1]);
    else
        return APR_EINIT;
}

struct apr_pollcb_t {
    apr_pool_t *pool;
    apr_uint32_t nalloc;
    struct kevent *pollset;
    int kqfd;
};

static apr_status_t cb_cleanup(void *b_)
{
    apr_pollcb_t *pollcb = (apr_pollcb_t *) b_;
    close(pollcb->kqfd);
    return APR_SUCCESS;
}


APR_DECLARE(apr_status_t) apr_pollcb_create(apr_pollcb_t **pollcb,
                                            apr_uint32_t size,
                                            apr_pool_t *p,
                                            apr_uint32_t flags)
{
    int fd;
    
    fd = kqueue();
    if (fd < 0) {
        *pollcb = NULL;
        return apr_get_netos_error();
    }
    
    *pollcb = apr_palloc(p, sizeof(**pollcb));
    (*pollcb)->nalloc = size;
    (*pollcb)->pool = p;
    (*pollcb)->kqfd = fd;
    (*pollcb)->pollset = (struct kevent *)apr_pcalloc(p, size * sizeof(struct kevent));
    apr_pool_cleanup_register(p, *pollcb, cb_cleanup, cb_cleanup);
    
    return APR_SUCCESS;
}

APR_DECLARE(apr_status_t) apr_pollcb_add(apr_pollcb_t *pollcb,
                                         apr_pollfd_t *descriptor)
{
    apr_os_sock_t fd;
    struct kevent ev;
    apr_status_t rv = APR_SUCCESS;
    
    if (descriptor->desc_type == APR_POLL_SOCKET) {
        fd = descriptor->desc.s->socketdes;
    }
    else {
        fd = descriptor->desc.f->filedes;
    }
    
    if (descriptor->reqevents & APR_POLLIN) {
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, descriptor);
        
        if (kevent(pollcb->kqfd, &ev, 1, NULL, 0, NULL) == -1) {
            rv = apr_get_netos_error();
        }
    }
    
    if (descriptor->reqevents & APR_POLLOUT && rv == APR_SUCCESS) {
        EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD, 0, 0, descriptor);
        
        if (kevent(pollcb->kqfd, &ev, 1, NULL, 0, NULL) == -1) {
            rv = apr_get_netos_error();
        }
    }
    
    return rv;
}

APR_DECLARE(apr_status_t) apr_pollcb_remove(apr_pollcb_t *pollcb,
                                            apr_pollfd_t *descriptor)
{
    apr_status_t rv = APR_SUCCESS;
    struct kevent ev;
    apr_os_sock_t fd;
    
    if (descriptor->desc_type == APR_POLL_SOCKET) {
        fd = descriptor->desc.s->socketdes;
    }
    else {
        fd = descriptor->desc.f->filedes;
    }
    
    if (descriptor->reqevents & APR_POLLIN) {
        EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        
        if (kevent(pollcb->kqfd, &ev, 1, NULL, 0, NULL) == -1) {
            rv = APR_NOTFOUND;
        }
    }
    
    if (descriptor->reqevents & APR_POLLOUT && rv == APR_SUCCESS) {
        /* XXXX: this is less than optimal, shouldn't we still try to 
         *        remove the FD even if it wasn't in the readset?
         */
        EV_SET(&ev, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        
        if (kevent(pollcb->kqfd, &ev, 1, NULL, 0, NULL) == -1) {
            rv = APR_NOTFOUND;
        }
    }
    
    return rv;
}


APR_DECLARE(apr_status_t) apr_pollcb_poll(apr_pollcb_t *pollcb,
                                          apr_interval_time_t timeout,
                                          apr_pollcb_cb_t func,
                                          void *baton)
{
    int ret, i;
    struct timespec tv, *tvptr;
    apr_status_t rv = APR_SUCCESS;
    
    if (timeout < 0) {
        tvptr = NULL;
    }
    else {
        tv.tv_sec = (long) apr_time_sec(timeout);
        tv.tv_nsec = (long) apr_time_usec(timeout) * 1000;
        tvptr = &tv;
    }
    
    ret = kevent(pollcb->kqfd, NULL, 0, pollcb->pollset, pollcb->nalloc,
                 tvptr);

    if (ret < 0) {
        rv = apr_get_netos_error();
    }
    else if (ret == 0) {
        rv = APR_TIMEUP;
    }
    else {
        for (i = 0; i < ret; i++) {
            apr_pollfd_t *pollfd = (apr_pollfd_t *)(pollcb->pollset[i].udata);
            
            pollfd->rtnevents = get_kqueue_revent(pollcb->pollset[i].filter,
                                                  pollcb->pollset[i].flags);
            
            rv = func(baton, pollfd);
            
            if (rv) {
                return rv;
            }
        }
    }

    return rv;
}

#endif /* POLLSET_USES_KQUEUE */