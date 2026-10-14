/* 仅用于测试的 LD_PRELOAD 故障注入器，不链接到正式程序。
 * SPARSEGET_FAULT=事件:次数:动作
 * 事件 data/state/rename/dir；动作 kill-before/kill-after/error/interrupt-error。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned data_calls, state_calls, rename_calls, dir_calls, truncate_calls, temp_calls, write_calls;

static int inject(const char *event, unsigned count, int after)
{
    const char *plan=getenv("SPARSEGET_FAULT");
    if (!plan) return 0;
    char key[128];
    snprintf(key,sizeof(key),"%s:%u:",event,count);
    if (strncmp(plan,key,strlen(key))) return 0;
    const char *action=plan+strlen(key);
    if (!strcmp(action,after ? "kill-after" : "kill-before")) {
        kill(getpid(),SIGKILL);
        _exit(99);
    }
    if (!after && (!strcmp(action,"error") || !strcmp(action,"interrupt-error"))) {
        if (!strcmp(action,"interrupt-error")) raise(SIGINT);
        errno=EIO;
        return 1;
    }
    if (!after && !strcmp(action,"interrupt-eintr")) {
        raise(SIGINT);
        errno=EINTR;
        return 1;
    }
    return 0;
}

int fdatasync(int fd)
{
    int (*real_fn)(int)=dlsym(RTLD_NEXT,"fdatasync");
    if (!real_fn) _exit(98);
    unsigned n=++data_calls;
    if (inject("data",n,0)) return -1;
    int rc=real_fn(fd);
    if (!rc) inject("data",n,1);
    return rc;
}

int fsync(int fd)
{
    int (*real_fn)(int)=dlsym(RTLD_NEXT,"fsync");
    if (!real_fn) _exit(98);
    struct stat s;
    if (fstat(fd,&s)) return -1;
    const char *event=S_ISDIR(s.st_mode) ? "dir" : "state";
    unsigned n=S_ISDIR(s.st_mode) ? ++dir_calls : ++state_calls;
    if (inject(event,n,0)) return -1;
    int rc=real_fn(fd);
    if (!rc) inject(event,n,1);
    return rc;
}

int rename(const char *old, const char *new)
{
    int (*real_fn)(const char *,const char *)=dlsym(RTLD_NEXT,"rename");
    if (!real_fn) _exit(98);
    unsigned n=++rename_calls;
    if (inject("rename",n,0)) return -1;
    int rc=real_fn(old,new);
    if (!rc) inject("rename",n,1);
    return rc;
}

int ftruncate64(int fd, off64_t size)
{
    int (*real_fn)(int,off64_t)=dlsym(RTLD_NEXT,"ftruncate64");
    if (!real_fn) _exit(98);
    if (inject("truncate",++truncate_calls,0)) return -1;
    return real_fn(fd,size);
}

int ftruncate(int fd, off_t size)
{
    int (*real_fn)(int,off_t)=dlsym(RTLD_NEXT,"ftruncate");
    if (!real_fn) _exit(98);
    if (inject("truncate",++truncate_calls,0)) return -1;
    return real_fn(fd,size);
}

int mkstemp64(char *path)
{
    int (*real_fn)(char *)=dlsym(RTLD_NEXT,"mkstemp64");
    if (!real_fn) _exit(98);
    if (inject("temp",++temp_calls,0)) return -1;
    return real_fn(path);
}

int mkstemp(char *path)
{
    int (*real_fn)(char *)=dlsym(RTLD_NEXT,"mkstemp");
    if (!real_fn) _exit(98);
    if (inject("temp",++temp_calls,0)) return -1;
    return real_fn(path);
}

ssize_t write(int fd, const void *data, size_t n)
{
    ssize_t (*real_fn)(int,const void *,size_t)=dlsym(RTLD_NEXT,"write");
    if (!real_fn) _exit(98);
    struct stat s;
    if (!fstat(fd,&s) && S_ISREG(s.st_mode) && inject("write",++write_calls,0)) return -1;
    return real_fn(fd,data,n);
}
