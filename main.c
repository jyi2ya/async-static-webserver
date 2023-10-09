#include <stdio.h>
#include <stdlib.h>

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

int main(int argc, char *argv[]) {
    return 0;
}
