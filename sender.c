// sender.c - TCP 혼잡제어 데모용 송신자 (UI강화 버전)
// 로직 변경 없음 — 오직 출력만 강화됨.
//
// 기능:
//  - 컬러 출력
//  - ASCII 박스 UI
//  - 송수신 화살표 기반 Flow 시각화
//  - SlowStart/CA/3DupACK/Timeout 이벤트 컬러 강조
//
// 빌드:
//    clang -O2 -Wall -Wextra -o sender sender.c
//
// 실행:
//    ./sender <dst_ip> <dst_port> <normal|dup3|timeout>

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
#define SLEEP_US 1500000

// 컬러 코드
#define RESET   "\033[0m"
#define RED     "\033[31m"
#define GREEN   "\033[32m"
#define YELLOW  "\033[33m"
#define BLUE    "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN    "\033[36m"
#define WHITE   "\033[37m"
#define BOLDRED "\033[1;31m"
#define BOLDYEL "\033[1;33m"
#define BOLDMAG "\033[1;35m"
#define BOLDCYN "\033[1;36m"

typedef enum { MODE_NORMAL, MODE_DUP3, MODE_TIMEOUT } Mode;

void die(const char* msg) {
    perror(msg);
    exit(1);
}

Mode parse_mode(const char* s) {
    if (strcmp(s, "normal") == 0)  return MODE_NORMAL;
    if (strcmp(s, "dup3") == 0)    return MODE_DUP3;
    if (strcmp(s, "timeout") == 0) return MODE_TIMEOUT;
    fprintf(stderr, "unknown mode: %s\n", s);
    exit(1);
}

void send_end(int s, struct sockaddr_in* dst) {
    const char* msg = "END";
    sendto(s, msg, strlen(msg), 0, (struct sockaddr*)dst, sizeof(*dst));
}

// ------------------------------ UI 유틸 ------------------------------

void box_top() {
    printf(MAGENTA "┌──────────────────────────────────────────────────────────┐\n" RESET);
}
void box_mid() {
    printf(MAGENTA "├──────────────────────────────────────────────────────────┤\n" RESET);
}
void box_bot() {
    printf(MAGENTA "└──────────────────────────────────────────────────────────┘\n" RESET);
}

void show_round_header(int round, double cwnd, double ssthresh) {
    box_top();
    printf(MAGENTA "│  ROUND %d  │  cwnd = %.2f MSS   ssthresh = %.2f MSS          │\n" RESET,
           round, cwnd/MSS, ssthresh/MSS);
    box_mid();
}

void show_event(const char* msg) {
    printf(BOLDRED "%s\n" RESET, msg);
}

void show_timer_event(const char* msg) {
    printf(BOLDCYN "%s\n" RESET, msg);
}

// ------------------------------ NORMAL ------------------------------

void run_normal(int s, struct sockaddr_in* dst) {
    double cwnd = MSS;
    int ssthresh = 15000;
    int seq = 0;
    int round = 1;

    printf(BOLDMAG "\n=== [NORMAL 시나리오 시작] ===\n" RESET);

    for (int step = 0; step < 6; step++) {
        int packets = cwnd / MSS;
        if (packets < 1) packets = 1;

        show_round_header(round, cwnd, ssthresh);

        // TX
        for (int i = 0; i < packets; i++) {
            printf(BLUE "  [TX] seq=%d len=%d   →→→   " RESET, seq, MSS);
            printf(WHITE "(송신)\n" RESET);

            char buf[BUF];
            socklen_t dlen = sizeof(*dst);
            snprintf(buf, sizeof(buf),
                     "DATA seq=%d len=%d", seq, MSS);
            sendto(s, buf, strlen(buf), 0, (struct sockaddr*)dst, dlen);
            seq += MSS;
            usleep(300000);
        }

        printf(CYAN "  --- RTT 경과: ACK 수신 시작 ---\n" RESET);

        // ACK
        for (int i = 0; i < packets; i++) {
            char buf[BUF];
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);

            int n = recvfrom(s, buf, sizeof(buf)-1, 0,
                             (struct sockaddr*)&src, &slen);
            if (n < 0) die("recvfrom");
            buf[n] = 0;

            int ack = 0;
            sscanf(buf, "ACK %d", &ack);

            printf(GREEN "  [RX] ACK=%d   ←←← (수신)\n" RESET, ack);

            if ((int)cwnd < ssthresh) {
                cwnd += MSS;
                printf(YELLOW "     ↳ Slow Start 증가 → cwnd = %.2f MSS\n" RESET, cwnd/MSS);
            } else {
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                printf(YELLOW "     ↳ Congestion Avoidance 증가 → cwnd = %.2f MSS\n" RESET,
                       cwnd/MSS);
            }
        }

        box_bot();
        usleep(SLEEP_US);
        round++;
    }

    printf(BOLDMAG "\n=== [NORMAL 시나리오 종료] ===\n" RESET);
    send_end(s, dst);
}

