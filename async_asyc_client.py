import asyncio
import struct
import json
import time

class AsyncAsyCClient:
    MAGIC = 0x4B444341
    VERSION = 1
    HEADER_FMT = '<IBHHIIQ'
    HEADER_SIZE = 25

    def __init__(self, host='127.0.0.1', port=8080):
        self.host = host
        self.port = port
        self.reader = None
        self.writer = None

    async def connect(self):
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)

    async def close(self):
        if self.writer:
            self.writer.close()
            await self.writer.wait_closed()

    async def send_packet(self, command, stream_id=0, payload_dict=None, binary_data=b''):
        if payload_dict is None:
            payload_dict = {}
        json_bytes = json.dumps(payload_dict).encode('utf-8')
        json_len = len(json_bytes)
        binary_len = len(binary_data)
        
        header = struct.pack(self.HEADER_FMT, self.MAGIC, self.VERSION, command, 0, stream_id, json_len, binary_len)
        self.writer.write(header + json_bytes + binary_data)
        await self.writer.drain()

    async def recv_packet(self):
        header_data = await self.reader.readexactly(self.HEADER_SIZE)
        magic, ver, cmd, status, stream_id, jlen, blen = struct.unpack(self.HEADER_FMT, header_data)
        
        j_payload = await self.reader.readexactly(jlen)
        b_payload = await self.reader.readexactly(blen)
            
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

    async def ping(self, stream_id=0):
        start_time = time.perf_counter()
        await self.send_packet(0, stream_id=stream_id)
        resp = await self.recv_packet()
        end_time = time.perf_counter()
        return resp, (end_time - start_time) * 1000 # ms
