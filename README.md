# Remote Shell in C

Demo: https://youtu.be/syqsO8UERHo

**Requirements**
1) concurrent handling multiple client connections.
2) receives commands from client, executes and sends results to client.
3) process forking and exec calls.
4) stdin, stdout, stderr redirection.
5) must work on macOS, Linux, FreeBSD
6) works with GCC and Clang compilers

**Running project**
1) ./generate-cmakelists.sh
2) ./change-compiler.sh -c [gcc or clang]
3) ./build.sh

_Server_
1) ./server [ip addr] [port]

_Client_
1) ./client [ip addr] [port]

