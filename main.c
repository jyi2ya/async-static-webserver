#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>

#include <netinet/in.h>
#include <arpa/inet.h>

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

void Promise_drop(Promise *self) {
    free(self);
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

int waker_wakeup_by_write(Waker *self, Promise *promise, int fd) {
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (self->is_write_slot_valid[i]) {
            self->on_write[i].fd = fd;
            self->on_write[i].promise = promise;
            self->is_write_slot_valid[i] = false;
            return 0;
        }
    }
    assert(0 && "no more slots for write promises");
}

int waker_wakeup_by_read(Waker *self, Promise *promise, int fd) {
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (self->is_read_slot_valid[i]) {
            self->on_read[i].fd = fd;
            self->on_read[i].promise = promise;
            self->is_read_slot_valid[i] = false;
            return 0;
        }
    }
    assert(0 && "no more slots for read promises");
}

int waker_wakeup_by_await(Waker *self, Promise *promise, Promise *await) {
    for (int i = 0; i < MAX_PROMISES; ++i) {
        if (self->is_await_slot_valid[i]) {
            self->await[i].await = await;
            self->await[i].promise = promise;
            self->is_await_slot_valid[i] = false;
            return 0;
        }
    }
    assert(0 && "no more slots for await promises");
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

void *IOLoop_block_on(Promise *start) {
    IOLoop *self = IOLoop_new();
    self->runnables[self->n_runnables++] = start;

    while (!promise_is_ready(start)) {
        assert(self->n_runnables != 0 && "no more executable promises");

        static Promise *new_runnables[MAX_PROMISES];
        int n_runnables = 0;

        while (!promise_is_ready(start)) {
            n_runnables = 0;

            for (int i = 0; i < self->n_runnables; ++i) {
                Promise *current = self->runnables[i]->poll(self->runnables[i], self->waker);
                if (promise_is_ready(current)) {
                    int waiting = waker_get_runnables_by_await(self->waker, current, new_runnables + n_runnables);
                    if (current != start && waiting == 0) {
                        free(current->ready.result);
                        Promise_drop(current);
                    } else {
                        n_runnables += waiting;
                    }
                }
            }

            n_runnables += waker_get_runnables_by_yield(self->waker, new_runnables + n_runnables);

            if (n_runnables == 0) {
                break;
            }

            self->n_runnables = n_runnables;
            memcpy(self->runnables, new_runnables, sizeof(Promise *) * n_runnables);
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

        self->n_runnables = n_runnables;
        memcpy(self->runnables, new_runnables, sizeof(Promise *) * n_runnables);
    }

    IOLoop_drop(self);
    void *result = start->ready.result;
    Promise_drop(start);
    return result;
}

#define ASYNC_MAKE_CTX(context_type) \
    context_type *ctx = (context_type *)(self->pending.context)


#define ASYNC_BEGIN(context_type) \
    if (promise_is_ready(self)) return self; \
    ASYNC_MAKE_CTX(context_type); \
    switch (ctx->_state) { \
        case 0:

#define ASYNC_WAIT() do { \
    ctx->_state = __LINE__; return self; case __LINE__: \
    ; \
} while (0)

#define ASYNC_SPAWN(promise) do { \
    waker_wakeup_by_yield(waker, promise); \
} while (0) \

#define ASYNC_YIELD() do { \
    waker_wakeup_by_yield(waker, self); \
    ASYNC_WAIT(); \
} while (0) \

#define ASYNC_RETURN(type, result) do { \
    type *_result = (type *)malloc(sizeof(type)); \
    *_result = result; \
    promise_make_ready(self, _result); \
    return self; \
} while (0)

#define ASYNC_AWAIT(type, result_, promise_) do { \
    ctx->_awaiting = (promise_); \
    waker_wakeup_by_await(waker, self, ctx->_awaiting); \
    waker_wakeup_by_yield(waker, ctx->_awaiting); \
    ASYNC_WAIT(); \
    if (ctx->_awaiting->ready.result != NULL) { \
        result_ = *(type *)ctx->_awaiting->ready.result; \
        free(ctx->_awaiting->ready.result); \
    } \
    Promise_drop(ctx->_awaiting); \
} while (0)

