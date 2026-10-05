#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <fcntl.h>

#define CHECK(c, n) do { if (!(c)) { printf("network check %d failed: %d\n", n, errno); return n; } } while (0)
static int listener;
static void* server(void* unused) {
    int fd = accept4(listener, 0, 0, SOCK_CLOEXEC);
    if (fd < 0) return (void*)1;
    char data[4];
    if (recv(fd, data, sizeof(data), MSG_WAITALL) != 4 || memcmp(data, "ping", 4)) return (void*)2;
    struct iovec buffers[] = {{"po", 2}, {"ng", 2}};
    struct msghdr message = {.msg_iov = buffers, .msg_iovlen = 2};
    if (sendmsg(fd, &message, 0) != 4) return (void*)3;
    shutdown(fd, SHUT_WR); close(fd); return 0;
}
int main(void) {
    struct addrinfo* hosts = 0;
    CHECK(!getaddrinfo("localhost", 0, 0, &hosts) && hosts, 1);
    freeaddrinfo(hosts);
    listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0, 2);
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    CHECK(!bind(listener, (struct sockaddr*)&address, sizeof(address)) && !listen(listener, 4), 3);
    socklen_t length = sizeof(address);
    CHECK(!getsockname(listener, (struct sockaddr*)&address, &length) && address.sin_port, 4);
    pthread_t worker;
    CHECK(!pthread_create(&worker, 0, server, 0), 5);
    int client = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(client >= 0 && !connect(client, (struct sockaddr*)&address, sizeof(address)), 6);
    CHECK(send(client, "ping", 4, 0) == 4, 7);
    char response[4] = {};
    struct iovec buffers[] = {{response, 2}, {response + 2, 2}};
    struct msghdr message = {.msg_iov = buffers, .msg_iovlen = 2};
    CHECK(recvmsg(client, &message, 0) == 4 && !memcmp(response, "pong", 4), 8);
    CHECK(recv(client, response, 4, 0) == 0, 9);
    void* result;
    CHECK(!pthread_join(worker, &result) && !result, 10);
    close(client); close(listener);
    int receiver = socket(AF_INET, SOCK_DGRAM, 0), sender = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(receiver >= 0 && sender >= 0, 11);
    int receive_errors = 1;
    CHECK(!setsockopt(sender, IPPROTO_IP, IP_RECVERR, &receive_errors, sizeof(receive_errors)), 21);
    address.sin_port = 0;
    CHECK(!bind(receiver, (struct sockaddr*)&address, sizeof(address)) &&
          !getsockname(receiver, (struct sockaddr*)&address, &length), 12);
    CHECK(sendto(sender, "udp", 3, 0, (struct sockaddr*)&address, length) == 3, 13);
    CHECK(recvfrom(receiver, response, sizeof(response), 0, (struct sockaddr*)&address, &length) == 3 &&
          !memcmp(response, "udp", 3), 14);
    CHECK(!getsockname(receiver, (struct sockaddr*)&address, &length), 15);
    struct iovec batch_data[] = {{"one", 3}, {"two", 3}};
    struct mmsghdr batch[2] = {0};
    for (int i = 0; i < 2; ++i) {
        batch[i].msg_hdr.msg_name = &address; batch[i].msg_hdr.msg_namelen = length;
        batch[i].msg_hdr.msg_iov = &batch_data[i]; batch[i].msg_hdr.msg_iovlen = 1;
    }
    CHECK(sendmmsg(sender, batch, 2, 0) == 2 && batch[0].msg_len == 3 && batch[1].msg_len == 3, 16);
    CHECK(recv(receiver, response, sizeof(response), 0) == 3 && !memcmp(response, "one", 3), 17);
    CHECK(recv(receiver, response, sizeof(response), 0) == 3 && !memcmp(response, "two", 3), 18);
    batch_data[1].iov_base = (void*)1;
    CHECK(sendmmsg(sender, batch, 2, 0) == 1, 19);
    CHECK(recv(receiver, response, sizeof(response), 0) == 3 && !memcmp(response, "one", 3), 20);
    close(receiver); close(sender);
    int wake = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(wake >= 0 && fcntl(wake, F_GETFD) == FD_CLOEXEC, 22);
    eventfd_t count = 0;
    CHECK(eventfd_read(wake, &count) == -1 && errno == EAGAIN, 23);
    CHECK(!eventfd_write(wake, 7), 24);
    struct pollfd ready = {.fd = wake, .events = POLLIN};
    CHECK(poll(&ready, 1, 0) == 1 && (ready.revents & POLLIN) && !eventfd_read(wake, &count) && count == 7, 25);
    CHECK(eventfd_write(wake, UINT64_MAX) == -1 && errno == EINVAL, 26);
    close(wake);
    wake = eventfd(2, EFD_SEMAPHORE | EFD_NONBLOCK);
    CHECK(wake >= 0 && !eventfd_read(wake, &count) && count == 1 && !eventfd_read(wake, &count) && count == 1, 27);
    close(wake);
    puts("TCP, UDP, socket vectors and localhost resolution ok");
    return 0;
}
