"""Laptop receiver + caregiver dashboard for the wearable.

    python tools/receiver.py

Board connects on TCP 2028, dashboard at http://localhost:8080.
Board sends:  EC1 boot=1A2B3C4D inc=3 seq=7 type=FALL event=ASSIST t=123456
We reply:     OK seq=7   (automatically, message received)
              ACK inc=3  (when someone clicks Acknowledge)
ACK doesn't close the incident, only the wearable can do that.

Console commands: ack <incident>, list, quit
"""
import argparse
import json
import socket
import sys
import threading
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

print_lock = threading.Lock()
state_lock = threading.Lock()

incidents = {}          # "boot:inc" -> dict
seen_messages = set()   # (boot, inc, seq), for spotting retries
connections = set()     # live sockets, so an ack can reach the board
board = {"connected": False, "peer": "", "last_seen": ""}
next_link = [1]

EVENT_WORDS = {
    "RESPONSE": "fall detected, waiting for the wearer",
    "ASSIST":   "assistance requested",
    "ESCALATE": "still down, escalated",
    "CANCEL":   "cancelled by the wearer",
    "RESOLVE":  "resolved, wearer recovered",
}

# incidents the wearable has closed (cancelled / resolved) - ack never adds to this
CLOSED_EVENTS = ("CANCEL", "RESOLVE")


def now():
    return datetime.now().strftime("%H:%M:%S")


def emit(text):
    with print_lock:
        print(text, flush=True)


# --------------------------------------------------------------------------
# What we know
# --------------------------------------------------------------------------

def parse(line):
    """Turn 'EC1 k=v k=v' into a dict, or None if the line is not ours."""
    if not line.startswith("EC1 "):
        return None
    fields = {}
    for piece in line.split()[1:]:
        if "=" in piece:
            key, value = piece.split("=", 1)
            fields[key] = value
    if not all(k in fields for k in ("boot", "inc", "seq", "type", "event")):
        return None
    return fields


def record(fields):
    """Update what we know. Returns (key, is_retry)."""
    key = "%s:%s" % (fields["boot"], fields["inc"])
    stamp = (fields["boot"], fields["inc"], fields["seq"])

    with state_lock:
        is_retry = stamp in seen_messages
        seen_messages.add(stamp)

        entry = incidents.get(key)
        if entry is None:
            entry = {
                "key": key,
                "boot": fields["boot"],
                "inc": fields["inc"],
                "type": fields["type"],
                "event": fields["event"],
                "first": now(),
                "last": now(),
                "acked": False,
                "acked_at": "",
                "history": [],
            }
            incidents[key] = entry

        if not is_retry:
            # keep the newest event so a cancel isn't hidden by an older alert
            entry["event"] = fields["event"]
            entry["last"] = now()
            entry["history"].append({"at": now(), "event": fields["event"]})

    return key, is_retry


def acknowledge(key_or_inc):
    """Mark an incident acknowledged and tell the board. Returns a message."""
    with state_lock:
        matches = [k for k in incidents
                   if k == key_or_inc or k.split(":")[1] == str(key_or_inc)]
        if not matches:
            return "no incident %s" % key_or_inc

        numbers = set()
        for key in matches:
            incidents[key]["acked"] = True
            incidents[key]["acked_at"] = now()
            numbers.add(incidents[key]["inc"])
        targets = list(connections)

    for number in numbers:
        for conn in targets:
            try:
                conn.sendall(("ACK inc=%s\n" % number).encode())
            except OSError:
                pass

    if not targets:
        return ("acknowledged %s; the board is away and will be told when it"
                " reconnects" % key_or_inc)
    return "acknowledged %s (this does not close the incident)" % key_or_inc


def snapshot():
    with state_lock:
        ordered = sorted(incidents.values(),
                         key=lambda e: (e["first"], int(e["inc"])), reverse=True)
        return {
            "board": dict(board),
            "incidents": [
                {
                    "key": e["key"],
                    "inc": e["inc"],
                    "type": e["type"],
                    "event": e["event"],
                    "words": EVENT_WORDS.get(e["event"], e["event"]),
                    "first": e["first"],
                    "last": e["last"],
                    "acked": e["acked"],
                    "acked_at": e["acked_at"],
                    "open": e["event"] not in CLOSED_EVENTS,
                    "history": list(e["history"]),
                }
                for e in ordered
            ],
        }


# --------------------------------------------------------------------------
# The board's connection
# --------------------------------------------------------------------------

