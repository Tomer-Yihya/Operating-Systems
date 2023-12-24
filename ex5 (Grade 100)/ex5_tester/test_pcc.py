import os
import random
import re
import socket
import string
import subprocess
import threading
import time
from tempfile import NamedTemporaryFile

import pytest

random.seed(32)


def build_c_file(file_name: str, binary_name: str):
    subprocess.run(
        f"gcc -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 {file_name} -o {binary_name}".split(
            " "
        ),
        check=True,
    )


def start_server(server_binary: str, port: int):
    return subprocess.Popen(
        f"./{server_binary} {port}".split(" "),
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
    )


def run_client(client_binary: str, ip: str, port: int, file_path: str):
    return subprocess.run(
        f"./{client_binary} {ip} {port} {file_path}".split(" "),
        stderr=subprocess.PIPE,
        stdout=subprocess.PIPE,
        check=True,
        text=True,
    )


@pytest.fixture(scope="module", autouse=True)
def server():
    build_c_file("pcc_server.c", "server")
    yield "server"
    os.remove("server")


@pytest.fixture(scope="module", autouse=True)
def client():
    build_c_file("pcc_client.c", "client")
    yield "client"
    os.remove("client")


def validate_server_counts(server_output: str, file_content: bytes):
    counts = {}
    count_line_regex = re.compile(r"char '(.?)' : (\d+) times")
    for line in server_output.splitlines():
        match = count_line_regex.match(line)
        if match is None:
            pytest.fail(f"unexpected server output line: {line}")
        counts[match.group(1)] = int(match.group(2))
    expected_counts = {
        chr(char): file_content.count(char)
        for char in set(file_content)
        if chr(char) in string.printable and char >= 32
    }
    expected_counts.update({char: 0 for char in counts if char not in expected_counts})
    assert counts == expected_counts, "server character counts mismatch"


def get_random_file(size: int):
    return bytes(random.choices(range(256), k=size))


@pytest.fixture
def server_instance(port):
    server = start_server("server", port)
    yield server
    server.kill()


@pytest.mark.parametrize(
    "port,msg",
    [
        pytest.param(9993, b"Hello^", id="simple text file"),
        pytest.param(9996, get_random_file(2048), id="arbitrary file"),
        pytest.param(9992, get_random_file(10 * 2**20), id="10MB arbitrary file"),
        pytest.param(9992, get_random_file(50 * 2**20), id="50MB arbitrary file"),
    ],
)
def test_server_happy_flow(server_instance, port, msg):
    time.sleep(0.5)
    sock = socket.create_connection(("127.0.0.1", port), timeout=30)
    size = len(msg).to_bytes(4, "big", signed=False)
    sock.sendall(size + msg)
    num_printable = int.from_bytes(sock.recv(4), byteorder="big", signed=False)
    sock.close()
    assert num_printable == sum(
        1 for char in msg if chr(char) in string.printable and char >= 32
    ), "number of printable characters mismatch"
    server_instance.send_signal(subprocess.signal.SIGINT)
    server_instance.wait()
    assert server_instance.stderr.read() == b""
    validate_server_counts(server_instance.stdout.read().decode(), msg)


@pytest.mark.parametrize(
    "port,msg",
    [
        pytest.param(9990, get_random_file(50 * 2**20), id="50MB file"),
    ],
)
def test_server_memory_usage(server_instance, port, msg):
    psutil = pytest.importorskip("psutil")
    time.sleep(0.5)
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    size = len(msg).to_bytes(4, "big", signed=False)
    sock.sendall(size + msg)
    sock.close()
    proc_info = psutil.Process(server_instance.pid)
    if (memory_used := proc_info.memory_full_info().uss) > 2 * 2**20:
        pytest.fail(f"Server using too much memory ({memory_used / 2**20:,.2}MB)")


