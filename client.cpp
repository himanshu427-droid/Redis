// client.cpp
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

// Response types
enum {
    TAG_NIL = 0,
    TAG_ERR = 1,
    TAG_STR = 2,
    TAG_INT = 3,
    TAG_DBL = 4,
    TAG_ARR = 5,
};

typedef std::vector<uint8_t> Buffer;

static void buf_append(Buffer &buf, const uint8_t *data, size_t len) {
    buf.insert(buf.end(), data, data + len);
}

static void buf_append_u32(Buffer &buf, uint32_t data) {
    buf_append(buf, (const uint8_t *)&data, 4);
}

static bool buf_read_u32(const uint8_t *&cur, const uint8_t *end, uint32_t &out) {
    if (cur + 4 > end) return false;
    memcpy(&out, cur, 4);
    cur += 4;
    return true;
}

static bool buf_read_i64(const uint8_t *&cur, const uint8_t *end, int64_t &out) {
    if (cur + 8 > end) return false;
    memcpy(&out, cur, 8);
    cur += 8;
    return true;
}

static bool buf_read_dbl(const uint8_t *&cur, const uint8_t *end, double &out) {
    if (cur + 8 > end) return false;
    memcpy(&out, cur, 8);
    cur += 8;
    return true;
}

static bool buf_read_str(const uint8_t *&cur, const uint8_t *end, size_t n, std::string &out) {
    if (cur + n > end) return false;
    out.assign(cur, cur + n);
    cur += n;
    return true;
}

// Encode request
static void encode_request(Buffer &req, int argc, char *argv[]) {
    buf_append_u32(req, (uint32_t)argc);
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        buf_append_u32(req, (uint32_t)len);
        buf_append(req, (const uint8_t *)argv[i], len);
    }
}

// Parse response
static void parse_response(const uint8_t *data, size_t size) {
    const uint8_t *cur = data;
    const uint8_t *end = data + size;
    
    if (cur >= end) {
        fprintf(stderr, "Empty response\n");
        return;
    }
    
    uint8_t type = *cur++;
    
    switch (type) {
        case TAG_NIL:
            printf("(nil)\n");
            break;
        
        case TAG_ERR: {
            uint32_t code = 0, msg_len = 0;
            if (!buf_read_u32(cur, end, code) || !buf_read_u32(cur, end, msg_len)) {
                fprintf(stderr, "Failed to read error\n");
                return;
            }
            std::string msg;
            if (!buf_read_str(cur, end, msg_len, msg)) {
                fprintf(stderr, "Failed to read error message\n");
                return;
            }
            printf("(error) [%u] %s\n", code, msg.c_str());
            break;
        }
        
        case TAG_STR: {
            uint32_t len = 0;
            if (!buf_read_u32(cur, end, len)) {
                fprintf(stderr, "Failed to read string length\n");
                return;
            }
            std::string str;
            if (!buf_read_str(cur, end, len, str)) {
                fprintf(stderr, "Failed to read string\n");
                return;
            }
            printf("\"%s\"\n", str.c_str());
            break;
        }
        
        case TAG_INT: {
            int64_t val = 0;
            if (!buf_read_i64(cur, end, val)) {
                fprintf(stderr, "Failed to read int\n");
                return;
            }
            printf("(integer) %ld\n", val);
            break;
        }
        
        case TAG_DBL: {
            double val = 0;
            if (!buf_read_dbl(cur, end, val)) {
                fprintf(stderr, "Failed to read double\n");
                return;
            }
            printf("(double) %.10g\n", val);
            break;
        }
        
        case TAG_ARR: {
            uint32_t n = 0;
            if (!buf_read_u32(cur, end, n)) {
                fprintf(stderr, "Failed to read array length\n");
                return;
            }
            printf("(array) %u items:\n", n);
            for (uint32_t i = 0; i < n; i++) {
                printf("  [%u] ", i);
                parse_response(cur, end - cur);
                // This is simplified; in production you'd need recursive parsing
            }
            break;
        }
        
        default:
            printf("Unknown response type: %d\n", type);
            break;
    }
    
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <command> [args...]\n", argv[0]);
        printf("Examples:\n");
        printf("  %s get mykey\n", argv[0]);
        printf("  %s set mykey myvalue\n", argv[0]);
        printf("  %s del mykey\n", argv[0]);
        printf("  %s keys\n", argv[0]);
        printf("  %s zadd myzset 10.5 member1\n", argv[0]);
        printf("  %s zscore myzset member1\n", argv[0]);
        return 1;
    }
    
    // Create socket
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    
    // Connect to server
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(1234);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sock);
        return 1;
    }
    
    // Encode request
    Buffer req;
    buf_append_u32(req, 0);  // Reserve space for message length
    size_t msg_start = req.size();
    encode_request(req, argc - 1, argv + 1);
    
    // Fill in message length
    uint32_t msg_len = (uint32_t)(req.size() - msg_start);
    memcpy(&req[0], &msg_len, 4);
    
    // Send request
    if (write(sock, req.data(), req.size()) < 0) {
        perror("write");
        close(sock);
        return 1;
    }
    
    // Read response
    uint8_t len_buf[4];
    if (read(sock, len_buf, 4) < 4) {
        fprintf(stderr, "Failed to read response length\n");
        close(sock);
        return 1;
    }
    
    uint32_t resp_len = 0;
    memcpy(&resp_len, len_buf, 4);
    
    if (resp_len > 32 << 20) {
        fprintf(stderr, "Response too large\n");
        close(sock);
        return 1;
    }
    
    Buffer resp(resp_len);
    size_t total_read = 0;
    while (total_read < resp_len) {
        ssize_t n = read(sock, &resp[total_read], resp_len - total_read);
        if (n <= 0) break;
        total_read += n;
    }
    
    if (total_read < resp_len) {
        fprintf(stderr, "Failed to read full response\n");
        close(sock);
        return 1;
    }
    
    // Parse and display response
    parse_response(resp.data(), resp.size());
    
    close(sock);
    return 0;
}