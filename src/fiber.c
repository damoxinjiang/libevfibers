/********************************************************************

  Copyright 2012 Konstantin Olkhovskiy <lupus@oxnull.net>

  This file is part of libevfibers.

  libevfibers is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or any later version.

  libevfibers is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General
  Public License along with libevfibers.  If not, see
  <http://www.gnu.org/licenses/>.

 ********************************************************************/

#include <sys/mman.h>
#include <assert.h>
#include <errno.h>
#include <utlist.h>
#include <stdio.h>
#include <err.h>
#include <valgrind/valgrind.h>

#include <evfibers_private/fiber.h>

#define ENSURE_ROOT_FIBER do {					\
	assert(fctx->__p->sp->fiber == &fctx->__p->root);	\
} while (0);

#define CURRENT_FIBER (fctx->__p->sp->fiber)
#define CALLED_BY_ROOT ((fctx->__p->sp - 1)->fiber == &fctx->__p->root)

static void mutex_async_cb(EV_P_ ev_async *w, _unused_ int revents)
{
	struct fbr_context *fctx;
	struct fbr_mutex *mutex;
	fctx = (struct fbr_context *)w->data;

	ENSURE_ROOT_FIBER;
	TAILQ_FOREACH(mutex, &fctx->__p->mutexes, entries) {
		TAILQ_REMOVE(&fctx->__p->mutexes, mutex, entries);
		if (TAILQ_EMPTY(&fctx->__p->mutexes))
			ev_async_stop(EV_A_ &fctx->__p->mutex_async);

		fbr_call_noinfo(FBR_A_ mutex->locked_by, 0);
	}
}

static void pending_async_cb(EV_P_ ev_async *w, _unused_ int revents)
{
	struct fbr_context *fctx;
	struct fbr_fiber *fiber;
	fctx = (struct fbr_context *)w->data;

	ENSURE_ROOT_FIBER;

	if (TAILQ_EMPTY(&fctx->__p->pending_fibers)) {
		ev_async_stop(EV_A_ &fctx->__p->pending_async);
		return;
	}

	fiber = TAILQ_FIRST(&fctx->__p->pending_fibers);
	TAILQ_REMOVE(&fctx->__p->pending_fibers, fiber, entries.call_pending);
	fbr_call_noinfo(FBR_A_ fiber, 0);
	if (TAILQ_EMPTY(&fctx->__p->pending_fibers))
		ev_async_stop(EV_A_ &fctx->__p->pending_async);
	else
		ev_async_send(EV_A_ &fctx->__p->pending_async);
}

static void *allocate_in_fiber(FBR_P_ size_t size, struct fbr_fiber *in)
{
	struct fbr_mem_pool *pool_entry;
	pool_entry = malloc(size + sizeof(struct fbr_mem_pool));
	if (NULL == pool_entry) {
		fbr_log_e(FBR_A_ "libevfibers: unable to allocate %zu bytes\n",
				size + sizeof(struct fbr_mem_pool));
		abort();
	}
	pool_entry->ptr = pool_entry;
	pool_entry->destructor = NULL;
	pool_entry->destructor_context = NULL;
	DL_APPEND(in->pool, pool_entry);
	return pool_entry + 1;
}

static void stdio_logger(struct fbr_logger *logger, enum fbr_log_level level,
		const char *format, va_list ap)
{
	if (level > logger->level)
		return;

	switch (level) {
		case FBR_LOG_ERROR:
			fprintf(stderr, "ERROR ");
			vfprintf(stderr, format, ap);
			fprintf(stderr, "\n");
			break;
		case FBR_LOG_WARNING:
			fprintf(stdout, "WARNING ");
			vfprintf(stdout, format, ap);
			fprintf(stderr, "\n");
			break;
		case FBR_LOG_NOTICE:
			fprintf(stdout, "NOTICE ");
			vfprintf(stdout, format, ap);
			fprintf(stderr, "\n");
			break;
		case FBR_LOG_INFO:
			fprintf(stdout, "INFO ");
			vfprintf(stdout, format, ap);
			fprintf(stderr, "\n");
			break;
		case FBR_LOG_DEBUG:
			fprintf(stdout, "DEBUG ");
			vfprintf(stdout, format, ap);
			fprintf(stderr, "\n");
			break;
		default:
			fprintf(stdout, "????? ");
			vfprintf(stdout, format, ap);
			fprintf(stderr, "\n");
			break;
	}
}

