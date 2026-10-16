#!/usr/bin/env python3
"""python-tidal helper for TCNG (--python-tidal-path).

TCNG embeds this file and runs it once per track with the interpreter from
--python-bin, the python-tidal checkout from --python-tidal-path on PYTHONPATH and the script
on stdin:

    python - resolve <track_id> --session <session.json>

It prints exactly one JSON object on stdout:

    {"ok": true, "trackId": "...", "audioQuality": "HI_RES_LOSSLESS", "audioMode": "STEREO",
     "manifestMimeType": "application/dash+xml", "codecs": "flac", "bitDepth": 24,
     "sampleRate": 96000, "duration": 312.09,
     "url": "..."                                            # single file (BTS manifest)
     "initUrl": "...", "segments": [{"url": "...", "duration": 4.096}, ...]}   # DASH (FLAC)

or, on failure, {"ok": false, "error": "...", "httpStatus": 404, "body": "<TIDAL error JSON>"}
with a non-zero exit code.

The session file is created once with `tidal-connect-ng --python-tidal-login ...`, which runs
this script's `login` command (it prints a TIDAL login link and reads the redirect URL back).
"""

import argparse
import base64
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

HI_RES = "HI_RES_LOSSLESS"
MPD = "application/dash+xml"
BTS = "application/vnd.tidal.bts"


def emit(obj, code=0):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()
    sys.exit(code)


def fail(message, http_status=0, body=""):
    emit({"ok": False, "error": message, "httpStatus": http_status, "body": body}, 1)


def import_tidalapi():
    try:
        import tidalapi
    except Exception as e:  # missing checkout or missing dependencies (requests, ...)
        fail("cannot import tidalapi: %s" % e)
    return tidalapi


def open_session(tidalapi, path):
    session = tidalapi.Session()
    if not path.is_file():
        fail("session file not found: %s (create it with the 'login' command)" % path)
    if not session.load_session_from_file(path):
        fail("could not load the TIDAL session from %s (log in again with the 'login' command)" % path)
    return session


def enum_value(v):
    return getattr(v, "value", v)


# --- DASH manifest -------------------------------------------------------------------

def local(tag):
    return tag.rsplit("}", 1)[-1]


def children(node, name):
    return [c for c in node if local(c.tag) == name]


def iso_duration(text):
    """'PT5M12.093S' -> 312.093 (days/hours/minutes/seconds only)."""
    m = re.fullmatch(r"P(?:(\d+(?:\.\d+)?)D)?(?:T(?:(\d+(?:\.\d+)?)H)?(?:(\d+(?:\.\d+)?)M)?(?:(\d+(?:\.\d+)?)S)?)?",
                     text or "")
    if not m:
        return 0.0
    d, h, mi, s = (float(x) if x else 0.0 for x in m.groups())
    return ((d * 24 + h) * 60 + mi) * 60 + s


def expand(template, rep, number):
    def sub(m):
        name, fmt = m.group(1), m.group(2)
        if name == "":
            return "$"
        if name == "RepresentationID":
            return rep.get("id", "")
        value = number if name == "Number" else int(rep.get("bandwidth", "0"))
        return ("%" + fmt[1:]) % value if fmt else str(value)

    return re.sub(r"\$(Number|RepresentationID|Bandwidth|)(%0\d+d)?\$", sub, template)


