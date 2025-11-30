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
    int next_step = 0;
    char buf[BUF];
    socklen_t dlen = sizeof(*dst);

    printf("=== [NORMAL] 시작: cwnd=%d, ssthresh=%d, MSS=%d ===\n",
           (int)cwnd, ssthresh, MSS);

    // 대략 12번 정도 왕복 (느린 시작 → ssthresh 도달 → 선형 증가 몇 번)
    const int MAX_STEPS = 12;

    while (next_step < MAX_STEPS)
    {
        printf("\n[SND] 패킷(seq=%d, size=%d) 송신 (cwnd=%.1f, ssthresh=%.1f)\n",
               seq, MSS, cwnd / MSS, (double)ssthresh / MSS);
        snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);

        usleep(SLEEP_US);

        // ACK 수신 (타임아웃 없이 블로킹)
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

        // cwnd 조정
        if ((int)cwnd < ssthresh)
        {
            // Slow Start: cwnd = cwnd + MSS
            cwnd += MSS;
            printf("[SND] Slow Start: cwnd += MSS → %.1f MSS\n", cwnd / MSS);
        }
        else
        {
            // Congestion Avoidance: cwnd = cwnd + MSS * (MSS / cwnd)
            double inc = MSS * ((double)MSS / cwnd);
            cwnd += inc;
            printf("[SND] Congestion Avoidance: cwnd += MSS*(MSS/cwnd) → %.2f MSS (증가량=%.2f bytes)\n",
                   cwnd / MSS, inc);
        }

        seq += MSS;
        next_step++;
        usleep(SLEEP_US);
    }

    printf("\n=== [NORMAL] 시나리오 종료 ===\n");
    send_end(s, dst);
}

// ---------- 3 DUP ACK SCENARIO ----------
// 1번째 패킷 정상, 그 뒤 3개는 손실/순서 오류로 가정해서 ACK가 3번 중복됨.
// 이후 3 Dup ACK 사건 처리(cwnd, ssthresh 조정) 후 재전송 한 번 하고 끝.

void run_dup3(int s, struct sockaddr_in *dst)
{
    double cwnd = 15000; // 넉넉하게 10 MSS
    int ssthresh = 15000;
    int seqs[] = {0, 1500, 3000, 4500, 1500}; // 마지막 1500은 재전송
    int count = sizeof(seqs) / sizeof(seqs[0]);
    int lastAck = 0;
    int dupCnt = 0;

    char buf[BUF];
    socklen_t dlen = sizeof(*dst);

    printf("=== [3-Dup ACK] 시작: cwnd=%.1f MSS, ssthresh=%.1f MSS ===\n",
           cwnd / MSS, (double)ssthresh / MSS);

    for (int i = 0; i < count; ++i)
    {
        int seq = seqs[i];
        printf("\n[SND] 패킷(seq=%d, size=%d) 송신 (cwnd=%.1f MSS)\n",
               seq, MSS, cwnd / MSS);

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
            // 첫 ACK: 기준 ACK 설정
            lastAck = ack;
            dupCnt = 0;
            printf("[SND] 첫 번째 ACK → lastAck=%d\n", lastAck);
        }
        else if (ack == lastAck)
        {
            dupCnt++;
            printf("[SND] 중복 ACK (ack=%d) count=%d\n", ack, dupCnt);

            if (dupCnt == 3)
            {
                // 3 Dup ACK 사건 발생
                printf("\n<<< [SND] 3-Dup ACK 사건 발생 >>>\n");
                printf("이전 cwnd = %.1f MSS\n", cwnd / MSS);

                // 간단 버전: cwnd와 ssthresh 모두 절반으로
                cwnd /= 2.0;
                if (cwnd < MSS)
                    cwnd = MSS;
                ssthresh = (int)cwnd;

                printf("cwnd를 1/2로 조정 → %.1f MSS\n", cwnd / MSS);
                printf("임계값(ssthresh)도 %.1f MSS로 설정\n", (double)ssthresh / MSS);
                printf("이후 손실 패킷 재전송(마지막 seq=1500)이 이루어짐\n");
            }
        }
        else
        {
            // ACK 값이 달라졌다면 중복 카운트 초기화
            lastAck = ack;
            dupCnt = 0;
            printf("[SND] 새로운 ACK 값 감지 → lastAck=%d, dupCnt=0\n", lastAck);
        }

        usleep(SLEEP_US);
    }

    printf("\n=== [3-Dup ACK] 시나리오 종료 ===\n");
    send_end(s, dst);
}