void fbr_init(FBR_P_ struct ev_loop *loop)
{
	struct fbr_fiber *root;
	struct fbr_logger *logger;

	fctx->__p = malloc(sizeof(struct fbr_context_private));
	LIST_INIT(&fctx->__p->reclaimed);
	LIST_INIT(&fctx->__p->root.children);
	TAILQ_INIT(&fctx->__p->mutexes);
	TAILQ_INIT(&fctx->__p->pending_fibers);

	root = &fctx->__p->root;
	root->name = "root";
	root->pool = NULL;
	coro_create(&root->ctx, NULL, NULL, NULL, 0);

	logger = allocate_in_fiber(FBR_A_ sizeof(struct fbr_logger), root);
	logger->logv = stdio_logger;
	logger->level = FBR_LOG_NOTICE;
	fctx->logger = logger;

	fctx->__p->sp = fctx->__p->stack;
	fctx->__p->sp->fiber = root;
	fill_trace_info(FBR_A_ &fctx->__p->sp->tinfo);
	fctx->__p->loop = loop;
	fctx->__p->mutex_async.data = fctx;
	fctx->__p->pending_async.data = fctx;
	fctx->__p->backtraces_enabled = 0;
	ev_async_init(&fctx->__p->mutex_async, mutex_async_cb);
	ev_async_init(&fctx->__p->pending_async, pending_async_cb);
}

const char *fbr_strerror(_unused_ FBR_P_ enum fbr_error_code code)
{
	switch (code) {
		case FBR_SUCCESS:
			return "Success";
		case FBR_EINVAL:
			return "Invalid argument";
		case FBR_ENOFIBER:
			return "No such fiber";
	}
	return "Unknown error";
}

void fbr_log_e(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(fctx->logger, FBR_LOG_DEBUG, format, ap);
	va_end(ap);
}

void fbr_log_w(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(fctx->logger, FBR_LOG_DEBUG, format, ap);
	va_end(ap);
}

void fbr_log_n(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(fctx->logger, FBR_LOG_DEBUG, format, ap);
	va_end(ap);
}

void fbr_log_i(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(fctx->logger, FBR_LOG_DEBUG, format, ap);
	va_end(ap);
}

void fbr_log_d(FBR_P_ const char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	(*fctx->logger->logv)(fctx->logger, FBR_LOG_DEBUG, format, ap);
	va_end(ap);
}

static void reclaim_children(FBR_P_ struct fbr_fiber *fiber)
{
	struct fbr_fiber *child;
	LIST_FOREACH(child, &fiber->children, entries.children) {
		fbr_reclaim(FBR_A_ child);
	}
}

void fbr_destroy(FBR_P)
{
	struct fbr_fiber *fiber, *tmp = NULL;

	ev_async_stop(fctx->__p->loop, &fctx->__p->mutex_async);

	reclaim_children(FBR_A_ &fctx->__p->root);

	LIST_FOREACH(fiber, &fctx->__p->reclaimed, entries.reclaimed) {
		if (0 != munmap(fiber->stack, fiber->stack_size))
			err(EXIT_FAILURE, "munmap");
		if (tmp)
			free(tmp);
		tmp = fiber;
	}
	if (tmp)
		free(tmp);

	free(fctx->__p);
}

