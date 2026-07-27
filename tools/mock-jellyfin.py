#!/usr/bin/env python3
"""
A tiny fake Jellyfin server for developing MiSTerFin off-hardware.

Serves just enough of the API for the client to browse, page, and
authenticate against, backed by a synthetic library that is deliberately
bigger than any fixed client-side buffer — 500 movies, so pagination has
something real to page through, which a small personal library wouldn't
exercise.

Stdlib only (http.server + zlib for the placeholder cover art). No
dependencies, and nothing here talks to a real server or real credentials.

Usage:
    python3 tools/mock-jellyfin.py [port]        # default 8096

Then point ./jellyfin.conf at it:
    http://127.0.0.1:8096
    mock-api-key
    mockuser
    PAL
"""

import json
import re
import struct
import sys
import zlib
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import urlparse, parse_qs

USER_ID = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
USER_NAME = "mockuser"

# Deliberately > 256 (the client's JF_MAX_ITEMS) so paging past the end of a
# single window is reachable, and not a round number so off-by-ones show up.
N_MOVIES = 503
N_SERIES = 30
SEASONS_PER_SERIES = 2
EPISODES_PER_SEASON = 10
N_ARTISTS = 20
ALBUMS_PER_ARTIST = 3
TRACKS_PER_ALBUM = 10

TICKS_PER_MIN = 60 * 10_000_000


def dotnet_escape(text):
    """Makes json.dumps output match what Jellyfin actually puts on the wire.

    Jellyfin configures no custom Encoder (see JsonDefaults.cs), so
    System.Text.Json uses JavaScriptEncoder.Default — which escapes every
    non-ASCII character AND the HTML-sensitive set below as \\uXXXX. Python's
    ensure_ascii=True covers the non-ASCII half; these are the rest.

    This matters for client testing far more than it looks: an apostrophe is
    escaped, so a perfectly ordinary title like "Don't Look Up" arrives as
    "Don\\u0027t Look Up". A mock that didn't do this would make a client's
    escape handling look correct when it isn't.
    """
    for ch in ("'", "<", ">", "&", "+"):
        text = text.replace(ch, "\\u%04x" % ord(ch))
    return text


def _png(r, g, b, w=8, h=8):
    """Smallest useful solid-colour PNG — same from-scratch encoder approach
    as tools/raw_to_png.py, for the same reason (no image library assumed)."""
    raw = b"".join(b"\x00" + bytes((r, g, b)) * w for _ in range(h))

    def chunk(tag, payload):
        return (struct.pack(">I", len(payload)) + tag + payload +
                struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) +
            chunk(b"IEND", b""))


# A few distinct colours so the cover grid and browse panel are visibly
# populated rather than uniformly grey.
COVERS = [_png(200, 60, 60), _png(60, 160, 200), _png(200, 170, 60),
          _png(120, 90, 200), _png(70, 190, 120)]

VIEWS = [
    {"Id": "view-movies", "Name": "Movies",   "CollectionType": "movies"},
    {"Id": "view-tv",     "Name": "TV Shows", "CollectionType": "tvshows"},
    {"Id": "view-music",  "Name": "Music",    "CollectionType": "music"},
]


def base_item(item_id, name, item_type, **extra):
    item = {
        "Id": item_id,
        "Name": name,
        "Type": item_type,
        "ServerId": "mockserver",
        "ImageTags": {"Primary": "tag-" + item_id},
        "BackdropImageTags": ["backdrop-" + item_id],
        "UserData": {"PlaybackPositionTicks": 0, "Played": False, "PlayCount": 0},
    }
    item.update(extra)
    return item


