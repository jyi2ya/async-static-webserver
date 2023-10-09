#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

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

#define MAX_PROMISES 1024

// FIXME: rewrite waker
typedef struct Waker {
    PromiseWakerOnRead on_read[MAX_PROMISES];
    bool is_read_slot_valid[MAX_PROMISES];
    PromiseWakerOnWrite on_write[MAX_PROMISES];
    bool is_write_slot_valid[MAX_PROMISES];
    PromiseWakerAwait await[MAX_PROMISES];
    bool is_await_slot_valid[MAX_PROMISES];
} Waker;

Waker *Waker_new(void) {
    Waker *self = (Waker *)malloc(sizeof(Waker));
    for (int i = 0; i < MAX_PROMISES; ++i) {
        self->is_read_slot_valid[i] = true;
        self->is_write_slot_valid[i] = true;
        self->is_await_slot_valid[i] = true;
    }
    return self;
}

int waker_get_read_fdset(Waker *self, fd_set *result) {
    int nfds = 0;
    FD_ZERO(result);
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_read_slot_valid[i]) {
            FD_SET(self->on_read[i].fd, result);
        }
    }
    return nfds;
}

int waker_get_write_fdset(Waker *self, fd_set *result) {
    int nfds = 0;
    FD_ZERO(result);
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (!self->is_write_slot_valid[i]) {
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

IOLoop *IOLoop_new(void) {
    IOLoop *self = (IOLoop *)malloc(sizeof(IOLoop));
    ioloop_init(self);
    return self;
}

void *IOLoop_spawn_blocking(Promise *start) {
    IOLoop *self = IOLoop_new();
    self->runnables[self->n_runnables++] = start;

    while (!promise_is_ready(start)) {
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
        select(nfds, &readfds, &writefds, NULL, 0);

        for (int i = 0; i < read_nfds; ++i) {
            if (FD_ISSET(i, &readfds)) {
                n_runnables += waker_get_runnables_by_read(self->waker, i, new_runnables + n_runnables);
            }
        }

        for (int i = 0; i < write_nfds; ++i) {
            if (FD_ISSET(i, &writefds)) {
                n_runnables += waker_get_runnables_by_write(self->waker, i, new_runnables + n_runnables);
            }
        }

        self->n_runnables = n_runnables;
        memcpy(self->runnables, new_runnables, sizeof(Promise *) * n_runnables);
    }

    return start->ready.result;
}

int main(int argc, char *argv[]) {
    return 0;
}
