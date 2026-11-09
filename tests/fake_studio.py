"""Stand-in XOXO Studio for tests/link_test.cpp: XOXO-Studio's own netmidi.Session
answering on 127.0.0.1:15004, plus an RTP receiver on 46030 that checks XO-Wave's
audio packets (L16 S16BE stereo, PT 127, 310 frames). Usage:
  python3 tests/fake_studio.py <path to XOXO-Studio/fm1-bt-bridge>"""
import socket, struct, sys, threading, time, math
sys.path.insert(0, sys.argv[1])
import netmidi

got = []
s = netmidi.Session("XOXO Studio on fakehost", 15004, lambda m: got.append(bytes(m)))
s.start()
threading.Thread(target=s.run, daemon=True).start()

def send_return():
    """the Studio's mix back to XO-Wave: a 330 Hz tone, RTP L16 stereo, 310-frame packets, to UDP 46016"""
    while not s.names():
        time.sleep(0.05)
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    seq, ts, ph, t0 = 0, 0, 0.0, time.time()
    for k in range(int(4 / 0.0064583)):
        frames = []
        for _ in range(310):
            v = int(0.25 * 32767 * math.sin(ph))
            frames.append(struct.pack(">hh", v, v))
            ph += 2 * math.pi * 330 / 48000
        out.sendto(struct.pack(">BBHII", 0x80, 127, seq & 0xFFFF, ts & 0xFFFFFFFF, 1234) + b"".join(frames),
                   ("127.0.0.1", 46016))
        seq += 1
        ts += 310
        time.sleep(max(0.0, t0 + (k + 1) * 310 / 48000 - time.time()))

threading.Thread(target=send_return, daemon=True).start()

rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
rtp.bind(("0.0.0.0", 46030))
rtp.settimeout(0.2)
packets, sizes, pts, seqs, peak, sent_note = 0, set(), set(), [], 0, False
end = time.time() + 8
while time.time() < end:
    if s.names() and not sent_note:
        time.sleep(0.3)
        s.send([bytes([0x91, 64, 90])])       # the Studio's routes play XO-Wave
        sent_note = True
    try:
        pkt, _ = rtp.recvfrom(2048)
    except socket.timeout:
        if packets and time.time() > end - 5:
            pass
        continue
    packets += 1
    sizes.add(len(pkt) - 12)
    pts.add(pkt[1] & 0x7F)
    seqs.append(struct.unpack(">H", pkt[2:4])[0])
    frames = struct.unpack(">%dh" % ((len(pkt) - 12) // 2), pkt[12:])
    peak = max(peak, max(abs(x) for x in frames))
names = s.names()
print("peers:", names, "linked_box:", [netmidi.linked_box(n) for n in names])
print("midi from XO-Wave:", [m.hex() for m in got])
gaps = sum(1 for a, b in zip(seqs, seqs[1:]) if (b - a) & 0xFFFF != 1)
print(f"rtp packets: {packets}  payload bytes: {sorted(sizes)}  payload types: {sorted(pts)}  seq gaps: {gaps}  peak: {peak}")
