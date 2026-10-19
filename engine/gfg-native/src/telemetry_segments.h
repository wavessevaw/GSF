/* SPDX-License-Identifier: GPL-3.0-or-later
 * Linux writer-owned JSONL segments. No rename/truncate of an active writer.
 * One FILE per requested path per process; multiple bridges share its lock.
 * Distinct processes use distinct monotonic names. Disk/size failures stop
 * detail explicitly and retain all earlier segments. Pixel path is untouched.
 */
#ifndef GSF_TELEMETRY_SEGMENTS_H
#define GSF_TELEMETRY_SEGMENTS_H
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

typedef struct gsf_log_sink {
    struct gsf_log_sink *next;
    FILE *stream;
    char *requested;
    char prefix[PATH_MAX],state_path[PATH_MAX];
    char *pending;
    size_t pending_size,pending_capacity;
    uint64_t segment_limit,total_limit,total_bytes,segment_bytes,records,dropped;
    unsigned segment,refs;
    int fd,closed;
    const char *stop;
} gsf_log_sink;
static pthread_mutex_t gsf_logs_lock=PTHREAD_MUTEX_INITIALIZER;
static gsf_log_sink *gsf_logs;
static uint64_t gsf_log_setting(const char *name,uint64_t fallback) {
    const char *v=getenv(name);if(!v || !*v)return fallback;
    char *end=NULL;errno=0;unsigned long long n=strtoull(v,&end,10);
    return !errno && end!=v && !*end && n>=256 && n<=1024ull*1024*1024?n:fallback;
}
static void gsf_log_state(gsf_log_sink *s) {
    char temp[PATH_MAX];
    if(snprintf(temp,sizeof(temp),"%s.tmp",s->state_path)>=(int)sizeof(temp))return;
    int fd=open(temp,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0)return;
    char body[1024];int n=snprintf(body,sizeof(body),
        "{\"schema\":1,\"pid\":%ld,\"closed\":%s,\"last_segment\":%u,"
        "\"bytes_written\":%llu,\"records_written\":%llu,\"dropped_bytes\":%llu,"
        "\"partial_line_bytes\":%zu,\"stop_reason\":\"%s\",\"segment_limit\":%llu,\"total_limit\":%llu}\n",
        (long)getpid(),s->closed?"true":"false",s->segment,(unsigned long long)s->total_bytes,
        (unsigned long long)s->records,(unsigned long long)s->dropped,s->pending_size,
        s->stop?s->stop:"none",(unsigned long long)s->segment_limit,(unsigned long long)s->total_limit);
    if(n>0 && n<(int)sizeof(body)) {
        ssize_t done=write(fd,body,(size_t)n);
        if(close(fd)==0 && done==n){(void)rename(temp,s->state_path);return;}
    } else close(fd);
    (void)unlink(temp);
}
static void gsf_log_stop(gsf_log_sink *s,const char *reason) {
    if(!s->stop){s->stop=reason;gsf_log_state(s);}
}
static int gsf_log_segment(gsf_log_sink *s) {
    char path[PATH_MAX];
    if(snprintf(path,sizeof(path),"%s.%06u.jsonl",s->prefix,s->segment)>=(int)sizeof(path)) {
        gsf_log_stop(s,"path_too_long");return 0;
    }
    int fd=open(path,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC|O_NOFOLLOW,0600);
    if(fd<0){gsf_log_stop(s,"segment_open_failed");return 0;}
    struct statvfs fs;
    if(fstatvfs(fd,&fs)==0 && fs.f_frsize && fs.f_bavail<64ull*1024*1024/fs.f_frsize) {
        close(fd);unlink(path);gsf_log_stop(s,"disk_reserve_reached");return 0;
    }
    if(s->fd>=0)close(s->fd);
    s->fd=fd;s->segment_bytes=0;gsf_log_state(s);return 1;
}
static int gsf_log_line(gsf_log_sink *s,const char *line,size_t size) {
    if(s->total_bytes>s->total_limit || size>s->total_limit-s->total_bytes) {
        gsf_log_stop(s,"detail_budget_reached");return 0;
    }
    if(s->segment_bytes && size>s->segment_limit-(s->segment_bytes<s->segment_limit?s->segment_bytes:s->segment_limit)) {
        ++s->segment;if(!gsf_log_segment(s))return 0;
    }
    const uint64_t before=s->segment_bytes;
    size_t offset=0;
    while(offset<size) {
        ssize_t n=write(s->fd,line+offset,size-offset);
        if(n<0 && errno==EINTR)continue;
        if(n<=0){
            if(ftruncate(s->fd,(off_t)before)==0){s->segment_bytes=before;s->total_bytes-=offset;}
            gsf_log_stop(s,"detail_write_failed");return 0;
        }
        offset+=(size_t)n;s->segment_bytes+=(size_t)n;s->total_bytes+=(size_t)n;
    }
    ++s->records;return 1;
}
static ssize_t gsf_log_write(void *cookie,const char *data,size_t size) {
    gsf_log_sink *s=cookie;
    if(s->stop){s->dropped+=size;return (ssize_t)size;}
    for(size_t pos=0;pos<size;) {
        const char *end=memchr(data+pos,'\n',size-pos);
        const size_t n=end?(size_t)(end-data-pos)+1:size-pos;
        if(s->pending_size+n>128*1024) {
            s->dropped+=size-pos;gsf_log_stop(s,"record_too_large");break;
        }
        if(s->pending_size+n>s->pending_capacity) {
            const size_t capacity=s->pending_size+n+1024;
            char *next=realloc(s->pending,capacity);
            if(!next){s->dropped+=size-pos;gsf_log_stop(s,"record_buffer_failed");break;}
            s->pending=next;s->pending_capacity=capacity;
        }
        memcpy(s->pending+s->pending_size,data+pos,n);s->pending_size+=n;pos+=n;
        if(end) {
            if(!gsf_log_line(s,s->pending,s->pending_size)) {
                s->dropped+=s->pending_size+size-pos;s->pending_size=0;break;
            }
            s->pending_size=0;
        }
    }
    return (ssize_t)size; /* Logging failure must never fail a game operation. */
}
static int gsf_log_close_cookie(void *cookie) {
    gsf_log_sink *s=cookie;
    if(s->pending_size)gsf_log_stop(s,"unterminated_record");
    s->closed=1;gsf_log_state(s);
    if(s->fd>=0)close(s->fd);
    free(s->requested);free(s->pending);free(s);return 0;
}
static FILE *gsf_telemetry_open(const char *path) {
    const char *enabled=getenv("GFG_NATIVE_TELEMETRY_SEGMENTED");
    if(!enabled || strcmp(enabled,"1")) {
        int fd=open(path,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)return NULL;
        FILE *f=fdopen(fd,"a");if(!f)close(fd);else (void)setvbuf(f,NULL,_IOFBF,65536);return f;
    }
    pthread_mutex_lock(&gsf_logs_lock);
    for(gsf_log_sink *s=gsf_logs;s;s=s->next)if(!strcmp(path,s->requested)) {
        ++s->refs;pthread_mutex_unlock(&gsf_logs_lock);return s->stream;
    }
    gsf_log_sink *s=calloc(1,sizeof(*s));
    if(!s){pthread_mutex_unlock(&gsf_logs_lock);return NULL;}
    s->fd=-1;s->refs=1;s->requested=strdup(path);
    s->segment_limit=gsf_log_setting("GFG_NATIVE_TELEMETRY_SEGMENT_BYTES",32ull*1024*1024);
    s->total_limit=gsf_log_setting("GFG_NATIVE_TELEMETRY_TOTAL_BYTES",512ull*1024*1024);
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    const size_t len=strlen(path),stem=len>6 && !strcmp(path+len-6,".jsonl")?len-6:len;
    int n=snprintf(s->prefix,sizeof(s->prefix),"%.*s.%ld.%llu",(int)stem,path,(long)getpid(),
        (unsigned long long)((uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec));
    if(!s->requested || n<0 || n>=(int)sizeof(s->prefix) ||
       snprintf(s->state_path,sizeof(s->state_path),"%s.state.json",s->prefix)>=(int)sizeof(s->state_path) ||
       !gsf_log_segment(s))goto fail;
    cookie_io_functions_t ops={.write=gsf_log_write,.close=gsf_log_close_cookie};
    s->stream=fopencookie(s,"a",ops);
    if(!s->stream)goto fail;
    (void)setvbuf(s->stream,NULL,_IOFBF,65536);
    s->next=gsf_logs;gsf_logs=s;pthread_mutex_unlock(&gsf_logs_lock);return s->stream;
fail:
    if(s->fd>=0)close(s->fd);
    free(s->requested);free(s);pthread_mutex_unlock(&gsf_logs_lock);return NULL;
}
static void gsf_telemetry_close(FILE *f) {
    if(!f)return;
    pthread_mutex_lock(&gsf_logs_lock);
    for(gsf_log_sink **p=&gsf_logs;*p;p=&(*p)->next)if((*p)->stream==f) {
        gsf_log_sink *s=*p;
        if(--s->refs){pthread_mutex_unlock(&gsf_logs_lock);return;}
        *p=s->next;pthread_mutex_unlock(&gsf_logs_lock);fclose(f);return;
    }
    pthread_mutex_unlock(&gsf_logs_lock);fclose(f);
}
#endif