@pytest.mark.parametrize(
    "port",
    [
        pytest.param(9991, id="premature client disconnect"),
    ],
)
def test_server_connection_error_handling(server_instance, port):
    time.sleep(0.5)
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    sock.sendall((50 * 2**20).to_bytes(4, "big", signed=False))
    sock.sendall(b"partial")
    sock.close()
    time.sleep(0.5)
    assert server_instance.poll() is None, "server terminated unexpectedly"
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    sock.sendall((3).to_bytes(4, "big", signed=False))
    sock.sendall(b"$$$")
    sock.close()
    server_instance.send_signal(subprocess.signal.SIGINT)
    server_instance.wait()
    validate_server_counts(server_instance.stdout.read().decode(), b"$$$")


@pytest.mark.parametrize(
    "port",
    [
        pytest.param(9991, id="SIGINT handling delayed until client processed"),
    ],
)
def test_server_sigint_client_atomicity(server_instance, port):
    time.sleep(0.5)
    msg = b"quod erat demonstrandum"
    sock = socket.create_connection(("127.0.0.1", port), timeout=3)
    sock.sendall(len(msg).to_bytes(4, "big", signed=False))
    sock.sendall(msg[:5])
    server_instance.send_signal(subprocess.signal.SIGINT)
    time.sleep(0.2)
    assert server_instance.poll() is None, "server terminated unexpectedly"
    sock.sendall(msg[5:])
    with pytest.raises(ConnectionError):
        another_sock = socket.create_connection(("127.0.0.1", port), timeout=3)
        another_sock.sendall(len(msg).to_bytes(4, "big", signed=False))
        another_sock.sendall(msg)
        another_sock.close()
    sock.close()
    server_instance.wait()
    validate_server_counts(server_instance.stdout.read().decode(), msg)


@pytest.fixture
def mock_server(port):
    sock = socket.create_server(("127.0.0.1", port), reuse_port=True)
    sock.listen(10)

    def connection_handler():
        con, _ = sock.accept()
        num_bytes = int.from_bytes(con.recv(4), byteorder="big", signed=False)
        msg = con.makefile("rb").read(num_bytes)
        num_printable = sum(
            1 for char in msg if chr(char) in string.printable and char >= 32
        )
        con.sendall(num_printable.to_bytes(4, "big", signed=False))
        con.close()

    con_handler = threading.Thread(target=connection_handler)
    con_handler.start()
    time.sleep(1)
    yield
    sock.close()


@pytest.mark.parametrize(
    "port,msg",
    [
        pytest.param(8999, b"Hello^", id="simple text file"),
        pytest.param(8998, get_random_file(2048), id="arbitrary file"),
        pytest.param(9999, get_random_file(13 * 2**20), id="13MB arbitrary file"),
    ],
)
def test_client_happy_flow(port, msg, mock_server):

    with NamedTemporaryFile() as f:
        f.write(msg)
        f.flush()
        res = run_client("client", "127.0.0.1", port, f.name)
    assert res.stderr == "", "client stderr is not empty"
    assert (
        res.stdout
        == f"# of printable characters: {sum(1 for char in msg if chr(char) in string.printable and char >= 32)}\n"
    ), "client stdout is not as expected"


@pytest.mark.parametrize(
    "port,msg",
    [
        pytest.param(8999, b"Hello^", id="simple text file"),
        pytest.param(8998, get_random_file(2048), id="arbitrary file"),
        pytest.param(9999, get_random_file(14 * 2**20), id="14MB arbitrary file"),
    ],
)
def test_integration(server_instance, port, msg):
    time.sleep(0.5)
    with NamedTemporaryFile() as f:
        f.write(msg)
        f.flush()
        res = run_client("client", "127.0.0.1", port, f.name)
    assert res.stderr == "", "client stderr is not empty"
    assert (
        res.stdout
        == f"# of printable characters: {sum(1 for char in msg if chr(char) in string.printable and char >= 32)}\n"
    ), "client stdout is not as expected"
    server_instance.send_signal(subprocess.signal.SIGINT)
    server_instance.wait()
    assert server_instance.stderr.read() == b""
    validate_server_counts(server_instance.stdout.read().decode(), msg)