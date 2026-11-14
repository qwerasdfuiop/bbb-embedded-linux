// bbb_server.c

/* IP 주소 변환 및 TCP/IP 통신에 필요한 헤더 */
#include <arpa/inet.h>

/* pthread, Mutex, Condition Variable 사용 */
#include <pthread.h>

/* bool, true, false 사용 */
#include <stdbool.h>

/* printf(), perror() 사용 */
#include <stdio.h>

/* EXIT_SUCCESS, EXIT_FAILURE 사용 */
#include <stdlib.h>

/* strcmp(), strlen(), snprintf() 사용 */
#include <string.h>

/* socket(), bind(), listen(), accept(), recv(), send() 사용 */
#include <sys/socket.h>

/* close() 사용 */
#include <unistd.h>

#include <fcntl.h>

/* BBB TCP 서버가 사용할 포트 번호 */
#define PORT 5000

/* 명령과 응답 문자열을 저장할 버퍼 크기 */
#define BUFFER_SIZE 128


/* =========================================================
 * 여러 스레드가 함께 사용하는 공유 변수
 * ========================================================= */

/* accept()로 연결된 PC 클라이언트의 소켓 번호 */
static int client_fd;

/*
 * 프로그램 실행 여부
 *
 * true  : 스레드 계속 실행
 * false : 모든 스레드 종료
 */
static bool running = true;


/*
 * PC에서 받은 명령을 저장한다.
 *
 * 예:
 * "LED ON"
 * "LED OFF"
 */
static char command[BUFFER_SIZE];

/*
 * command에 새로운 명령이 들어왔는지 나타낸다.
 *
 * true  : 처리할 명령이 있음
 * false : 처리할 명령이 없음
 */
static bool command_ready = false;


/*
 * command_thread가 해석한 LED 동작을 저장한다.
 *
 *  1 : LED 켜기
 *  0 : LED 끄기
 * -1 : 잘못된 명령
 */
static int requested_led;

/* GPIO 제어 스레드가 처리할 동작이 있는지 표시 */
static bool action_ready = false;


/*
 * PC로 보낼 처리 결과
 *
 * 예:
 * "LED ON\n"
 * "LED OFF\n"
 * "ERROR: invalid command\n"
 */
static char status[BUFFER_SIZE];

/* PC로 보낼 새로운 처리 결과가 있는지 표시 */
static bool status_ready = false;


/*
 * 공유 변수를 보호하는 Mutex
 *
 * 여러 스레드가 command, status 등의 공유 변수에
 * 동시에 접근하지 못하도록 잠근다.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * 공유 데이터의 상태가 변경됐다는 것을
 * 대기 중인 스레드에 알려주는 Condition Variable
 */
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;


/*
 * 모든 스레드에 종료를 알리는 함수
 *
 * 클라이언트 연결이 끊어지거나 통신 오류가 발생하면 호출한다.
 */
static void stop_threads(void)
{
    /* running은 공유 변수이므로 먼저 Mutex를 잠근다. */
    pthread_mutex_lock(&lock);

    running = false;

    /*
     * pthread_cond_wait()에서 잠들어 있는
     * 모든 스레드를 깨운다.
     *
     * 깨어난 스레드는 running이 false인지 확인하고 종료한다.
     */
    pthread_cond_broadcast(&changed);

    pthread_mutex_unlock(&lock);
}


/* =========================================================
 * 스레드 1: PC에서 TCP 명령 수신
 * ========================================================= */
static void *receive_thread(void *arg)
{
    /* 사용하지 않는 매개변수 경고 방지 */
    (void)arg;

    char buffer[BUFFER_SIZE];

    while (true) {
        /*
         * 클라이언트가 보낸 문자열을 받을 때까지 기다린다.
         *
         * recv() 반환값:
         *  1 이상 : 받은 데이터 크기
         *  0      : 클라이언트가 연결 종료
         * -1      : 통신 오류
         */
        ssize_t size = recv(
            client_fd,
            buffer,
            sizeof(buffer) - 1,
            0
        );

        if (size <= 0) {
            printf("Client disconnected.\n");
            stop_threads();
            break;
        }

        /*
         * recv()가 받은 데이터는 C 문자열이 아닐 수 있으므로
         * 마지막에 '\0'을 추가한다.
         */
        buffer[size] = '\0';

        /*
         * nc에서 Enter를 입력하면 \n 또는 \r\n이 포함된다.
         * strcmp() 비교가 가능하도록 줄바꿈 문자를 제거한다.
         */
        buffer[strcspn(buffer, "\r\n")] = '\0';

        /* 공유 변수 command를 변경하기 전에 Mutex를 잠근다. */
        pthread_mutex_lock(&lock);

        /*
         * 이전 명령이 아직 처리되지 않았다면 기다린다.
         *
         * pthread_cond_wait()는 기다리는 동안 Mutex를 자동으로 풀고,
         * 깨어났을 때 다시 Mutex를 잠근다.
         */
        while (command_ready && running)
            pthread_cond_wait(&changed, &lock);

        if (running) {
            /* 수신한 문자열을 공유 명령 버퍼에 복사한다. */
            snprintf(command, sizeof(command), "%s", buffer);

            /* 처리할 명령이 있다고 표시한다. */
            command_ready = true;

            /*
             * 명령을 기다리는 command_thread를 깨운다.
             */
            pthread_cond_broadcast(&changed);
        }

        pthread_mutex_unlock(&lock);

        if (!running)
            break;
    }

    return NULL;
}