void fbr_enable_backtraces(FBR_P_ int enabled)
{
	if (enabled)
		fctx->__p->backtraces_enabled = 1;
	else
		fctx->__p->backtraces_enabled = 0;

}

static void ev_wakeup_io(_unused_ EV_P_ ev_io *w, _unused_ int event)
{
	struct fbr_context *fctx;
	struct fbr_fiber *fiber;
	fiber = container_of(w, struct fbr_fiber, w_io);
	fctx = (struct fbr_context *)w->data;

	ENSURE_ROOT_FIBER;
	if (1 != fiber->w_io_expected) {
		fbr_log_e(FBR_A_ "libevfibers: fiber ``%s'' is about to be"
				" woken up by an io event but it does not"
				" expect this.", fiber->name);
		fbr_log_e(FBR_A_ "libevfibers: last registered io request for "
				"this fiber was:");
		fbr_log_e(FBR_A_ "--- begin trace ---");
		print_trace_info(FBR_A_ &fiber->w_io_tinfo, fbr_log_e);
		fbr_log_e(FBR_A_ "--- end trace ---");
		abort();
	}

	fbr_call_noinfo(FBR_A_ fiber, 0);
}

static void ev_wakeup_timer(_unused_ EV_P_ ev_timer *w, _unused_ int event)
{
	struct fbr_context *fctx;
	struct fbr_fiber *fiber;
	fiber = container_of(w, struct fbr_fiber, w_timer);
	fctx = (struct fbr_context *)w->data;

	ENSURE_ROOT_FIBER;
	if (1 != fiber->w_timer_expected) {
		fbr_log_e(FBR_A_ "libevfibers: fiber ``%s'' is about to be"
				" woken up by a timer event but it does not"
				" expect this.", fiber->name);
		fbr_log_e(FBR_A_ "libevfibers: last registered timer request"
				" for this fiber was:");
		fbr_log_e(FBR_A_ "--- begin trace ---");
		print_trace_info(FBR_A_ &fiber->w_io_tinfo, fbr_log_e);
		fbr_log_e(FBR_A_ "--- end trace ---");
		abort();
	}

	fbr_call_noinfo(FBR_A_ fiber, 0);
}

static void fbr_free_in_fiber(_unused_ FBR_P_ struct fbr_fiber *fiber, void *ptr)
{
	struct fbr_mem_pool *pool_entry = NULL;
	if (NULL == ptr)
		return;
	pool_entry = (struct fbr_mem_pool *)ptr - 1;
	if (pool_entry->ptr != pool_entry) {
		fbr_log_e(FBR_A_ "libevfibers: address %p does not look like "
				"fiber memory pool entry", ptr);
		if (!RUNNING_ON_VALGRIND)
			abort();
	}
	DL_DELETE(fiber->pool, pool_entry);
	if (pool_entry->destructor)
		pool_entry->destructor(ptr, pool_entry->destructor_context);
	free(pool_entry);
}

static void fiber_cleanup(FBR_P_ struct fbr_fiber *fiber)
{
	struct fbr_mem_pool *p_elt, *p_tmp;
	/* coro_destroy(&fiber->ctx); */
	ev_io_stop(fctx->__p->loop, &fiber->w_io);
	ev_timer_stop(fctx->__p->loop, &fiber->w_timer);
	DL_FOREACH_SAFE(fiber->pool, p_elt, p_tmp) {
		fbr_free_in_fiber(FBR_A_ fiber, p_elt + 1);
	}
}

void fbr_reclaim(FBR_P_ struct fbr_fiber *fiber)
{
	if (fiber->reclaimed)
		return;
	fill_trace_info(FBR_A_ &fiber->reclaim_tinfo);
	reclaim_children(FBR_A_ fiber);
	fiber_cleanup(FBR_A_ fiber);
	fiber->reclaimed = 1;
	LIST_INSERT_HEAD(&fctx->__p->reclaimed, fiber, entries.reclaimed);
}

