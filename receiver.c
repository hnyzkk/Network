// receiver.c - TCP 혼잡제어 데모용 수신자
// 사용법:
//   clang -O2 -Wall -Wextra -o receiver receiver.c
//   ./receiver <listen_port> <normal|dup3|timeout>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MSS 1500
#define BUF 256
#define SLEEP_US 1500000

typedef enum { MODE_NORMAL, MODE_DUP3, MODE_TIMEOUT } Mode;

void die(const char* msg) {
    perror(msg);
    exit(1);
}

Mode parse_mode(const char* s) {
    if (strcmp(s, "normal") == 0)  return MODE_NORMAL;
    if (strcmp(s, "dup3") == 0)    return MODE_DUP3;
    if (strcmp(s, "timeout") == 0) return MODE_TIMEOUT;
    fprintf(stderr, "unknown mode: %s (use normal|dup3|timeout)\n", s);
    exit(1);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <listen_port> <normal|dup3|timeout>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    Mode mode = parse_mode(argv[2]);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) die("socket");

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family      = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port        = htons(port);

    if (bind(s, (struct sockaddr*)&me, sizeof(me)) < 0) die("bind");

    printf("=== [RCV] Receiver on port %d, mode=%s ===\n",
           port,
           mode == MODE_NORMAL ? "normal" :
           mode == MODE_DUP3   ? "dup3"   : "timeout");

    int next_expected = 0;
    int dup_drop_count = 0;     // dup3 모드에서 손실/중복 처리용
    int timeout_drop_count = 0; // timeout 모드에서 손실 처리용

    while (1) {
        char buf[BUF];
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);

        int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr*)&cli, &clen);
        if (n < 0) die("recvfrom");
        buf[n] = 0;

        // 종료 메시지
        if (strncmp(buf, "END", 3) == 0) {
            printf("\n=== [RCV] END 수신, 시나리오 종료 ===\n");
            break;
        }

        int seq = -1, len = -1;
        if (sscanf(buf, "DATA seq=%d len=%d", &seq, &len) != 2) {
            printf("[RCV] 알 수 없는 메시지: %s\n", buf);
            continue;
        }

        printf("\n[RCV] 패킷 수신: seq=%d, len=%d\n", seq, len);

        // ---------- NORMAL 모드 ----------
        if (mode == MODE_NORMAL) {
            if (seq == next_expected) {
                next_expected += len;
            } else {
                printf("[RCV] out-of-order로 간주 (next_expected=%d)\n", next_expected);
            }

            int ack = next_expected;
            printf("[RCV] ACK %d 송신 (누적)\n", ack);
            char ackbuf[BUF];
            int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
            sendto(s, ackbuf, m, 0, (struct sockaddr*)&cli, clen);
            usleep(SLEEP_US);
        }

        // ---------- 3 DUP ACK 모드 ----------
        else if (mode == MODE_DUP3) {
            char ackbuf[BUF];
            int ack;

            // 시나리오:
            // - 첫 패킷(seq=1500)은 정상 → ACK=3000
            // - 다음 3개(seq=3000,4500,6000)는 오류/순서오류 → 계속 ACK=3000 (중복 3번)
            // - 마지막 재전송(seq=3000)은 복구 완료 → ACK=7500
            if (seq == 1500 && dup_drop_count == 0) {
                next_expected = 3000;
                ack = next_expected;
                printf("[RCV] 첫 패킷 정상 수신 → ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr*)&cli, clen);
            } else if (dup_drop_count < 3 &&
                       (seq == 3000 || seq == 4500 || seq == 6000)) {
                dup_drop_count++;
                ack = 3000; // 계속 같은 ACK
                printf("[RCV] 패킷(seq=%d) 오류 또는 순서 오류 가정 → ACK %d (중복 #%d)\n",
                       seq, ack, dup_drop_count);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr*)&cli, clen);
            } else {
                // 재전송된 세그먼트 도착 → 손실 구간 복구 완료라고 가정
                next_expected = 7500;
                ack = next_expected;
                printf("[RCV] 재전송 패킷 수신 → 손실 구간 복구 완료 → ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr*)&cli, clen);
            }
            usleep(SLEEP_US);
        }

        // ---------- TIMEOUT 모드 ----------
        else if (mode == MODE_TIMEOUT) {
            char ackbuf[BUF];
            int ack;

            // 시나리오:
            // - seq=0: 정상 → ACK=1500
            // - seq=1500,3000,4500,6000: 손실로 가정 → ACK 안 보냄
            // - 이후(재전송 포함)는 정상 수신으로 간주하여 누적 ACK 전송
            if (seq == 0) {
                next_expected = MSS;
                ack = next_expected;
                printf("[RCV] 첫 번째 패킷 정상 수신 → ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr*)&cli, clen);
            } else if (timeout_drop_count < 4 &&
                       (seq == 1500 || seq == 3000 || seq == 4500 || seq == 6000)) {
                timeout_drop_count++;
                printf("[RCV] 패킷(seq=%d) 손실로 가정 → ACK 전송 안 함 (drop #%d)\n",
                       seq, timeout_drop_count);
                // 아무 것도 안 보냄
            } else {
                // 타임아웃 이후 회복 구간: 정상 누적 ACK 동작
                if (seq == next_expected) {
                    next_expected += len;
                } else if (seq > next_expected) {
                    // 단순화: 구멍이 있어도 next_expected는 그대로 두고, 현재까지 받은 것만 ACK
                    printf("[RCV] out-of-order로 간주 (next_expected=%d)\n", next_expected);
                }
                ack = next_expected;
                printf("[RCV] (회복 구간) ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr*)&cli, clen);
            }
            usleep(SLEEP_US);
        }
    }

    close(s);
    return 0;
}