# udp_server.py — PC
# Reaproveita o exemplo do professor, mas devolve um ECO com timestamp do PC
import socket, time, json

ADDR = ("0.0.0.0", 6010)
def now_ms(): return int(time.time() * 1000)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(ADDR)
print("Aguardando UDP em", ADDR, "…")

while True:
    data, src = s.recvfrom(2048)
    txt = data.decode(errors="ignore").strip()
    tpc = now_ms()
    print(f"de {src} => {txt}  [t_pc_recv_ms={tpc}, size={len(data)}]")
    # Eco para a ESP medir RTT (a ESP não precisa parsear, só medir o tempo de ida e volta)
    reply = json.dumps({
        "proto": "UDP",
        "t_pc_recv_ms": tpc,
        "size": len(data),
        "echo": txt
    })
    s.sendto(reply.encode(), src)
