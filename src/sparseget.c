/* 空洞文件与 HTTP 断点续传。Linux / C11，数据写入使用 lseek + write。
 * HTTP 演示范围：明文 HTTP、HEAD、Content-Length、强 ETag、单段 Range。
 * 不支持 HTTPS、重定向、分块传输或压缩；不满足条件时明确报错。
 */
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include "ui.h"

#define MIN_SIZE (128LL * 1024)
#define BUF_SIZE 65536
#define CHECKPOINT_BYTES (1024 * 1024)
#define URL_SIZE 2048
#define ETAG_SIZE 256
#define HASH_INIT UINT64_C(14695981039346656037)

static volatile sig_atomic_t stopped;
static void on_signal(int sig) { (void)sig; stopped = 1; }
static int fail(const char *s) { ui_error(s); return -1; }
static int syserr(const char *s) { int error=errno; ui_end_progress(); errno=error; perror(s); return -1; }

/* 非负整数，可选 K/M/G 后缀，分别表示 1024 的 1/2/3 次方。 */
static int number(const char *s, off_t *out)
{
    char *end;
    if (!s[0] || !isdigit((unsigned char)s[0])) return -1;
    errno = 0;
    unsigned long long n = strtoull(s, &end, 10), scale = 1;
    if (errno) return -1;
    if (*end) {
        if (end[1]) return -1;
        switch (toupper((unsigned char)*end)) {
        case 'K': scale = 1024; break;
        case 'M': scale = 1024 * 1024; break;
        case 'G': scale = 1024ULL * 1024 * 1024; break;
        default: return -1;
        }
    }
    if (n > INT64_MAX / scale) return -1;
    *out = (off_t)(n * scale);
    return 0;
}

/* write 可能短写或被信号打断，必须循环处理。 */
static int write_all(int fd, const void *data, size_t len)
{
    const unsigned char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return syserr("write");
        p += n; len -= (size_t)n;
    }
    return 0;
}

/* SIGINT 可在落盘过程中到达：EINTR 需要重试，真实 I/O 错误必须上报。 */
static int sync_fd(int fd, int data_only)
{
    int rc;
    do { rc=data_only ? fdatasync(fd) : fsync(fd); } while (rc && errno==EINTR);
    return rc;
}

static int regular_locked(int fd)
{
    struct stat s;
    if (fstat(fd, &s)) return syserr("fstat");
    if (!S_ISREG(s.st_mode)) return fail("a regular file is required");
    if (flock(fd, LOCK_EX | LOCK_NB)) return fail("file is busy (another writer holds the lock)");
    if (fstat(fd,&s)) return syserr("fstat locked file");
    if (s.st_nlink!=1) return fail("hard-linked files are not supported for writing or downloading");
    return 0;
}

static char *sidecar(const char *path)
{
    size_t n = strlen(path) + 8;
    char *s = malloc(n);
    if (s) snprintf(s, n, "%s.resume", path);
    return s;
}

static int cmd_create(const char *path, const char *size)
{
    off_t n;
    if (number(size, &n) || n < MIN_SIZE) return fail("size must be at least 128K (131072 bytes)");
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
    if (fd < 0) return syserr("create (existing files are never overwritten)");
    /* 扩展逻辑长度，不逐字节填零；ext4 上未写入部分不分配数据块。 */
    int rc = ftruncate(fd, n) || sync_fd(fd,0);
    if (rc) syserr("create/ftruncate/fsync");
    if (close(fd)) rc = syserr("close");
    if (!rc) ui_created(path,n);
    return rc ? -1 : 0;
}

