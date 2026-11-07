#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <string>
#include <vector>
#include <sstream>
#include <iostream>

using namespace std;

enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

typedef vector<uint8_t> Buffer;

static void buf_append(Buffer &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_append_u32(Buffer &buf, uint32_t data) {
    buf_append(buf, (const uint8_t *)&data, 4);
}

static bool buf_read_u32(
    const uint8_t *&cur,
    const uint8_t *end,
    uint32_t &out
) {
    if (cur + 4 > end) return false;

    memcpy(&out, cur, 4);
    cur += 4;

    return true;
}

static bool buf_read_i64(
    const uint8_t *&cur,
    const uint8_t *end,
    int64_t &out
) {
    if (cur + 8 > end) return false;

    memcpy(&out, cur, 8);
    cur += 8;

    return true;
}

static bool buf_read_dbl(
    const uint8_t *&cur,
    const uint8_t *end,
    double &out
) {
    if (cur + 8 > end) return false;

    memcpy(&out, cur, 8);
    cur += 8;

    return true;
}

static bool buf_read_str(
    const uint8_t *&cur,
    const uint8_t *end,
    size_t n,
    string &out
) {
    if (cur + n > end) return false;

    out.assign((const char *)cur, n);
    cur += n;

    return true;
}


// ------------------------------------------------------------
// Encode request
// ------------------------------------------------------------

static void encode_request(
    Buffer &req,
    const vector<string> &args
) {
    buf_append_u32(req, (uint32_t)args.size());

    for (const string &arg : args) {
        uint32_t len = (uint32_t)arg.size();

        buf_append_u32(req, len);

        buf_append(
            req,
            (const uint8_t *)arg.data(),
            arg.size()
        );
    }
}


// ------------------------------------------------------------
// Parse response
// ------------------------------------------------------------

static bool parse_response(
    const uint8_t *data,
    size_t size
) {
    const uint8_t *cur = data;
    const uint8_t *end = data + size;

    if (cur >= end) {
        fprintf(stderr, "Empty response\n");
        return false;
    }

    uint8_t type = *cur++;

    switch (type) {

        case TAG_NIL:
            printf("(nil)\n");
            return true;


        case TAG_ERR: {
            uint32_t code = 0;
            uint32_t msg_len = 0;

            if (!buf_read_u32(cur, end, code) ||
                !buf_read_u32(cur, end, msg_len)) {

                fprintf(stderr, "Failed to read error\n");
                return false;
            }

            string msg;

            if (!buf_read_str(cur, end, msg_len,  msg)) {
                fprintf(stderr, "Failed to read error message\n");
                return false;
            }

            printf("(error) [%u] %s\n",
                   code,
                   msg.c_str());

            return true;
        }


        case TAG_STR: {
            uint32_t len = 0;

            if (!buf_read_u32(cur, end, len)) {
                fprintf(stderr, "Failed to read string length\n");
                return false;
            }

            string str;

            if (!buf_read_str(cur, end, len, str)) {
                fprintf(stderr, "Failed to read string\n");
                return false;
            }

            printf("\"%s\"\n", str.c_str());

            return true;
        }


        case TAG_INT: {
            int64_t val = 0;

            if (!buf_read_i64(cur, end, val)) {
                fprintf(stderr, "Failed to read int\n");
                return false;
            }

            printf("(integer) %ld\n", val);

            return true;
        }


        case TAG_DBL: {
            double val = 0;

            if (!buf_read_dbl(cur, end, val)) {
                fprintf(stderr, "Failed to read double\n");
                return false;
            }

            printf("(double) %.10g\n", val);

            return true;
        }


        case TAG_ARR: {
            uint32_t n = 0;

            if (!buf_read_u32(cur, end, n)) {
                fprintf(stderr, "Failed to read array length\n");
                return false;
            }

            printf("(array) %u items:\n", n);

            /*
             * For now this prints the elements recursively.
             */
            for (uint32_t i = 0; i < n; i++) {

                printf("  [%u] ", i);

                if (!parse_response(cur, end - cur)) {
                    return false;
                }
            }

            return true;
        }


        default:
            printf("Unknown response type: %d\n", type);
            return false;
    }
}


// ------------------------------------------------------------
// Send one command
// ------------------------------------------------------------

static bool send_command(
    int sock,
    const vector<string> &args
) {
    /*
     * Request format:
     *
     * [4 bytes message length]
     * [4 bytes argc]
     * [4 bytes arg length]
     * [arg bytes]
     * [4 bytes arg length]
     * [arg bytes]
     * ...
     */

    Buffer req;

    // Reserve first 4 bytes for message length
    buf_append_u32(req, 0);

    size_t msg_start = req.size();

    // Encode command
    encode_request(req, args);

    // Calculate message length
    uint32_t msg_len =
        (uint32_t)(req.size() - msg_start);

    // Fill first 4 bytes
    memcpy(&req[0], &msg_len, 4);


    // --------------------------------------------------------
    // Send entire request
    // --------------------------------------------------------

    size_t total_sent = 0;

    while (total_sent < req.size()) {

        ssize_t n = write(
            sock,
            req.data() + total_sent,
            req.size() - total_sent
        );

        if (n < 0) {

            if (errno == EINTR)
                continue;

            perror("write");
            return false;
        }

        if (n == 0)
            return false;

        total_sent += n;
    }


    // --------------------------------------------------------
    // Read response length
    // --------------------------------------------------------

    uint8_t len_buf[4];

    size_t total_read = 0;

    while (total_read < 4) {

        ssize_t n = read(
            sock,
            len_buf + total_read,
            4 - total_read
        );

        if (n < 0) {

            if (errno == EINTR)
                continue;

            perror("read");
            return false;
        }

        if (n == 0) {
            fprintf(stderr, "Server closed connection\n");
            return false;
        }

        total_read += n;
    }


    uint32_t resp_len = 0;

    memcpy(&resp_len, len_buf, 4);


    if (resp_len > (32u << 20)) {

        fprintf(
            stderr,
            "Response too large: %u\n",
            resp_len
        );

        return false;
    }


    // --------------------------------------------------------
    // Read response body
    // --------------------------------------------------------

    Buffer resp(resp_len);

    total_read = 0;

    while (total_read < resp_len) {

        ssize_t n = read(
            sock,
            resp.data() + total_read,
            resp_len - total_read
        );

        if (n < 0) {

            if (errno == EINTR)
                continue;

            perror("read");
            return false;
        }

        if (n == 0) {
            fprintf(
                stderr,
                "Server closed connection\n"
            );

            return false;
        }

        total_read += n;
    }


    // --------------------------------------------------------
    // Parse response
    // --------------------------------------------------------

    return parse_response(
        resp.data(),
        resp.size()
    );
}


// ------------------------------------------------------------
// Split command line
// ------------------------------------------------------------

static vector<string> split_command(
    const string &line
) {
    vector<string> args;

    istringstream iss(line);

    string word;

    while (iss >> word) {
        args.push_back(word);
    }

    return args;
}


// ------------------------------------------------------------
// Main
// ------------------------------------------------------------

int main() {

    // --------------------------------------------------------
    // Create socket
    // --------------------------------------------------------

    int sock = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (sock < 0) {
        perror("socket");
        return 1;
    }


    // --------------------------------------------------------
    // Server address
    // --------------------------------------------------------

    struct sockaddr_in addr = {};

    addr.sin_family = AF_INET;

    addr.sin_port = htons(6379);

    addr.sin_addr.s_addr =
        inet_addr("127.0.0.1");


    // --------------------------------------------------------
    // Connect
    // --------------------------------------------------------

    if (connect(
            sock,
            (struct sockaddr *)&addr,
            sizeof(addr)
        ) < 0) {

        perror("connect");

        close(sock);

        return 1;
    }


    printf("Connected to Redis server at 127.0.0.1:1234\n");
    printf("Type 'exit' or 'quit' to close.\n");
    printf("Type 'help' for commands.\n\n");


    // --------------------------------------------------------
    // Interactive shell
    // --------------------------------------------------------

    string line;

    while (true) {

        printf("redis> ");
        fflush(stdout);

        if (!getline(cin, line))
            break;


        // Empty line
        if (line.empty())
            continue;


        // Exit
        if (line == "exit" ||
            line == "quit") {

            break;
        }


        // Help
        if (line == "help") {

            printf(
                "Commands:\n"
                "  set key value\n"
                "  get key\n"
                "  del key\n"
                "  keys\n"
                "  pexpire key ms\n"
                "  pttl key\n"
                "  zadd zset score member\n"
                "  zscore zset member\n"
                "  zrem zset member\n"
                "  zquery zset score name offset limit\n"
                "  exit\n"
            );

            continue;
        }


        // Parse command
        vector<string> args =
            split_command(line);


        if (args.empty())
            continue;


        // Send to server
        if (!send_command(sock, args)) {

            fprintf(
                stderr,
                "Connection to server lost.\n"
            );

            break;
        }
    }


    close(sock);

    printf("Disconnected.\n");

    return 0;
}