def parse_mpd(xml_text):
    root = ET.fromstring(xml_text)
    total = iso_duration(root.get("mediaPresentationDuration"))
    best = None
    for period in children(root, "Period"):
        for aset in children(period, "AdaptationSet"):
            for rep in children(aset, "Representation"):
                tmpl = (children(rep, "SegmentTemplate") or children(aset, "SegmentTemplate") or [None])[0]
                if tmpl is None:
                    continue
                codecs = rep.get("codecs") or aset.get("codecs") or ""
                bw = int(rep.get("bandwidth", "0"))
                if best is None or bw > best[0]:
                    best = (bw, rep, tmpl, codecs)
    if best is None:
        raise ValueError("no Representation with a SegmentTemplate")
    _, rep, tmpl, codecs = best
    timescale = int(tmpl.get("timescale", "1"))
    number = int(tmpl.get("startNumber", "1"))
    media = tmpl.get("media")
    if not media:
        raise ValueError("SegmentTemplate without media")
    durations = []
    timeline = (children(tmpl, "SegmentTimeline") or [None])[0]
    if timeline is not None:
        for s in children(timeline, "S"):
            d = int(s.get("d"))
            r = int(s.get("r", "0"))
            if r < 0:  # repeat until the end of the presentation
                done = sum(durations) * timescale
                r = max(0, int(round((total * timescale - done) / d)) - 1)
            durations.extend([d] * (r + 1))
        durations = [d / timescale for d in durations]
    elif tmpl.get("duration") and total > 0:
        seg = int(tmpl.get("duration")) / timescale
        count = int(-(-total // seg))
        durations = [seg] * count
        durations[-1] = total - seg * (count - 1)
    else:
        raise ValueError("SegmentTemplate without timeline or duration")
    segments = [{"url": expand(media, rep, number + i), "duration": d} for i, d in enumerate(durations)]
    init = tmpl.get("initialization")
    return {
        "codecs": codecs,
        "sampleRate": int(rep.get("audioSamplingRate", "0") or 0),
        "duration": total or sum(durations),
        "initUrl": expand(init, rep, number) if init else "",
        "segments": segments,
    }


# --- commands -------------------------------------------------------------------------

def resolve(args):
    tidalapi = import_tidalapi()
    path = Path(args.session)
    session = open_session(tidalapi, path)
    token = session.access_token
    session.audio_quality = HI_RES
    try:
        track = session.track(args.track_id)
        stream = track.get_stream()
    except Exception as e:
        response = getattr(e, "response", None)
        status = getattr(response, "status_code", 0) or 0
        body = getattr(response, "text", "") or ""
        if not status and type(e).__name__ in ("ObjectNotFound", "StreamNotAvailable"):
            status, body = 404, json.dumps({"status": 404, "subStatus": 4005, "userMessage": str(e)})
        fail("%s: %s" % (type(e).__name__, e), status, body)
    finally:
        if session.access_token != token:  # tidalapi refreshed the token: keep it
            try:
                session.save_session_to_file(path)
            except Exception as e:
                sys.stderr.write("could not save the refreshed session: %s\n" % e)

    mime = enum_value(stream.manifest_mime_type)
    raw = base64.b64decode(stream.manifest)
    out = {
        "ok": True,
        "trackId": str(args.track_id),
        "audioQuality": enum_value(stream.audio_quality),
        "audioMode": enum_value(stream.audio_mode),
        "manifestMimeType": mime,
        "bitDepth": int(stream.bit_depth or 0),
        "sampleRate": int(stream.sample_rate or 0),
        "duration": float(getattr(track, "duration", 0) or 0),
    }
    try:
        if mime == MPD:
            dash = parse_mpd(raw.decode("utf-8"))
            if dash["codecs"].lower() != "flac":  # TCNG unwraps FLAC from the MP4 segments
                fail("unsupported DASH codec %s" % dash["codecs"])
            out["codecs"] = dash["codecs"]
            out["initUrl"] = dash["initUrl"]
            out["segments"] = dash["segments"]
            out["sampleRate"] = out["sampleRate"] or dash["sampleRate"]
            out["duration"] = dash["duration"] or out["duration"]
        elif mime == BTS:
            bts = json.loads(raw)
            if bts.get("encryptionType", "NONE") != "NONE":
                fail("encrypted stream (%s) is not supported" % bts.get("encryptionType"))
            out["codecs"] = bts.get("codecs", "")
            out["url"] = (bts.get("urls") or [""])[0]
        else:
            fail("unsupported manifest type %s" % mime)
    except (ValueError, ET.ParseError) as e:
        fail("invalid %s manifest: %s" % (mime, e))
    emit(out)


def login(args):
    tidalapi = import_tidalapi()
    path = Path(args.session)
    path.parent.mkdir(parents=True, exist_ok=True)
    session = tidalapi.Session()
    # PKCE: the login flow that is allowed HI_RES_LOSSLESS streams.
    if not session.login_session_file(path, do_pkce=True, fn_print=lambda s: print(s, file=sys.stderr)):
        print("login failed", file=sys.stderr)
        sys.exit(1)
    print("session saved to %s" % path, file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description="python-tidal helper for TCNG")
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("resolve", help="print the stream of a track as JSON")
    p.add_argument("track_id")
    p.add_argument("--session", required=True)
    p.set_defaults(fn=resolve)
    p = sub.add_parser("login", help="log in (PKCE) and save the session file")
    p.add_argument("--session", required=True)
    p.set_defaults(fn=login)
    args = parser.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