// ------------------------------ 3 DUP ACK ------------------------------

void run_dup3(int s, struct sockaddr_in* dst) {
    double cwnd = 15000;
    int ssthresh = 15000;

    printf(BOLDMAG "\n=== [3 DUP ACK 시나리오 시작] ===\n" RESET);

    int seqs[] = {1500, 3000, 4500, 6000, 3000};
    int count = sizeof(seqs)/sizeof(seqs[0]);
    int lastAck = -1;
    int dupCnt = 0;
    int halved = 0;

    for (int i = 0; i < count; i++) {
        char buf[BUF];
        int seq = seqs[i];

        printf(BLUE "\n[TX] seq=%d len=%d\n" RESET, seq, MSS);
        snprintf(buf, sizeof(buf),
                 "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0,
               (struct sockaddr*)dst, sizeof(*dst));
        usleep(SLEEP_US);

        //ACK
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int n = recvfrom(s, buf, sizeof(buf)-1, 0,
                         (struct sockaddr*)&src, &slen);
        if (n < 0) die("recvfrom dup3");
        buf[n] = 0;

        int ack = 0;
        sscanf(buf, "ACK %d", &ack);
        printf(GREEN "[RX] ACK %d 수신\n" RESET, ack);

        if (i == 0) {
            lastAck = ack;
            dupCnt = 0;
        } else if (ack == lastAck) {
            dupCnt++;
            printf(YELLOW "    중복 ACK (%d회)\n" RESET, dupCnt);

            if (dupCnt == 3 && !halved) {
                show_event("\n*** <<< 3 DUP ACK 사건 발생 >>> ***");

                double prev = cwnd;
                cwnd /= 2.0;
                if (cwnd < MSS) cwnd = MSS;
                ssthresh = cwnd;

                printf(BOLDYEL "    cwnd: %.1f MSS → %.1f MSS\n" RESET,
                       prev/MSS, cwnd/MSS);
                printf(BOLDMAG "    ssthresh = %.1f MSS\n" RESET, ssthresh/MSS);
                halved = 1;
            }

        } else {
            printf(CYAN "    새로운 ACK → 누적 구간 복구 처리\n" RESET);

            if (halved) {
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                printf(YELLOW "    CA 증가 1회 → cwnd=%.2f MSS\n" RESET, cwnd/MSS);
            }

            lastAck = ack;
            dupCnt = 0;
        }
    }

    printf(BOLDMAG "\n=== [3 DUP ACK 시나리오 종료] ===\n" RESET);
    send_end(s, dst);
}

// ------------------------------ TIMEOUT ------------------------------