#define ASYNC_END() \
    } \
    return self;

#define CONTEXT_ALLOCATION_FN(fn_prefix, context_type) \
    void * fn_prefix ## _create_context(void) { \
        context_type *ctx = (context_type *)malloc(sizeof(context_type)); \
        ctx->_state = 0; \
        return (void *)ctx; \
    }

typedef struct {
    int _state;
    Promise *_awaiting;
    int fd;
    char *buf;
    size_t count;
} ReadPContext;

CONTEXT_ALLOCATION_FN(read_p, ReadPContext)

Promise *read_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(ReadPContext);
    waker_wakeup_by_read(waker, self, ctx->fd);
    ASYNC_WAIT();
    ASYNC_RETURN(int, read(ctx->fd, ctx->buf, ctx->count));
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

typedef struct {
    int _state;
    Promise *_awaiting;
    int fd;
    char ch;
    int nbytes;
} GetcharPContext;

CONTEXT_ALLOCATION_FN(getchar_p, GetcharPContext)

Promise *getchar_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(GetcharPContext);
    ASYNC_AWAIT(int, ctx->nbytes, read_p(ctx->fd, &ctx->ch, 1));
    int ch = ctx->nbytes == 0 ? EOF : ctx->ch;
    ASYNC_RETURN(int, ch);
    ASYNC_END();
}

Promise *getchar_p(int fd) {
    Promise *self = Promise_new(getchar_p_poll, getchar_p_create_context);
    ASYNC_MAKE_CTX(GetcharPContext);
    ctx->fd = fd;
    return self;
}

typedef struct {
    int _state;
    Promise *_awaiting;
    int fd;

    int ch;
    char *line;
    size_t capacity;
    size_t length;

    bool got_cr;
} ReadlinePContext;

CONTEXT_ALLOCATION_FN(readline_p, ReadlinePContext)

Promise *readline_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(ReadlinePContext);

    for (;;) {
        if (ctx->length == ctx->capacity) {
            ctx->capacity = ctx->capacity * 2 + 4;
            ctx->line = (char *)realloc(ctx->line, ctx->capacity);
        }

        ASYNC_AWAIT(int, ctx->ch, getchar_p(ctx->fd));

        if (ctx->ch == EOF) {
            break;
        }

        if (ctx->got_cr && ctx->ch == '\n') {
            break;
        }

        if (ctx->ch == '\r') {
            ctx->got_cr = true;
        } else {
            ctx->got_cr = false;
        }

        ctx->line[ctx->length++] = ctx->ch;
    }

    ctx->line[ctx->length++] = '\0';

    ASYNC_RETURN(char *, ctx->line);
    ASYNC_END();
}

Promise *readline_p(int fd) {
    Promise *self = Promise_new(readline_p_poll, readline_p_create_context);
    ASYNC_MAKE_CTX(ReadlinePContext);
    ctx->fd = fd;

    ctx->line = NULL;
    ctx->capacity = 0;
    ctx->length = 0;
    ctx->got_cr = false;
    return self;
}

typedef struct {
    int _state;
    Promise *_awaiting;
    int fd;
    const char *buf;
    size_t count;
} WritePContext;

CONTEXT_ALLOCATION_FN(write_p, WritePContext)

Promise *write_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(WritePContext);
    waker_wakeup_by_write(waker, self, ctx->fd);
    ASYNC_WAIT();
    ASYNC_RETURN(int, write(ctx->fd, ctx->buf, ctx->count));
    ASYNC_END();
}

Promise *write_p(int fd, const char *buf, size_t count) {
    Promise *self = Promise_new(write_p_poll, write_p_create_context);
    ASYNC_MAKE_CTX(WritePContext);
    ctx->fd = fd;
    ctx->buf = buf;
    ctx->count = count;
    return self;
}