int fbr_is_reclaimed(_unused_ FBR_P_ struct fbr_fiber *fiber)
{
	return fiber->reclaimed;
}

static void fiber_prepare(FBR_P_ struct fbr_fiber *fiber)
{
	ev_init(&fiber->w_io, ev_wakeup_io);
	ev_init(&fiber->w_timer, ev_wakeup_timer);
	fiber->w_io.data = FBR_A;
	fiber->w_timer.data = FBR_A;
	fiber->reclaimed = 0;
}

static void call_wrapper(FBR_P_ _unused_ void (*func) (FBR_P))
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	fiber_prepare(FBR_A_ fiber);

	fiber->func(FBR_A);

	fbr_reclaim(FBR_A_ fiber);
	fbr_yield(FBR_A);
}

struct fbr_fiber_arg fbr_arg_i(int i)
{
	struct fbr_fiber_arg arg;
	arg.i = i;
	return arg;
}

struct fbr_fiber_arg fbr_arg_v(void *v)
{
	struct fbr_fiber_arg arg;
	arg.v = v;
	return arg;
}

int fbr_vcall(FBR_P_ struct fbr_fiber *callee, int leave_info, int
		argnum, va_list ap)
{
	struct fbr_fiber *caller = fctx->__p->sp->fiber;
	int i;
	struct fbr_call_info *info;

	if (argnum >= FBR_MAX_ARG_NUM) {
		fbr_log_n(FBR_A_ "libevfibers: attempt to pass %d argumens"
				" while FBR_MAX_ARG_NUM is %d", argnum,
				FBR_MAX_ARG_NUM);
		fctx->f_errno = FBR_EINVAL;
		return -1;
	}

	if (1 == callee->reclaimed) {
		fbr_log_n(FBR_A_ "libevfibers: fiber %p is about to be called "
				"but it was reclaimed here:", callee);
		print_trace_info(FBR_A_ &callee->reclaim_tinfo, fbr_log_n);
		fctx->f_errno = FBR_ENOFIBER;
		return -1;
	}

	fctx->__p->sp++;

	fctx->__p->sp->fiber = callee;
	fill_trace_info(FBR_A_ &fctx->__p->sp->tinfo);

	if (0 == leave_info) {
		coro_transfer(&caller->ctx, &callee->ctx);
		fctx->f_errno = FBR_SUCCESS;
		return 0;
	}

	info = fbr_alloc(FBR_A_ sizeof(struct fbr_call_info));
	info->caller = caller;
	info->argc = argnum;
	for (i = 0; i < argnum; i++)
		info->argv[i] = va_arg(ap, struct fbr_fiber_arg);

	DL_APPEND(callee->call_list, info);
	callee->call_list_size++;
	if (callee->call_list_size >= FBR_CALL_LIST_WARN) {
		fbr_log_n(FBR_A_ "libevfibers: call list for ``%s'' contains"
				" %zu elements, which looks suspicious. Is"
				" anyone fetching the calls?", callee->name,
				callee->call_list_size);
		fbr_dump_stack(FBR_A_ fbr_log_n);
	}

	coro_transfer(&caller->ctx, &callee->ctx);

	fctx->f_errno = FBR_SUCCESS;
	return 0;
}

int fbr_call_noinfo(FBR_P_ struct fbr_fiber *callee, int argnum, ...)
{
	int retval;
	va_list ap;
	va_start(ap, argnum);
	retval = fbr_vcall(FBR_A_ callee, 0, argnum, ap);
	va_end(ap);
	return retval;
}

int fbr_call(FBR_P_ struct fbr_fiber *callee, int argnum, ...)
{
	int retval;
	va_list ap;
	va_start(ap, argnum);
	retval = fbr_vcall(FBR_A_ callee, 1, argnum, ap);
	va_end(ap);
	return retval;
}

