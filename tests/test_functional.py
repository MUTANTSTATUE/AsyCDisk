import pytest
import os
import hashlib
import time
import struct
from asyc_client import AsyCClient

SERVER_HOST = '127.0.0.1'
SERVER_PORT = 8080

@pytest.fixture
def client():
    c = AsyCClient(SERVER_HOST, SERVER_PORT)
    c.connect()
    yield c
    c.close()

def test_auth_flow(client):
    username = f"testuser_{int(time.time())}"
    password = "password123"

    # A-01: Register
    resp = client.register(username, password)
    assert resp['status'] == 200, f"Register failed: {resp}"

    # A-02: Register conflict
    resp = client.register(username, password)
    assert resp['status'] == 409, f"Should return 409 for conflict: {resp}"

    # A-03: Login success
    resp = client.login(username, password)
    assert resp['status'] == 200, f"Login failed: {resp}"
    assert client.user_id != -1

    # A-04: Login failure (wrong password)
    c2 = AsyCClient(SERVER_HOST, SERVER_PORT)
    c2.connect()
    resp = c2.login(username, "wrong_pass")
    assert resp['status'] == 401, f"Should return 401 for wrong pass: {resp}"
    c2.close()

def test_directory_ops(client):
    # Setup: Login
    username = f"diruser_{int(time.time())}"
    client.register(username, "pass")
    client.login(username, "pass")

    # B-01: ListDir
    resp = client.list_dir(parent_id=0)
    assert resp['status'] == 200
    assert 'files' in resp['json']

    # B-02: Unauthorized access
    c_unauth = AsyCClient(SERVER_HOST, SERVER_PORT)
    c_unauth.connect()
    resp = c_unauth.list_dir(0)
    assert resp['status'] == 403, f"Unauth list_dir should return 403: {resp}"
    c_unauth.close()

def test_file_transfer_basic(client):
    # Setup: Login
    username = f"fileuser_{int(time.time())}"
    client.register(username, "pass")
    client.login(username, "pass")

    # C-01: Upload & Download small file
    filename = "small_test.txt"
    content = b"Hello AsyCDisk! This is a test file."
    filesize = len(content)
    
    # Upload
    resp = client.upload_req(filename, filesize)
    assert resp['status'] == 200
    
    client.upload_data(stream_id=1, binary_data=content)
    resp = client.upload_data(stream_id=1, binary_data=b"") # EOF
    assert resp['status'] == 200
    
    # Get file list to find file_id
    resp = client.list_dir(0)
    files = resp['json']['files']
    file_id = -1
    for f in files:
        if f['filename'] == filename:
            file_id = f['id']
            break
    assert file_id != -1, "File not found after upload"

    # Download
    resp = client.download_req(file_id)
    assert resp['status'] == 200
    
    downloaded_data = b""
    while True:
        resp = client.recv_packet()
        downloaded_data += resp['binary']
        if resp['json'].get('eof'):
            break
    
    assert downloaded_data == content, "Downloaded content mismatch"
    
    # MD5 check
    assert hashlib.md5(downloaded_data).hexdigest() == hashlib.md5(content).hexdigest()

def test_resumable_upload(client):
    username = f"resumeuser_{int(time.time())}"
    client.register(username, "pass")
    client.login(username, "pass")

    filename = "resume_test.dat"
    content = os.urandom(1024 * 10) # 10KB
    half = 1024 * 5
    
    # 1. Start upload and send half
    resp = client.upload_req(filename, len(content), stream_id=10)
    assert resp['status'] == 200
    client.upload_data(stream_id=10, binary_data=content[:half])
    
    # Simulating crash/disconnect
    client.close()
    
    # 2. Reconnect and resume
    client.connect()
    client.login(username, "pass")
    
    resp = client.upload_req(filename, len(content), stream_id=11)
    assert resp['status'] == 200
    offset = resp['json'].get('offset', 0)
    assert offset == half, f"Expected offset {half}, got {offset}"
    
    client.upload_data(stream_id=11, binary_data=content[offset:])
    resp = client.upload_data(stream_id=11, binary_data=b"") # EOF
    assert resp['status'] == 200
    
    # Verify file size in list
    resp = client.list_dir(0)
    found = False
    for f in resp['json']['files']:
        if f['filename'] == filename:
            assert f['filesize'] == len(content)
            found = True
            break
    assert found

def test_robustness_magic(client):
    # D-01: Invalid Magic
    client.sock.sendall(b"INVALID_MAGIC_DATA_HERE_BLAH_BLAH")
    try:
        # Server should close connection
        data = client.sock.recv(1024)
        assert not data, "Server should have closed connection for invalid magic"
    except socket.error:
        pass # Expected

def test_robustness_malformed_json(client):
    # D-03: Malformed JSON
    # Magic(I), Ver(B), Cmd(H), Status(H), Stream(I), JLen(I), BLen(Q)
    header = struct.pack('<IBHHIIQ', 0x4B444341, 1, 1, 0, 1, 10, 0)
    client.sock.sendall(header + b"{invalid:}")
    # Depending on implementation, server might close or return 400
    try:
        resp = client.recv_packet()
        # If server returns error
        if resp:
            # Protocol.h says it might return 400 for unknown, 
            # Session.cpp closes connection on JSON parse error
            pass 
    except:
        pass # Closed is also fine

def test_remove_file(client):
    username = f"rmuser_{int(time.time())}"
    client.register(username, "pass")
    client.login(username, "pass")

    # Upload file
    client.upload_req("to_delete.txt", 5)
    client.upload_data(1, b"hello")
    client.upload_data(1, b"")
    
    # Get ID
    resp = client.list_dir(0)
    file_id = resp['json']['files'][0]['id']
    
    # Remove
    resp = client.remove(file_id)
    assert resp['status'] == 200
    
    # Verify gone
    resp = client.list_dir(0)
    assert len(resp['json']['files']) == 0