static int cmd_write(const char *path, const char *offset, const char *text)
{
    off_t off;
    if (number(offset, &off)) return fail("invalid offset");
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return syserr("open");
    int rc = -1;
    struct stat s;
    char *state = sidecar(path);
    if (!state) { fail("out of memory"); goto done; }
    if (regular_locked(fd)) goto done;
    if (lstat(state, &s) == 0) { fail("download-managed file: manual write would invalidate resume data"); goto done; }
    if (errno != ENOENT) { syserr("state check"); goto done; }
    if (fstat(fd, &s)) { syserr("fstat"); goto done; }
    size_t len = strlen(text);
    if (off > s.st_size || (uintmax_t)len > (uintmax_t)(s.st_size - off)) {
        fail("write would exceed the logical file size"); goto done;
    }
    if (lseek(fd, off, SEEK_SET) < 0) { syserr("lseek"); goto done; }
    if (write_all(fd, text, len) || sync_fd(fd,0)) { syserr("write/fsync"); goto done; }
    ui_written(path,off,len);
    rc = 0;
done:
    free(state);
    if (close(fd)) rc = syserr("close");
    return rc;
}

static int cmd_status(const char *path)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat s;
    if (fd < 0) return syserr("open");
    if (fstat(fd, &s) || !S_ISREG(s.st_mode)) { close(fd); return fail("cannot stat regular file"); }
    /* st_blocks 的单位是 512 字节，不是 st_blksize。 */
    uintmax_t allocated = (uintmax_t)s.st_blocks * 512;
    double saving = s.st_size ? 100.0 * (1.0 - (double)allocated / (double)s.st_size) : 0.0;
    /* SEEK_DATA/SEEK_HOLE 报告文件系统视角的空洞，不把零字节误认为空洞。 */
    off_t pos = 0, holes = 0;
    int supported = 1;
    while (pos < s.st_size) {
        off_t data = lseek(fd, pos, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) { holes += s.st_size - pos; break; }
            supported = 0; break;
        }
        if (data > s.st_size) data = s.st_size;
        holes += data - pos;
        if (data == s.st_size) break;
        off_t end = lseek(fd, data, SEEK_HOLE);
        if (end <= data) { supported = 0; break; }
        pos = end;
    }
    ui_status(path,s.st_size,(uintmax_t)s.st_blocks,allocated,saving,supported,holes);
    close(fd);
    return 0;
}

struct url { char host[256], port[8], target[URL_SIZE]; };
struct response {
    int fd, status;
    off_t length, start, end, total;
    char etag[ETAG_SIZE];
};

static int parse_url(const char *s, struct url *u)
{
    if (strlen(s) >= URL_SIZE || strncmp(s, "http://", 7)) return fail("use an http:// URL shorter than 2048 bytes");
    for (const char *p = s; *p; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '#') return fail("URL must be ASCII and percent-encoded, with no fragment");
    const char *begin = s + 7, *slash = strchr(begin, '/');
    size_t n = slash ? (size_t)(slash - begin) : strlen(begin);
    if (!n || n >= sizeof(u->host)) return fail("invalid HTTP host");
    memcpy(u->host, begin, n); u->host[n] = 0;
    if (strpbrk(u->host, "@[]?")) return fail("URL user-info, IPv6 literals and host queries are not supported");
    strcpy(u->port, "80");
    char *colon = strchr(u->host, ':');
    if (colon) {
        off_t port;
        if (number(colon + 1, &port) || port < 1 || port > 65535 || strspn(colon+1,"0123456789") != strlen(colon+1)) return fail("invalid port");
        snprintf(u->port, sizeof(u->port), "%jd", (intmax_t)port);
        *colon = 0;
    }
    if (!u->host[0]) return fail("empty host");
    snprintf(u->target, sizeof(u->target), "%s", slash ? slash : "/");
    return 0;
}

static int connect_http(const struct url *u)
{
    struct addrinfo hints = {0}, *list, *p;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    int err = getaddrinfo(u->host, u->port, &hints, &list);
    if (err) { fprintf(stderr, "DNS: %s\n", gai_strerror(err)); return -1; }
    int fd = -1;
    for (p = list; p && !stopped; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        struct timeval tv = {5, 0};
        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ||
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv))) { close(fd); fd=-1; continue; }
        if (!connect(fd, p->ai_addr, p->ai_addrlen)) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0) fail("HTTP connection failed or interrupted (5-second socket timeout)");
    return fd;
}

static int decimal(const char *s, off_t *n)
{
    if (!*s || strspn(s, "0123456789") != strlen(s)) return -1;
    return number(s, n);
}