def handle(conn, peer, reply):
    link = next_link[0]
    next_link[0] += 1
    emit("[%s] (link %d) board connected from %s:%d" % (now(), link, peer[0], peer[1]))

    with state_lock:
        connections.add(conn)
        board["connected"] = True
        board["peer"] = "%s:%d" % peer
        board["last_seen"] = now()

    buffered = b""
    try:
        while True:
            chunk = conn.recv(1024)
            if not chunk:
                break
            buffered += chunk
            while b"\n" in buffered:
                raw, buffered = buffered.split(b"\n", 1)
                text = raw.decode("utf-8", "replace").strip()
                if not text:
                    continue

                if text.startswith("EC1K "):
                    # keepalive, just update last-seen
                    with state_lock:
                        board["last_seen"] = now()
                    if reply:
                        conn.sendall(b"OK seq=0\n")
                    continue

                fields = parse(text)
                if fields is None:
                    emit("[%s] not ours: %s" % (now(), text))
                    continue

                key, is_retry = record(fields)
                with state_lock:
                    board["last_seen"] = now()
                    already_acked = incidents[key]["acked"]

                emit("[%s] incident %s (%s) %s%s" % (
                    now(), fields["inc"], fields["type"],
                    EVENT_WORDS.get(fields["event"], fields["event"]),
                    "   [retry of seq %s]" % fields["seq"] if is_retry else ""))

                if reply:
                    conn.sendall(("OK seq=%s\n" % fields["seq"]).encode())
                    if already_acked:
                        # resend the ack in case the board missed it while offline
                        conn.sendall(("ACK inc=%s\n" % fields["inc"]).encode())
    except (ConnectionResetError, ConnectionAbortedError, OSError):
        pass
    finally:
        with state_lock:
            connections.discard(conn)
            board["connected"] = bool(connections)
        conn.close()
        emit("[%s] (link %d) board disconnected" % (now(), link))


# --------------------------------------------------------------------------
# The dashboard
# --------------------------------------------------------------------------

PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ElderCare Caregiver Dashboard</title>
<style>
  :root {
    --bg: #f4f5f7; --card: #ffffff; --ink: #14161a; --muted: #5b6270;
    --line: #dfe3e8; --alarm: #c0392b; --warn: #b9770e; --ok: #1e8449;
    --accent: #1f4e8c;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #14161a; --card: #1d2026; --ink: #eef0f4; --muted: #99a1b0;
      --line: #2c313a; --alarm: #ef6a5c; --warn: #e0a458; --ok: #5fd08a;
      --accent: #7aa7e8;
    }
  }
  * { box-sizing: border-box; }
  body { margin: 0; padding: 24px 16px 48px; background: var(--bg); color: var(--ink);
         font: 16px/1.5 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
  .wrap { max-width: 860px; margin: 0 auto; }
  h1 { font-size: 22px; margin: 0 0 4px; }
  .sub { color: var(--muted); font-size: 14px; margin-bottom: 20px; }
  .status { display: flex; align-items: center; gap: 8px; padding: 10px 14px;
            background: var(--card); border: 1px solid var(--line);
            border-radius: 10px; margin-bottom: 20px; font-size: 14px; }
  .dot { width: 10px; height: 10px; border-radius: 50%; background: var(--muted); }
  .dot.on { background: var(--ok); }
  .dot.off { background: var(--alarm); }
  .card { background: var(--card); border: 1px solid var(--line);
          border-left: 5px solid var(--muted); border-radius: 10px;
          padding: 16px 18px; margin-bottom: 14px; }
  .card.open { border-left-color: var(--alarm); }
  .card.closed { border-left-color: var(--ok); }
  .row { display: flex; justify-content: space-between; align-items: flex-start;
         gap: 16px; flex-wrap: wrap; }
  .id { font-weight: 650; font-size: 18px; }
  .badge { display: inline-block; font-size: 12px; font-weight: 600;
           padding: 2px 8px; border-radius: 999px; border: 1px solid var(--line);
           color: var(--muted); margin-left: 8px; vertical-align: 2px; }
  .badge.fall { color: var(--alarm); border-color: var(--alarm); }
  .badge.sos { color: var(--warn); border-color: var(--warn); }
  .what { margin: 6px 0 10px; }
  .times { color: var(--muted); font-size: 13px; }
  .hist { margin: 10px 0 0; padding: 0; list-style: none;
          border-top: 1px solid var(--line); padding-top: 8px; }
  .hist li { font-size: 13px; color: var(--muted); }
  .hist time { font-variant-numeric: tabular-nums; margin-right: 8px; }
  button { font: inherit; font-weight: 600; padding: 9px 16px; border-radius: 8px;
           border: 1px solid var(--accent); background: var(--accent);
           color: #fff; cursor: pointer; }
  button:disabled { background: transparent; color: var(--muted);
                    border-color: var(--line); cursor: default; }
  .note { color: var(--muted); font-size: 13px; margin-top: 6px; }
  .empty { color: var(--muted); text-align: center; padding: 40px 0; }
</style>
</head>
<body>
<div class="wrap">
  <h1>ElderCare caregiver dashboard</h1>
  <div class="sub">Group 33 &middot; incidents reported by the wearable</div>

  <div class="status">
    <span class="dot" id="dot"></span>
    <span id="link">connecting&hellip;</span>
  </div>

  <div id="list"></div>
</div>

<script>
function draw(state) {
  const dot = document.getElementById('dot');
  const link = document.getElementById('link');
  dot.className = 'dot ' + (state.board.connected ? 'on' : 'off');
  link.textContent = state.board.connected
    ? 'Wearable connected from ' + state.board.peer + ' \\u00b7 last heard ' + state.board.last_seen
    : (state.board.last_seen
        ? 'Wearable not connected \\u00b7 last heard ' + state.board.last_seen
        : 'Waiting for the wearable to connect');

  const list = document.getElementById('list');
  if (!state.incidents.length) {
    list.innerHTML = '<div class="empty">No incidents. The wearable is monitoring.</div>';
    return;
  }

  list.innerHTML = state.incidents.map(function (i) {
    const history = i.history.map(function (h) {
      return '<li><time>' + h.at + '</time>' + h.event + '</li>';
    }).join('');

    const button = i.acked
      ? '<button disabled>Acknowledged ' + i.acked_at + '</button>'
      : '<button onclick="ack(\\'' + i.key + '\\')">Acknowledge</button>';

    return '<div class="card ' + (i.open ? 'open' : 'closed') + '">' +
      '<div class="row"><div>' +
        '<span class="id">Incident ' + i.inc + '</span>' +
        '<span class="badge ' + i.type.toLowerCase() + '">' + i.type + '</span>' +
        '<div class="what">' + i.words + '</div>' +
        '<div class="times">first ' + i.first + ' &middot; latest ' + i.last + '</div>' +
      '</div><div>' + button +
        (i.open ? '<div class="note">Acknowledging does not close this</div>' : '') +
      '</div></div>' +
      (history ? '<ul class="hist">' + history + '</ul>' : '') +
    '</div>';
  }).join('');
}

function refresh() {
  fetch('/api/state').then(function (r) { return r.json(); }).then(draw).catch(function () {});
}

function ack(key) {
  fetch('/api/ack', {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify({key: key})
  }).then(refresh);
}

refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
"""


class Dashboard(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass        # the console belongs to the incident log

    def _send(self, code, body, kind):
        raw = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def do_GET(self):
        if self.path == "/" or self.path.startswith("/?"):
            self._send(200, PAGE, "text/html; charset=utf-8")
        elif self.path == "/api/state":
            self._send(200, json.dumps(snapshot()), "application/json")
        else:
            self._send(404, "not found", "text/plain")

    def do_POST(self):
        if self.path != "/api/ack":
            self._send(404, "not found", "text/plain")
            return

        length = int(self.headers.get("Content-Length", "0"))
        try:
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            key = str(payload.get("key", ""))
        except (ValueError, UnicodeDecodeError):
            self._send(400, '{"error":"bad request"}', "application/json")
            return

        message = acknowledge(key)
        emit("[%s] dashboard: %s" % (now(), message))
        self._send(200, json.dumps({"message": message}), "application/json")


# --------------------------------------------------------------------------
# Plumbing
# --------------------------------------------------------------------------

def commands():
    for line in sys.stdin:
        parts = line.strip().split()
        if not parts:
            continue
        word = parts[0].lower()
        if word == "ack" and len(parts) > 1:
            emit(acknowledge(parts[1]))
        elif word == "list":
            state = snapshot()
            if not state["incidents"]:
                emit("nothing yet")
            for i in state["incidents"]:
                emit("  incident %s  %s  %s  %s" % (
                    i["inc"], i["type"], i["words"],
                    "acknowledged " + i["acked_at"] if i["acked"] else "not acknowledged"))
        elif word in ("quit", "exit"):
            emit("stopping")
            return
        else:
            emit("commands: ack <incident>, list, quit")


def own_addresses():
    found = []
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            if info[4][0] not in found:
                found.append(info[4][0])
    except socket.gaierror:
        pass
    return found


def serve_board(port, reply):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)

    # don't start if another receiver already has the port
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)

    try:
        listener.bind(("", port))
    except OSError as error:
        print("Cannot listen on port %d: %s" % (port, error))
        print("Another receiver is probably already running. Close that window,")
        print("or run this one with --port and change RECEIVER_PORT to match.")
        sys.exit(1)

    listener.listen(5)
    while True:
        conn, peer = listener.accept()
        threading.Thread(target=handle, args=(conn, peer, reply), daemon=True).start()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=2028, help="port the board connects to")
    parser.add_argument("--web-port", type=int, default=8080, help="port for the dashboard")
    parser.add_argument("--no-reply", action="store_true", help="do not answer the board")
    args = parser.parse_args()

    try:
        web = ThreadingHTTPServer(("", args.web_port), Dashboard)
    except OSError as error:
        print("Cannot serve the dashboard on port %d: %s" % (args.web_port, error))
        sys.exit(1)

    print("ElderCare receiver")
    print("  wearable connects to port %d" % args.port)
    print("  dashboard at http://localhost:%d" % args.web_port)
    for address in own_addresses():
        print("    (from another device on the same network: http://%s:%d)"
              % (address, args.web_port))
    print("  put the hotspot address in wifi_config.h as RECEIVER_IP")
    print("  commands: ack <incident>, list, quit\n")

    threading.Thread(target=web.serve_forever, daemon=True).start()
    threading.Thread(target=commands, daemon=True).start()
    serve_board(args.port, not args.no_reply)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nstopped")
        sys.exit(0)
