/* Saved-token validation tests. A tiny one-shot HTTP server exercises the
 * real curl transport so the distinction between accepted, rejected, and
 * temporarily unavailable credentials cannot drift away from HTTP behavior. */
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "jellyfin.h"

static int fails;
static int checks;

static void check(const char *label, int got, int expected)
{
    checks++;
    if (got != expected) {
        fails++;
        printf("  FAIL %s: got %d, want %d\n", label, got, expected);
    }
}

static pid_t start_server(int status, const char *body, int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    *port = ntohs(addr.sin_port);

    pid_t pid = fork();
    if (pid != 0) {
        close(fd);
        return pid;
    }

    int client = accept(fd, NULL, NULL);
    if (client >= 0) {
        char request[2048];
        ssize_t received = read(client, request, sizeof(request));
        if (received < 0) _exit(1);
        const char *reason = status == 200 ? "OK" :
                             status == 401 ? "Unauthorized" :
                             status == 403 ? "Forbidden" : "Service Unavailable";
        char response[1024];
        int n = snprintf(response, sizeof(response),
                         "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                         "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                         status, reason, strlen(body), body);
        ssize_t sent = write(client, response, (size_t)n);
        if (sent != n) _exit(1);
        close(client);
    }
    close(fd);
    _exit(0);
}

static void run_case(const char *label, int http_status, const char *body,
                     JfCredentialStatus expected)
{
    int port = 0;
    pid_t server = start_server(http_status, body, &port);
    if (server < 0) {
        check(label, JF_CREDENTIAL_UNAVAILABLE, expected);
        return;
    }

    JfConfig cfg = {0};
    snprintf(cfg.server, sizeof(cfg.server), "http://127.0.0.1:%d", port);
    snprintf(cfg.token, sizeof(cfg.token), "test-token");
    snprintf(cfg.user_id, sizeof(cfg.user_id), "test-user");
    snprintf(cfg.device_id, sizeof(cfg.device_id), "test-device");

    check(label, jf_credential_status(&cfg), expected);
    waitpid(server, NULL, 0);
}

int main(void)
{
    run_case("accepted token", 200, "{\"Items\":[]}", JF_CREDENTIAL_VALID);
    run_case("401 rejects token", 401, "", JF_CREDENTIAL_REJECTED);
    run_case("403 rejects token", 403, "", JF_CREDENTIAL_REJECTED);
    run_case("server error preserves token", 503, "", JF_CREDENTIAL_UNAVAILABLE);
    run_case("malformed success preserves token", 200, "not json", JF_CREDENTIAL_UNAVAILABLE);

    printf("jellyfin auth: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
