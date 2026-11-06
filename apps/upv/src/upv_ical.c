/* upv_ical.c — streaming iCal parser for the UPV timetable feed.
 *
 * The feed is ~341 KB / 2 years of events; the phone must NOT hold it all.
 * So this parses line-by-line as bytes arrive, keeps only the events whose
 * LOCAL start date matches a target day, and discards everything else.
 *
 * Pure C, no MRE calls — so it host-compiles for testing (tools/host_ical.c)
 * and links unchanged into the .vxp.
 */
#include "upv_ical.h"
#include <string.h>

/* ---- small helpers ---- */
static int is_leap(int y){ return (y%4==0 && y%100!=0) || y%400==0; }
static int mdays(int y,int m){
    static const int d[12]={31,28,31,30,31,30,31,31,30,31,30,31};
    return (m==2 && is_leap(y)) ? 29 : d[m-1];
}
/* day-of-week (0=Sun..6=Sat) via Sakamoto */
int ical_dow(int y,int m,int d){
    static int t[]={0,3,2,5,0,3,5,1,4,6,2,4};
    if(m<3) y-=1;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

/* ============================================================================
 * TODO(human): decide whether a given LOCAL calendar date is in summer time.
 *
 * The feed's times are UTC. UPV is Europe/Madrid, which is UTC+1 in winter
 * (CET) and UTC+2 in summer (CEST). EU DST runs from the LAST SUNDAY OF MARCH
 * to the LAST SUNDAY OF OCTOBER.
 *
 * Return 1 if (y,mo,d) falls in summer time (UTC+2), else 0 (UTC+1).
 *
 * Hints:
 *   - Apr..Sep are always summer; Jan,Feb,Nov,Dec always winter. Only March
 *     and October need the "last Sunday" check.
 *   - ical_dow(y,m,d) gives 0=Sun..6=Sat. The last Sunday of a month is the
 *     largest d <= mdays(y,m) with dow==0; or: lastSun = last_day -
 *     ical_dow(y, m, last_day). The DST switch happens at that date.
 *   - Edge at the exact changeover day is fine to approximate by date (we show
 *     class times, not 02:00–03:00 clock-shift corner cases).
 * ========================================================================== */
static int is_summer_time(int y,int mo,int d){
    /* EU DST: last Sunday of March .. last Sunday of October (CEST = UTC+2). */
    if(mo < 3 || mo > 10) return 0;          /* Nov..Feb: winter */
    if(mo > 3 && mo < 10) return 1;          /* Apr..Sep: summer */
    /* March or October: find the last Sunday (dow 0=Sun) */
    int last = mdays(y, mo);                  /* 31 for both March and October */
    int last_sunday = last - ical_dow(y, mo, last);
    if(mo == 3)  return d >= last_sunday;     /* on/after last Sun Mar -> summer */
    else         return d <  last_sunday;     /* before last Sun Oct  -> summer */
}

/* add `minutes` to a UTC datetime, rolling over fields → local time */
static void shift_minutes(ical_dt *t,int minutes){
    t->mi += minutes;
    while(t->mi>=60){ t->mi-=60; t->h++; }
    while(t->h>=24){ t->h-=24; t->d++;
        if(t->d>mdays(t->y,t->mo)){ t->d=1; t->mo++;
            if(t->mo>12){ t->mo=1; t->y++; } } }
}

/* parse "20260202T090000Z" (or without Z) into UTC fields, then to Madrid local */
static int parse_dt(const char *v, ical_dt *out){
    int y,mo,d,h=0,mi=0;
    if(strlen(v) < 8) return 0;
    y =(v[0]-'0')*1000+(v[1]-'0')*100+(v[2]-'0')*10+(v[3]-'0');
    mo=(v[4]-'0')*10+(v[5]-'0');
    d =(v[6]-'0')*10+(v[7]-'0');
    if(v[8]=='T'){
        h =(v[9]-'0')*10+(v[10]-'0');
        mi=(v[11]-'0')*10+(v[12]-'0');
    }
    out->y=y; out->mo=mo; out->d=d; out->h=h; out->mi=mi;
    /* feed uses Z (UTC). Convert to Europe/Madrid. */
    shift_minutes(out, is_summer_time(y,mo,d) ? 120 : 60);
    return 1;
}

static void copy_field(char *dst,int size,const char *src){
    int i=0;
    while(src[i] && i<size-1){ dst[i]=src[i]; i++; }
    dst[i]=0;
}

/* ---- streaming state machine ---- */
void ical_init(ical_parser *p, int ty,int tmo,int td,
               ical_emit_fn emit, void *user){
    memset(p,0,sizeof(*p));
    p->ty=ty; p->tmo=tmo; p->td=td; p->emit=emit; p->user=user;
}

/* feed ONE unfolded logical line (no trailing CR/LF) */
void ical_feed_line(ical_parser *p, const char *line){
    if(!strcmp(line,"BEGIN:VEVENT")){ memset(&p->cur,0,sizeof(p->cur)); p->in=1; return; }
    if(!strcmp(line,"END:VEVENT")){
        /* ty==0 is the "no filter" sentinel: emit every event (the app keeps the
         * whole feed and filters by day itself). Otherwise match the target day. */
        if(p->in && (p->ty==0 ||
                (p->cur.start.y==p->ty && p->cur.start.mo==p->tmo && p->cur.start.d==p->td))){
            p->emit(&p->cur, p->user);
        }
        p->in=0; return;
    }
    if(!p->in) return;
    const char *c = strchr(line, ':');
    if(!c) return;
    int nlen = (int)(c - line);
    const char *val = c+1;
    /* property name is up to ':' or ';' */
    int pn = nlen; const char *sc = memchr(line,';',nlen);
    if(sc) pn = (int)(sc-line);
    if(pn==7 && !strncmp(line,"DTSTART",7)) parse_dt(val,&p->cur.start);
    else if(pn==5 && !strncmp(line,"DTEND",5)) parse_dt(val,&p->cur.end);
    else if(pn==7 && !strncmp(line,"SUMMARY",7)) copy_field(p->cur.summary,sizeof p->cur.summary,val);
    else if(pn==8 && !strncmp(line,"LOCATION",8)) copy_field(p->cur.location,sizeof p->cur.location,val);
}

/* Feed a raw byte chunk straight from the network. Handles CRLF line endings
 * and RFC 5545 line folding (a line continued by a leading space/tab), across
 * chunk boundaries, so the phone never holds more than one logical line. */
void ical_feed_bytes(ical_parser *p, const char *data, int n){
    int i;
    for(i=0;i<n;i++){
        char c = data[i];
        if(c=='\r') continue;
        if(p->pending_nl){
            if(c==' ' || c=='\t'){ p->pending_nl=0; continue; } /* fold: same line */
            p->line[p->llen]=0;                 /* previous line is complete */
            ical_feed_line(p, p->line);
            p->llen=0; p->pending_nl=0;
            /* fall through: c starts the next line */
        }
        if(c=='\n'){ p->pending_nl=1; continue; }
        if(p->llen < ICAL_LINE_MAX-1) p->line[p->llen++]=c;
        /* else: oversized line (e.g. DESCRIPTION) — keep consuming, drop extra */
    }
}

void ical_flush(ical_parser *p){
    if(p->pending_nl || p->llen){
        p->line[p->llen]=0;
        ical_feed_line(p, p->line);
        p->llen=0; p->pending_nl=0;
    }
}
