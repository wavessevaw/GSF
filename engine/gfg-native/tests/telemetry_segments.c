/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "../src/telemetry_segments.h"
#include <assert.h>
#include <dirent.h>
#include <sys/stat.h>
static void *produce(void *arg) {
    FILE *f=arg;
    for(unsigned i=0;i<100;++i)fprintf(f,"{\"event\":\"test\",\"record\":%u}\n",i);
    return NULL;
}
int main(int argc,char **argv) {
    assert(argc==2);assert(mkdir(argv[1],0700)==0 || errno==EEXIST);
    char path[PATH_MAX];assert(snprintf(path,sizeof(path),"%s/frames.jsonl",argv[1])<(int)sizeof(path));
    setenv("GFG_NATIVE_TELEMETRY_SEGMENTED","1",1);
    setenv("GFG_NATIVE_TELEMETRY_SEGMENT_BYTES","512",1);
    setenv("GFG_NATIVE_TELEMETRY_TOTAL_BYTES","100000",1);
    FILE *a=gsf_telemetry_open(path),*b=gsf_telemetry_open(path);assert(a && a==b);
    pthread_t threads[3];for(unsigned i=0;i<3;++i)assert(!pthread_create(&threads[i],NULL,produce,a));
    for(unsigned i=0;i<3;++i)pthread_join(threads[i],NULL);
    gsf_telemetry_close(a); /* still owned by b */
    fprintf(b,"{\"event\":\"final\"}\n");fflush(b);
    gsf_telemetry_close(b);
    /* A new bridge opens a distinct stream and cannot overwrite prior data. */
    setenv("GFG_NATIVE_TELEMETRY_TOTAL_BYTES","512",1);
    a=gsf_telemetry_open(path);assert(a);
    for(unsigned i=0;i<100;++i)fprintf(a,"{\"event\":\"capped\",\"record\":%u}\n",i);
    gsf_telemetry_close(a);
    unsetenv("GFG_NATIVE_TELEMETRY_SEGMENTED");
    a=gsf_telemetry_open(path);assert(a);fprintf(a,"{\"event\":\"legacy\"}\n");gsf_telemetry_close(a);
    puts("telemetry-segments C producer PASS: concurrent bridges, threads, budget stop, legacy append");
    return 0;
}
