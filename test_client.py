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

    # 1. 正常包测试
    print("[*] 1. 正在发送正常包 (完整的单次发送)...")
    pkt1 = create_packet(0, {"msg": "Hello Normal"})
    s.sendall(pkt1)
    
    # 读取服务器 Echo 回来的响应
    resp_header = s.recv(21)
    print(f"    [Echo] 收到响应头: {len(resp_header)} 字节")
    
    time.sleep(0.5)

    # 2. 拆包测试 (模拟网络极度拥堵/慢速)
    print("\n[*] 2. 正在发送拆包/半包 (分两次发送一个完整的包)...")
    pkt2 = create_packet(2, {"msg": "Hello Fragmented"}, b"BinaryPayload123")
    print(f"    发送前 10 字节: {pkt2[:10]}")
    s.sendall(pkt2[:10]) # 发送前 10 字节 (连包头都没发完)
    
    time.sleep(1) # 睡眠1秒，此时服务器收到10字节，不足21字节，不应该触发提取逻辑
    
    print(f"    发送剩余的 {len(pkt2[10:])} 字节...")
    s.sendall(pkt2[10:]) # 发送剩余部分，此时服务器凑齐了包，应该触发提取
    time.sleep(0.5)

    # 3. 粘包测试 (模拟快速/小包积压)
    print("\n[*] 3. 正在发送粘包 (将三个包拼接到一起一次性发给服务器)...")
    pkt3_1 = create_packet(10, {"task": "sticky 1"})
    pkt3_2 = create_packet(11, {"task": "sticky 2"})
    pkt3_3 = create_packet(12, {"task": "sticky 3"})
    
    s.sendall(pkt3_1 + pkt3_2 + pkt3_3)
    
    time.sleep(1)
    s.close()
    print("\n[*] 测试完成，连接已关闭。")

if __name__ == '__main__':
    test()
