// sender.c - TCP 혼잡제어 데모용 송신자
// 사용법:
//   clang -O2 -Wall -Wextra -o sender sender.c
//   ./sender <dst_ip> <dst_port> <normal|dup3|timeout>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#define MSS 1500
#define BUF 256
#define SLEEP_US 1500000 // 1.5초 딜레이

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

void send_end(int s, struct sockaddr_in *dst)
{
    const char *msg = "END";
    sendto(s, msg, strlen(msg), 0,
           (struct sockaddr *)dst, sizeof(*dst));
}

// ---------- NORMAL SCENARIO: Slow Start → ssthresh → CA ----------

void run_normal(int s, struct sockaddr_in *dst)
{
    double cwnd = MSS;    // 1 MSS
    int ssthresh = 15000; // 10 MSS
    int seq = 0;
    int step = 0;
    char buf[BUF];
    socklen_t dlen = sizeof(*dst);

    printf("=== [NORMAL] 시작: cwnd=%d, ssthresh=%d, MSS=%d ===\n",
           (int)cwnd, ssthresh, MSS);

    const int MAX_STEPS = 6; // RTT 라운드 6번 정도만 시연

    while (step < MAX_STEPS)
    {
        int packets = (int)(cwnd / MSS); // 이번 RTT에 보낼 패킷 개수
        if (packets < 1)
            packets = 1;

        printf("\n===== [ROUND %d] cwnd=%.1f MSS (packets=%d) =====\n",
               step + 1, cwnd / MSS, packets);

        // 1) 이번 RTT에 보낼 패킷들을 연속으로 전송
        for (int i = 0; i < packets; ++i)
        {
            printf("[SND] TX seq=%d len=%d (i=%d/%d)\n",
                   seq, MSS, i + 1, packets);
            snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
            sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
            seq += MSS;
            usleep(300 * 1000); // 패킷 사이 간격은 0.3초 정도
        }

        // 2) 그에 대한 ACK를 packets 개수 만큼 수신
        for (int i = 0; i < packets; ++i)
        {
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &slen);
            if (n < 0)
                die("recvfrom");
            buf[n] = 0;

            int ack = -1;
            if (sscanf(buf, "ACK %d", &ack) != 1)
            {
                printf("[SND] 알 수 없는 메시지: %s\n", buf);
                continue;
            }

            printf("[SND] RX ACK %d (i=%d/%d)\n", ack, i + 1, packets);

            // ACK 하나 받을 때마다 cwnd 증가
            if ((int)cwnd < ssthresh)
            {
                // Slow Start
                cwnd += MSS;
                printf("      → Slow Start: cwnd += MSS → %.1f MSS\n", cwnd / MSS);
            }
            else
            {
                // Congestion Avoidance
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                printf("      → Congestion Avoidance: cwnd += MSS*(MSS/cwnd) → %.2f MSS (증가량=%.2f bytes)\n",
                       cwnd / MSS, inc);
            }
        }

        // RTT 끝
        printf("[SND] ROUND %d 종료 → cwnd=%.2f MSS, ssthresh=%.2f MSS\n",
               step + 1, cwnd / MSS, (double)ssthresh / MSS);

        step++;
        usleep(SLEEP_US);
    }

    printf("\n=== [NORMAL] 시나리오 종료 ===\n");
    send_end(s, dst);
}

// ---------- 3 DUP ACK SCENARIO ----------
// seq=1500에서 시작.
// 1500은 정상 수신, 3000/4500/6000에서 같은 ACK(3000)가 3번 중복되도록.
// 3Dup ACK 시 cwnd/2, ssthresh도 /2.
// 이후 손실된 세그먼트(3000) 재전송 후, 새로운 ACK(7500)를 받아
// Congestion Avoidance 증가 한 번 보여주고 종료.

