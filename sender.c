// sender.c - TCP 혼잡제어 송신자
// 실행 방법:
//    ./sender <dst_ip> <dst_port> <normal|dup3|timeout>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

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

typedef enum // 시나리오 구분을 위한 구조체
{
    MODE_NORMAL,
    MODE_DUP3,
    MODE_TIMEOUT
} Mode;

// 에러 발생 시 프로그램 종료을 위한 함수
void die(const char *msg)
{
    perror(msg);
    exit(1);
}

// 인자로 받은 시나리오 명을 구조체로 바꿔주는 함수
Mode parse_mode(const char *s)
{
    if (strcmp(s, "normal") == 0)
        return MODE_NORMAL;
    if (strcmp(s, "dup3") == 0)
        return MODE_DUP3;
    if (strcmp(s, "timeout") == 0)
        return MODE_TIMEOUT;
    fprintf(stderr, "unknown mode: %s\n", s);
    exit(1);
}

// 송수신 끝을 알리는 함수
void send_end(int s, struct sockaddr_in *dst)
{
    const char *msg = "END";
    sendto(s, msg, strlen(msg), 0, (struct sockaddr *)dst, sizeof(*dst));
}

// ------------------------------ UI 유틸 ------------------------------
void box_top()
{
    printf(MAGENTA "┌──────────────────────────────────────────────────────────┐\n" RESET);
}
void box_mid()
{
    printf(MAGENTA "├──────────────────────────────────────────────────────────┤\n" RESET);
}
void box_bot()
{
    printf(MAGENTA "└──────────────────────────────────────────────────────────┘\n" RESET);
}

void show_round_header(int round, double cwnd, double ssthresh)
{
    box_top();
    printf(MAGENTA "│  ROUND %d  │  cwnd = %.2f MSS   ssthresh = %.2f MSS          │\n" RESET,
           round, cwnd / MSS, ssthresh / MSS);
    box_mid();
}
void show_event(const char *msg)
{
    printf(BOLDRED "%s\n" RESET, msg);
}
void show_timer_event(const char *msg)
{
    printf(BOLDCYN "%s\n" RESET, msg);
}

// ------------------------------ NORMAL ------------------------------
void run_normal(int s, struct sockaddr_in *dst)
{
    double cwnd = MSS;
    int ssthresh = 15000;
    int seq = 0;
    int round = 1; // 한 시나리오 내 라운드 구분을 위한 변수

    printf(BOLDMAG "\n=== [NORMAL 시나리오 시작] ===\n" RESET);

    int slow_start_rounds = 0; // 느린시작 라운드
    int ca_rounds = 0;         // 위험회피 라운드

    while (1)
    {
        int packets = cwnd / MSS; // 보낼 패킷 수 = 윈도우 크기 / MSS
        if (packets < 1)
            packets = 1;

        show_round_header(round, cwnd, ssthresh);

        // 패킷 수만큼 보내기
        for (int i = 0; i < packets; i++)
        {
            printf(BLUE "  [TX] seq=%d len=%d\n" RESET, seq, MSS);
            char buf[BUF];
            snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);           // buf에 seq와 len에 대한 문자열 저장
            sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, sizeof(*dst)); // buf의 내용을 그대로 전송
            seq += MSS;                                                           // 보낸만큼 seq 업데이트
            usleep(300000);                                                       // 0.3초 딜레이
        }

        printf(CYAN "  --- RTT 경과: ACK 수신 ---\n" RESET);

        // ACK 받기
        for (int i = 0; i < packets; i++)
        {
            char buf[BUF];
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);

            int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&src, &slen);
            if (n < 0)
                die("recvfrom normal");
            buf[n] = 0;
            int ack = 0;
            sscanf(buf, "ACK %d", &ack);
            printf(GREEN "  [RX] ACK %d\n" RESET, ack);

            // 윈도우 크기가 임계치보다 작다면 느린시작 -> 지수적 증가
            if ((int)cwnd < ssthresh)
            {
                slow_start_rounds++;
                cwnd += MSS;
                printf(YELLOW "     ↳ Slow Start 증가 → cwnd=%.2f MSS\n" RESET, cwnd / MSS);
            }
            // 아니면 혼잡회피 -> 선형적 증가
            else
            {
                ca_rounds++;
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                printf(YELLOW "     ↳ CA 증가 → cwnd=%.2f MSS\n" RESET, cwnd / MSS);
            }
        }

        box_bot();
        usleep(SLEEP_US);
        round++;

        // 종료 조건(임의)
        if (slow_start_rounds >= 3 && ca_rounds >= 2)
            break;
    }

    printf(BOLDMAG "\n=== [NORMAL 시나리오 종료] ===\n" RESET);
    send_end(s, dst);
}