/* =========================================================
 * 스레드 2: 문자열 명령 해석
 * ========================================================= */
static void *command_thread(void *arg)
{
    (void)arg;

    while (true) {
        int action;

        pthread_mutex_lock(&lock);

        /*
         * 새로운 명령이 들어올 때까지 기다린다.
         *
         * receive_thread가 command_ready를 true로 바꾸면
         * 이 스레드가 깨어난다.
         */
        while (!command_ready && running)
            pthread_cond_wait(&changed, &lock);

        if (!running) {
            pthread_mutex_unlock(&lock);
            break;
        }

        /*
         * 문자열 명령을 숫자 동작으로 변환한다.
         *
         * strcmp()는 두 문자열이 같으면 0을 반환한다.
         */
        if (strcmp(command, "LED ON") == 0) {
            action = 1;
        } else if (strcmp(command, "LED OFF") == 0) {
            action = 0;
        } else {
            action = -1;
        }

        /*
         * 현재 명령 처리가 끝났으므로
         * receive_thread가 다음 명령을 저장할 수 있게 한다.
         */
        command_ready = false;
        pthread_cond_broadcast(&changed);

        /*
         * 이전 GPIO 동작이 아직 처리 중이라면 기다린다.
         */
        while (action_ready && running)
            pthread_cond_wait(&changed, &lock);

        if (!running) {
            pthread_mutex_unlock(&lock);
            break;
        }

        /* 해석한 LED 동작을 공유 변수에 저장한다. */
        requested_led = action;
        action_ready = true;

        /*
         * 동작을 기다리는 control_thread를 깨운다.
         */
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    }

    return NULL;
}


/* =========================================================
 * 실제 GPIO를 제어할 함수
 * ========================================================= */

/*
 * 7세그먼트 표시 상태. 드라이버 bbb_led.h의 BBB_LED_STATE_*와
 * 값이 일치해야 한다.
 *
 * DISPLAY_BLANK : 소등        (LED OFF)
 * DISPLAY_ON    : " On " 표시 (LED ON)
 * DISPLAY_ERR   : "Err " 표시 (잘못된 명령)
 */
enum display_state {
    DISPLAY_BLANK = 0,
    DISPLAY_ON    = 1,
    DISPLAY_ERR   = 2
};

 static void apply_gpio(int state)
 {
     static const char cmds[] = { '0', '1', '2' };
     static const char *names[] = { "OFF", "ON", "ERR" };
     static int led_fd = -1;   /* control_thread만 호출하므로 별도 잠금 불필요 */

     /* -1 같은 내부 센티넬이 드라이버로 새어 나가지 않도록 차단 */
     if (state < DISPLAY_BLANK || state > DISPLAY_ERR)
         return;

     if (led_fd < 0)
         led_fd = open("/dev/bbb_led", O_WRONLY);
     if (led_fd < 0 || write(led_fd, &cmds[state], 1) != 1) {
         perror("bbb_led");
         printf("[GPIO] DISPLAY %s\n", names[state]);  /* 폴백 */
         return;
     }
     printf("[GPIO] DISPLAY %s\n", names[state]);
 }


/* =========================================================
 * 스레드 3: GPIO 제어
 * ========================================================= */
