#include <string>

#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>

#include <sqlite3.h>

#include "d3des.h"
#include "utils.h"
#include "marshal.h"
#include "polling.h"

using namespace std;
using namespace rpc;

// make sure that writes to fd1 will be read from fd2, and vice versa
void tie_fd(PollMgr* poll, int fd1, int fd2) {
    class EndPoint: public Pollable {
        PollMgr* poll_;
        int fd_;
        EndPoint* peer_;
        bool closed_;

        // hold data sent from peer
        Marshal buf_;
        // guard my buf_
        pthread_mutex_t m_;
    public:
        EndPoint(PollMgr* pmgr, int fd)
                : poll_(pmgr), fd_(fd), peer_(NULL), closed_(false) {
            Pthread_mutex_init(&m_, NULL);
        }

        ~EndPoint() {
            Pthread_mutex_destroy(&m_);
        }

        void tie(EndPoint* o) {
            this->peer_ = o;
            o->peer_ = this;
        }

        void handle_read() {
            Pthread_mutex_lock(&peer_->m_);
            int cnt = peer_->buf_.read_from_fd(fd_);
            if (cnt > 0) {
                poll_->update_mode(peer_, Pollable::READ | Pollable::WRITE);
            }
            Pthread_mutex_unlock(&peer_->m_);
            //Log::debug("read (fd=%d): cnt=%d", fd_, cnt);
        }

        void handle_write() {
            Pthread_mutex_lock(&m_);
            int cnt = buf_.write_to_fd(fd_);
            if (buf_.content_size_gt(0)) {
                poll_->update_mode(this, Pollable::READ | Pollable::WRITE);
            } else {
                poll_->update_mode(this, Pollable::READ);
            }
            Pthread_mutex_unlock(&m_);
            //Log::debug("write (fd=%d): cnt=%d", fd_, cnt);
        }

        int fd() {
            return fd_;
        }

        void handle_error() {
            //Log::error("error: fd=%d", fd_);
            close(fd_);
            poll_->remove(this);
            this->closed_ = true;
            if (!peer_->closed_) {
                peer_->handle_error();
            }
        }

        int poll_mode() {
            return Pollable::READ | Pollable::WRITE;
        }
    };

    EndPoint* ep1 = new EndPoint(poll, fd1);
    EndPoint* ep2 = new EndPoint(poll, fd2);

    ep1->tie(ep2);

    poll->add(ep1);
    poll->add(ep2);
}

int connect_to(const char* addr) {
    int sock;
    string addr_str(addr);
    int idx = addr_str.find(":");
    if (idx == string::npos) {
        Log::error("rpc::Client: bad connect address: %s", addr);
        errno = EINVAL;
        return -1;
    }
    string host = addr_str.substr(0, idx);
    string port = addr_str.substr(idx + 1);

    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_family = AF_INET; // ipv4
    hints.ai_socktype = SOCK_STREAM; // tcp

    int r = getaddrinfo(host.c_str(), port.c_str(), &hints, &result);
    if (r != 0) {
        Log::error("rpc::Client: getaddrinfo(): %s", gai_strerror(r));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == -1) {
            continue;
        }

        const int yes = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (::connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        ::close(sock);
        sock = -1;
    }
    freeaddrinfo(result);

    if (rp == NULL) {
        // failed to connect
        Log::error("rpc::Client: connect(): %s", strerror(errno));
        return -1;
    }

    verify(set_nonblocking(sock, true) == 0);
    //Log::info("rpc::Client: connected to %s", addr);

    return sock;
}

int main(int argc, char* argv[]) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGHUP, SIG_IGN);

    const char* bind_addr = "127.0.0.1:1987";
    const char* forward_to = "kernel.org:80";

    int server_sock;

    string addr(bind_addr);
    int idx = addr.find(":");
    if (idx == string::npos) {
        Log::error("rpc::Server: bad bind address: %s", bind_addr);
        errno = EINVAL;
        return -1;
    }
    string host = addr.substr(0, idx);
    string port = addr.substr(idx + 1);

    struct addrinfo hints, *result, *rp;
    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_family = AF_INET; // ipv4
    hints.ai_socktype = SOCK_STREAM; // tcp
    hints.ai_flags = AI_PASSIVE; // server side

    int r = getaddrinfo((host == "0.0.0.0") ? NULL : host.c_str(), port.c_str(), &hints, &result);
    if (r != 0) {
        Log::error("rpc::Server: getaddrinfo(): %s", gai_strerror(r));
        return -1;
    }

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        server_sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (server_sock == -1) {
            continue;
        }

        const int yes = 1;
        setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        if (bind(server_sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            break;
        }
        close(server_sock);
        server_sock = -1;
    }

    if (rp == NULL) {
        // failed to bind
        Log::error("rpc::Server: bind(): %s", strerror(errno));
        freeaddrinfo(result);
        return -1;
    }

    // about backlog: http://www.linuxjournal.com/files/linuxjournal.com/linuxjournal/articles/023/2333/2333s2.html
    const int backlog = SOMAXCONN;
    verify(listen(server_sock, backlog) == 0);
    verify(set_nonblocking(server_sock, true) == 0);

    PollMgr* poll = new PollMgr;

    bool stop_flag = false;
    fd_set fds;
    while (stop_flag == false) {
        FD_ZERO(&fds);
        FD_SET(server_sock, &fds);

        // use select to avoid waiting on accept when closing server
        timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 50 * 1000; // 0.05 sec
        int fdmax = server_sock;

        int n_ready = select(fdmax + 1, &fds, NULL, NULL, &tv);
        if (n_ready == 0) {
            continue;
        }
        if (stop_flag) {
            break;
        }

        int clnt_socket = accept(server_sock, rp->ai_addr, &rp->ai_addrlen);
        if (clnt_socket >= 0) {
            //Log::debug("rpc::Server: got new client, fd=%d", clnt_socket);
            verify(set_nonblocking(clnt_socket, true) == 0);

            int remote_fd = connect_to(forward_to);

            // TODO check for vnc auth info, and then connect to target machine
            tie_fd(poll, clnt_socket, remote_fd);
        }
    }

    poll->release();

    freeaddrinfo(result);

    return 0;
}