def build_library():
    """One flat dict of every item, plus parent->children ordering."""
    items, children = {}, {}

    # Titles the real server will escape as \uXXXX: System.Text.Json's default
    # encoder escapes apostrophes and every non-ASCII character, so these are
    # what an ordinary library actually looks like over the wire.
    tricky = [
        "Don't Look Up", "Amélie", "A Fistful of Dollars — Extended",
        "Spinal Tap: This Is It", "Zoë & Co.", "Æon Flux",
    ]

    movies = []
    for idx, title in enumerate(tricky):
        mid = f"movie-tricky-{idx}"
        items[mid] = base_item(mid, title, "Movie",
                               ProductionYear=2000 + idx,
                               RunTimeTicks=100 * TICKS_PER_MIN)
        movies.append(mid)

    for i in range(N_MOVIES):
        mid = f"movie-{i:04d}"
        # A handful get watched/resume state so those badges are exercised.
        user_data = {"PlaybackPositionTicks": 0, "Played": False, "PlayCount": 0}
        if i % 17 == 0:
            user_data = {"PlaybackPositionTicks": 0, "Played": True, "PlayCount": 1}
        elif i % 11 == 0:
            user_data = {"PlaybackPositionTicks": 23 * TICKS_PER_MIN,
                         "Played": False, "PlayCount": 0}
        items[mid] = base_item(
            mid, f"Movie {i:04d} The Motion Picture", "Movie",
            ProductionYear=1980 + (i % 45),
            RunTimeTicks=(90 + i % 50) * TICKS_PER_MIN,
            Overview=f"Synthetic movie #{i} used for client testing. " * 4,
            CommunityRating=round(4.0 + (i % 60) / 10.0, 1),
            UserData=user_data)
        movies.append(mid)
    stress_id = "movie-stress"
    items[stress_id] = base_item(stress_id, "Stress Test Many Tracks", "Movie",
                                 ProductionYear=2024, RunTimeTicks=120 * TICKS_PER_MIN)
    movies.append(stress_id)

    children["view-movies"] = movies

    series_ids = []
    for s in range(N_SERIES):
        sid = f"series-{s:03d}"
        n_eps = SEASONS_PER_SERIES * EPISODES_PER_SEASON
        items[sid] = base_item(
            sid, f"Series {s:03d}", "Series",
            ProductionYear=1995 + (s % 30),
            ChildCount=SEASONS_PER_SERIES,      # season count
            RecursiveItemCount=n_eps,           # episode count, batched server-side
            Overview=f"Synthetic series #{s}. " * 6)
        series_ids.append(sid)

        season_ids = []
        for se in range(1, SEASONS_PER_SERIES + 1):
            season_id = f"{sid}-s{se}"
            items[season_id] = base_item(season_id, f"Season {se}", "Season",
                                         IndexNumber=se, ChildCount=EPISODES_PER_SEASON)
            season_ids.append(season_id)

            ep_ids = []
            for e in range(1, EPISODES_PER_SEASON + 1):
                eid = f"{season_id}e{e:02d}"
                items[eid] = base_item(
                    eid, f"Episode {e:02d}", "Episode",
                    IndexNumber=e, ParentIndexNumber=se,
                    RunTimeTicks=42 * TICKS_PER_MIN,
                    SeriesId=sid, SeriesName=f"Series {s:03d}",
                    ParentBackdropItemId=sid,
                    ParentBackdropImageTags=["backdrop-" + sid],
                    ParentLogoItemId=sid, ParentLogoImageTag="logo-" + sid,
                    Overview=f"Synthetic episode S{se}E{e}. " * 4)
                ep_ids.append(eid)
            children[season_id] = ep_ids
        children[sid] = season_ids
    children["view-tv"] = series_ids

    artist_ids = []
    for a in range(N_ARTISTS):
        aid = f"artist-{a:03d}"
        items[aid] = base_item(aid, f"Artist {a:03d}", "MusicArtist",
                               ChildCount=ALBUMS_PER_ARTIST)
        artist_ids.append(aid)

        album_ids = []
        for al in range(ALBUMS_PER_ARTIST):
            alid = f"{aid}-album{al}"
            items[alid] = base_item(alid, f"Album {al} by Artist {a:03d}", "MusicAlbum",
                                    ProductionYear=1990 + ((a + al) % 35),
                                    ChildCount=TRACKS_PER_ALBUM,
                                    RecursiveItemCount=TRACKS_PER_ALBUM)
            album_ids.append(alid)

            track_ids = []
            for t in range(1, TRACKS_PER_ALBUM + 1):
                tid = f"{alid}-t{t:02d}"
                items[tid] = base_item(
                    tid, f"Track {t:02d}", "Audio",
                    IndexNumber=t, RunTimeTicks=(3 + t % 4) * TICKS_PER_MIN,
                    Album=f"Album {al} by Artist {a:03d}",
                    AlbumArtist=f"Artist {a:03d}",
                    AlbumId=alid, AlbumPrimaryImageTag="tag-" + alid)
                track_ids.append(tid)
            children[alid] = track_ids
        children[aid] = album_ids
    children["view-music"] = artist_ids

    return items, children


