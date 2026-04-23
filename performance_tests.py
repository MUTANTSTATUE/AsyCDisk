import asyncio
import time
import numpy as np
from async_asyc_client import AsyncAsyCClient

SERVER_HOST = '127.0.0.1'
SERVER_PORT = 8080

async def test_concurrency(n_clients):
    print(f"[*] Starting Concurrency Test: C={n_clients}")
    clients = []
    
    # 1. Connect all
    start_connect = time.perf_counter()
    for _ in range(n_clients):
        c = AsyncAsyCClient(SERVER_HOST, SERVER_PORT)
        try:
            await c.connect()
            clients.append(c)
        except Exception as e:
            print(f"[!] Connection failed: {e}")
    end_connect = time.perf_counter()
    
    avg_conn_time = ((end_connect - start_connect) / len(clients)) * 1000 if clients else 0
    print(f"    - Connected {len(clients)} clients. Avg connect time: {avg_conn_time:.2f}ms")

    # 2. Parallel Ping-Pong
    latencies = []
    
    async def run_ping(client):
        try:
            _, lat = await client.ping()
            return lat
        except Exception as e:
            return None

    start_ping = time.perf_counter()
    results = await asyncio.gather(*(run_ping(c) for c in clients))
    end_ping = time.perf_counter()
    
    valid_results = [r for r in results if r is not None]
    success_rate = (len(valid_results) / n_clients) * 100
    
    if valid_results:
        avg_lat = np.mean(valid_results)
        p99_lat = np.percentile(valid_results, 99)
        print(f"    - Success Rate: {success_rate:.1f}%")
        print(f"    - Avg Latency: {avg_lat:.2f}ms")
        print(f"    - P99 Latency: {p99_lat:.2f}ms")
    else:
        print("    - No valid results.")

    # 3. Cleanup
    for c in clients:
        await c.close()
    
    return {
        "concurrency": n_clients,
        "success_rate": success_rate,
        "avg_conn_time": avg_conn_time,
        "avg_lat": np.mean(valid_results) if valid_results else 0,
        "p99_lat": np.percentile(valid_results, 99) if valid_results else 0
    }

async def test_throughput(n_concurrent_streams, direction='upload'):
    print(f"[*] Starting Throughput Test: {direction}, streams={n_concurrent_streams}")
    CHUNK_SIZE = 64 * 1024 # 64KB
    TOTAL_SIZE = 1 * 1024 * 1024 # 1MB
    data = b'x' * CHUNK_SIZE
    
    async def upload_worker(client_id):
        c = AsyncAsyCClient(SERVER_HOST, SERVER_PORT)
        await c.connect()
        ts = int(time.time() * 1000)
        user = f"perf_up_{client_id}_{ts}"
        await c.send_packet(5, payload_dict={"username": user, "password": "p"})
        await c.recv_packet()
        await c.send_packet(1, payload_dict={"username": user, "password": "p"})
        await c.recv_packet()
        
        start = time.perf_counter()
        filename = f"perf_{client_id}_{ts}.dat"
        await c.send_packet(10, stream_id=1, payload_dict={"filename": filename, "filesize": TOTAL_SIZE})
        resp = await c.recv_packet()
        
        if resp['status'] == 200:
            if resp['json'].get('msg') == "already uploaded":
                pass # skip data
            else:
                sent = 0
                while sent < TOTAL_SIZE:
                    await c.send_packet(11, stream_id=1, binary_data=data)
                    sent += CHUNK_SIZE
                
                await c.send_packet(11, stream_id=1, binary_data=b"") # EOF
                res = await c.recv_packet()
                if res['status'] != 200:
                    print(f"    [!] Worker {client_id} failed on finish: {res}")
        else:
            print(f"    [!] Worker {client_id} failed on req: {resp}")

        end = time.perf_counter()
        await c.close()
        return TOTAL_SIZE / (end - start) / (1024 * 1024)

    async def download_worker(client_id):
        c = AsyncAsyCClient(SERVER_HOST, SERVER_PORT)
        await c.connect()
        ts = int(time.time() * 1000)
        user = f"perf_dl_{client_id}_{ts}"
        
        await c.send_packet(5, payload_dict={"username": user, "password": "p"})
        await c.recv_packet()
        await c.send_packet(1, payload_dict={"username": user, "password": "p"})
        await c.recv_packet()
        
        # Setup file
        filename = f"bench_{ts}.dat"
        await c.send_packet(10, stream_id=1, payload_dict={"filename": filename, "filesize": TOTAL_SIZE})
        resp = await c.recv_packet()
        
        if resp['json'].get('msg') != "already uploaded":
            await c.send_packet(11, stream_id=1, binary_data=b'y' * TOTAL_SIZE)
            await c.send_packet(11, stream_id=1, binary_data=b"")
            await c.recv_packet()
        
        await asyncio.sleep(0.1) # Give server time to sync DB
        
        await c.send_packet(2, payload_dict={"parent_id": 0})
        resp = await c.recv_packet()
        if 'files' not in resp['json'] or not resp['json']['files']:
            print(f"    [!] Worker {client_id} ListDir failed: {resp}")
            await c.close()
            return 0
            
        file_id = resp['json']['files'][0]['id']
        
        start = time.perf_counter()
        await c.send_packet(12, stream_id=2, payload_dict={"file_id": file_id, "offset": 0})
        await c.recv_packet() 
        
        received = 0
        while True:
            resp = await c.recv_packet()
            received += len(resp['binary'])
            if resp['json'].get('eof'):
                break
        end = time.perf_counter()
        await c.close()
        return TOTAL_SIZE / (end - start) / (1024 * 1024)

    if direction == 'upload':
        results = await asyncio.gather(*(upload_worker(i) for i in range(n_concurrent_streams)))
    else:
        results = await asyncio.gather(*(download_worker(i) for i in range(n_concurrent_streams)))
    
    total_throughput = sum(results)
    avg_per_stream = np.mean(results)
    print(f"    - Total Throughput: {total_throughput:.2f} MB/s")
    print(f"    - Avg Per Stream: {avg_per_stream:.2f} MB/s")
    return total_throughput

async def main():
    results_concurrency = []
    for c in [100, 500, 1000]:
        res = await test_concurrency(c)
        results_concurrency.append(res)
        await asyncio.sleep(1)

    print("\n" + "="*50)
    print("CONCURRENCY RESULTS")
    print("="*50)
    print("C\tSuccess\tAvgConn\tAvgLat\tP99Lat")
    for r in results_concurrency:
        print(f"{r['concurrency']}\t{r['success_rate']:.1f}%\t{r['avg_conn_time']:.2f}ms\t{r['avg_lat']:.2f}ms\t{r['p99_lat']:.2f}ms")

    results_throughput = {}
    results_throughput['up_1'] = await test_throughput(1, 'upload')
    results_throughput['up_5'] = await test_throughput(5, 'upload')
    results_throughput['dl_1'] = await test_throughput(1, 'download')
    results_throughput['dl_5'] = await test_throughput(5, 'download')

    print("\n" + "="*50)
    print("THROUGHPUT RESULTS")
    print("="*50)
    print(f"Upload (1 stream):  {results_throughput['up_1']:.2f} MB/s")
    print(f"Upload (5 streams): {results_throughput['up_5']:.2f} MB/s")
    print(f"Download (1 stream): {results_throughput['dl_1']:.2f} MB/s")
    print(f"Download (5 streams): {results_throughput['dl_5']:.2f} MB/s")

if __name__ == "__main__":
    asyncio.run(main())