// ---------- TIMEOUT SCENARIO ----------
// 1번째 패킷 정상 수신 후, 2~5번째 패킷은 모두 손실(ACK 없음)로 가정.
// 2~5 번째를 보내고 나서 ACK를 기다리다가 타임아웃 발생 → cwnd, ssthresh 조정.
// 그 후 손실된 두 번째 패킷(seq=1500) 재전송 한 번 하고 종료.

void run_timeout(int s, struct sockaddr_in *dst)
{
    double cwnd = 15000;
    int ssthresh = 15000;
    char buf[BUF];
    socklen_t dlen = sizeof(*dst);

    // 타임아웃을 위해 recv 타임아웃 설정 (3초)
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0)
    {
        die("setsockopt");
    }

    printf("=== [TIMEOUT] 시작: cwnd=%.1f MSS, ssthresh=%.1f MSS ===\n",
           cwnd / MSS, (double)ssthresh / MSS);

    // (1) 첫 번째 패킷 정상
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
    printf("[SND] ACK %d 수신 (첫 번째 패킷 정상)\n", ack);
    usleep(SLEEP_US);

    // (2) 두 번째 ~ 다섯 번째 패킷은 손실 구간으로 가정
    int lost_seqs[] = {1500, 3000, 4500, 6000};
    int lost_cnt = sizeof(lost_seqs) / sizeof(lost_seqs[0]);

    for (int i = 0; i < lost_cnt; ++i)
    {
        seq = lost_seqs[i];
        printf("\n[SND] 패킷(seq=%d, size=%d) 송신 (손실 구간)\n", seq, MSS);
        snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
        usleep(SLEEP_US);
    }

    // (3) 이제 ACK를 기다리지만 수신측은 ACK를 보내지 않음 → 타임아웃
    printf("\n[SND] 손실 구간 패킷들에 대한 ACK 대기 (타임아웃 기대)\n");
    n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                 (struct sockaddr *)&src, &slen);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        printf("[SND] <<< TIMEOUT 사건 발생 >>>\n");
        printf("이전 cwnd = %.1f MSS\n", cwnd / MSS);

        // 타임아웃 처리: ssthresh=cwnd/2, cwnd=1 MSS
        double prev = cwnd;
        cwnd /= 2.0;
        if (cwnd < MSS)
            cwnd = MSS;
        ssthresh = (int)cwnd; // 간단하게 동일하게
        cwnd = MSS;

        printf("임계값 ssthresh = %.1f MSS 로 설정\n", (double)ssthresh / MSS);
        printf("cwnd = 1 MSS 로 감소 (%.1f MSS, prev=%.1f MSS)\n",
               cwnd / MSS, prev / MSS);
    }
    else
    {
        printf("[SND] 예상과 다르게 ACK를 수신했습니다: %s\n", (n > 0 ? buf : "에러"));
    }
    usleep(SLEEP_US);

    // (4) 손실된 두 번째 패킷(seq=1500) 재전송 한 번
    seq = 1500;
    printf("\n[SND] 손실 패킷 재전송: seq=%d\n", seq);
    snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
    sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
    usleep(SLEEP_US);

    // 재전송에 대한 ACK 수신 (수신자는 여기서부터 정상 처리)
    n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                 (struct sockaddr *)&src, &slen);
    if (n > 0)
    {
        buf[n] = 0;
        int ack2 = -1;
        sscanf(buf, "ACK %d", &ack2);
        printf("[SND] 재전송에 대한 ACK %d 수신\n", ack2);
        // Slow Start 한 스텝
        cwnd += MSS;
        printf("[SND] Slow Start 한 스텝: cwnd=%.1f MSS\n", cwnd / MSS);
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