int fbr_next_call_info(FBR_P_ struct fbr_call_info **info_ptr)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	struct fbr_call_info *tmp;

	if (NULL == fiber->call_list)
		return 0;
	tmp = fiber->call_list;
	DL_DELETE(fiber->call_list, fiber->call_list);
	fiber->call_list_size--;

	if (NULL == info_ptr)
		fbr_free(FBR_A_ tmp);
	else {
		if (NULL != *info_ptr)
			fbr_free(FBR_A_ *info_ptr);
		*info_ptr = tmp;
	}
	return 1;
}

void fbr_yield(FBR_P)
{
	struct fbr_fiber *callee = fctx->__p->sp->fiber;
	struct fbr_fiber *caller = (--fctx->__p->sp)->fiber;
	coro_transfer(&callee->ctx, &caller->ctx);
}

static void io_start(FBR_P_ struct fbr_fiber *fiber, int fd, int events)
{
	ev_io_set(&fiber->w_io, fd, events);
	ev_io_start(fctx->__p->loop, &fiber->w_io);
	assert(0 == fiber->w_io_expected);
	fiber->w_io_expected = 1;
	fill_trace_info(FBR_A_ &fiber->w_io_tinfo);
}

static void io_stop(FBR_P_ struct fbr_fiber *fiber)
{
	assert(1 == fiber->w_io_expected);
	fiber->w_io_expected = 0;
	ev_io_stop(fctx->__p->loop, &fiber->w_io);
}

ssize_t fbr_read(FBR_P_ int fd, void *buf, size_t count)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	ssize_t r;

	io_start(FBR_A_ fiber, fd, EV_READ);
	fbr_yield(FBR_A);
	if (!CALLED_BY_ROOT) {
		errno = EINTR;
		r = -1;
		goto finish;
	}
	do {
		r = read(fd, buf, count);
	} while (-1 == r && EINTR == errno);

finish:
	io_stop(FBR_A_ fiber);
	return r;
}

ssize_t fbr_read_all(FBR_P_ int fd, void *buf, size_t count)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	ssize_t r;
	size_t done = 0;

	io_start(FBR_A_ fiber, fd, EV_READ);
	while (count != done) {
next:
		fbr_yield(FBR_A);
		if (!CALLED_BY_ROOT)
			continue;
		for (;;) {
			r = read(fd, buf + done, count - done);
			if (-1 == r) {
				switch (errno) {
					case EINTR:
						continue;
					case EAGAIN:
						goto next;
					default:
						goto error;
				}
			}
			break;
		}
		if (0 == r)
			break;
		done += r;
	}
	io_stop(FBR_A_ fiber);
	return (ssize_t)done;

error:
	io_stop(FBR_A_ fiber);
	return -1;
}

ssize_t fbr_readline(FBR_P_ int fd, void *buffer, size_t n)
{
	ssize_t num_read;                    /* # of bytes fetched by last read() */
	size_t total_read;                     /* Total bytes read so far */
	char *buf;
	char ch;

	if (n <= 0 || buffer == NULL) {
		errno = EINVAL;
		return -1;
	}

	buf = buffer;                       /* No pointer arithmetic on "void *" */

	total_read = 0;
	for (;;) {
		num_read = fbr_read(FBR_A_ fd, &ch, 1);

		if (num_read == -1) {
			if (errno == EINTR)         /* Interrupted --> restart read() */
				continue;
			else
				return -1;              /* Some other error */

		} else if (num_read == 0) {      /* EOF */
			if (total_read == 0)           /* No bytes read; return 0 */
				return 0;
			else                        /* Some bytes read; add '\0' */
				break;

		} else {                        /* 'numRead' must be 1 if we get here */
			if (total_read < n - 1) {      /* Discard > (n - 1) bytes */
				total_read++;
				*buf++ = ch;
			}

			if (ch == '\n')
				break;
		}
	}

	*buf = '\0';
	return total_read;
}

