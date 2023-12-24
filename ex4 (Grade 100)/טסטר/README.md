# OS HW 4 Tester
## Setup
1. Make sure you have python3 and pip3 installed:
```bash
sudo apt update && sudo apt install python3 python3-pip
```
2. Make sure you have pytest installed:
```bash
pip3 install pytest
```
3. (Optional) If you want the memory leaks test to run, make sure you have valgrind installed:
```bash
sudo apt install valgrind
```
4. Download the test script to your project's directory.
## Runnning the Test
```bash
python3 -m pytest --verbosity=1
```