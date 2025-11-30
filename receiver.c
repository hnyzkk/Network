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

typedef enum
{
    MODE_NORMAL,
    MODE_DUP3,
    MODE_TIMEOUT
} Mode;

void die(const char *msg)
{
    perror(msg);
    exit(1);
}

Mode parse_mode(const char *s)
{
    if (strcmp(s, "normal") == 0)
        return MODE_NORMAL;
    if (strcmp(s, "dup3") == 0)
        return MODE_DUP3;
    if (strcmp(s, "timeout") == 0)
        return MODE_TIMEOUT;
    fprintf(stderr, "unknown mode: %s (use normal|dup3|timeout)\n", s);
    exit(1);
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <listen_port> <normal|dup3|timeout>\n", argv[0]);
        return 1;
    }
    int port = atoi(argv[1]);
    Mode mode = parse_mode(argv[2]);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        die("socket");

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family = AF_INET;
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    me.sin_port = htons(port);

    if (bind(s, (struct sockaddr *)&me, sizeof(me)) < 0)
        die("bind");

    printf("=== [RCV] Receiver on port %d, mode=%s ===\n",
           port,
           mode == MODE_NORMAL ? "normal" : mode == MODE_DUP3 ? "dup3"
                                                              : "timeout");

    int next_expected = 0;
    int dup_drop_count = 0;     // dup3 모드에서 손실/중복 처리용
    int timeout_drop_count = 0; // timeout 모드에서 손실 처리용

    while (1)
    {
        char buf[BUF];
        struct sockaddr_in cli;
        socklen_t clen = sizeof(cli);

        int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&cli, &clen);
        if (n < 0)
            die("recvfrom");
        buf[n] = 0;

        // 종료 메시지
        if (strncmp(buf, "END", 3) == 0)
        {
            printf("\n=== [RCV] END 수신, 시나리오 종료 ===\n");
            break;
        }

        int seq = -1, len = -1;
        if (sscanf(buf, "DATA seq=%d len=%d", &seq, &len) != 2)
        {
            printf("[RCV] 알 수 없는 메시지: %s\n", buf);
            continue;
        }

        printf("\n[RCV] 패킷 수신: seq=%d, len=%d\n", seq, len);

        // ---------- NORMAL 모드 ----------
        if (mode == MODE_NORMAL)
        {
            if (seq == next_expected)
            {
                next_expected += len;
            }
            else
            {
                // 단순 시뮬이므로 out-of-order는 무시하고 현재 next_expected 기준으로만 ACK
                printf("[RCV] out-of-order로 간주 (next_expected=%d)\n", next_expected);
            }

            int ack = next_expected;
            printf("[RCV] ACK %d 송신 (누적)\n", ack);
            char ackbuf[BUF];
            int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
            sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            usleep(SLEEP_US);
        }

        // ---------- 3 DUP ACK 모드 ----------
        else if (mode == MODE_DUP3)
        {
            char ackbuf[BUF];
            int ack;

            if (seq == 0)
            {
                // 첫 번째 패킷 정상 수신
                next_expected = MSS;
                ack = next_expected;
                printf("[RCV] 첫 번째 패킷 정상 수신 → ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            else if (dup_drop_count < 3 &&
                     (seq == 1500 || seq == 3000 || seq == 4500))
            {
                // 두 번째~네 번째 패킷: 손실/순서 오류로 가정
                dup_drop_count++;
                ack = MSS; // 계속 1500 (중복 ACK)
                printf("[RCV] 패킷(seq=%d) 오류 또는 순서 오류 가정 → ACK %d (중복 #%d)\n",
                       seq, ack, dup_drop_count);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            else
            {
                // 재전송된 두 번째 패킷 도착했다고 보고 누적 처리
                // 여기서는 0~6000까지 다 받은 것으로 가정
                next_expected = 6000;
                ack = next_expected;
                printf("[RCV] 재전송 패킷 수신 → 모든 손실 구간 복구 → ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            usleep(SLEEP_US);
        }

        // ---------- TIMEOUT 모드 ----------
        else if (mode == MODE_TIMEOUT)
        {
            char ackbuf[BUF];
            int ack;

            if (seq == 0)
            {
                // 첫 번째 패킷만 정상 수신
                next_expected = MSS;
                ack = next_expected;
                printf("[RCV] 첫 번째 패킷 정상 수신 → ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            else if (timeout_drop_count < 4 &&
                     (seq == 1500 || seq == 3000 || seq == 4500 || seq == 6000))
            {
                // 두 번째~다섯 번째 패킷은 모두 손실로 가정 → ACK 보내지 않음
                timeout_drop_count++;
                printf("[RCV] 패킷(seq=%d) 손실로 가정 → ACK 전송 안 함 (drop #%d)\n",
                       seq, timeout_drop_count);
                // 아무 것도 안 보냄
            }
            else
            {
                // 이후 재전송된 패킷은 정상 수신으로 가정
                next_expected = 7500; // 예시로 0~7499까지 정상 수신 완료라고 가정
                ack = next_expected;
                printf("[RCV] 재전송 패킷 정상 수신 → ACK %d 송신\n", ack);
                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            usleep(SLEEP_US);
        }
    }

    close(s);
    return 0;
}