ssize_t fbr_write(FBR_P_ int fd, const void *buf, size_t count)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	ssize_t r;

	io_start(FBR_A_ fiber, fd, EV_WRITE);
	fbr_yield(FBR_A);
	if (!CALLED_BY_ROOT) {
		errno = EINTR;
		r = -1;
		goto finish;
	}
	do {
		r = write(fd, buf, count);
	} while (-1 == r && EINTR == errno);

finish:
	io_stop(FBR_A_ fiber);
	return r;
}

ssize_t fbr_write_all(FBR_P_ int fd, const void *buf, size_t count)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	ssize_t r;
	size_t done = 0;

	io_start(FBR_A_ fiber, fd, EV_WRITE);
	while (count != done) {
next:
		fbr_yield(FBR_A);
		if (!CALLED_BY_ROOT)
			continue;
		for (;;) {
			r = write(fd, buf + done, count - done);
			if (-1 == r) {
				switch (errno) {
					case EINTR:
						continue;
					case EAGAIN:
						goto next;
					default:
						goto error;
				}
			}
			break;
		}
		done += r;
	}
	io_stop(FBR_A_ fiber);
	return (ssize_t)done;

error:
	io_stop(FBR_A_ fiber);
	return -1;
}

ssize_t fbr_recvfrom(FBR_P_ int sockfd, void *buf, size_t len, int flags,
		struct sockaddr *src_addr, socklen_t *addrlen)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	int nbytes;

	io_start(FBR_A_ fiber, sockfd, EV_READ);
	fbr_yield(FBR_A);
	io_stop(FBR_A_ fiber);
	if (CALLED_BY_ROOT) {
		nbytes = recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
		return nbytes;
	}
	errno = EINTR;
	return -1;
}

ssize_t fbr_sendto(FBR_P_ int sockfd, const void *buf, size_t len, int flags, const
		struct sockaddr *dest_addr, socklen_t addrlen)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	int nbytes;

	io_start(FBR_A_ fiber, sockfd, EV_WRITE);
	fbr_yield(FBR_A);
	io_stop(FBR_A_ fiber);
	if (CALLED_BY_ROOT) {
		nbytes = sendto(sockfd, buf, len, flags, dest_addr, addrlen);
		return nbytes;
	}
	errno = EINTR;
	return -1;
}

int fbr_accept(FBR_P_ int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	int r;

	io_start(FBR_A_ fiber, sockfd, EV_READ);
	fbr_yield(FBR_A);
	if (!CALLED_BY_ROOT) {
		io_stop(FBR_A_ fiber);
		errno = EINTR;
		return -1;
	}
	do {
		r = accept(sockfd, addr, addrlen);
	} while (-1 == r && EINTR == errno);

	io_stop(FBR_A_ fiber);

	return r;
}

static void timer_start(FBR_P_ struct fbr_fiber *fiber, ev_tstamp timeout,
		ev_tstamp repeat)
{
	ev_timer_set(&fiber->w_timer, timeout, repeat);
	ev_timer_start(fctx->__p->loop, &fiber->w_timer);
	fiber->w_timer_expected = 1;
	fill_trace_info(FBR_A_ &fiber->w_timer_tinfo);
}

static void timer_stop(FBR_P_ struct fbr_fiber *fiber)
{
	fiber->w_timer_expected = 0;
	ev_timer_stop(fctx->__p->loop, &fiber->w_timer);
}

ev_tstamp fbr_sleep(FBR_P_ ev_tstamp seconds)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	ev_tstamp expected = ev_now(fctx->__p->loop) + seconds;
	timer_start(FBR_A_ fiber, seconds, 0.);
	fbr_yield(FBR_A);
	timer_stop(FBR_A_ fiber);
	return max(0., expected - ev_now(fctx->__p->loop));
}

static size_t round_up_to_page_size(size_t size)
{
	static long sz;
	size_t remainder;
	if (0 == sz)
		sz = sysconf(_SC_PAGESIZE);
	remainder = size % sz;
	if (remainder == 0)
		return size;
	return size + sz - remainder;
}

