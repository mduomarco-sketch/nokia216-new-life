/* upv_ical.h — streaming iCal parser for the UPV timetable feed. */
#ifndef UPV_ICAL_H
#define UPV_ICAL_H
#include <stddef.h>

typedef struct { int y, mo, d, h, mi; } ical_dt;   /* local (Madrid) time */

typedef struct {
    ical_dt start, end;
    char summary[64];     /* subject, e.g. "Matemáticas II" (UTF-8) */
    char location[48];    /* classroom, e.g. "Aula N25 (Edificio 7E)" */
} ical_event;

typedef void (*ical_emit_fn)(const ical_event *ev, void *user);

#define ICAL_LINE_MAX 512

typedef struct {
    int ty, tmo, td;          /* target local date to keep */
    int in;                   /* inside a VEVENT */
    ical_event cur;
    ical_emit_fn emit;
    void *user;
    /* streaming line assembly (RFC 5545 unfolding) */
    char line[ICAL_LINE_MAX];
    int  llen;
    int  pending_nl;          /* saw '\n', waiting to see if next byte is a fold */
} ical_parser;

int  ical_dow(int y, int m, int d);          /* 0=Sun..6=Sat */
void ical_init(ical_parser *p, int ty, int tmo, int td, ical_emit_fn emit, void *user);
void ical_feed_line(ical_parser *p, const char *line);        /* one unfolded line */
void ical_feed_bytes(ical_parser *p, const char *data, int n);/* raw stream chunk */
void ical_flush(ical_parser *p);                              /* emit trailing line */

#endif
