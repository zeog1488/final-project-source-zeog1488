#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <syslog.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <signal.h>
#include <pthread.h>
#include <sys/time.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <termios.h>

#define PORT 9000
#define START_LEN 128
#define CRTSCTS 020000000000

volatile sig_atomic_t exitRequested = 0;

static void sigint_handler(int signo)
{
    syslog(LOG_DEBUG, "Caught signal, exiting");
    exitRequested = 1;
}

int readFromSocket(int conn_fd, char *buffer, int buflen)
{
    ssize_t len_recv;
    int pos = 0;
    char *temp = NULL;
    do
    {
        if (exitRequested)
        {
            return 0;
        }

        len_recv = recv(conn_fd, buffer + pos, buflen - pos - 1, 0);
        if (len_recv == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return 0;
            }
            perror("recv");
            printf("Receive failed\n");
            return -1;
        }
        else if (len_recv == buflen - pos - 1)
        {
            buflen += START_LEN;
            pos = buflen - START_LEN - 1;
            temp = (char *)realloc(buffer, buflen);
            if (temp == NULL)
            {
                perror("realloc");
                printf("Realloc failed\n");
                return -1;
            }
            buffer = temp;
        }
        else if (len_recv == 0)
        {
            return -2;
        }
        else
        {
            pos += len_recv;
        }
        *(buffer + (buflen - 1)) = 0;
    } while (strchr(buffer, '\n') == NULL);
    return buflen;
}

int readBytesFromSerial(const int fd, void *const buffer, size_t const bytes)
{
    char *head = (char *)buffer;
    char *const tail = (char *)buffer + bytes;
    int saved_errno;
    ssize_t n;

    saved_errno = errno;

    while (head < tail)
    {

        n = read(fd, head, (size_t)(tail - head));
        if (n > (ssize_t)0)
            head += n;
        else if (n != (ssize_t)-1)
            return errno = ENOENT;
        else if (errno != EINTR)
            return errno;
    }

    errno = saved_errno;
    return 0;
}

