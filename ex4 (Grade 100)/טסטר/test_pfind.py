import os
import string
import subprocess
import time
from glob import glob
from random import choices, randint, seed
from tempfile import TemporaryDirectory

import pytest

seed(97)


def rand_str(length: int):
    return "".join(choices(string.ascii_letters, k=length))


def populate_dir_tree(base_path: str, max_depth: int = 6):
    def __populate_dir_tree_rec(base_path: str, depth: int):
        for i in range(randint(1, 10)):
            term = "" if randint(0, 1) else "freedom"
            filename = f"{rand_str(5)}_{term}_file{i}"
            with open(os.path.join(base_path, filename), "w") as f:
                f.write("@")
        for i in range(randint(1, 10)):
            dirname = f"{rand_str(5)}_dir{i}"
            dirpath = os.path.join(base_path, f"{dirname}_dir{i}")
            os.mkdir(dirpath)
            if randint(0, 1) and depth < max_depth:
                __populate_dir_tree_rec(dirpath, depth + 1)

    return __populate_dir_tree_rec(base_path, 0)


@pytest.fixture
def dir_tree():
    td = TemporaryDirectory()
    populate_dir_tree(td.name)
    yield td.name
    td.cleanup()


@pytest.fixture(autouse=True)
def build_pfind():
    subprocess.run(
        "gcc -g -O3 -D_POSIX_C_SOURCE=200809 -Wall -std=c11 -pthread pfind.c".split(
            " "
        ),
        capture_output=True,
        text=True,
    ).check_returncode()
    yield
    os.remove("./a.out")


def run_pfind(dirpath: str, term: str, num_threads: int):
    res = subprocess.run(
        ["./a.out", dirpath, term, str(num_threads)], capture_output=True, text=True
    )
    return res


@pytest.mark.parametrize(
    "num_threads",
    [
        pytest.param(i, id=f"{i} thread{'s' if i > 1 else ''}")
        for i in [1, 2, 4, 8, 16, 32, 64]
    ],
)
def test_happy_flow(dir_tree, num_threads):
    expected_results = glob(f"{dir_tree}/**/*freedom*", recursive=True)
    expected_results.append(f"Done searching, found {len(expected_results)} files")
    res = run_pfind(dir_tree, "freedom", num_threads)
    res.check_returncode()
    actual_results = res.stdout.splitlines()
    assert set(expected_results) == set(actual_results)


def test_inner_dir_access_error(dir_tree):
    dirpath = os.path.join(dir_tree, "inaccessible_dir")
    os.mkdir(dirpath)
    os.chmod(dirpath, 0o000)
    res = run_pfind(dir_tree, "freedom", 5)
    res.check_returncode()
    results = res.stdout.splitlines()
    exp_line = f"Directory {dirpath}: Permission denied."
    assert exp_line in results
    results.remove(exp_line)
    expected_results = glob(f"{dir_tree}/**/*freedom*", recursive=True)
    expected_results.append(f"Done searching, found {len(expected_results)} files")
    assert set(results) == set(expected_results)


def test_root_dir_access_error(dir_tree):
    os.chmod(dir_tree, 0o000)
    with pytest.raises(
        subprocess.CalledProcessError, match="returned non-zero exit status 1"
    ):
        res = run_pfind(dir_tree, "freedom", 5)
        res.check_returncode()


def test_file_passed_as_root_dir_error(dir_tree):
    with open(os.path.join(dir_tree, "testfile.txt"), "w") as f:
        f.write("liberty")
    with pytest.raises(
        subprocess.CalledProcessError, match="returned non-zero exit status 1"
    ):
        res = run_pfind(os.path.join(dir_tree, "testfile.txt"), "freedom", 5)
        res.check_returncode()


def test_performance_gain(dir_tree):
    num_trials = 10
    single_thread_time = 0
    multi_thread_time = 0
    for _ in range(num_trials):
        t0 = time.perf_counter()
        res = run_pfind(dir_tree, "freedom", 1)
        res.check_returncode()
        running_time = time.perf_counter() - t0
        single_thread_time += running_time
        t0 = time.perf_counter()
        res = run_pfind(dir_tree, "freedom", 2)
        res.check_returncode()
        running_time = time.perf_counter() - t0
        multi_thread_time += running_time
    single_thread_time /= num_trials
    multi_thread_time /= num_trials
    assert single_thread_time > multi_thread_time, (
        f"Two-threaded performance ({multi_thread_time:.3f}s) is "
        f"worse than single-threaded ({single_thread_time:.3f}s), suggesting "
        "your program is effectively sequntial."
    )


def test_memory_leaks(dir_tree):
    valgrind_res = subprocess.run(
        f"valgrind --track-origins=yes --leak-check=full --error-exitcode=1 ./a.out {dir_tree} freedom 5".split(
            " "
        ),
        capture_output=True,
        text=True,
    )
    if valgrind_res.returncode == 127:
        pytest.skip("Valgrind not installed, skipping memory leak test")
    assert (
        valgrind_res.returncode == 0
    ), f"Memory leaks detected:\n{valgrind_res.stderr}"