static int strong_etag(const char *s)
{
    size_t n = strlen(s);
    if (n < 2 || s[0] != '"' || s[n-1] != '"') return 0;
    for (size_t i=1; i+1<n; ++i)
        if ((unsigned char)s[i] < 33 || (unsigned char)s[i] > 126 || s[i] == '"') return 0;
    return 1;
}

/* 只读取响应头；保留 socket 中的全部正文，避免首块数据丢失。 */
static int request(const char *url, const char *method, off_t start,
                   const char *etag, struct response *r)
{
    struct url u;
    memset(r, 0, sizeof(*r)); r->fd = -1;
    r->length = r->start = r->end = r->total = -1;
    if (parse_url(url, &u)) return -1;
    int fd = connect_http(&u);
    if (fd < 0) return -1;
    char req[4096], range[512] = "";
    if (!strcmp(method, "GET")) snprintf(range, sizeof(range),
        "Range: bytes=%jd-\r\nIf-Range: %s\r\n", (intmax_t)start, etag);
    int len = snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: %s:%s\r\nConnection: close\r\nAccept-Encoding: identity\r\n%s\r\n",
                       method, u.target, u.host, u.port, range);
    if (len < 0 || (size_t)len >= sizeof(req) || write_all(fd, req, (size_t)len)) goto bad;
    char header[16384]; size_t used = 0;
    while (used + 1 < sizeof(header) && !stopped) {
        ssize_t n = read(fd, header + used, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n != 1) { fail("HTTP header incomplete or timed out"); goto bad; }
        used++;
        if (used >= 4 && !memcmp(header + used - 4, "\r\n\r\n", 4)) break;
    }
    if (stopped || used < 4 || memcmp(header + used - 4, "\r\n\r\n", 4)) { fail("HTTP header too large or interrupted"); goto bad; }
    if (memchr(header, 0, used)) { fail("NUL in HTTP header"); goto bad; }
    header[used] = 0;
    char *save, *line = strtok_r(header, "\r\n", &save);
    if (!line || strlen(line)<12 || (strncmp(line,"HTTP/1.1 ",9) && strncmp(line,"HTTP/1.0 ",9)) ||
        line[9]<'1' || line[9]>'5' || !isdigit((unsigned char)line[10]) ||
        !isdigit((unsigned char)line[11]) || (line[12] && line[12]!=' ')) {
        fail("invalid HTTP status"); goto bad;
    }
    r->status=(line[9]-'0')*100+(line[10]-'0')*10+line[11]-'0';
    while ((line = strtok_r(NULL, "\r\n", &save))) {
        char *sep = strchr(line, ':');
        if (!sep) { fail("invalid HTTP header field"); goto bad; }
        *sep++ = 0;
        while (*sep == ' ' || *sep == '\t') sep++;
        size_t n = strlen(sep);
        while (n && (sep[n-1] == ' ' || sep[n-1] == '\t')) sep[--n] = 0;
        if (!strcasecmp(line, "Content-Length")) {
            if (r->length != -1 || decimal(sep, &r->length)) { fail("invalid or duplicate Content-Length"); goto bad; }
        } else if (!strcasecmp(line, "ETag")) {
            if (r->etag[0] || strlen(sep) >= sizeof(r->etag) || !strong_etag(sep)) { fail("a unique strong ETag is required"); goto bad; }
            strcpy(r->etag, sep);
        } else if (!strcasecmp(line, "Content-Range")) {
            /* 不用 sscanf 转换不可信大整数；逐段检查纯数字和溢出。 */
            char *dash=strchr(sep,'-'), *slash=strchr(sep,'/');
            if (r->start!=-1 || strncmp(sep,"bytes ",6) || !dash || !slash || dash>=slash) {
                fail("invalid Content-Range"); goto bad;
            }
            *dash=0; *slash=0;
            if (decimal(sep+6,&r->start) || decimal(dash+1,&r->end) || decimal(slash+1,&r->total) ||
                r->end<r->start || r->total<=r->end) {
                fail("invalid Content-Range"); goto bad;
            }
        } else if (!strcasecmp(line, "Transfer-Encoding") ||
                   (!strcasecmp(line, "Content-Encoding") && strcasecmp(sep, "identity"))) {
            fail("chunked or compressed HTTP responses are not supported"); goto bad;
        }
    }
    if (r->length < 0 || !r->etag[0]) { fail("server must provide Content-Length and strong ETag"); goto bad; }
    r->fd = fd;
    return 0;
