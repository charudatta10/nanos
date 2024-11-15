#include <unix_internal.h>

//#define PIPE_DEBUG
#ifdef PIPE_DEBUG
#define pipe_debug(x, ...) do {tprintf(sym(pipe), 0, ss(x), ##__VA_ARGS__);} while(0)
#else
#define pipe_debug(x, ...)
#endif

#define INITIAL_PIPE_DATA_SIZE  100
#define PIPE_MIN_CAPACITY       PAGESIZE
#define DEFAULT_PIPE_MAX_SIZE   (16 * PAGESIZE) /* see pipe(7) */
#define PIPE_READ               0
#define PIPE_WRITE              1

typedef struct pipe *pipe;

typedef struct pipe_file *pipe_file;

struct pipe_file {
    struct fdesc f;       /* must be first */
    int fd;
    pipe pipe;
    blockq bq;
    closure_struct(file_io, io);
    closure_struct(fdesc_events, events);
    closure_struct(fdesc_close, close);
};

struct pipe {
    struct pipe_file files[2];
    process proc;
    heap h;
    u64 ref_cnt;
    u64 max_size;
    buffer data;
    struct spinlock lock;
};

#define pipe_lock(p)    spin_lock(&(p)->lock)
#define pipe_unlock(p)  spin_unlock(&(p)->lock)

boolean pipe_init(unix_heaps uh)
{
    kernel_heaps kh = get_kernel_heaps();
    heap h = heap_locked(kh);
    heap backed = (heap)heap_page_backed(kh);

    uh->pipe_cache = allocate_objcache(h, backed, sizeof(struct pipe), PAGESIZE, true);
    return (uh->pipe_cache == INVALID_ADDRESS ? false : true);
}

static inline void pipe_notify_reader(pipe_file pf, int events)
{
    pipe_file read_pf = &pf->pipe->files[PIPE_READ];
    if (read_pf->fd != -1) {
        if (events & EPOLLHUP)
            blockq_flush(read_pf->bq);
        else
            blockq_wake_one(read_pf->bq);
        notify_dispatch(read_pf->f.ns, events);
    }
}

static inline void pipe_notify_writer(pipe_file pf, int events)
{
    pipe_file write_pf = &pf->pipe->files[PIPE_WRITE];
    if (write_pf->fd != -1) {
        if (events & EPOLLHUP)
            blockq_flush(write_pf->bq);
        else
            blockq_wake_one(write_pf->bq);
        notify_dispatch(write_pf->f.ns, events);
    }
}

static void pipe_file_release(pipe_file pf)
{
    release_fdesc(&(pf->f));

    if (pf->bq != INVALID_ADDRESS) {
        deallocate_blockq(pf->bq);
        pf->bq = INVALID_ADDRESS;
    }
}

static void pipe_release(pipe p)
{
    if (!p->ref_cnt || (fetch_and_add(&p->ref_cnt, -1) == 1)) {
        pipe_debug("%s(%p): deallocating pipe\n", func_ss, p);
        if (p->data != INVALID_ADDRESS)
            deallocate_buffer(p->data);

        unix_cache_free(get_unix_heaps(), pipe, p);
    }
}

static inline void pipe_dealloc_end(pipe p, pipe_file pf)
{
    pf->fd = -1;    /* fd has already been deallocated by the close() syscall */
    if (&p->files[PIPE_READ] == pf) {
        pipe_notify_writer(pf, EPOLLHUP);
        pipe_debug("%s(%p): writer notified\n", func_ss, p);
    }
    if (&p->files[PIPE_WRITE] == pf) {
        pipe_notify_reader(pf, (buffer_length(p->data) ? EPOLLIN : 0) | EPOLLHUP);
        pipe_debug("%s(%p): reader notified\n", func_ss, p);
    }
    pipe_file_release(pf);
    pipe_release(p);
}

closure_func_basic(fdesc_close, sysreturn, pipe_close,
                   context ctx, io_completion completion)
{
    pipe_file pf = struct_from_closure(pipe_file, close);
    pipe_dealloc_end(pf->pipe, pf);
    return io_complete(completion, 0);
}