typedef struct {
    int _state;
    Promise *_awaiting;
    int fd;
    const char *buf;
    size_t count;

    const char *pos;
    int nbytes;
} WriteAllPContext;

CONTEXT_ALLOCATION_FN(write_all_p, WriteAllPContext)

Promise *write_all_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(WriteAllPContext);

    while (ctx->count > 0) {
        ASYNC_AWAIT(int, ctx->nbytes, write_p(ctx->fd, ctx->buf, ctx->count));
        if (ctx->nbytes < 0) {
            perror("write_all: ");
            ASYNC_RETURN(int, 1);
        }
        ctx->pos += ctx->nbytes;
        ctx->count -= ctx->nbytes;
    }

    ASYNC_RETURN(int, 0);
    ASYNC_END();
}

Promise *write_all_p(int fd, const char *buf, size_t count) {
    Promise *self = Promise_new(write_all_p_poll, write_all_p_create_context);
    ASYNC_MAKE_CTX(WriteAllPContext);

    if (count == 0) {
        count = strlen(buf);
    }

    ctx->fd = fd;
    ctx->buf = buf;
    ctx->count = count;

    ctx->pos = ctx->buf;
    return self;
}

typedef struct {
    int _state;
    Promise *_awaiting;

    int fd;
    struct sockaddr *addr;
    socklen_t *len;
} AcceptPContext;

CONTEXT_ALLOCATION_FN(accept_p, AcceptPContext)

Promise *accept_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(AcceptPContext);
    waker_wakeup_by_read(waker, self, ctx->fd);
    ASYNC_WAIT();
    ASYNC_RETURN(int, accept(ctx->fd, ctx->addr, ctx->len));
    ASYNC_END();
}

Promise *accept_p(int fd, struct sockaddr *addr, socklen_t *len) {
    Promise *self = Promise_new(accept_p_poll, accept_p_create_context);
    ASYNC_MAKE_CTX(AcceptPContext);
    ctx->fd = fd;
    ctx->addr = addr;
    ctx->len = len;

    return self;
}

const char *content_types[] = {
    ".aac", "audio/aac",
    ".abw", "application/x-abiword",
    ".arc", "application/x-freearc",
    ".avif", "image/avif",
    ".avi", "video/x-msvideo",
    ".azw", "application/vnd.amazon.ebook",
    ".bin", "application/octet-stream",
    ".bmp", "image/bmp",
    ".bz", "application/x-bzip",
    ".bz2", "application/x-bzip2",
    ".cda", "application/x-cdf",
    ".csh", "application/x-csh",
    ".css", "text/css",
    ".csv", "text/csv",
    ".doc", "application/msword",
    ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    ".eot", "application/vnd.ms-fontobject",
    ".epub", "application/epub+zip",
    ".gz", "application/gzip",
    ".gif", "image/gif",
    ".html", "text/html",
    ".ico", "image/vnd.microsoft.icon",
    ".ics", "text/calendar",
    ".jar", "application/java-archive",
    ".jpg", "image/jpeg",
    ".jpeg", "image/jpeg",
    ".js", "text/javascript",
    ".json", "application/json",
    ".jsonld", "application/ld+json",
    ".mid", "audio/midi, audio/x-midi",
    ".mjs", "text/javascript",
    ".mp3", "audio/mpeg",
    ".mp4", "video/mp4",
    ".mpeg", "video/mpeg",
    ".mpkg", "application/vnd.apple.installer+xml",
    ".odp", "application/vnd.oasis.opendocument.presentation",
    ".ods", "application/vnd.oasis.opendocument.spreadsheet",
    ".odt", "application/vnd.oasis.opendocument.text",
    ".oga", "audio/ogg",
    ".ogv", "video/ogg",
    ".ogx", "application/ogg",
    ".opus", "audio/opus",
    ".otf", "font/otf",
    ".png", "image/png",
    ".pdf", "application/pdf",
    ".php", "application/x-httpd-php",
    ".ppt", "application/vnd.ms-powerpoint",
    ".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation",
    ".rar", "application/vnd.rar",
    ".rtf", "application/rtf",
    ".sh", "application/x-sh",
    ".svg", "image/svg+xml",
    ".tar", "application/x-tar",
    ".tiff", "image/tiff",
    ".ts", "video/mp2t",
    ".ttf", "font/ttf",
    ".txt", "text/plain",
    ".vsd", "application/vnd.visio",
    ".wav", "audio/wav",
    ".weba", "audio/webm",
    ".webm", "video/webm",
    ".webp", "image/webp",
    ".woff", "font/woff",
    ".woff2", "font/woff2",
    ".xhtml", "application/xhtml+xml",
    ".xls", "application/vnd.ms-excel",
    ".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    ".xml", "application/xml",
    ".xul", "application/vnd.mozilla.xul+xml",
    ".zip", "application/zip",
    ".3gp", "video/3gpp",
    ".3g2", "video/3gpp2",
    ".7z", "application/x-7z-compressed",
};