void run_dup3(int s, struct sockaddr_in *dst)
{
    double cwnd = 15000; // 넉넉하게 10 MSS
    int ssthresh = 15000;
    // 마지막 3000은 재전송
    int seqs[] = {1500, 3000, 4500, 6000, 3000};
    int count = sizeof(seqs) / sizeof(seqs[0]);
    int lastAck = 0;
    int dupCnt = 0;
    int did_halve = 0;   // cwnd 절반 조정 했는지
    int did_ca_once = 0; // 선형 증가 한 번 수행했는지

    char buf[BUF];
    socklen_t dlen = sizeof(*dst);

    printf("=== [3-Dup ACK] 시작: cwnd=%.1f MSS, ssthresh=%.1f MSS ===\n",
           cwnd / MSS, (double)ssthresh / MSS);

    for (int i = 0; i < count; ++i)
    {
        int seq = seqs[i];
        printf("\n[SND] 패킷(seq=%d, size=%d) 송신 (cwnd=%.1f MSS, ssthresh=%.1f MSS)\n",
               seq, MSS, cwnd / MSS, (double)ssthresh / MSS);

        snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
        usleep(SLEEP_US);

        // ACK 수신
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 0)
            die("recvfrom");
        buf[n] = 0;

        int ack = -1;
        if (sscanf(buf, "ACK %d", &ack) != 1)
        {
            printf("[SND] 알 수 없는 메시지: %s\n", buf);
            continue;
        }

        printf("[SND] ACK %d 수신\n", ack);

        if (i == 0)
        {
            // 첫 ACK: 기준 ACK 설정(예: 3000)
            lastAck = ack;
            dupCnt = 0;
            printf("[SND] 첫 번째 ACK → lastAck=%d\n", lastAck);
        }
        else if (ack == lastAck)
        {
            // 중복 ACK
            dupCnt++;
            printf("[SND] 중복 ACK (ack=%d) count=%d\n", ack, dupCnt);

            if (dupCnt == 3 && !did_halve)
            {
                // 3 Dup ACK 사건 발생
                printf("\n<<< [SND] 3-Dup ACK 사건 발생 >>>\n");
                printf("이전 cwnd = %.1f MSS\n", cwnd / MSS);

                cwnd /= 2.0;
                if (cwnd < MSS)
                    cwnd = MSS;
                ssthresh = (int)cwnd; // 간단히 동일하게
                did_halve = 1;

                printf("cwnd를 1/2로 조정 → %.1f MSS\n", cwnd / MSS);
                printf("임계값(ssthresh)도 %.1f MSS로 설정\n", (double)ssthresh / MSS);
            }
        }
        else
        {
            // 새로운 ACK 값(예: 7500) → 손실 구간 복구 완료
            printf("[SND] 새로운 ACK 값 감지 → lastAck %d → %d\n", lastAck, ack);
            lastAck = ack;
            dupCnt = 0;

            if (did_halve && !did_ca_once)
            {
                // Fast Recovery 이후 Congestion Avoidance 한 번 보여주기
                if (cwnd >= ssthresh)
                {
                    double inc = MSS * ((double)MSS / cwnd);
                    cwnd += inc;
                    printf("[SND] Fast Recovery 이후 Congestion Avoidance: cwnd += MSS*(MSS/cwnd) → %.2f MSS (증가량=%.2f bytes)\n",
                           cwnd / MSS, inc);
                }
                else
                {
                    // 혹시 cwnd < ssthresh 상태라면 Slow Start로 한 스텝
                    cwnd += MSS;
                    printf("[SND] Fast Recovery 이후 Slow Start: cwnd += MSS → %.1f MSS\n",
                           cwnd / MSS);
                }
                did_ca_once = 1;
            }
        }

        usleep(SLEEP_US);
    }

    printf("\n=== [3-Dup ACK] 시나리오 종료 ===\n");
    send_end(s, dst);
}

// ---------- TIMEOUT SCENARIO ----------
// 1) 첫 번째 패킷 정상 (seq=0)
// 2) 1500/3000/4500/6000은 모두 손실 구간으로 가정 → ACK 안 옴
// 3) ACK 대기 중 타임아웃 → cwnd, ssthresh 조정
// 4) 그 이후에는 패킷들을 정상 수신 가정하면서
//    - cwnd=1 MSS에서 시작해서 ssthresh까지 Slow Start(지수 증가)
//    - ssthresh 도달 후 Congestion Avoidance(선형 증가)까지 보여주고 종료