// ------------------------------ 3 DUP ACK ------------------------------
void run_dup3(int s, struct sockaddr_in *dst)
{
    // 초기 윈도우 크기와 임계치 설정
    double cwnd = 15000;
    int ssthresh = 15000;

    printf(BOLDMAG "\n=== [3 DUP ACK 시나리오 시작] ===\n" RESET);

    int seqs[] = {1500, 3000, 4500, 6000, 3000}; // 임의적으로 3 dup를 발생시키기 위해 seq 순서에 대한 배열 생성(3000에 대해 3 dup인 상황)
    int count = sizeof(seqs) / sizeof(seqs[0]);
    int lastAck = -1; // 마지막으로 받은 ack
    int dupCnt = 0;   // 중복 ack 횟수
    int halved = 0;   // cwnd 절반 감소 여부를 나타내는 플래그

    for (int i = 0; i < count; i++)
    {
        char buf[BUF];
        int seq = seqs[i];

        // send
        printf(BLUE "\n[TX] seq=%d len=%d\n" RESET, seq, MSS);
        snprintf(buf, sizeof(buf),
                 "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0,
               (struct sockaddr *)dst, sizeof(*dst));
        usleep(SLEEP_US);

        // recv ack
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &slen);
        if (n < 0)
            die("recvfrom dup3");
        buf[n] = 0;

        int ack = 0;
        sscanf(buf, "ACK %d", &ack);
        printf(GREEN "[RX] ACK %d 수신\n" RESET, ack);

        if (i == 0)
        {
            lastAck = ack;
            dupCnt = 0;
        }
        // 중복 ack 수신
        else if (ack == lastAck)
        {
            dupCnt++;
            printf(YELLOW "    중복 ACK (%d회)\n" RESET, dupCnt);

            if (dupCnt == 3 && !halved) // 중복 횟수가 3이고 cwnd가 절반으로 감소된 적이 없다면
            {
                show_event("\n*** <<< 3 DUP ACK 사건 발생 >>> ***");

                double prev = cwnd; // 감소 이전 cwnd 값
                cwnd /= 2.0;
                if (cwnd < MSS)
                    cwnd = MSS;
                ssthresh = cwnd; // 임계치 조정

                printf(BOLDYEL "    cwnd: %.1f MSS → %.1f MSS\n" RESET,
                       prev / MSS, cwnd / MSS);
                printf(BOLDMAG "    ssthresh = %.1f MSS\n" RESET, ssthresh / MSS);
                halved = 1; // 절반으로 감소했음을 표시
            }
        }
        // 중복 ack가 아닌 새로운 값이 도착 = 복구 및 위험회피 구간
        else
        {
            printf(CYAN "    새로운 ACK → 누적 구간 복구 처리\n" RESET);

            // 위험회피
            if (halved)
            {
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                printf(YELLOW "    CA 증가 1회 → cwnd=%.2f MSS\n" RESET, cwnd / MSS);
            }

            lastAck = ack;
            dupCnt = 0;
        }
    }

    printf(BOLDMAG "\n=== [3 DUP ACK 시나리오 종료] ===\n" RESET);
    send_end(s, dst);
}