typedef struct {
    int _state;
    Promise *_awaiting;
    int fd;
    const char *path;
    const char *header;
    int file_fd;
    char buf[4096];
    int nbytes;
    char *headers;
    int total_bytes;
} SendFileWithHeaderPContext;

CONTEXT_ALLOCATION_FN(send_file_with_header_p, SendFileWithHeaderPContext)

Promise *send_file_with_header_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(SendFileWithHeaderPContext);

    fprintf(stderr, "response header: %s\n", ctx->header);

    struct stat sb;
    if (stat(ctx->path, &sb) == -1) {
        perror("stat: ");
        ASYNC_RETURN(int, 1);
    }
    ctx->total_bytes = sb.st_size;


    const char *postfix = ctx->path;
    while (*postfix != '\0') {
        postfix += 1;
    }
    while (*postfix != '.' && postfix != ctx->path) {
        postfix -= 1;
    }
    const char *content_type = "application/octet-stream";
    for (int i = 0; i < sizeof(content_types) / sizeof(content_types[0]); i += 2) {
        if (strcmp(content_types[i], postfix) == 0) {
            content_type = content_types[i + 1];
            break;
        }
    }
    ctx->headers = (char *)malloc(
            strlen(ctx->header) + strlen("\r\n")
            + strlen("Content-Type: ") + strlen(content_type) + strlen("\r\n")
            + strlen("Content-Length: XXXXXXXXXXXXXXXXXXX\r\n")
            );
    strcpy(ctx->headers, ctx->header);
    strcat(ctx->headers, "\r\n");
    strcat(ctx->headers, "Content-Type: ");
    strcat(ctx->headers, content_type);
    strcat(ctx->headers, "\r\n");
    strcat(ctx->headers, "Content-Length: ");
    sprintf(ctx->headers + strlen(ctx->headers), "%d\r\n\r\n", ctx->total_bytes);

    ASYNC_AWAIT(int, ctx->nbytes, write_all_p(ctx->fd, ctx->headers, 0));
    free(ctx->headers);

    ctx->file_fd = open(ctx->path, O_RDWR | O_NONBLOCK);
    if (ctx->file_fd == -1) {
        perror("open: ");
        ASYNC_RETURN(int, 2);
    }

    while (ctx->total_bytes > 0) {
        ASYNC_AWAIT(int, ctx->nbytes, read_p(ctx->file_fd, ctx->buf, 4096));
        if (ctx->nbytes == -1) {
            perror("read: ");
            ASYNC_RETURN(int, 3);
        }
        ctx->total_bytes -= ctx->nbytes;
        if (ctx->nbytes > 0) {
            ASYNC_AWAIT(int, ctx->nbytes, write_all_p(ctx->fd, ctx->buf, ctx->nbytes));
        }
    }

    close(ctx->file_fd);

    ASYNC_RETURN(int, 0);
    ASYNC_END();
}

