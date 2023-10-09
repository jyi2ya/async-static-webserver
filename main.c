#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

#include <sys/select.h>

struct Waker;

typedef enum {
    PromiseTagReady,
    PromiseTagPending,
} PromiseTag;

typedef struct {
    void *result;
} PromiseReady;

typedef struct {
    void *context;
} PromisePending;

typedef struct Promise {
    PromiseTag tag;
    union {
        PromiseReady ready;
        PromisePending pending;
    };

    struct Promise *(*poll)(struct Promise *self, struct Waker *waker);
    void *(*create_context)(void);
} Promise;

Promise* Promise_new(struct Promise *(*poll)(struct Promise *self, struct Waker *waker), void *(*create_context)(void)) {
    Promise *promise = (Promise *)malloc(sizeof(Promise));
    promise->create_context = create_context;
    promise->poll = poll;
    promise->tag = PromiseTagPending;
    promise->pending.context = create_context();
    return promise;
}

bool promise_is_ready(Promise *self) {
    return self->tag == PromiseTagReady;
}

bool promise_is_pending(Promise *self) {
    return self->tag == PromiseTagPending;
}

Promise* promise_make_ready(Promise *self, void *result) {
    free(self->pending.context);
    self->tag = PromiseTagReady;
    self->ready.result = result;
    return self;
}

typedef struct {
    int fd;
    Promise *promise;
} PromiseWakerOnRead;

typedef struct {
    int fd;
    Promise *promise;
} PromiseWakerOnWrite;

typedef struct {
    Promise *await;
    Promise *promise;
} PromiseWakerAwait;

typedef struct {
    Promise *promise;
} PromiseWakerYield;

#define MAX_PROMISES 1024

// FIXME: rewrite waker
typedef struct Waker {
    PromiseWakerOnRead on_read[MAX_PROMISES];
    bool is_read_slot_valid[MAX_PROMISES];
    PromiseWakerOnWrite on_write[MAX_PROMISES];
    bool is_write_slot_valid[MAX_PROMISES];
    PromiseWakerAwait await[MAX_PROMISES];
    bool is_await_slot_valid[MAX_PROMISES];
    PromiseWakerYield yield[MAX_PROMISES];
    bool is_yield_slot_valid[MAX_PROMISES];
} Waker;

void Waker_drop(Waker *self) {
    free(self);
}

Waker *Waker_new(void) {
    Waker *self = (Waker *)malloc(sizeof(Waker));
    for (int i = 0; i < MAX_PROMISES; ++i) {
        self->is_read_slot_valid[i] = true;
        self->is_write_slot_valid[i] = true;
        self->is_await_slot_valid[i] = true;
        self->is_yield_slot_valid[i] = true;
    }
    return self;
}

int waker_get_read_fdset(Waker *self, fd_set *result) {
    int nfds = -1;
    FD_ZERO(result);
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_read_slot_valid[i]) {
            if (nfds < self->on_read[i].fd) {
                nfds = self->on_read[i].fd;
            }
            FD_SET(self->on_read[i].fd, result);
        }
    }
    return nfds;
}

int waker_get_write_fdset(Waker *self, fd_set *result) {
    int nfds = -1;
    FD_ZERO(result);
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_write_slot_valid[i]) {
            if (nfds < self->on_write[i].fd) {
                nfds = self->on_write[i].fd;
            }
            FD_SET(self->on_write[i].fd, result);
        }
    }
    return nfds;
}

int waker_get_runnables_by_read(Waker *self, int fd, Promise **result) {
    int n = 0;
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_read_slot_valid[i]) {
            if (self->on_read[i].fd == fd) {
                result[n++] = self->on_read[i].promise;
                self->is_read_slot_valid[i] = true;
            }
        }
    }
    return n;
}

int waker_get_runnables_by_write(Waker *self, int fd, Promise **result) {
    int n = 0;
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_write_slot_valid[i]) {
            if (self->on_write[i].fd == fd) {
                result[n++] = self->on_write[i].promise;
                self->is_write_slot_valid[i] = true;
            }
        }
    }
    return n;
}

int waker_get_runnables_by_await(Waker *self, Promise *resolved, Promise **result) {
    int n = 0;
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_await_slot_valid[i]) {
            if (self->await[i].await == resolved) {
                result[n++] = self->await[i].promise;
                self->is_await_slot_valid[i] = true;
            }
        }
    }
    return n;
}

int waker_get_runnables_by_yield(Waker *self, Promise **result) {
    int n = 0;
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_yield_slot_valid[i]) {
            result[n++] = self->yield[i].promise;
            self->is_yield_slot_valid[i] = true;
        }
    }
    return n;
}