void run_timeout(int s, struct sockaddr_in *dst)
{
    double cwnd = 15000;
    int ssthresh = 15000;
    char buf[BUF];
    socklen_t dlen = sizeof(*dst);

    // recv 타임아웃 (3초)
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
        die("setsockopt");
    }

    printf("=== [TIMEOUT] 시작: cwnd=%.1f MSS, ssthresh=%.1f MSS ===\n",
           cwnd / MSS, (double)ssthresh / MSS);

    // (1) 첫 번째 패킷 정상 전송
    int seq = 0;
    printf("\n[SND] 패킷(seq=%d, size=%d) 송신 (정상)\n", seq, MSS);
    snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
    sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
    usleep(SLEEP_US);

    // ACK 수신
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&src, &slen);
    if (n < 0)
        die("recvfrom");
    buf[n] = 0;
    int ack = -1;
    sscanf(buf, "ACK %d", &ack);
    printf("[SND] ACK %d 수신 (첫 번째 정상)\n", ack);
    usleep(SLEEP_US);

    // (2) 손실 구간 전송
    int losses[] = {1500, 3000, 4500, 6000};
    int loss_count = sizeof(losses) / sizeof(losses[0]);

    for (int i = 0; i < loss_count; i++)
    {
        seq = losses[i];
        printf("\n[SND] 패킷(seq=%d) 송신 (손실 구간)\n", seq);
        snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
        usleep(SLEEP_US);
    }

    // (3) ACK 대기 -> 타임아웃 발생
    printf("\n[SND] 손실 패킷들에 대한 ACK 대기... (타임아웃 예상)\n");
    n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                 (struct sockaddr *)&src, &slen);

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        printf("[SND] <<< TIMEOUT 발생 >>>\n");
        printf("이전 cwnd = %.1f MSS\n", cwnd / MSS);

        // 타임아웃 처리
        double new_ssthresh = cwnd / 2.0;
        if (new_ssthresh < MSS)
            new_ssthresh = MSS;
        ssthresh = (int)new_ssthresh;
        cwnd = MSS; // 1 MSS로 감소

        printf("ssthresh = %.1f MSS\n", (double)ssthresh / MSS);
        printf("cwnd = 1 MSS로 감소\n");
    }
    else
    {
        printf("[SND] 예상과 다른 ACK 수신: %s\n", buf);
    }

    usleep(SLEEP_US);

    // (4) 회복 구간: 정상 시나리오와 동일한 RTT 진행
    printf("\n=== [TIMEOUT 이후: Slow Start + CongAvoid 시연] ===\n");

    seq = 1500;
    int RTT_round = 0;
    const int MAX_ROUNDS = 8;

    while (RTT_round < MAX_ROUNDS)
    {
        int packets = (int)(cwnd / MSS);
        if (packets < 1)
            packets = 1;

        printf("\n[ROUND %d] cwnd=%.2f MSS → 이번 RTT 패킷 %d개 전송\n",
               RTT_round + 1, cwnd / MSS, packets);

        // 1) 패킷 연속 전송
        for (int i = 0; i < packets; i++)
        {
            printf("[SND] TX seq=%d len=%d\n", seq, MSS);
            snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
            sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
            seq += MSS;
            usleep(300 * 1000);
        }

        // 2) ACK packets개 수신
        for (int i = 0; i < packets; i++)
        {
            int n2 = recvfrom(s, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&src, &slen);
            if (n2 < 0)
                die("recvfrom");
            buf[n2] = 0;
            int ack2 = -1;
            sscanf(buf, "ACK %d", &ack2);
            printf("[SND] RX ACK %d (i=%d/%d)\n", ack2, i + 1, packets);

            // cwnd 증가
            if ((int)cwnd < ssthresh)
            {
                cwnd += MSS;
                printf("      → Slow Start: cwnd=%.2f MSS\n", cwnd / MSS);
            }
            else
            {
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                printf("      → Congestion Avoidance: cwnd=%.2f MSS (+%.1f bytes)\n",
                       cwnd / MSS, inc);
            }
        }

        usleep(SLEEP_US);
        RTT_round++;
    }

    printf("\n=== [TIMEOUT] 시나리오 종료 ===\n");
    send_end(s, dst);
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <dst_ip> <dst_port> <normal|dup3|timeout>\n", argv[0]);
        return 1;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);
    Mode mode = parse_mode(argv[3]);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        die("socket");

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &dst.sin_addr) != 1)
        die("inet_pton");

    switch (mode)
    {
    case MODE_NORMAL:
        run_normal(s, &dst);
        break;
    case MODE_DUP3:
        run_dup3(s, &dst);
        break;
    case MODE_TIMEOUT:
        run_timeout(s, &dst);
        break;
    }

    close(s);
    return 0;
}