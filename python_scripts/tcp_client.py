# tcp_keys_client.py — Windows (usa msvcrt.getch)
import socket, time, msvcrt

ESP_IP = "192.168.100.185"   # coloque o IP da sua ESP32
PORT   = 5000

def now_ms(): return int(time.time()*1000)

def recv_line(sock, timeout=2.0):
    sock.settimeout(timeout)
    data = b""
    try:
        while True:
            chunk = sock.recv(4096)
            if not chunk: break
            data += chunk
            if b"\n" in chunk: break
    except Exception:
        pass
    return data.decode(errors="ignore").strip()

def main():
    with socket.create_connection((ESP_IP, PORT), timeout=5) as s:
        # mensagem de boas-vindas (se houver)
        try:
            print(recv_line(s, timeout=1.0))
        except Exception:
            pass

        print("\nControles: [1]=SORT  [2]=SAFE  [3]=SET 3000  [4]=ping  [q]=sair\n")
        while True:
            ch = msvcrt.getch()
            if not ch: 
                continue
            key = ch.decode('ascii', errors='ignore').lower()
            if key == 'q':
                print("Saindo…")
                break
            if key == '1':
                msg = "SORT"
            elif key == '2':
                msg = "SAFE"
            elif key == '3':
                msg = "SET 3000"
            elif key == '4':
                msg = "ping"
            else:
                continue  # ignora outras teclas

            t0 = now_ms()
            s.sendall((msg + "\n").encode())
            resp = recv_line(s, timeout=2.0)
            rtt = now_ms() - t0
            print(f"> {msg} | ESP respondeu: {resp}  [tcp_rtt_ms={rtt}]")

if __name__ == "__main__":
    main()