closure_function(4, 1, sysreturn, pipe_read_bh,
                 pipe_file, pf, void *, dest, u64, length, io_completion, completion,
                 u64 flags)
{
    pipe_file pf = bound(pf);
    int rv;

    if (flags & BLOCKQ_ACTION_NULLIFY) {
        rv = -ERESTARTSYS;
        goto out;
    }

    context ctx = get_current_context(current_cpu());
    pipe_lock(pf->pipe);
    buffer b = pf->pipe->data;
    rv = MIN(buffer_length(b), bound(length));
    if (rv == 0) {
        if (pf->pipe->files[PIPE_WRITE].fd == -1)
            goto unlock;
        if (pf->f.flags & O_NONBLOCK) {
            rv = -EAGAIN;
            goto unlock;
        }
        pipe_unlock(pf->pipe);
        return blockq_block_required((unix_context)ctx, flags);
    }

    if (context_set_err(ctx)) {
        rv = -EFAULT;
        goto unlock;
    }
    buffer_read(b, bound(dest), rv);
    context_clear_err(ctx);

    // If we have consumed all of the buffer, reset it. This might prevent future writes to allocte new buffer
    // in buffer_write/buffer_extend. Can improve things until a proper circular buffer is available
    if (buffer_length(b) == 0) {
        buffer_clear(b);
        pipe_unlock(pf->pipe);
        notify_dispatch(pf->f.ns, 0); /* for edge trigger */
        goto notify_writer;
    }
  unlock:
    pipe_unlock(pf->pipe);
  notify_writer:
    if (rv > 0)
        pipe_notify_writer(pf, EPOLLOUT);
  out:
    apply(bound(completion), rv);
    closure_finish();
    return rv;
}

closure_func_basic(file_io, sysreturn, pipe_read,
                   void *dest, u64 length, u64 offset_arg, context ctx, boolean bh, io_completion completion)
{
    pipe_file pf = struct_from_closure(pipe_file, io);

    if (length == 0)
        return io_complete(completion, 0);

    blockq_action ba = closure_from_context(ctx, pipe_read_bh, pf, dest, length, completion);
    return blockq_check(pf->bq, ba, bh);
}

closure_function(4, 1, sysreturn, pipe_write_bh,
                 pipe_file, pf, void *, dest, u64, length, io_completion, completion,
                 u64 flags)
{
    sysreturn rv = 0;
    pipe_file pf = bound(pf);

    if (flags & BLOCKQ_ACTION_NULLIFY) {
        rv = -ERESTARTSYS;
        goto out;
    }

    u64 length = bound(length);
    pipe p = pf->pipe;
    buffer b = p->data;
    context ctx = get_current_context(current_cpu());
    pipe_lock(p);
    u64 avail = p->max_size - buffer_length(b);

    if (avail == 0) {
        if (pf->pipe->files[PIPE_READ].fd == -1) {
            rv = -EPIPE;
            goto unlock;
        }
        if (pf->f.flags & O_NONBLOCK) {
            rv = -EAGAIN;
            goto unlock;
        }
        pipe_unlock(p);
        return blockq_block_required((unix_context)ctx, flags);
    }

    u64 real_length = MIN(length, avail);
    if (!context_set_err(ctx)) {
        assert(buffer_write(b, bound(dest), real_length));
        context_clear_err(ctx);
        rv = real_length;
    } else {
        rv = -EFAULT;
    }
  unlock:
    pipe_unlock(p);
    if (avail == length)
        notify_dispatch(pf->f.ns, 0); /* for edge trigger */
    if (rv > 0)
        pipe_notify_reader(pf, EPOLLIN);
  out:
    apply(bound(completion), rv);
    closure_finish();
    return rv;
}

closure_func_basic(file_io, sysreturn, pipe_write,
                   void *dest, u64 length, u64 offset, context ctx, boolean bh, io_completion completion)
{
    if (length == 0)
        return io_complete(completion, 0);

    pipe_file pf = struct_from_closure(pipe_file, io);
    blockq_action ba = closure_from_context(ctx, pipe_write_bh, pf, dest, length, completion);
    return blockq_check(pf->bq, ba, bh);
}

closure_func_basic(fdesc_events, u32, pipe_read_events,
                   thread t)
{
    pipe_file pf = struct_from_closure(pipe_file, events);
    assert(pf->f.read);
    pipe_lock(pf->pipe);
    u32 events = buffer_length(pf->pipe->data) ? EPOLLIN : 0;
    if (pf->pipe->files[PIPE_WRITE].fd == -1)
        events |= EPOLLHUP;
    pipe_unlock(pf->pipe);
    return events;
}

closure_func_basic(fdesc_events, u32, pipe_write_events,
                   thread t)
{
    pipe_file pf = struct_from_closure(pipe_file, events);
    assert(pf->f.write);
    pipe_lock(pf->pipe);
    u32 events = buffer_length(pf->pipe->data) < pf->pipe->max_size ? EPOLLOUT : 0;
    if (pf->pipe->files[PIPE_READ].fd == -1)
        events |= EPOLLHUP;
    pipe_unlock(pf->pipe);
    return events;
}