static void *control_thread(void *arg)
{
    (void)arg;

    while (true) {
        pthread_mutex_lock(&lock);

        /*
         * command_thread가 명령 해석을 완료할 때까지 기다린다.
         */
        while (!action_ready && running)
            pthread_cond_wait(&changed, &lock);

        if (!running) {
            pthread_mutex_unlock(&lock);
            break;
        }

        /* 처리할 LED 값을 지역 변수로 복사한다. */
        int value = requested_led;

        /*
         * 공유된 동작을 가져왔으므로 처리 대기 상태를 해제한다.
         */
        action_ready = false;
        pthread_cond_broadcast(&changed);

        /*
         * 이전 결과가 아직 PC로 전송되지 않았다면 기다린다.
         */
        while (status_ready && running)
            pthread_cond_wait(&changed, &lock);

        if (!running) {
            pthread_mutex_unlock(&lock);
            break;
        }

        if (value == 0 || value == 1) {
            /* 정상 명령이면 GPIO를 제어한다. */
            apply_gpio(value);

            /* PC로 보낼 결과 문자열을 만든다. */
            snprintf(
                status,
                sizeof(status),
                "LED %s\n",
                value ? "ON" : "OFF"
            );
        } else {
            /* LED ON/OFF가 아닌 명령: 표시기에 Err를 띄운다. */
            apply_gpio(DISPLAY_ERR);

            /* PC로 보낼 결과 문자열을 만든다. */
            snprintf(
                status,
                sizeof(status),
                "ERROR: invalid command\n"
            );
        }

        /* 새로운 전송 결과가 있다고 표시한다. */
        status_ready = true;

        /* status_thread를 깨운다. */
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);
    }

    return NULL;
}


/* =========================================================
 * 스레드 4: 처리 결과를 PC로 전송
 * ========================================================= */
static void *status_thread(void *arg)
{
    (void)arg;

    char response[BUFFER_SIZE];

    while (true) {
        pthread_mutex_lock(&lock);

        /*
         * control_thread가 처리 결과를 만들 때까지 기다린다.
         */
        while (!status_ready && running)
            pthread_cond_wait(&changed, &lock);

        if (!running) {
            pthread_mutex_unlock(&lock);
            break;
        }

        /*
         * 공유 버퍼의 내용을 지역 버퍼로 복사한다.
         *
         * 이렇게 하면 Mutex를 푼 이후에도
         * response의 내용은 다른 스레드의 영향을 받지 않는다.
         */
        snprintf(response, sizeof(response), "%s", status);

        /*
         * 현재 결과를 가져갔으므로
         * 다음 결과를 저장할 수 있게 한다.
         */
        status_ready = false;
        pthread_cond_broadcast(&changed);
        pthread_mutex_unlock(&lock);

        /*
         * 처리 결과를 TCP 클라이언트로 보낸다.
         *
         * MSG_NOSIGNAL:
         * 연결이 끊어진 소켓에 send()를 호출했을 때
         * SIGPIPE로 프로그램이 강제 종료되는 것을 방지한다.
         */
        if (send(
                client_fd,
                response,
                strlen(response),
                MSG_NOSIGNAL
            ) <= 0) {
            stop_threads();
            break;
        }
    }

    return NULL;
}


/* =========================================================
 * 프로그램 시작 지점
 * ========================================================= */
int main(void)
{
    int server_fd;

    /*
     * 서버 주소 설정
     *
     * AF_INET     : IPv4 사용
     * INADDR_ANY  : BBB의 모든 네트워크 인터페이스에서 접속 허용
     * htons(PORT) : CPU 바이트 순서를 네트워크 바이트 순서로 변환
     */
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(PORT)
    };

    /*
     * TCP 소켓 생성
     *
     * AF_INET     : IPv4
     * SOCK_STREAM : TCP
     * 0           : TCP 기본 프로토콜 자동 선택
     */
    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    /*
     * 생성한 소켓에 BBB의 주소와 포트 번호를 연결한다.
     */
    if (bind(
            server_fd,
            (struct sockaddr *)&address,
            sizeof(address)
        ) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    /*
     * 클라이언트 접속을 기다리는 서버 소켓으로 변경한다.
     *
     * 두 번째 인자 1은 대기 가능한 연결 수다.
     */
    if (listen(server_fd, 1) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Waiting on TCP port %d...\n", PORT);

    /*
     * PC가 접속할 때까지 여기서 대기한다.
     *
     * 접속되면 통신에 사용할 새로운 소켓 번호가 반환된다.
     */
    client_fd = accept(server_fd, NULL, NULL);

    if (client_fd < 0) {
        perror("accept");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Client connected.\n");

    /* 네 개 스레드의 식별자를 저장한다. */
    pthread_t threads[4];

    /* 각각의 작업을 담당하는 스레드를 생성한다. */
    pthread_create(&threads[0], NULL, receive_thread, NULL);
    pthread_create(&threads[1], NULL, command_thread, NULL);
    pthread_create(&threads[2], NULL, control_thread, NULL);
    pthread_create(&threads[3], NULL, status_thread, NULL);

    /*
     * 네 스레드가 모두 종료될 때까지 main 함수가 기다린다.
     */
    for (int i = 0; i < 4; ++i)
        pthread_join(threads[i], NULL);

    /* 사용이 끝난 소켓을 닫는다. */
    close(client_fd);
    close(server_fd);

    return EXIT_SUCCESS;
}