Promise *send_file_with_header_p(int fd, const char *path, const char *header) {
    Promise *self = Promise_new(send_file_with_header_p_poll, send_file_with_header_p_create_context);
    ASYNC_MAKE_CTX(SendFileWithHeaderPContext);
    ctx->fd = fd;
    ctx->path = path;
    ctx->header = header;
    return self;
}

int strprefixcmp(const char *prefix, const char *str) {
    return strncmp(prefix, str, strlen(prefix));
}

typedef struct {
    int _state;
    Promise *_awaiting;

    int fd;
    int ln;
    char *method;
    char *file;
    const char *basedir;
    int file_fd;
    int ret;
} WorkerPContext;

CONTEXT_ALLOCATION_FN(worker_p, WorkerPContext)

Promise *worker_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(WorkerPContext);

    ASYNC_AWAIT(char *, ctx->method, readline_p(ctx->fd));

    fprintf(stderr, "request header: %s\n", ctx->method);

    if (strprefixcmp("GET ", ctx->method) != 0) {
        ASYNC_AWAIT(int, ctx->ret,
                send_file_with_header_p(
                    ctx->fd,
                    "./assets/405.html",
                    "HTTP/1.1 405 Method Not Allowed"
                    )
                );
        close(ctx->fd);
        ASYNC_RETURN(int, 1);
    }

    char *file = ctx->method + strlen("GET ");
    for (int i = 0; file[i] != '\0'; ++i) {
        if (file[i] == ' ') {
            file[i] = '\0';
            break;
        }
    }

    ctx->file = (char *)malloc(strlen(ctx->basedir) + strlen(file) + 1);
    strcpy(ctx->file, ctx->basedir);
    strcat(ctx->file, file);
    free(ctx->method);

    ctx->file_fd = open(ctx->file, O_RDWR | O_NONBLOCK);

    if (ctx->file_fd == -1) {
        perror("open: ");
        if (errno == EACCES) {
            ASYNC_AWAIT(int, ctx->ret,
                    send_file_with_header_p(
                        ctx->fd,
                        "./assets/403.html",
                        "HTTP/1.1 403 Forbidden"
                        )
                    );
        } else if (errno == ENOENT) {
            ASYNC_AWAIT(int, ctx->ret,
                    send_file_with_header_p(
                        ctx->fd,
                        "./assets/404.html",
                        "HTTP/1.1 404 Not Found"
                        )
                    );
        } else {
            ASYNC_AWAIT(int, ctx->ret,
                    send_file_with_header_p(
                        ctx->fd,
                        "./assets/500.html",
                        "HTTP/1.1 500 Internal Server Error"
                        )
                    );
        }
    } else {
        close(ctx->file_fd);

        ASYNC_AWAIT(int, ctx->ret,
                send_file_with_header_p(
                    ctx->fd,
                    ctx->file,
                    "HTTP/1.1 200 OK"
                    )
                );
    }

    free(ctx->file);
    shutdown(ctx->fd, SHUT_WR);
    for (;;) {
        ASYNC_AWAIT(int, ctx->ret, getchar_p(ctx->fd));
        if (ctx->ret == EOF) {
            break;
        }
    }
    shutdown(ctx->fd, SHUT_RD);

    ASYNC_RETURN(int, 0);
    ASYNC_END();
}

Promise *worker_p(int fd, const char *basedir) {
    Promise *self = Promise_new(worker_p_poll, worker_p_create_context);
    ASYNC_MAKE_CTX(WorkerPContext);
    ctx->fd = fd;
    ctx->ln = 0;
    ctx->basedir = basedir;
    return self;
}

typedef struct {
    int _state;
    Promise *_awaiting;

    char *address;
    int port;
    int backlog;
    const char *basedir;

    int listen_fd;
    int incoming_fd;
    struct sockaddr_storage client_addr;
    socklen_t len;
    int request_cnt;
} ServerPContext;

CONTEXT_ALLOCATION_FN(server_p, ServerPContext)