bad:
    close(fd);
    return -1;
}

struct progress {
    uintmax_t dev, ino;
    off_t size, offset;
    uint64_t hash;
    char url[URL_SIZE], etag[ETAG_SIZE];
};

/* FNV-1a 用于发现本地已完成前缀被意外修改，不作为密码学安全校验。 */
static uint64_t hash_bytes(uint64_t h, const unsigned char *p, size_t n)
{
    while (n--) { h ^= *p++; h *= UINT64_C(1099511628211); }
    return h;
}

static int verify_prefix(int fd, const struct progress *s)
{
    unsigned char buf[BUF_SIZE];
    uint64_t h = HASH_INIT;
    off_t remaining = s->offset;
    if (lseek(fd, 0, SEEK_SET) < 0) return syserr("lseek");
    while (remaining > 0 && !stopped) {
        size_t want = remaining > BUF_SIZE ? BUF_SIZE : (size_t)remaining;
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail("cannot verify downloaded prefix");
        h = hash_bytes(h, buf, (size_t)n); remaining -= n;
    }
    if (stopped) return fail("prefix verification interrupted");
    return h == s->hash ? 0 : fail("downloaded prefix was modified; refusing unsafe resume");
}

static int sync_parent(const char *path)
{
    char *p = strdup(path);
    if (!p) return fail("out of memory");
    char *slash = strrchr(p, '/');
    if (slash) { if (slash == p) slash[1] = 0; else *slash = 0; }
    int fd = open(slash ? p : ".", O_RDONLY | O_DIRECTORY);
    free(p);
    if (fd < 0) return syserr("open state directory");
    int rc = sync_fd(fd,0);
    close(fd);
    return rc ? syserr("fsync state directory") : 0;
}

/* 顺序：数据落盘 -> 写临时状态 -> 状态落盘 -> 原子替换 -> 目录落盘。 */
static int checkpoint(int datafd, const char *path, const struct progress *s, int *published)
{
    if (published) *published=0;
    if (sync_fd(datafd,1)) return syserr("fdatasync download");
    size_t n = strlen(path) + 12;
    char *tmp = malloc(n);
    if (!tmp) return fail("out of memory");
    snprintf(tmp, n, "%s.XXXXXX", path);
    int fd = mkstemp(tmp), rc = -1;
    if (fd < 0) { free(tmp); return syserr("mkstemp state"); }
    char data[4096];
    int len = snprintf(data, sizeof(data), "SPARSEGET1\n%ju %ju %jd %jd %" PRIu64 "\n%s\n%s\n",
                       s->dev, s->ino, (intmax_t)s->size, (intmax_t)s->offset, s->hash, s->url, s->etag);
    if (len < 0 || (size_t)len >= sizeof(data)) { fail("state too large"); goto done; }
    if (write_all(fd, data, (size_t)len) || sync_fd(fd,0)) { syserr("write/fsync state"); goto done; }
    if (close(fd)) { fd=-1; syserr("close state"); goto done; }
    fd=-1;
    if (rename(tmp, path)) { syserr("rename state"); goto done; }
    if (published) *published=1;
    rc = sync_parent(path);
done:
    if (fd >= 0) close(fd);
    if (rc) unlink(tmp);
    free(tmp);
    return rc;
}