ITEMS, CHILDREN = build_library()


def descendants(parent_id, kinds=None):
    """Every item under parent_id, optionally filtered to BaseItemKinds."""
    out, stack = [], list(CHILDREN.get(parent_id, []))
    while stack:
        item_id = stack.pop(0)
        item = ITEMS.get(item_id)
        if not item:
            continue
        if kinds is None or item["Type"] in kinds:
            out.append(item_id)
        stack = list(CHILDREN.get(item_id, [])) + stack
    return out


def media_streams(item_id=""):
    """Multiple audio tracks and a mix of text/image subtitles, so the track
    pickers have something non-trivial to show.

    An item id containing "stress" instead returns the maximum the client
    stores (JF_MAX_SUBS / JF_MAX_AUDIO = 8 each), which is what the track
    picker has to stay on screen for — the menu box is sized from its content,
    so the full list on a 240-line NTSC frame is the case that overflows."""
    if "stress" in item_id:
        streams = [{"Index": 0, "Type": "Video", "Codec": "h264", "Width": 1920,
                    "Height": 1080, "BitRate": 8_000_000, "AspectRatio": "16:9"}]
        langs = ["eng", "jpn", "fra", "deu", "spa", "ita", "por", "rus"]
        for n, lang in enumerate(langs):
            streams.append({"Index": 1 + n, "Type": "Audio", "Codec": "ac3",
                            "Language": lang, "Channels": 6, "IsDefault": n == 0,
                            "DisplayTitle": f"{lang.upper()} - Dolby Digital 5.1 - Default"})
        for n, lang in enumerate(langs):
            streams.append({"Index": 9 + n, "Type": "Subtitle", "Codec": "subrip",
                            "Language": lang,
                            "DisplayTitle": f"{lang.upper()} - SUBRIP - External"})
        return streams

    return [
        {"Index": 0, "Type": "Video", "Codec": "h264", "Width": 1920,
         "Height": 1080, "BitRate": 8_000_000, "AspectRatio": "16:9",
         "DisplayTitle": "1080p H264"},
        {"Index": 1, "Type": "Audio", "Codec": "ac3", "Language": "eng",
         "Channels": 6, "IsDefault": True,
         "DisplayTitle": "English - Dolby Digital 5.1 - Default"},
        {"Index": 2, "Type": "Audio", "Codec": "aac", "Language": "jpn",
         "Channels": 2, "IsDefault": False, "DisplayTitle": "Japanese - AAC - Stereo"},
        {"Index": 3, "Type": "Audio", "Codec": "aac", "Language": "eng",
         "Channels": 2, "IsDefault": False,
         "DisplayTitle": "English - AAC - Stereo - Commentary"},
        # Two English subtitle tracks — dialogue and signs/songs — which is
        # the case a bare language code can't distinguish. DisplayTitle is
        # composed by the server exactly as below: the track's own Title
        # first, then Forced/Default/codec/External as they apply.
        {"Index": 4, "Type": "Subtitle", "Codec": "subrip", "Language": "eng",
         "IsDefault": True, "IsForced": False,
         "DisplayTitle": "English - Default - SUBRIP"},
        {"Index": 5, "Type": "Subtitle", "Codec": "ass", "Language": "eng",
         "Title": "Signs & Songs", "IsDefault": False, "IsForced": True,
         "DisplayTitle": "Signs & Songs - English - Forced - ASS"},
        {"Index": 6, "Type": "Subtitle", "Codec": "subrip", "Language": "eng",
         "IsHearingImpaired": True, "IsDefault": False, "IsForced": False,
         "DisplayTitle": "English - Hearing Impaired - SUBRIP"},
        {"Index": 7, "Type": "Subtitle", "Codec": "pgssub", "Language": "jpn",
         "DisplayTitle": "Japanese - PGSSUB"},
    ]


