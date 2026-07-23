"""Minimal BOOTP+TFTP server speaking QEMU's -netdev socket framing
(4-byte BE length + raw ethernet frame). Logs everything for RE."""
import socket, struct, sys, threading

MY_MAC  = bytes.fromhex('525500000202')
MY_IP   = bytes([10,0,2,2])
CL_IP   = bytes([10,0,2,15])
TFTP_FILE = sys.argv[2] if len(sys.argv) > 2 else '/workspace/home/dev/e17-re/tftp/uboot.bin'
PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18475

def csum(b):
    if len(b) % 2: b += b'\0'
    s = sum(struct.unpack('>%dH' % (len(b)//2), b))
    while s >> 16: s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff

def ip_udp(dst_mac, src_port, dst_port, payload, dst_ip=CL_IP):
    udp = struct.pack('>HHHH', src_port, dst_port, 8+len(payload), 0) + payload
    ip = struct.pack('>BBHHHBBH4s4s', 0x45, 0, 20+len(udp), 0, 0, 64, 17, 0, MY_IP, dst_ip)
    ip = ip[:10] + struct.pack('>H', csum(ip)) + ip[12:]
    return dst_mac + MY_MAC + b'\x08\x00' + ip + udp

def handle(conn):
    data = open(TFTP_FILE,'rb').read()
    print(f'serving {TFTP_FILE}, {len(data)} bytes', flush=True)
    tftp_client_port = None
    def send(frame):
        if len(frame) < 60:
            frame = frame + bytes(60 - len(frame))
        conn.sendall(struct.pack('>I', len(frame)) + frame)
    buf = b''
    while True:
        r = conn.recv(65536)
        if not r: break
        buf += r
        while len(buf) >= 4:
            n = struct.unpack('>I', buf[:4])[0]
            if len(buf) < 4 + n: break
            f = buf[4:4+n]; buf = buf[4+n:]
            dst, src, et = f[0:6], f[6:12], struct.unpack('>H', f[12:14])[0]
            print('FRAME len', n, 'et %04x' % et, f[:48].hex(), flush=True)
            if et == 0x0806:  # ARP
                op = struct.unpack('>H', f[20:22])[0]
                tpa = f[38:42]
                print('ARP', 'req' if op==1 else 'rep', 'for', '.'.join(map(str,tpa)), flush=True)
                if op == 1 and tpa == MY_IP:
                    rep = f[0:6]  # will rebuild below
                    arp = struct.pack('>HHBBH', 1, 0x800, 6, 4, 2) + MY_MAC + MY_IP + src + f[28:32]
                    send(src + MY_MAC + b'\x08\x06' + arp)
            elif et == 0x8035:  # RARP
                print('RARP request', flush=True)
                arp = struct.pack('>HHBBH', 1, 0x800, 6, 4, 4) + MY_MAC + MY_IP + src + CL_IP
                send(src + MY_MAC + b'\x80\x35' + arp)
            elif et == 0x0800:  # IP
                ihl = (f[14] & 0xf) * 4
                proto = f[23]
                if proto != 17: continue
                u = 14 + ihl
                sp, dp, ulen = struct.unpack('>HHH', f[u:u+6])
                pl = f[u+8:u+ulen]
                if dp == 67:  # BOOTP request
                    xid = pl[4:8]
                    print('BOOTP request xid', xid.hex(), flush=True)
                    rep = bytearray(300)
                    rep[0] = 2; rep[1] = 1; rep[2] = 6
                    rep[4:8] = xid
                    rep[16:20] = CL_IP          # yiaddr
                    rep[20:24] = MY_IP          # siaddr
                    rep[28:34] = src            # chaddr
                    sname = b'qemuserv'
                    rep[44:44+len(sname)] = sname
                    bf = b'uboot.bin'
                    rep[108:108+len(bf)] = bf   # file
                    rep[236:240] = bytes([99,130,83,99])  # magic cookie
                    rep[240] = 255
                    send(ip_udp(src, 67, 68, bytes(rep), dst_ip=bytes([255,255,255,255])))
                elif dp == 69:  # TFTP RRQ
                    parts = pl[2:].split(b'\0')
                    print('TFTP RRQ:', parts[0], parts[1], 'from port', sp, flush=True)
                    blk = data[0:512]
                    send(ip_udp(src, 6969, sp, struct.pack('>HH', 3, 1) + blk))
                elif dp == 6969:  # TFTP ACK
                    op, blkno = struct.unpack('>HH', pl[0:4])
                    if op == 4:
                        nxt = blkno * 512
                        blk = data[nxt:nxt+512]
                        if nxt <= len(data):
                            send(ip_udp(src, 6969, sp, struct.pack('>HH', 3, blkno+1) + blk))
                        if blkno % 50 == 0 or len(blk) < 512:
                            print('TFTP ack', blkno, 'sent block', blkno+1, len(blk), flush=True)
                else:
                    print('UDP', sp, '->', dp, len(pl), 'bytes', flush=True)
    print('netserv: connection closed', flush=True)

srv = socket.socket()
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(('127.0.0.1', PORT)); srv.listen(1)
print('netserv listening on', PORT, flush=True)
while True:
    c, _ = srv.accept()
    threading.Thread(target=handle, args=(c,), daemon=True).start()
