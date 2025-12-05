// receiver.c - TCP 혼잡제어 수신자
// 실행 방법:
//   ./receiver <listen_port> <normal|dup3|timeout>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// 기본 설정
#define MSS 1500 // 초기 임계치
#define BUF 256
#define SLEEP_US 1500000

// 컬러 코드
#define RESET "\033[0m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define WHITE "\033[37m"
#define BOLDRED "\033[1;31m"
#define BOLDYEL "\033[1;33m"
#define BOLDMAG "\033[1;35m"
#define BOLDCYN "\033[1;36m"

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

    printf(BOLDMAG "=== [RCV] Receiver 시작 (port=%d, mode=%s) ===\n" RESET,
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
            printf(BOLDMAG "\n=== [RCV] END 수신 → 시나리오 종료 ===\n" RESET);
            break;
        }

        int seq = -1, len = -1;
        if (sscanf(buf, "DATA seq=%d len=%d", &seq, &len) != 2)
        {
            printf(RED "[RCV] 알 수 없는 메시지: %s\n" RESET, buf);
            continue;
        }

        printf("\n" CYAN "--------------------------------------------------------\n" RESET);
        printf(BLUE "[RCV] DATA 수신   ◀◀◀   " RESET
                    "seq=%d, len=%d\n",
               seq, len);

        // ================= NORMAL 모드 =================
        if (mode == MODE_NORMAL)
        {
            if (seq == next_expected)
            {
                next_expected += len;
            }
            else
            {
                printf(YELLOW "[RCV] out-of-order (next_expected=%d) → 누적 ACK만 보냄\n" RESET,
                       next_expected);
            }

            int ack = next_expected;
            printf(GREEN "[RCV] ACK 송신   ▶▶▶   ACK %d (누적)\n" RESET, ack);

            char ackbuf[BUF];
            int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
            sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);

            usleep(SLEEP_US);
        }

        // ================= 3 DUP ACK 모드 =================
        else if (mode == MODE_DUP3)
        {
            char ackbuf[BUF];
            int ack = 0;

            // 시나리오:
            //  - seq=1500: 정상 수신 → ACK=3000
            //  - seq=3000,4500,6000: 손실/순서오류 → 계속 ACK 3000 (dup 3회)
            //  - 마지막 재전송된 패킷: 손실 구간 복구 완료 → ACK=7500
            if (seq == 1500 && dup_drop_count == 0)
            {
                next_expected = 3000;
                ack = next_expected;
                printf(GREEN "[RCV] 첫 패킷 정상 수신 → ACK %d 송신\n" RESET, ack);

                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            else if (dup_drop_count < 3 &&
                     (seq == 3000 || seq == 4500 || seq == 6000))
            {
                dup_drop_count++;
                ack = 3000;
                printf(YELLOW "[RCV] 손실/순서 오류 가정 → 중복 ACK %d (dup=%d)\n" RESET,
                       ack, dup_drop_count);

                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            else
            {
                // 재전송된 패킷 도착 → 손실 구간 복구 완료라고 가정
                next_expected = 7500;
                ack = next_expected;
                printf(GREEN "[RCV] 재전송 패킷 수신 → 손실 구간 복구 → ACK %d 송신\n" RESET,
                       ack);

                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }

            usleep(SLEEP_US);
        }

        // ================= TIMEOUT 모드 =================
        else if (mode == MODE_TIMEOUT)
        {
            char ackbuf[BUF];
            int ack = 0;

            // 시나리오:
            //  - seq=0: 정상 수신 → ACK=1500
            //  - seq=1500,3000,4500,6000: 손실로 가정 → ACK 안 보냄
            //  - 이후(회복 구간): 정상 수신 → 누적 ACK 동작

            if (seq == 0)
            {
                next_expected = MSS;
                ack = next_expected;
                printf(GREEN "[RCV] 첫 패킷 정상 수신 → ACK %d 송신\n" RESET, ack);

                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }
            else if (timeout_drop_count < 4 &&
                     (seq == 1500 || seq == 3000 || seq == 4500 || seq == 6000))
            {
                timeout_drop_count++;
                printf(RED "[RCV] 손실로 가정 → ACK 전송 안 함 (drop #%d)\n" RESET,
                       timeout_drop_count);
                // 아무 것도 안 보냄
            }
            else
            {
                // 회복 구간: 정상 누적 ACK
                if (seq == next_expected)
                {
                    next_expected += len;
                    printf(GREEN "[RCV] in-order 수신 → next_expected=%d\n" RESET,
                           next_expected);
                }
                else if (seq > next_expected)
                {
                    printf(YELLOW "[RCV] out-of-order (next_expected=%d) → 기존 ACK 유지\n" RESET,
                           next_expected);
                }

                ack = next_expected;
                printf(GREEN "[RCV] 회복 구간 ACK 송신   ▶▶▶   ACK %d\n" RESET, ack);

                int m = snprintf(ackbuf, sizeof(ackbuf), "ACK %d", ack);
                sendto(s, ackbuf, m, 0, (struct sockaddr *)&cli, clen);
            }

            usleep(SLEEP_US);
        }
    }

    close(s);
    return 0;
}