int main(int argc, char *argv[])
{
    int socket_fd;
    int conn_fd;
    int reuseaddr = 1;
    struct sockaddr_in server, client;
    char client_str[INET_ADDRSTRLEN];
    socklen_t client_size;
    bool useDaemon;
    pid_t pid;
    int serial_fd;
    char tagBuf[16];
    struct termios serial_port_settings;
    int retval;
    unsigned long bytes_av;

    struct timeval ts;
    ts.tv_sec = 0;
    ts.tv_usec = 50000;

    char *cmdBuffer = NULL;
    int cmdBufferLen = START_LEN;
    int ret;

    if (argc == 2)
    {
        if (strcmp(argv[1], "-d") == 0)
        {
            useDaemon = true;
        }
        else
        {
            printf("Command Line arg not recognized. Only -d can be used.\n");
            exit(-1);
        }
    }
    else
    {
        useDaemon = false;
    }

    openlog("aesdsocket.c", 0, LOG_USER);

    if (signal(SIGINT, sigint_handler) == SIG_ERR)
    {
        perror("signal");
        printf("Couldn't handle SIGINT");
        exit(-1);
    }
    if (signal(SIGTERM, sigint_handler) == SIG_ERR)
    {
        perror("signal");
        printf("Couldn't handle SIGTERM");
        exit(-1);
    }

    socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd == -1)
    {
        perror("socket");
        printf("Socket couldn't be created\n");
        exit(-1);
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuseaddr, sizeof(int)) == -1)
    {
        perror("setsockopt");
        printf("Couldn't set socket reusability\n");
        exit(-1);
    }

    if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &ts, sizeof(ts)) == -1)
    {
        perror("setsockopt");
        printf("Couldn't set socket timeout\n");
        exit(-1);
    }

    fcntl(socket_fd, F_SETFL, O_NONBLOCK);

    memset(&server, 0, sizeof(server));

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    server.sin_port = htons(PORT);

    if ((bind(socket_fd, (struct sockaddr *)&server, sizeof(server))) < 0)
    {
        perror("bind");
        printf("Socket bind failed\n");
        exit(-1);
    }

    if (useDaemon)
    {
        pid = fork();
        if (pid == -1)
        {
            perror("fork");
            printf("Fork failed\n");
            exit(-1);
        }
        else if (pid != 0)
        {
            exit(EXIT_SUCCESS);
        }
    }

    if ((listen(socket_fd, 10)) != 0)
    {
        perror("listen");
        printf("Listen failed\n");
        exit(-1);
    }

    serial_fd = open("/dev/ttyUSB0", O_RDWR | O_NOCTTY | O_SYNC);
    if (serial_fd < 0)
    {
        perror("open");
        printf("Failed to open serial device\n");
        exit(-1);
    }

    struct termios tty;
    if (tcgetattr(serial_fd, &tty) != 0)
    {
        perror("tcgetattr");
        return -1;
    }

    cfsetospeed(&tty, B9600);
    cfsetispeed(&tty, B9600);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
    // disable IGNBRK for mismatched speed tests; otherwise receive break
    // as \000 chars
    tty.c_iflag &= ~IGNBRK; // disable break processing
    tty.c_lflag = 0;        // no signaling chars, no echo,
                            // no canonical processing
    tty.c_oflag = 0;        // no remapping, no delays
    tty.c_cc[VMIN] = 0;     // read doesn't block
    tty.c_cc[VTIME] = 5;    // 0.5 seconds read timeout

    tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl

    tty.c_cflag |= (CLOCAL | CREAD);   // ignore modem controls,
                                       // enable reading
    tty.c_cflag &= ~(PARENB | PARODD); // shut off parity
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(serial_fd, TCSANOW, &tty) != 0)
    {
        perror("tcsetattr");
        exit(-1);
    }

    client_size = sizeof(client);
    while (exitRequested == 0)
    {
        conn_fd = accept(socket_fd, (struct sockaddr *)&client, &client_size);
        if (conn_fd == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }
            perror("accept");
            printf("Accept failed\n");
            exit(-1);
        }

        inet_ntop(AF_INET, &(client.sin_addr), client_str, INET_ADDRSTRLEN);
        syslog(LOG_DEBUG, "Accepted connection from %s", client_str);

        cmdBufferLen = START_LEN;
        cmdBuffer = (char *)malloc(sizeof(char) * cmdBufferLen);
        if (cmdBuffer == NULL)
        {
            perror("malloc");
            printf("buffer malloc failed\n");
            exit(-1);
        }

        while (exitRequested == 0)
        {
            memset(cmdBuffer, 0, cmdBufferLen);
            ret = readFromSocket(conn_fd, cmdBuffer, cmdBufferLen);
            if (ret == 0)
            {
                memset(tagBuf, 0, sizeof(tagBuf));
                retval = read(serial_fd, tagBuf, 14);
                if (retval > 0)
                {
                    tagBuf[11] = '\n';
                    char temp[] = "Tag received: ";
                    write(conn_fd, temp, strlen(temp));
                    write(conn_fd, tagBuf + 3, 8);
                }
                continue;
            }
            else if (ret == -1)
            {
                free(cmdBuffer);
                exit(-1);
            }
            else if (ret == -2)
            {
                break;
            }
            else
            {
                cmdBufferLen = ret;
                if (strcmp(cmdBuffer, "ADD\n") == 0)
                {
                    char temp[] = "Add tag command received\n";
                    write(conn_fd, temp, strlen(temp));
                }
                else if (strcmp(cmdBuffer, "DELETE\n") == 0)
                {
                    char temp[] = "Delete tag command received\n";
                    write(conn_fd, temp, strlen(temp));
                }
                else if (strcmp(cmdBuffer, "EDIT\n") == 0)
                {
                    char temp[] = "Edit tag command received\n";
                    write(conn_fd, temp, strlen(temp));
                }
                else
                {
                    char temp[] = "Unrecognized command\n";
                    write(conn_fd, temp, strlen(temp));
                }
            }
        }
    }
    close(socket_fd);
    close(serial_fd);
}