// ------------------------------ TIMEOUT ------------------------------
void run_timeout(int s, struct sockaddr_in *dst)
{
    double cwnd = 15000;
    int ssthresh = 15000;

    printf(BOLDMAG "\n=== [TIMEOUT 시나리오 시작] ===\n" RESET);

    char buf[BUF];
    socklen_t dlen = sizeof(*dst);

    // recv timeout = 3초
    struct timeval tv;
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    // (1) 첫 패킷 정상
    int seq = 0;

    printf(BLUE "\n[TX] seq=%d len=%d\n" RESET, seq, MSS);
    snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
    sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
    usleep(SLEEP_US);

    // ACK
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int n = recvfrom(s, buf, sizeof(buf) - 1, 0,
                     (struct sockaddr *)&src, &slen);
    if (n < 0)
        die("first ack timeout");
    buf[n] = 0;
    int ack;
    sscanf(buf, "ACK %d", &ack);
    printf(GREEN "[RX] ACK %d 수신\n" RESET, ack);

    // (2) 1500~6000 손실 구간
    int losses[] = {1500, 3000, 4500, 6000};
    int lossCount = sizeof(losses) / sizeof(losses[0]);

    for (int i = 0; i < lossCount; i++)
    {
        seq = losses[i];
        printf(BLUE "\n[TX] seq=%d (손실 구간)\n" RESET, seq);

        // 타이머 걸기 시작 (첫 손실 구간에서)
        if (i == 0)
        {
            show_timer_event("*** (타이머 시작) seq=1500 ***");
        }

        snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0, (struct sockaddr *)dst, dlen);
        usleep(SLEEP_US);
    }

    // (3) ACK 기다리기 → Timeout
    printf(CYAN "\n[TX] 손실 패킷 ACK 대기 중...\n" RESET);

    n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src, &slen);
    // Timeout 발생
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        show_event("\n*** <<< TIMEOUT 발생 >>> ***");

        double prev = cwnd;
        ssthresh = prev / 2.0; // 임계치 조정
        if (ssthresh < MSS)
            ssthresh = MSS;

        cwnd = MSS; // 1 MSS로 조정

        printf(BOLDMAG "    ssthresh = %.2f MSS\n" RESET, ssthresh / MSS);
        printf(BOLDYEL "    cwnd = 1 MSS 로 감소\n" RESET);
    }

    // (4) 회복 구간 (지수 증가 + 선형 증가)
    printf(BOLDMAG "\n=== [회복 구간: Slow Start + CA 시연] ===\n" RESET);

    seq = 1500;
    int slow_rounds = 0;
    int ca_rounds = 0;

    while (1)
    {
        int packets = cwnd / MSS;
        if (packets < 1)
            packets = 1;

        show_round_header(slow_rounds + ca_rounds + 1, cwnd, ssthresh);

        // send
        for (int i = 0; i < packets; i++)
        {
            printf(BLUE "  [TX] seq=%d len=%d\n" RESET, seq, MSS);
            snprintf(buf, sizeof(buf),
                     "DATA seq=%d len=%d", seq, MSS);
            sendto(s, buf, strlen(buf), 0,
                   (struct sockaddr *)dst, dlen);
            seq += MSS;
            usleep(300000);
        }

        printf(CYAN "  --- RTT 경과: ACK 수신 ---\n" RESET);

        // recv ack
        for (int i = 0; i < packets; i++)
        {
            int n2 = recvfrom(s, buf, sizeof(buf) - 1, 0,
                              (struct sockaddr *)&src, &slen);
            if (n2 < 0)
                die("recvfrom recovery");
            buf[n2] = 0;

            int ack2;
            sscanf(buf, "ACK %d", &ack2);
            printf(GREEN "  [RX] ACK %d\n" RESET, ack2);

            // 느린시작
            if ((int)cwnd < ssthresh)
            {
                cwnd += MSS;
                slow_rounds++;
                printf(YELLOW "     ↳ Slow Start 증가 → cwnd=%.2f MSS\n" RESET,
                       cwnd / MSS);
            }
            // 혼잡회피
            else
            {
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                ca_rounds++;
                printf(YELLOW "     ↳ CA 증가 → cwnd=%.2f MSS\n" RESET,
                       cwnd / MSS);
            }
        }

        box_bot();
        usleep(SLEEP_US);

        // 종료 조건(임의)
        if (slow_rounds >= 3 && ca_rounds >= 2)
        {
            printf(BOLDMAG "\n=== [TIMEOUT 시나리오 종료] ===\n" RESET);
            send_end(s, dst);
            break;
        }
    }
}

// ------------------------------ MAIN ------------------------------
int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s <dst_ip> <dst_port> <mode>\n", argv[0]);
        return 1;
    }

    // 인자: 목적지 ip, 목적지 port, 시나리오
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    Mode mode = parse_mode(argv[3]);

    // 소켓 설정
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        die("socket");

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    inet_pton(AF_INET, ip, &dst.sin_addr);

    // 인자에 따라 시나리오 실행
    if (mode == MODE_NORMAL)
        run_normal(s, &dst);
    else if (mode == MODE_DUP3)
        run_dup3(s, &dst);
    else if (mode == MODE_TIMEOUT)
        run_timeout(s, &dst);

    close(s);
    return 0;
}