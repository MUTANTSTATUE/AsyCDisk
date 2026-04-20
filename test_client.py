import socket
import struct
import json
import time

MAGIC = 0x4B444341
VERSION = 1
# struct format: < (little endian), I (4 bytes), B (1 byte), H (2 bytes), H (2 bytes), I (4 bytes), Q (8 bytes) = 21 bytes
HEADER_FMT = '<IBHHIQ'

def create_packet(command, payload_dict, binary_data=b''):
    json_bytes = json.dumps(payload_dict).encode('utf-8')
    json_len = len(json_bytes)
    binary_len = len(binary_data)
    
    header = struct.pack(HEADER_FMT, MAGIC, VERSION, command, 0, json_len, binary_len)
    return header + json_bytes + binary_data

def test():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    print("[*] 成功连接到服务器 127.0.0.1:8080")

    # 1. 登录测试
    print("[*] 1. 正在测试登录 (admin/admin123)...")
    pkt_login = create_packet(1, {"username": "admin", "password": "admin123"})
    s.sendall(pkt_login)
    
    resp_header = s.recv(21)
    magic, ver, cmd, status, jlen, blen = struct.unpack(HEADER_FMT, resp_header)
    resp_json = json.loads(s.recv(jlen).decode())
    print(f"    [Login Response] Status: {status}, JSON: {resp_json}")
    
    time.sleep(0.5)

    # 2. 目录列表测试
    print("\n[*] 2. 正在测试获取文件列表...")
    pkt_list = create_packet(2, {"parent_id": 0})
    s.sendall(pkt_list)
    
    resp_header = s.recv(21)
    magic, ver, cmd, status, jlen, blen = struct.unpack(HEADER_FMT, resp_header)
    resp_json = json.loads(s.recv(jlen).decode())
    print(f"    [ListDir Response] Status: {status}, JSON: {resp_json}")

    time.sleep(1)
    s.close()
    print("\n[*] 测试完成，连接已关闭。")

if __name__ == '__main__':
    test()