struct fbr_fiber *fbr_create(FBR_P_ const char *name, void (*func) (FBR_P),
		size_t stack_size)
{
	struct fbr_fiber *fiber;
	if (!LIST_EMPTY(&fctx->__p->reclaimed)) {
		fiber = LIST_FIRST(&fctx->__p->reclaimed);
		LIST_REMOVE(fiber, entries.reclaimed);
	} else {
		fiber = malloc(sizeof(struct fbr_fiber));
		memset(fiber, 0x00, sizeof(struct fbr_fiber));
		if (0 == stack_size)
			stack_size = FBR_STACK_SIZE;
		stack_size = round_up_to_page_size(stack_size);
		fiber->stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (MAP_FAILED == fiber->stack)
			err(EXIT_FAILURE, "mmap failed");
		fiber->stack_size = stack_size;
		(void)VALGRIND_STACK_REGISTER(fiber->stack, fiber->stack +
				stack_size);
	}
	coro_create(&fiber->ctx, (coro_func)call_wrapper, FBR_A, fiber->stack,
			FBR_STACK_SIZE);
	fiber->w_io_expected = 0;
	fiber->w_timer_expected = 0;
	fiber->reclaimed = 0;
	fiber->call_list = NULL;
	fiber->call_list_size = 0;
	LIST_INIT(&fiber->children);
	fiber->pool = NULL;
	fiber->name = name;
	fiber->func = func;
	LIST_INSERT_HEAD(&CURRENT_FIBER->children, fiber, entries.children);
	fiber->parent = CURRENT_FIBER;
	return fiber;
}

void *fbr_calloc(FBR_P_ unsigned int nmemb, size_t size)
{
	void *ptr;
	ptr = allocate_in_fiber(FBR_A_ nmemb * size, CURRENT_FIBER);
	memset(ptr, 0x00, nmemb * size);
	return ptr;
}

void *fbr_alloc(FBR_P_ size_t size)
{
	return allocate_in_fiber(FBR_A_ size, CURRENT_FIBER);
}

void fbr_alloc_set_destructor(_unused_ FBR_P_ void *ptr, fbr_alloc_destructor_func
		func, void *context)
{
	struct fbr_mem_pool *pool_entry;
	pool_entry = (struct fbr_mem_pool *)ptr - 1;
	pool_entry->destructor = func;
	pool_entry->destructor_context = context;
}

void fbr_free(FBR_P_ void *ptr)
{
	fbr_free_in_fiber(FBR_A_ CURRENT_FIBER, ptr);
}

void fbr_dump_stack(FBR_P_ fbr_logutil_func_t log)
{
	struct fbr_stack_item *ptr = fctx->__p->sp;
	(*log)(FBR_A_ "%s\n%s", "Fiber call stack:",
			"-------------------------------");
	while (ptr >= fctx->__p->stack) {
		(*log)(FBR_A_ "fiber_call: %p\t%s",
				ptr->fiber,
				ptr->fiber->name);
		print_trace_info(FBR_A_ &ptr->tinfo, log);
		(*log)(FBR_A_ "%s", "-------------------------------");
		ptr--;
	}
}

struct fbr_mutex *fbr_mutex_create(_unused_ FBR_P)
{
	struct fbr_mutex *mutex;
	mutex = malloc(sizeof(struct fbr_mutex));
	mutex->locked_by = NULL;
	TAILQ_INIT(&mutex->pending);
	return mutex;
}

void fbr_mutex_lock(FBR_P_ struct fbr_mutex *mutex)
{
	if (NULL == mutex->locked_by) {
		mutex->locked_by = CURRENT_FIBER;
		return;
	}
	TAILQ_INSERT_TAIL(&mutex->pending, CURRENT_FIBER, entries.mutex);
	fbr_yield(FBR_A);
	while (mutex->locked_by != CURRENT_FIBER)
		fbr_yield(FBR_A);
}