static int read_state(const char *path, struct progress *s)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return syserr("open resume state");
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > 4095) { close(fd); return fail("invalid state file"); }
    FILE *f = fdopen(fd,"r");
    if (!f) { close(fd); return syserr("fdopen"); }
    /* 单独为行末换行预留空间，保证最大合法 URL/ETag 能写回再读出。 */
    char magic[32], nums[256], url_line[URL_SIZE+2], etag_line[ETAG_SIZE+2];
    int ok = fgets(magic,sizeof(magic),f) && !strcmp(magic,"SPARSEGET1\n") &&
        fgets(nums,sizeof(nums),f) && strchr(nums,'\n') &&
        fgets(url_line,sizeof(url_line),f) && strchr(url_line,'\n') &&
        fgets(etag_line,sizeof(etag_line),f) && strchr(etag_line,'\n') && fgetc(f)==EOF && !ferror(f);
    fclose(f);
    if (!ok) return fail("invalid resume state");
    uintmax_t values[5];
    char *save, *token=strtok_r(nums," \n",&save);
    for (int i=0;i<5;i++) {
        if (!token || !*token || strspn(token,"0123456789")!=strlen(token)) return fail("invalid state number");
        errno=0;
        values[i]=strtoumax(token,NULL,10);
        if (errno) return fail("state number overflow");
        token=strtok_r(NULL," \n",&save);
    }
    if (token || values[2]>INT64_MAX || values[3]>values[2] || values[4]>UINT64_MAX) return fail("invalid state bounds");
    s->dev=values[0]; s->ino=values[1]; s->size=(off_t)values[2]; s->offset=(off_t)values[3]; s->hash=values[4];
    url_line[strcspn(url_line,"\n")]=0;
    etag_line[strcspn(etag_line,"\n")]=0;
    if (strlen(url_line)>=sizeof(s->url) || strlen(etag_line)>=sizeof(s->etag)) return fail("state field too long");
    strcpy(s->url,url_line);
    strcpy(s->etag,etag_line);
    if (!strong_etag(s->etag)) return fail("invalid state ETag");
    return 0;
}

/* 首次初始化在状态发布前失败：仅撤销本次新建且身份未变的目标。
 * 不能把“已有文件缺状态”当成可覆盖的空文件；状态已发布时保留完整配对。
 */
static int rollback_initial(int fd, const char *path, const char *state)
{
    struct stat own, current, side;
    if (flock(fd,LOCK_EX|LOCK_NB)) return fail("initial destination is busy; rollback refused");
    if (fstat(fd,&own) || lstat(path,&current)) return syserr("identify initial destination");
    if (own.st_dev!=current.st_dev || own.st_ino!=current.st_ino)
        return fail("initial destination path changed; rollback refused");
    if (!lstat(state,&side) || errno!=ENOENT)
        return fail("resume state exists or is inaccessible; rollback refused");
    if (unlink(path)) return syserr("rollback initial destination");
    if (sync_parent(path)) return -1;
    fprintf(stderr,"initialization rolled back; rerun the same command after resolving the I/O error\n");
    return 0;
}