int waker_wakeup_by_yield(Waker *self, Promise *promise) {
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (self->is_yield_slot_valid[i]) {
            self->yield[i].promise = promise;
            self->is_yield_slot_valid[i] = false;
            return 0;
        }
    }
    assert(0 && "no more slots for yield promises");
}

// FIXME: rewrite ioloop
typedef struct {
    Waker *waker;
    Promise *runnables[MAX_PROMISES];
    int n_runnables;
} IOLoop;

void ioloop_init(IOLoop *self) {
    self->waker = Waker_new();
    self->n_runnables = 0;
}

void IOLoop_drop(IOLoop *self) {
    Waker_drop(self->waker);
    free(self);
}

IOLoop *IOLoop_new(void) {
    IOLoop *self = (IOLoop *)malloc(sizeof(IOLoop));
    ioloop_init(self);
    return self;
}

void *IOLoop_spawn_blocking(Promise *start) {
    IOLoop *self = IOLoop_new();
    self->runnables[self->n_runnables++] = start;

    while (!promise_is_ready(start)) {
        assert(self->n_runnables != 0 && "no more executable promises");

        static Promise *new_runnables[MAX_PROMISES];
        int n_runnables = 0;

        for (int i = 0; i < self->n_runnables; ++i) {
            Promise *current = self->runnables[i]->poll(self->runnables[i], self->waker);
            if (promise_is_ready(current)) {
                n_runnables += waker_get_runnables_by_await(self->waker, current, new_runnables + n_runnables);
            }
        }

        fd_set readfds, writefds;
        int read_nfds = waker_get_read_fdset(self->waker, &readfds);
        int write_nfds = waker_get_write_fdset(self->waker, &writefds);
        int nfds = read_nfds > write_nfds ? read_nfds : write_nfds;

        if (nfds > -1) {
            select(nfds + 1, &readfds, &writefds, NULL, NULL);
        }

        for (int i = 0; i <= read_nfds; ++i) {
            if (FD_ISSET(i, &readfds)) {
                n_runnables += waker_get_runnables_by_read(self->waker, i, new_runnables + n_runnables);
            }
        }

        for (int i = 0; i <= write_nfds; ++i) {
            if (FD_ISSET(i, &writefds)) {
                n_runnables += waker_get_runnables_by_write(self->waker, i, new_runnables + n_runnables);
            }
        }

        n_runnables += waker_get_runnables_by_yield(self->waker, new_runnables + n_runnables);

        self->n_runnables = n_runnables;
        memcpy(self->runnables, new_runnables, sizeof(Promise *) * n_runnables);
    }

    IOLoop_drop(self);
    return start->ready.result;
}

#define ASYNC_MAKE_CTX(context_type) \
    context_type *ctx = (context_type *)(self->pending.context)


#define ASYNC_BEGIN(context_type) \
    if (promise_is_ready(self)) return self; \
    ASYNC_MAKE_CTX(context_type); \
    switch (ctx->_state) { \
        case 0:

#define ASYNC_YIELD() \
        ctx->_state = __LINE__; waker_wakeup_by_yield(waker, self); return self; case __LINE__:

#define ASYNC_RETURN(result) \
        promise_make_ready(self, result); \

#define ASYNC_END() \
    } \
    return self;

typedef struct {
    int _state;
    int fd;
    char *buf;
    size_t count;
} ReadPContext;

void *read_p_create_context(void) {
    ReadPContext *ctx = (ReadPContext *)malloc(sizeof(ReadPContext));
    ctx->_state = 0;
    return (void *)ctx;
}

Promise *read_p_poll(Promise *self, Waker *waker) {
    printf("entering!\n");

    ASYNC_BEGIN(ReadPContext);

    printf("step 1 %d\n", ctx->fd);

    ASYNC_YIELD();

    printf("step 2 %p\n", ctx->buf);

    ASYNC_YIELD();

    printf("step 3 %ld\n", ctx->count);

    ASYNC_YIELD();

    ASYNC_RETURN(NULL);

    ASYNC_END();
}

Promise *read_p(int fd, char *buf, size_t count) {
    Promise *self = Promise_new(read_p_poll, read_p_create_context);
    ASYNC_MAKE_CTX(ReadPContext);
    ctx->fd = fd;
    ctx->buf = buf;
    ctx->count = count;
    return self;
}

int main(int argc, char *argv[]) {
    IOLoop_spawn_blocking(read_p(1, (char *)0x22, 333));
    return 0;
}