int fbr_mutex_trylock(FBR_P_ struct fbr_mutex *mutex)
{
	if (NULL == mutex->locked_by) {
		mutex->locked_by = CURRENT_FIBER;
		return 1;
	}
	return 0;
}

void fbr_mutex_unlock(FBR_P_ struct fbr_mutex *mutex)
{
	struct fbr_fiber *fiber;

	if (TAILQ_EMPTY(&mutex->pending)) {
		mutex->locked_by = NULL;
		return;
	}

	fiber = TAILQ_FIRST(&mutex->pending);
	mutex->locked_by = fiber;
	TAILQ_REMOVE(&mutex->pending, fiber, entries.mutex);

	if (TAILQ_EMPTY(&mutex->pending))
		ev_async_start(fctx->__p->loop, &fctx->__p->mutex_async);

	TAILQ_INSERT_TAIL(&fctx->__p->mutexes, mutex, entries);

	ev_async_send(fctx->__p->loop, &fctx->__p->mutex_async);
}

void fbr_mutex_destroy(_unused_ FBR_P_ struct fbr_mutex *mutex)
{
	free(mutex);
}

struct fbr_cond_var * fbr_cond_create(_unused_ FBR_P)
{
	struct fbr_cond_var *cond;
	cond = malloc(sizeof(struct fbr_cond_var));
	cond->mutex = NULL;
	TAILQ_INIT(&cond->waiting);
	return cond;
}

void fbr_cond_destroy(_unused_ FBR_P_ struct fbr_cond_var *cond)
{
	free(cond);
}

int fbr_cond_wait(FBR_P_ struct fbr_cond_var *cond, struct fbr_mutex *mutex)
{
	struct fbr_fiber *fiber = CURRENT_FIBER;
	if(NULL == mutex->locked_by) {
		fctx->f_errno = FBR_EINVAL;
		return -1;
	}
	TAILQ_INSERT_TAIL(&cond->waiting, fiber, entries.call_pending);
	fbr_mutex_unlock(FBR_A_ mutex);
	fbr_yield(FBR_A);
	while (!CALLED_BY_ROOT)
		fbr_yield(FBR_A);
	fbr_mutex_lock(FBR_A_ mutex);
	fctx->f_errno = FBR_SUCCESS;
	return 0;
}

void fbr_cond_broadcast(FBR_P_ struct fbr_cond_var *cond)
{
	int was_empty;
	if (TAILQ_EMPTY(&cond->waiting))
		return;
	was_empty = TAILQ_EMPTY(&fctx->__p->pending_fibers);
	TAILQ_CONCAT(&fctx->__p->pending_fibers, &cond->waiting, entries.call_pending);
	if (was_empty && !TAILQ_EMPTY(&fctx->__p->pending_fibers))
		ev_async_start(fctx->__p->loop, &fctx->__p->pending_async);
	ev_async_send(fctx->__p->loop, &fctx->__p->pending_async);
}

void fbr_cond_signal(FBR_P_ struct fbr_cond_var *cond)
{
	struct fbr_fiber *fiber;
	int was_empty;
	if (TAILQ_EMPTY(&cond->waiting))
		return;
	was_empty = TAILQ_EMPTY(&fctx->__p->pending_fibers);
	fiber = TAILQ_FIRST(&cond->waiting);
	TAILQ_REMOVE(&cond->waiting, fiber, entries.call_pending);
	TAILQ_INSERT_TAIL(&fctx->__p->pending_fibers, fiber, entries.call_pending);
	if (was_empty && !TAILQ_EMPTY(&fctx->__p->pending_fibers))
		ev_async_start(fctx->__p->loop, &fctx->__p->pending_async);
	ev_async_send(fctx->__p->loop, &fctx->__p->pending_async);
}