int do_pipe2(int fds[2], int flags)
{
    pipe pipe = unix_cache_alloc(get_unix_heaps(), pipe);
    if (pipe == INVALID_ADDRESS) {
        msg_err("pipe: failed to allocate structure");
        return -ENOMEM;
    }

    if (flags & ~(O_CLOEXEC | O_DIRECT | O_NONBLOCK))
        return -EINVAL;

    if (flags & O_DIRECT) {
        msg_err("pipe: O_DIRECT unsupported");
        return -EOPNOTSUPP;
    }

    pipe->h = heap_locked(get_kernel_heaps());
    pipe->data = INVALID_ADDRESS;
    pipe->proc = current->p;

    pipe->files[PIPE_READ].fd = -1;
    pipe->files[PIPE_READ].pipe = pipe;
    pipe->files[PIPE_READ].bq = INVALID_ADDRESS;

    pipe->files[PIPE_WRITE].fd = -1;
    pipe->files[PIPE_WRITE].pipe = pipe;
    pipe->files[PIPE_WRITE].bq = INVALID_ADDRESS;

    pipe->ref_cnt = 0;
    pipe->max_size = DEFAULT_PIPE_MAX_SIZE;

    pipe->data = allocate_buffer(pipe->h, INITIAL_PIPE_DATA_SIZE);
    if (pipe->data == INVALID_ADDRESS) {
        msg_err("pipe: failed to allocate data buffer");
        goto err;
    }
    spin_lock_init(&pipe->lock);

    /* init reader */
    {
        pipe_file reader = &pipe->files[PIPE_READ];
        init_fdesc(pipe->h, &reader->f, FDESC_TYPE_PIPE);
        pipe->ref_cnt = 1;

        reader->f.read = init_closure_func(&reader->io, file_io, pipe_read);
        reader->f.close = init_closure_func(&reader->close, fdesc_close, pipe_close);
        reader->f.events = init_closure_func(&reader->events, fdesc_events, pipe_read_events);
        reader->f.flags = (flags & O_NONBLOCK) | O_RDONLY;

        reader->bq = allocate_blockq(pipe->h, ss("pipe read"));
        if (reader->bq == INVALID_ADDRESS) {
            msg_err("pipe: failed to allocate blockq");
            apply(reader->f.close, 0, io_completion_ignore);
            return -ENOMEM;
        }
        reader->fd = fds[PIPE_READ] = allocate_fd(pipe->proc, reader);
        if (reader->fd == INVALID_PHYSICAL) {
            msg_err("pipe: failed to allocate fd");
            apply(reader->f.close, 0, io_completion_ignore);
            return -EMFILE;
        }
    }

    /* init writer */
    {
        pipe_file writer = &pipe->files[PIPE_WRITE];
        init_fdesc(pipe->h, &writer->f, FDESC_TYPE_PIPE);
        fetch_and_add(&pipe->ref_cnt, 1);

        writer->f.write = init_closure_func(&writer->io, file_io, pipe_write);
        writer->f.close = init_closure_func(&writer->close, fdesc_close, pipe_close);
        writer->f.events = init_closure_func(&writer->events, fdesc_events, pipe_write_events);
        writer->f.flags = (flags & O_NONBLOCK) | O_WRONLY;

        writer->bq = allocate_blockq(pipe->h, ss("pipe write"));
        if (writer->bq == INVALID_ADDRESS) {
            msg_err("pipe: failed to allocate blockq");
            apply(writer->f.close, 0, io_completion_ignore);
            deallocate_fd(pipe->proc, fds[PIPE_READ]);
            fdesc_put(&pipe->files[PIPE_READ].f);
            return -ENOMEM;
        }
        writer->fd = fds[PIPE_WRITE] = allocate_fd(pipe->proc, writer);
        if (writer->fd == INVALID_PHYSICAL) {
            msg_err("pipe: failed to allocate fd");
            apply(writer->f.close, 0, io_completion_ignore);
            deallocate_fd(pipe->proc, fds[PIPE_READ]);
            fdesc_put(&pipe->files[PIPE_READ].f);
            return -EMFILE;
        }
    }

    return 0;

err:
    pipe_release(pipe);
    return -ENOMEM;
}

int pipe_set_capacity(fdesc f, int capacity)
{
    pipe_file pf = (pipe_file)f;
    pipe p = pf->pipe;
    if (capacity < PIPE_MIN_CAPACITY)
        capacity = PIPE_MIN_CAPACITY;
    int rv;
    pipe_lock(p);
    if (capacity < buffer_length(p->data)) {
        rv = -EBUSY;
    } else {
        p->max_size = buffer_set_capacity(p->data, (bytes)capacity);
        rv = (int)p->max_size;
    }
    pipe_unlock(p);
    return rv;
}

int pipe_get_capacity(fdesc f)
{
    pipe_file pf = (pipe_file)f;
    return (int)pf->pipe->max_size;
}