void run_timeout(int s, struct sockaddr_in* dst) {
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
    sendto(s, buf, strlen(buf), 0, (struct sockaddr*)dst, dlen);
    usleep(SLEEP_US);

    // ACK
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    int n = recvfrom(s, buf, sizeof(buf)-1, 0,
                     (struct sockaddr*)&src, &slen);
    if (n < 0) die("first ack timeout");
    buf[n] = 0;
    int ack;
    sscanf(buf, "ACK %d", &ack);
    printf(GREEN "[RX] ACK %d 수신\n" RESET, ack);

    // (2) 1500~6000 손실 구간
    int losses[] = {1500, 3000, 4500, 6000};
    int lossCount = sizeof(losses)/sizeof(losses[0]);

    for (int i = 0; i < lossCount; i++) {
        seq = losses[i];
        printf(BLUE "\n[TX] seq=%d (손실 구간)\n" RESET, seq);

        // 타이머 걸기 시작 (첫 손실 구간에서)
        if (i == 0) {
            show_timer_event("*** (타이머 시작) seq=1500 ***");
        }

        snprintf(buf, sizeof(buf), "DATA seq=%d len=%d", seq, MSS);
        sendto(s, buf, strlen(buf), 0, (struct sockaddr*)dst, dlen);
        usleep(SLEEP_US);
    }

    // (3) ACK 기다리기 → TimeOut
    printf(CYAN "\n[TX] 손실 패킷 ACK 대기 중...\n" RESET);

    n = recvfrom(s, buf, sizeof(buf)-1, 0, (struct sockaddr*)&src, &slen);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        show_event("\n*** <<< TIMEOUT 발생 >>> ***");

        double prev = cwnd;
        ssthresh = prev / 2.0;
        if (ssthresh < MSS) ssthresh = MSS;

        cwnd = MSS;

        printf(BOLDMAG "    ssthresh = %.2f MSS\n" RESET, ssthresh/MSS);
        printf(BOLDYEL "    cwnd = 1 MSS 로 감소\n" RESET);
    }

    // (4) 회복 구간 (지수 증가 + 선형 증가)
    printf(BOLDMAG "\n=== [회복 구간: Slow Start + CA 시연] ===\n" RESET);

    seq = 1500;
    for (int round = 1; round <= 7; round++) {

        int packets = cwnd / MSS;
        if (packets < 1) packets = 1;

        show_round_header(round, cwnd, ssthresh);

        // TX 연속
        for (int i = 0; i < packets; i++) {
            printf(BLUE "  [TX] seq=%d len=%d\n" RESET, seq, MSS);
            snprintf(buf, sizeof(buf),
                     "DATA seq=%d len=%d", seq, MSS);
            sendto(s, buf, strlen(buf), 0,
                   (struct sockaddr*)dst, dlen);
            seq += MSS;
            usleep(300000);
        }

        printf(CYAN "  --- RTT 경과: ACK 수신 ---\n" RESET);

        // ACK 연속
        for (int i = 0; i < packets; i++) {
            int n2 = recvfrom(s, buf, sizeof(buf)-1, 0,
                              (struct sockaddr*)&src, &slen);
            if (n2 < 0) die("recvfrom recovery");
            buf[n2] = 0;

            int ack2;
            sscanf(buf, "ACK %d", &ack2);
            printf(GREEN "  [RX] ACK %d\n" RESET, ack2);

            if ((int)cwnd < ssthresh) {
                cwnd += MSS;
                printf(YELLOW "     ↳ Slow Start 증가 → cwnd = %.2f MSS\n" RESET,
                       cwnd/MSS);
            } else {
                double inc = MSS * ((double)MSS / cwnd);
                cwnd += inc;
                printf(YELLOW "     ↳ CA 증가 → cwnd = %.2f MSS (+%.1f bytes)\n" RESET,
                       cwnd/MSS, inc);
            }
        }

        box_bot();
        usleep(SLEEP_US);
    }

    printf(BOLDMAG "\n=== [TIMEOUT 시나리오 종료] ===\n" RESET);
    send_end(s, dst);
}

// ------------------------------ MAIN ------------------------------

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <dst_ip> <dst_port> <mode>\n", argv[0]);
        return 1;
    }

    const char* ip = argv[1];
    int port = atoi(argv[2]);
    Mode mode = parse_mode(argv[3]);

    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) die("socket");

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port   = htons(port);
    inet_pton(AF_INET, ip, &dst.sin_addr);

    if (mode == MODE_NORMAL)      run_normal(s, &dst);
    else if (mode == MODE_DUP3)   run_dup3(s, &dst);
    else if (mode == MODE_TIMEOUT)run_timeout(s, &dst);

    close(s);
    return 0;
}