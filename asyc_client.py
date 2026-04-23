import socket
import struct
import json
import hashlib
import os

class AsyCClient:
    MAGIC = 0x4B444341
    VERSION = 1
    # magic(I), version(B), command(H), status(H), stream_id(I), jlen(I), blen(Q) = 25 bytes
    HEADER_FMT = '<IBHHIIQ'
    HEADER_SIZE = 25

    def __init__(self, host='127.0.0.1', port=8080):
        self.host = host
        self.port = port
        self.sock = None
        self.user_id = -1

    def connect(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.connect((self.host, self.port))
        self.sock.settimeout(10.0)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def send_packet(self, command, stream_id=0, payload_dict=None, binary_data=b''):
        if payload_dict is None:
            payload_dict = {}
        json_bytes = json.dumps(payload_dict).encode('utf-8')
        json_len = len(json_bytes)
        binary_len = len(binary_data)
        
        header = struct.pack(self.HEADER_FMT, self.MAGIC, self.VERSION, command, 0, stream_id, json_len, binary_len)
        self.sock.sendall(header + json_bytes + binary_data)

    def recv_packet(self):
        header_data = b""
        while len(header_data) < self.HEADER_SIZE:
            chunk = self.sock.recv(self.HEADER_SIZE - len(header_data))
            if not chunk:
                return None
            header_data += chunk
            
        magic, ver, cmd, status, stream_id, jlen, blen = struct.unpack(self.HEADER_FMT, header_data)
        
        if magic != self.MAGIC:
            raise ValueError(f"Invalid magic number: {hex(magic)}")
            
        j_payload = b""
        while len(j_payload) < jlen:
            chunk = self.sock.recv(jlen - len(j_payload))
            if not chunk: break
            j_payload += chunk
        
        b_payload = b""
        while len(b_payload) < blen:
            chunk = self.sock.recv(min(blen - len(b_payload), 8192))
            if not chunk: break
            b_payload += chunk
            
        json_obj = {}
        if jlen > 0:
            json_obj = json.loads(j_payload.decode('utf-8'))
            
        return {
            "command": cmd,
            "status": status,
            "stream_id": stream_id,
            "json": json_obj,
            "binary": b_payload
        }

    def register(self, username, password):
        self.send_packet(5, payload_dict={"username": username, "password": password})
        return self.recv_packet()

    def login(self, username, password):
        self.send_packet(1, payload_dict={"username": username, "password": password})
        resp = self.recv_packet()
        if resp and resp['status'] == 200:
            self.user_id = resp['json'].get('user_id', -1)
        return resp

    def list_dir(self, parent_id=0):
        self.send_packet(2, payload_dict={"parent_id": parent_id})
        return self.recv_packet()

    def upload_req(self, filename, filesize, stream_id=1):
        self.send_packet(10, stream_id=stream_id, payload_dict={"filename": filename, "filesize": filesize})
        return self.recv_packet()

    def upload_data(self, stream_id, binary_data):
        self.send_packet(11, stream_id=stream_id, binary_data=binary_data)
        # UploadData might not return immediately if it's just a chunk, 
        # but the protocol seems to expect a response after EOF (empty binary)
        if len(binary_data) == 0:
            return self.recv_packet()
        return None

    def download_req(self, file_id, offset=0, stream_id=2):
        self.send_packet(12, stream_id=stream_id, payload_dict={"file_id": file_id, "offset": offset})
        return self.recv_packet()

    def remove(self, file_id, parent_id=0):
        self.send_packet(4, payload_dict={"file_id": file_id, "parent_id": parent_id})
        return self.recv_packet()

    def ping(self, stream_id=0):
        self.send_packet(0, stream_id=stream_id)
        return self.recv_packet()
