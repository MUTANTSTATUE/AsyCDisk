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

def recv_packet(s):
    header_data = s.recv(21)
    if not header_data:
        return None, None, None, None, None, None
    magic, ver, cmd, status, jlen, blen = struct.unpack(HEADER_FMT, header_data)
    
    j_payload = b""
    while len(j_payload) < jlen:
        chunk = s.recv(jlen - len(j_payload))
        if not chunk: break
        j_payload += chunk
    
    b_payload = b""
    while len(b_payload) < blen:
        chunk = s.recv(blen - len(b_payload))
        if not chunk: break
        b_payload += chunk
        
    json_obj = {}
    if jlen > 0:
        json_obj = json.loads(j_payload.decode('utf-8'))
        
    return cmd, status, json_obj, b_payload

def test():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    print("[*] 成功连接到服务器 127.0.0.1:8080")

    # 1. 登录测试
    print("[*] 1. 正在测试登录 (admin/admin123)...")
    pkt_login = create_packet(1, {"username": "admin", "password": "admin123"})
    s.sendall(pkt_login)
    
    cmd, status, resp_json, _ = recv_packet(s)
    print(f"    [Login Response] Status: {status}, JSON: {resp_json}")
    
    if status != 200:
        print("[!] 登录失败，终止测试。")
        return

    time.sleep(0.5)

    # 2. 目录列表测试
    print("\n[*] 2. 正在测试获取文件列表...")
    pkt_list = create_packet(2, {"parent_id": 0})
    s.sendall(pkt_list)
    
    cmd, status, resp_json, _ = recv_packet(s)
    print(f"    [ListDir Response] Status: {status}, JSON: {resp_json}")

    # 3. 文件上传测试
    print("\n[*] 3. 正在测试文件上传...")
    filename = "你好，这是中文.txt"
    content = "你好，这是中文".encode('utf-8')
    
    print(f"    [3.1] 发送上传请求: {filename}")
    pkt_up_req = create_packet(10, {"filename": filename, "filesize": len(content)})
    s.sendall(pkt_up_req)
    
    cmd, status, resp_json, _ = recv_packet(s)
    print(f"    [UploadReq Response] Status: {status}, JSON: {resp_json}")
    
    if status == 200:
        print(f"    [3.2] 发送数据块 (大小: {len(content)} 字节)...")
        pkt_data = create_packet(11, {}, content)
        s.sendall(pkt_data)
        
        print(f"    [3.3] 发送结束标志 (空数据块)...")
        pkt_eof = create_packet(11, {}, b"")
        s.sendall(pkt_eof)
        
        cmd, status, resp_json, _ = recv_packet(s)
        print(f"    [Upload Result] Status: {status}, JSON: {resp_json}")

    time.sleep(1)

    # 4. 文件下载测试
    print("\n[*] 4. 正在测试文件下载...")
    filename = "hello_world.txt"
    print(f"    [4.1] 发送下载请求: {filename}")
    pkt_down_req = create_packet(12, {"filename": filename})
    s.sendall(pkt_down_req)
    
    downloaded_content = b""
    while True:
        cmd, status, j_payload, b_payload = recv_packet(s)
        if cmd is None: break
        
        downloaded_content += b_payload
        if j_payload.get("eof"):
            print(f"    [4.2] 下载完成! 收到 {len(downloaded_content)} 字节")
            break
        if status != 200:
            print(f"    [!] 下载出错: {j_payload}")
            break
    
    if downloaded_content:
        try:
            print(f"    [4.3] 校验内容: {downloaded_content.decode()}")
        except:
            print(f"    [4.3] 校验内容: {downloaded_content}")

    time.sleep(1)
    s.close()
    print("\n[*] 测试完成，连接已关闭。")

if __name__ == '__main__':
    test()