static int cmd_download(const char *url, const char *path, unsigned delay)
{
    int fd=-1, net=-1, rc=-1;
    int ready=0, save_failed=0;
    int created=0, initial_published=0;
    off_t saved_offset=0;
    char *state = sidecar(path);
    struct progress s = {0};
    struct response head, body;
    struct stat st;
    if (!state) return fail("out of memory");
    /* 先验证服务端元数据，不截断或覆盖任何既有文件。 */
    if (request(url,"HEAD",0,NULL,&head)) goto done;
    close(head.fd);
    if (head.status != 200) { fail("HEAD must return 200"); goto done; }
    fd = open(path,O_RDWR | O_NOFOLLOW | O_NONBLOCK);
    if (fd >= 0) {
        if (regular_locked(fd) || fstat(fd,&st) || read_state(state,&s)) goto done;
        if (s.dev != (uintmax_t)st.st_dev || s.ino != (uintmax_t)st.st_ino || s.size != st.st_size ||
            s.size != head.length || strcmp(s.url,url) || strcmp(s.etag,head.etag)) {
            fail("file, URL or remote content changed; refusing unsafe resume"); goto done;
        }
        if (verify_prefix(fd,&s)) goto done;
    } else {
        if (errno != ENOENT) { syserr("open destination"); goto done; }
        if (!lstat(state,&st) || errno != ENOENT) { fail("orphaned resume state exists or is inaccessible"); goto done; }
        fd = open(path,O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW,0644);
        if (fd < 0) { syserr("create destination"); goto done; }
        created=1;
        if (regular_locked(fd) || ftruncate(fd,head.length) || fstat(fd,&st)) { syserr("prepare sparse destination"); goto done; }
        s.dev=st.st_dev; s.ino=st.st_ino; s.size=head.length; s.hash=HASH_INIT;
        snprintf(s.url,sizeof(s.url),"%s",url);
        strcpy(s.etag,head.etag);
        if (checkpoint(fd,state,&s,&initial_published)) { save_failed=1; goto done; }
    }
    ready=1;
    saved_offset=s.offset;
    ui_start(path,s.offset,s.size);
    if (s.offset == s.size) { rc=0; goto done; }
    if (stopped) { rc=130; goto done; }
    if (request(url,"GET",s.offset,s.etag,&body)) goto done;
    net=body.fd;
    /* 忽略 Range 的 200 响应不能直接拼接；否则会悄悄破坏文件。 */
    if (body.status != 206 || body.start != s.offset || body.end != s.size-1 ||
        body.total != s.size || body.length != s.size-s.offset || strcmp(body.etag,s.etag)) {
        fail("server did not honor Range/If-Range or returned inconsistent metadata"); goto done;
    }
    unsigned char buf[BUF_SIZE];
    while (s.offset < s.size && !stopped) {
        size_t want = s.size-s.offset > BUF_SIZE ? BUF_SIZE : (size_t)(s.size-s.offset);
        ssize_t n = read(net,buf,want);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { fail("HTTP body interrupted, truncated or timed out; rerun to resume"); goto done; }
        if (lseek(fd,s.offset,SEEK_SET)<0) { syserr("lseek download"); goto done; }
        if (write_all(fd,buf,(size_t)n)) goto done;
        s.hash=hash_bytes(s.hash,buf,(size_t)n); s.offset+=n;
        /* 写入进度与持久化进度分离；崩溃后只相信上次原子提交的状态。 */
        if (s.offset-saved_offset >= CHECKPOINT_BYTES) {
            if (checkpoint(fd,state,&s,NULL)) { save_failed=1; goto done; }
            saved_offset=s.offset;
        }
        ui_progress(s.offset,s.size,saved_offset);
        if (delay && s.offset < s.size) {
            struct timespec ts = {delay/1000,(long)(delay%1000)*1000000};
            while (nanosleep(&ts,&ts) && errno==EINTR && !stopped) { }
        }
    }
    rc=stopped ? 130 : 0;
done:
    /* 正常结束、中断、网络或写入错误：保存已完整写入且已计入 hash 的前缀。
     * 若某次 checkpoint 已失败，不在退出路径重试或谎报保存成功。
     * 部分短写留下的尾部不计入 offset，下次从已提交偏移覆盖重传。
     */
    if (ready && !save_failed && s.offset!=saved_offset) {
        if (checkpoint(fd,state,&s,NULL)) save_failed=1;
        else saved_offset=s.offset;
    }
    if (created && !ready && !initial_published && rollback_initial(fd,path,state))
        fail("initialization cleanup failed; inspect the reported paths before retrying");
    if (net>=0) close(net);
    if (fd>=0 && close(fd)) { syserr("close destination"); save_failed=1; }
    free(state);
    if (save_failed) {
        fail("checkpoint/close failed; latest progress is NOT confirmed saved; retry will validate the state on disk");
        return -1;
    }
    if (stopped) {
        if (ready) ui_paused(saved_offset,s.size);
        return 130;
    }
    if (!rc) ui_complete(saved_offset,s.size);
    else ui_end_progress();
    return rc;
}

/* Reset removes only the two active names, retaining bytes/inodes in a
 * unique adjacent directory. A preview is the default; no permanent unlink.
 * The pair is not a crash-atomic transaction. Ordinary second-rename errors
 * roll the first rename back; abrupt termination requires inspecting paths.
 */
static int move_no_replace(const char *from, const char *to)
{
    if (renameat2(AT_FDCWD,from,AT_FDCWD,to,RENAME_NOREPLACE))
        return syserr("rename without overwrite");
    return 0;
}