Promise *server_p_poll(Promise *self, Waker *waker) {
    ASYNC_BEGIN(ServerPContext);

    ctx->listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (ctx->listen_fd == -1) {
        perror("socket: ");
        assert(0);
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(ctx->port);
    addr.sin_addr.s_addr = inet_addr(ctx->address);

    if (bind(ctx->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind: ");
        assert(0);
    }

    if (listen(ctx->listen_fd, ctx->backlog) == -1) {
        perror("listen: ");
        assert(0);
    }

    fprintf(stderr, "listening on %s:%d\n", ctx->address, ctx->port);

    for (;;) {
        ASYNC_AWAIT(int, ctx->incoming_fd, accept_p(ctx->listen_fd, (struct sockaddr *)&ctx->client_addr, &ctx->len));

        ctx->request_cnt += 1;

        if (ctx->client_addr.ss_family != AF_INET) {
            fprintf(stderr, "currently we support ipv4 protocol only\n");
            fprintf(stderr, "required family is %u\n", ctx->client_addr.ss_family);
            fprintf(stderr, "ipv4 is %u, ipv6 is %u, uds is %u\n", AF_INET, AF_INET6, AF_UNIX);
        } else {
            char ip[INET_ADDRSTRLEN];
            struct sockaddr_in *s = (struct sockaddr_in *)&ctx->client_addr;
            int port = ntohs(s->sin_port);
            inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));

            fprintf(stderr, "accepting request from %s:%d\n", ip, port);
        }

        ASYNC_SPAWN(worker_p(ctx->incoming_fd, ctx->basedir));
    }

    ASYNC_RETURN(int, 0);
    ASYNC_END();
}

Promise *server_p(char *address, int port, int backlog, const char *basedir) {
    Promise *self = Promise_new(server_p_poll, server_p_create_context);

    ASYNC_MAKE_CTX(ServerPContext);
    ctx->address = address;
    ctx->port = port;
    ctx->backlog = backlog;
    ctx->basedir = basedir;
    ctx->request_cnt = 0;

    ctx->len = sizeof(ctx->client_addr);
    return self;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <config>\n", argv[0]);
        exit(1);
    }

    char address[4096];
    char basedir[4096];
    int port = 0;
    int backlog = 0;

    int config_nl = 0;

    FILE *fp = fopen(argv[1], "r");
    char line[4096];
    while (!feof(fp)) {
        fgets(line, sizeof(line), fp);
        if (line[strlen(line) - 1] == '\n') {
            line[strlen(line) - 1] = '\0';
        }
        char *split = strchr(line, '=');
        if (split == NULL) {
            continue;
        }
        char *left = line;
        char *right = split + 1;
        *split = '\0';
        if (strcmp(left, "address") == 0) {
            strcpy(address, right);
            if (config_nl & 1) {
                fprintf(stderr, "warning: duplicated configuration for `%s`\n", left);
            }
            config_nl |= 1;
        } else if (strcmp(left, "basedir") == 0) {
            strcpy(basedir, right);
            if (config_nl & 2) {
                fprintf(stderr, "warning: duplicated configuration for `%s`\n", left);
            }
            config_nl |= 2;
        } else if (strcmp(left, "port") == 0) {
            port = atoi(right);
            if (config_nl & 4) {
                fprintf(stderr, "warning: duplicated configuration for `%s`\n", left);
            }
            config_nl |= 4;
        } else if (strcmp(left, "backlog") == 0) {
            backlog = atoi(right);
            if (config_nl & 8) {
                fprintf(stderr, "warning: duplicated configuration for `%s`\n", left);
            }
            config_nl |= 8;
        } else {
            fprintf(stderr, "warning: config `%s => %s` ignored\n", left, right);
        }
    }

    if (config_nl != 0xf) {
        fprintf(stderr, "invalid configuration file\n");
        exit(1);
    }

    IOLoop_block_on(server_p(address, port, backlog, basedir));
    return 0;
}
