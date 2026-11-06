#!/usr/bin/env python3
"""
UPV timetable via personal iCal URL — the simple route.

Reads your private iCal subscription URL from  upv/poc/ical_url.txt  (gitignored),
fetches it, parses the VEVENTs, and prints your week. This is exactly what the
Nokia app will do (fetch one URL over HTTP/HTTPS, parse BEGIN:VEVENT). No OAuth,
no app client, no credentials beyond the URL's own token.

The .ics is also saved to upv/poc/timetable.ics so we can study the real format
for the C port.

Run:  toolchain/pyenv/Scripts/python.exe -I upv/poc/ical.py
"""
import ssl, sys, os, datetime, urllib.request, urllib.error

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))

def make_ctx():
    ctx = ssl.create_default_context()
    try:
        pem = ""
        for store in ("ROOT", "CA"):
            try:
                for der, enc, trust in ssl.enum_certificates(store):
                    if enc == "x509_asn":
                        pem += ssl.DER_cert_to_PEM_cert(der)
            except Exception:
                pass
        if pem:
            ctx.load_verify_locations(cadata=pem)
    except Exception:
        pass
    return ctx

def fetch(url):
    # try HTTPS first (what the Nokia/BearSSL prefers), then the given scheme
    tries = []
    if url.startswith("http://"):
        tries.append("https://" + url[len("http://"):])
    tries.append(url)
    last = None
    for u in tries:
        req = urllib.request.Request(u, headers={"User-Agent": "Mozilla/5.0"})
        try:
            with urllib.request.urlopen(req, context=make_ctx(), timeout=40) as r:
                return u, r.status, r.read().decode("utf-8", "replace")
        except Exception as e:
            last = f"{type(e).__name__}: {e}"
    return tries[-1], -1, last

def unfold(text):
    # iCal line folding: a line starting with space/tab continues the previous one
    out = []
    for line in text.replace("\r\n", "\n").replace("\r", "\n").split("\n"):
        if line[:1] in (" ", "\t") and out:
            out[-1] += line[1:]
        else:
            out.append(line)
    return out

def parse_dt(val):
    # val like 20261110T090000Z  or  20261110T090000  or  20261110
    v = val.strip()
    try:
        if v.endswith("Z"):
            return datetime.datetime.strptime(v, "%Y%m%dT%H%M%SZ")
        if "T" in v:
            return datetime.datetime.strptime(v, "%Y%m%dT%H%M%S")
        return datetime.datetime.strptime(v, "%Y%m%d")
    except Exception:
        return None

def parse_events(text):
    events, cur = [], None
    for line in unfold(text):
        if line == "BEGIN:VEVENT":
            cur = {}
        elif line == "END:VEVENT":
            if cur is not None:
                events.append(cur); cur = None
        elif cur is not None and ":" in line:
            key, val = line.split(":", 1)
            name = key.split(";", 1)[0].upper()
            if name in ("DTSTART", "DTEND"):
                cur[name] = parse_dt(val)
            elif name in ("SUMMARY", "LOCATION", "DESCRIPTION"):
                cur[name] = val.replace("\\,", ",").replace("\\n", " ").replace("\\;", ";").strip()
    return events

def main():
    url_file = os.path.join(HERE, "ical_url.txt")
    if not os.path.exists(url_file):
        sys.exit("ERROR: upv/poc/ical_url.txt not found.")
    url = open(url_file, encoding="utf-8").read().strip()
    if not url:
        sys.exit("ERROR: ical_url.txt is empty.")

    print("[*] Fetching your iCal feed (token hidden)…")
    used, st, body = fetch(url)
    scheme = used.split("://", 1)[0]
    print(f"    scheme={scheme}  HTTP {st}  {len(body) if isinstance(body,str) else 0} bytes")
    if st != 200 or "BEGIN:VCALENDAR" not in (body or ""):
        print("    !! not an iCal response. First 200 chars:")
        print("   ", (body or "")[:200])
        sys.exit(2)

    open(os.path.join(HERE, "timetable.ics"), "w", encoding="utf-8").write(body)
    print(f"    saved raw feed -> upv/poc/timetable.ics")

    events = parse_events(body)
    dated = [e for e in events if e.get("DTSTART")]
    dated.sort(key=lambda e: e["DTSTART"])
    print(f"    parsed {len(events)} events ({len(dated)} with a start time)")
    if dated:
        print(f"    date range: {dated[0]['DTSTART'].date()}  ->  {dated[-1]['DTSTART'].date()}")

    # show the current week (Mon..Sun around today)
    today = datetime.date.today()
    mon = today - datetime.timedelta(days=today.weekday())
    sun = mon + datetime.timedelta(days=6)
    week = [e for e in dated if mon <= e["DTSTART"].date() <= sun]
    days = ["Lunes","Martes","Miércoles","Jueves","Viernes","Sábado","Domingo"]
    print(f"\n============  THIS WEEK  ({mon} .. {sun})  ============")
    if not week:
        print("  (no events this week — showing the next 10 upcoming instead)")
        week = [e for e in dated if e["DTSTART"].date() >= today][:10]
    cur_day = None
    for e in week:
        d = e["DTSTART"].date()
        if d != cur_day:
            cur_day = d
            print(f"\n{days[d.weekday()]} {d.strftime('%d/%m')}")
        ini = e["DTSTART"].strftime("%H:%M")
        fin = e["DTEND"].strftime("%H:%M") if e.get("DTEND") else "     "
        loc = f"  [{e['LOCATION']}]" if e.get("LOCATION") else ""
        print(f"   {ini}-{fin}  {e.get('SUMMARY','(sin título)')}{loc}")
    print("\n==================================================")
    print("If this is your real timetable, say 'build it' and I'll make the Nokia .vxp.")

if __name__ == "__main__":
    main()
