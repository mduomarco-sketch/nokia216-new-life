/* host_ical.c — PC test harness for upv_ical.c.
 * Reads an .ics file, feeds it to the streaming parser for a target date,
 * prints the matching events. Verifies parsing before the phone build.
 *
 * Build (from src/upv-build):
 *   ../../toolchain/w64devkit/bin/gcc.exe -O1 -w -I src -o host_ical.exe \
 *       tools/host_ical.c src/upv_ical.c
 * Run:
 *   ./host_ical.exe upv/poc/timetable.ics 2026 11 4
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "upv_ical.h"

static const char *DOW[] = {"Dom","Lun","Mar","Mie","Jue","Vie","Sab"};

static void emit(const ical_event *e, void *user){
    int *n = (int*)user; (*n)++;
    printf("  %02d:%02d-%02d:%02d  %-38s %s\n",
           e->start.h, e->start.mi, e->end.h, e->end.mi,
           e->summary, e->location);
}

int main(int argc, char **argv){
    if(argc < 5){ fprintf(stderr,"usage: %s file.ics YYYY MM DD\n",argv[0]); return 1; }
    const char *path = argv[1];
    int ty=atoi(argv[2]), tmo=atoi(argv[3]), td=atoi(argv[4]);

    FILE *f = fopen(path,"rb");
    if(!f){ perror("open"); return 1; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    char *buf = malloc(sz+1); fread(buf,1,sz,f); buf[sz]=0; fclose(f);

    ical_parser p; int n=0;
    ical_init(&p, ty,tmo,td, emit, &n);

    /* feed in small, awkward chunks to prove fold/line handling survives packet
     * boundaries (this is what arrives over TCP on the phone) */
    long i; int CH = 37;
    for(i=0;i<sz;i+=CH){
        int n2 = (i+CH <= sz) ? CH : (int)(sz-i);
        ical_feed_bytes(&p, buf+i, n2);
    }
    ical_flush(&p);
    free(buf);
    printf("\n%s %04d-%02d-%02d: %d event(s)\n",
           DOW[ical_dow(ty,tmo,td)], ty,tmo,td, n);
    return 0;
}