static int same_name(int fd, const char *path)
{
    struct stat a,b;
    if (fstat(fd,&a) || lstat(path,&b)) return syserr("identify reset file");
    if (!S_ISREG(b.st_mode) || a.st_dev!=b.st_dev || a.st_ino!=b.st_ino)
        return fail("file path changed; operation refused");
    return 0;
}

static int complete_pair(const char *path, const char *state, int *datafd, int *statefd)
{
    struct progress s={0};
    struct stat st;
    *datafd=open(path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (*datafd<0) return syserr("open completed download");
    if (regular_locked(*datafd)) return -1;
    *statefd=open(state,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (*statefd<0) return syserr("open completed state");
    if (regular_locked(*statefd) || same_name(*datafd,path) || same_name(*statefd,state) ||
        read_state(state,&s) || fstat(*datafd,&st)) return -1;
    if (s.dev!=(uintmax_t)st.st_dev || s.ino!=(uintmax_t)st.st_ino ||
        s.size!=st.st_size || s.offset!=s.size)
        return fail("reset/restore requires a completed, matching download and state");
    return verify_prefix(*datafd,&s);
}

static char *join_path(const char *dir, const char *name)
{
    size_t len=strlen(dir)+strlen(name)+2;
    char *p=malloc(len);
    if (p) snprintf(p,len,"%s/%s",dir,name);
    return p;
}

static int absent(const char *path)
{
    struct stat st;
    if (!lstat(path,&st)) return fail("destination exists; it will not be overwritten");
    return errno==ENOENT ? 0 : syserr("check destination");
}

static int move_pair(const char *data, const char *state, const char *newdata,
                     const char *newstate, int datafd, int statefd)
{
    if (same_name(datafd,data) || same_name(statefd,state)) return -1;
    if (stopped) return fail("interrupted before moving files");
    if (move_no_replace(data,newdata)) return -1;
    if (move_no_replace(state,newstate)) {
        if (move_no_replace(newdata,data))
            fprintf(stderr,"rollback failed; retained data: %s; state: %s\n",newdata,state);
        else sync_parent(data);
        return -1;
    }
    /* Always attempt both directory syncs, even if one reports an error. */
    int a=sync_parent(data), b=sync_parent(newdata);
    if (a || b) {
        fprintf(stderr,"files moved, durability unconfirmed: %s and %s\n",newdata,newstate);
        return -1;
    }
    return 0;
}

static int cmd_reset(const char *path, int confirmed)
{
    int datafd=-1,statefd=-1,rc=-1;
    char *state=sidecar(path), *full=NULL, *backup=NULL, *data=NULL, *saved=NULL;
    if (!state) return fail("out of memory");
    if (complete_pair(path,state,&datafd,&statefd)) goto done;
    full=realpath(path,NULL);
    if (!full) { syserr("resolve download path"); goto done; }
    printf("reset preview: 2 active paths\n  %s\n  %s.resume\n",full,full);
    puts("Effect: move completed data + checkpoint to a unique adjacent backup.\n"
         "The same download command will then start at 0; no bytes are permanently deleted.");
    if (!confirmed) { puts("Preview only. Run reset FILE --confirm after reviewing the paths."); rc=0; goto done; }
    size_t n=strlen(full)+24;
    backup=malloc(n);
    if (!backup) { fail("out of memory"); goto done; }
    snprintf(backup,n,"%s.reset-XXXXXX",full);
    if (!mkdtemp(backup)) { syserr("create reset backup"); goto done; }
    data=join_path(backup,"data.bin"); saved=join_path(backup,"data.bin.resume");
    if (!data || !saved) { fail("out of memory; empty backup retained"); goto done; }
    if (move_pair(path,state,data,saved,datafd,statefd)) {
        fprintf(stderr,"inspect retained backup: %s\n",backup); goto done;
    }
    printf("reset complete: restart the original download command to demonstrate again.\n"
           "backup_dir=%s\nRestore with: sparseget restore FILE BACKUP_DIR\n",backup);
    rc=0;
done:
    if (statefd>=0) close(statefd);
    if (datafd>=0) close(datafd);
    free(state); free(full); free(backup); free(data); free(saved);
    return rc;
}

static int cmd_restore(const char *path, const char *backup)
{
    int datafd=-1,statefd=-1,rc=-1;
    struct stat dir;
    char *data=join_path(backup,"data.bin"), *saved=join_path(backup,"data.bin.resume");
    char *state=sidecar(path);
    if (!data || !saved || !state) { fail("out of memory"); goto done; }
    if (lstat(backup,&dir) || !S_ISDIR(dir.st_mode)) { fail("backup must be a real directory"); goto done; }
    if (absent(path) || absent(state) || complete_pair(data,saved,&datafd,&statefd)) goto done;
    if (move_pair(data,saved,path,state,datafd,statefd)) goto done;
    printf("restored: %s and %s; empty backup directory retained\n",path,state);
    rc=0;
done:
    if (statefd>=0) close(statefd);
    if (datafd>=0) close(datafd);
    free(data); free(saved); free(state);
    return rc;
}

static void usage(void)
{
    if (ui_enabled()) { ui_help(); return; }
    puts("Usage:\n"
         "  sparseget create FILE SIZE          SIZE >= 128K; K/M/G = powers of 1024\n"
         "  sparseget write FILE OFFSET TEXT    lseek + write; never extends FILE\n"
         "  sparseget status FILE               logical size, blocks, hole ratio\n"
         "  sparseget download URL FILE [--delay-ms N]\n"
         "                                      Ctrl+C pauses; same command resumes\n"
         "  sparseget reset FILE [--confirm]    preview / retain completed pair, restart demo\n"
         "  sparseget restore FILE BACKUP_DIR  restore retained pair, never overwrite\n"
         "Checkpoints: about every 1 MiB, plus handled exit; progress is not saved_offset.\n"
         "After a crash, bytes beyond the last saved offset are downloaded again.\n"
         "HTTP scope: plain HTTP + HEAD + Content-Length + strong ETag + Range.\n"
         "No HTTPS, redirects, compressed or chunked responses.\n"
         "ETag identifies a remote version; it is NOT necessarily a content hash.\n"
         "FNV checks local prefix changes; compare source and destination SHA-256 separately.\n"
         "Writing/downloading requires a regular file with exactly one hard link.\n"
         "Exit codes: 0 success, 1 error, 2 usage, 130 interrupted.\n"
         "Existing files are preserved; FILE.resume stores download checkpoints.");
}

int main(int argc, char **argv)
{
    ui_init();
    struct sigaction sa = {0};
    sa.sa_handler=on_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT,&sa,NULL) || sigaction(SIGTERM,&sa,NULL)) return 1;
    signal(SIGPIPE,SIG_IGN);
    int rc;
    if (argc==2 && (!strcmp(argv[1],"--help") || !strcmp(argv[1],"help"))) { usage(); return 0; }
    if (argc==4 && !strcmp(argv[1],"create")) rc=cmd_create(argv[2],argv[3]);
    else if (argc==5 && !strcmp(argv[1],"write")) rc=cmd_write(argv[2],argv[3],argv[4]);
    else if (argc==3 && !strcmp(argv[1],"status")) rc=cmd_status(argv[2]);
    else if ((argc==3 || (argc==4 && !strcmp(argv[3],"--confirm"))) && !strcmp(argv[1],"reset"))
        rc=cmd_reset(argv[2],argc==4);
    else if (argc==4 && !strcmp(argv[1],"restore")) rc=cmd_restore(argv[2],argv[3]);
    else if ((argc==4 || argc==6) && !strcmp(argv[1],"download")) {
        off_t delay=0;
        if (argc==6 && (strcmp(argv[4],"--delay-ms") || decimal(argv[5],&delay) || delay>10000)) {
            fail("--delay-ms must be an integer from 0 to 10000"); return 2;
        }
        rc=cmd_download(argv[2],argv[3],(unsigned)delay);
    } else { usage(); return 2; }
    return rc==130 ? 130 : rc ? 1 : 0;
}
