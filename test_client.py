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

    # 0. 注册测试
    print("[*] 0. 正在测试用户注册 (testuser/test123)...")
    pkt_reg = create_packet(5, {"username": "testuser", "password": "test123"})
    s.sendall(pkt_reg)
    cmd, status, resp_json, _ = recv_packet(s)
    print(f"    [Register Response] Status: {status}, JSON: {resp_json}")

    time.sleep(0.5)

    # 1. 登录测试
    print("[*] 1. 正在测试登录 (testuser/test123)...")
    pkt_login = create_packet(1, {"username": "testuser", "password": "test123"})
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

    # 3. 文件上传测试 (断点续传)
    print("\n[*] 3. 正在测试断点续传上传...")
    filename = "resume_test.txt"
    content = b"Part1_Data_Part2_Data_Part3_Data"
    
    # [3.1] 先传前半部分
    print(f"    [3.1] 模拟上传前半部分 (10 字节)...")
    pkt_up_req = create_packet(10, {"filename": filename, "filesize": len(content)})
    s.sendall(pkt_up_req)
    cmd, status, resp_json, _ = recv_packet(s)
    
    if status == 200:
        s.sendall(create_packet(11, {}, content[:10]))
        # 故意不发完就关闭连接
        time.sleep(0.5)
        print("    [!] 故意断开连接...")
        s.close()
    
    time.sleep(1)
    
    # [3.2] 重新连接并续传
    print(f"    [3.2] 重新连接并请求续传...")
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 8080))
    s.sendall(create_packet(1, {"username": "testuser", "password": "test123"}))
    recv_packet(s) # skip login resp
    
    s.sendall(create_packet(10, {"filename": filename, "filesize": len(content)}))
    cmd, status, resp_json, _ = recv_packet(s)
    offset = resp_json.get("offset", 0)
    print(f"    [3.3] 服务器返回 offset: {offset}")
    
    if offset == 10:
        print(f"    [3.4] 从 offset {offset} 继续发送剩余数据...")
        s.sendall(create_packet(11, {}, content[offset:]))
        s.sendall(create_packet(11, {}, b"")) # EOF
        
        cmd, status, resp_json, _ = recv_packet(s)
        print(f"    [Upload Result] Status: {status}, JSON: {resp_json}")

        # [3.5] 校验数据库同步：再次获取文件列表
        print(f"    [3.5] 校验数据库同步：正在获取文件列表...")
        s.sendall(create_packet(2, {"parent_id": 0}))
        cmd, status, resp_json, _ = recv_packet(s)
        print(f"    [ListDir After Upload] JSON: {resp_json}")
        
        found = False
        for f in resp_json.get("files", []):
            if f["filename"] == filename:
                found = True
                print(f"    [V] 数据库同步校验成功：找到文件 {filename}，大小 {f['filesize']}")
                break
        if not found:
            print(f"    [X] 数据库同步校验失败：未在列表中找到 {filename}")

    time.sleep(1)

    # 4. 文件下载测试 (断点续传/Range)
    print("\n[*] 4. 正在测试断点续传下载 (从 offset 6 开始)...")
    print(f"    [4.1] 发送下载请求: {filename}, offset=6")
    pkt_down_req = create_packet(12, {"filename": filename, "offset": 6})
    s.sendall(pkt_down_req)
    
    downloaded_content = b""
    while True:
        cmd, status, j_payload, b_payload = recv_packet(s)
        if cmd is None: break
        downloaded_content += b_payload
        if j_payload.get("eof"): break
    
    print(f"    [4.2] 下载完成! 内容: {downloaded_content.decode()}")
    expected = content[6:].decode()
    if downloaded_content.decode() == expected:
        print("    [V] 下载内容校验通过!")
    else:
        print(f"    [X] 内容不匹配! 期望: {expected}")

    time.sleep(1)


    
    # 5. 文件删除测试
    print("\n[*] 5. 正在测试文件删除...")
    print(f"    [5.1] 发送删除请求: {filename}")
    pkt_rm = create_packet(4, {"filename": filename, "parent_id": 0})
    s.sendall(pkt_rm)
    cmd, status, resp_json, _ = recv_packet(s)
    print(f"    [Remove Response] Status: {status}, JSON: {resp_json}")

    # [5.2] 再次获取列表确认删除
    print("    [5.2] 再次获取文件列表以确认删除...")
    s.sendall(create_packet(2, {"parent_id": 0}))
    cmd, status, resp_json, _ = recv_packet(s)
    print(f"    [ListDir After Remove] JSON: {resp_json}")
    
    found = False
    for f in resp_json.get("files", []):
        if f["filename"] == filename:
            found = True
            break
    if not found:
        print("    [V] 文件删除校验成功！")
    else:
        print("    [X] 文件删除校验失败：文件依然存在。")

    time.sleep(1)
    s.close()
    print("\n[*] 测试完成，连接已关闭。")

if __name__ == '__main__':
    test()