def people():
    return [{"Id": f"person-{i}", "Name": f"Actor {i}", "Role": f"Character {i}",
             "Type": "Actor", "PrimaryImageTag": f"tag-person-{i}"} for i in range(6)]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    # ── plumbing ────────────────────────────────────────────────────────────
    def log_message(self, fmt, *args):
        if "-v" in sys.argv:
            sys.stderr.write("%s %s\n" % (self.command, self.path))

    def _token(self):
        """Extracts Token="..." from the MediaBrowser Authorization header."""
        header = self.headers.get("Authorization", "")
        m = re.search(r'Token="([^"]*)"', header)
        return m.group(1) if m else ""

    def _authorized(self):
        """Real Jellyfin rejects an unknown or revoked token with 401, and the
        client depends on that: it's how a saved token that the server has
        since forgotten gets detected and replaced by a fresh Quick Connect,
        rather than the app appearing to work while every request fails.
        A mock that accepted anything would make that path untestable."""
        return self._token() in VALID_TOKENS

    def _send_401(self):
        return self._send({"Error": "invalid token"}, status=401)

    def _send(self, payload, content_type="application/json", status=200):
        if isinstance(payload, (dict, list)):
            # Compact separators on purpose: real Jellyfin sets
            # WriteIndented = false, and a client that scans the raw bytes
            # would behave differently against pretty-printed output.
            text = json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
            payload = dotnet_escape(text).encode()
        elif isinstance(payload, str):
            payload = payload.encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _query_result(self, ids, query):
        """Applies StartIndex/Limit the way the real server does, and always
        reports the untruncated TotalRecordCount — that total is the whole
        point for a paging client."""
        total = len(ids)
        start = int(query.get("StartIndex", query.get("startIndex", [0]))[0])
        limit = query.get("Limit", query.get("limit", [None]))[0]
        window = ids[start:]
        if limit is not None:
            window = window[:int(limit)]
        return {"Items": [ITEMS[i] for i in window],
                "TotalRecordCount": total,
                "StartIndex": start}

    # ── routes ──────────────────────────────────────────────────────────────
    def do_GET(self):
        url = urlparse(self.path)
        path, query = url.path, parse_qs(url.query)

        # Everything needs a credential except the Quick Connect handshake
        # (which is how you get one) and image requests — real Jellyfin serves
        # artwork without auth, and the client relies on that: it fetches
        # covers with a bare curl that sends no Authorization header at all.
        exempt = path.startswith("/QuickConnect/") or "/Images/" in path
        if not exempt and not self._authorized():
            return self._send_401()

        if path == "/Users":
            return self._send([{"Id": USER_ID, "Name": USER_NAME}])

        if path == "/UserViews":
            views = [base_item(v["Id"], v["Name"], "CollectionFolder",
                               CollectionType=v["CollectionType"]) for v in VIEWS]
            return self._send({"Items": views, "TotalRecordCount": len(views)})

        if path == "/QuickConnect/Enabled":
            return self._send("true")

        if path == "/QuickConnect/Connect":
            secret = query.get("secret", [""])[0]
            state = QUICK_CONNECT.get(secret)
            if state is None:
                return self._send({"Error": "Unknown secret"}, status=404)
            # Auto-approves after a few polls so an unattended run completes;
            # a real server waits for the user to type the code.
            state["Polls"] += 1
            if state["Polls"] >= 3:
                state["Authenticated"] = True
            return self._send(state)

        m = re.match(r"^/Items/([^/]+)/Images/([^/?]+)", path)
        if m:
            return self._send(COVERS[hash(m.group(1)) % len(COVERS)], "image/png")

        m = re.match(r"^/Shows/([^/]+)/Seasons$", path)
        if m:
            return self._send(self._query_result(CHILDREN.get(m.group(1), []), query))

        m = re.match(r"^/Shows/([^/]+)/Episodes$", path)
        if m:
            season = query.get("seasonId", [None])[0]
            ids = CHILDREN.get(season, []) if season else descendants(m.group(1), {"Episode"})
            return self._send(self._query_result(ids, query))

        if path == "/Shows/NextUp":
            # First unwatched episode of each series, mimicking the real thing
            # closely enough for the client's purposes.
            ids = [descendants(s, {"Episode"})[0] for s in CHILDREN["view-tv"][:12]]
            return self._send(self._query_result(ids, query))

        if path == "/UserItems/Resume":
            ids = [i for i in CHILDREN["view-movies"]
                   if ITEMS[i]["UserData"]["PlaybackPositionTicks"] > 0][:12]
            return self._send(self._query_result(ids, query))

        m = re.match(r"^/Items/([^/?]+)$", path)
        if m and m.group(1) in ITEMS:
            item = dict(ITEMS[m.group(1)])
            item["MediaStreams"] = media_streams(m.group(1))
            item["People"] = people()
            item["ImageTags"] = dict(item.get("ImageTags", {}))
            item["ImageTags"]["Logo"] = "logo-" + m.group(1)
            return self._send(item)

        if path == "/Items":
            parent = query.get("ParentId", query.get("parentId", [None]))[0]
            recursive = query.get("Recursive", ["false"])[0].lower() == "true"
            kinds = query.get("IncludeItemTypes", [None])[0]
            kinds = set(kinds.split(",")) if kinds else None

            if parent is None:
                ids = []
            elif recursive:
                ids = descendants(parent, kinds)
            else:
                ids = [i for i in CHILDREN.get(parent, [])
                       if kinds is None or ITEMS[i]["Type"] in kinds]

            if query.get("SortBy", [""])[0] == "Random":
                import random
                ids = list(ids)
                random.shuffle(ids)
            return self._send(self._query_result(ids, query))

        return self._send({"Error": "not found: " + path}, status=404)

    def do_POST(self):
        url = urlparse(self.path)
        path = url.path

        if path == "/QuickConnect/Initiate":
            secret = "mock-secret-%d" % len(QUICK_CONNECT)
            QUICK_CONNECT[secret] = {"Authenticated": False, "Secret": secret,
                                     "Code": "123456", "Polls": 0}
            return self._send(QUICK_CONNECT[secret])

        if path == "/Users/AuthenticateWithQuickConnect":
            length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(length) or b"{}")
            state = QUICK_CONNECT.get(body.get("Secret", ""))
            if not state or not state["Authenticated"]:
                return self._send({"Error": "not authorized"}, status=401)
            VALID_TOKENS.add("mock-access-token")
            return self._send({"AccessToken": "mock-access-token",
                               "ServerId": "mockserver",
                               "User": {"Id": USER_ID, "Name": USER_NAME}})

        # Playback reporting / user data writes — accepted and ignored.
        length = int(self.headers.get("Content-Length", 0))
        if length:
            self.rfile.read(length)
        return self._send({}, status=204)


QUICK_CONNECT = {}

# Credentials the mock accepts: the API key a jellyfin.conf might carry, plus
# any access token handed out by Quick Connect.
VALID_TOKENS = {"mock-api-key"}


if __name__ == "__main__":
    port = 8096
    for arg in sys.argv[1:]:
        if arg.isdigit():
            port = int(arg)
    print(f"mock jellyfin on http://127.0.0.1:{port}  "
          f"({N_MOVIES} movies, {N_SERIES} series, {N_ARTISTS} artists)")
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
