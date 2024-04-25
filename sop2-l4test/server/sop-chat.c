#include "l4-common.h"
#include <string.h>
#include <sys/epoll.h>

#define BACKLOG_SIZE 10
#define MAX_CLIENT_COUNT 4
#define MAX_EVENTS 10

#define NAME_OFFSET 0
#define NAME_SIZE 64
#define MESSAGE_OFFSET NAME_SIZE
#define MESSAGE_SIZE 448
#define BUFF_SIZE (NAME_SIZE + MESSAGE_SIZE)

void usage(char *program_name) {
    fprintf(stderr, "USAGE: %s port key\n", program_name);
    exit(EXIT_FAILURE);
}

struct Message {
    char username[NAME_SIZE];
    char message[MESSAGE_SIZE];
};

struct connection{
    int clientFd;
    int active;
};

volatile int do_work = 1;

int set_nonblock(int);
void doServer(int serverSocket, char* key);
void manageClients(int serverSocket, int* clients, struct connection* connected, char* key, struct epoll_event* ev, int epoll_fd);
void ReceiveMsg(char buffer[BUFF_SIZE], struct Message* authMsg, int clientSocket, int* clients, struct connection connected[MAX_CLIENT_COUNT], int epoll_fd);
void BroadcastMsg(char msg[BUFF_SIZE], struct connection connected[MAX_CLIENT_COUNT], int self_fd, int* clients);
void setAsInactive(struct connection connected[MAX_CLIENT_COUNT], int fd);
void setAsActive(struct connection connected[MAX_CLIENT_COUNT], int fd);
void sigint_handler() { do_work = 0; }
DisconnectAll(struct connection connected[MAX_CLIENT_COUNT], int* clients);


int main(int argc, char **argv) {
    // GET INPUT:
    char *program_name = argv[0];
    if (argc != 3) {
        usage(program_name);
    }

    uint16_t port = atoi(argv[1]);
    if (port == 0){
        usage(argv[0]);
    }

    char *key = argv[2];

    // Create the server socket
    int serverSocket = bind_tcp_socket(port, BACKLOG_SIZE);

    if (sethandler(SIG_IGN, SIGPIPE))
        ERR("Seting SIGPIPE:");
    if (sethandler(sigint_handler, SIGINT))
        ERR("Seting SIGINT:");

    doServer(serverSocket, key);

    // Close the socket
    close(serverSocket);

    return EXIT_SUCCESS;
}

void doServer(int serverSocket, char* key)
{
    int clients = 0;
    // initialize array of connected clients file descriptors
    struct connection connected[MAX_CLIENT_COUNT];
    for(int i = 0; i < MAX_CLIENT_COUNT; i++) connected[i].active = 0;

    // Create an epoll instance 
    int epoll_fd = epoll_create(100);
    // Create an event that occurs when sb tries to connect to the server
    struct epoll_event ev;
    ev.data.fd = serverSocket;
    ev.events = EPOLLIN; // enables reading
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, serverSocket, &ev) < 0)
        ERR("epoll_ctl");

    // each field in the array holds information about:
    //      .event - type of event as a bitmask
    //      .data  - ie fd associated with the event
    struct epoll_event events[MAX_EVENTS];

    sigset_t mask, oldmask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, &oldmask);

    while(do_work)
    {
        printf("Calling epoll_wait()!\n");
        // returns how many events occured and puts them in events array
        int evNumber = epoll_pwait(epoll_fd, events, MAX_EVENTS, -1, &oldmask);
        if (evNumber < 0) 
            ERR("epoll_pwait");

        for (int i = 0; i < evNumber; ++i) { // for each event
            int fd = events[i].data.fd; // fd to socket the event is associated with

            if (events[i].events & EPOLLIN) { // if the event means there's sth new to read
                if (fd == serverSocket)
                {   // this event is associated with the server
                    // so it means that sb is trying to connect
                    // or closed the connection
                    manageClients(serverSocket, &clients, connected, key, &ev, epoll_fd);
                }
                else {
                    char buff[BUFF_SIZE];
                    struct Message msg;
                    ReceiveMsg(buff, &msg, fd, &clients, connected, epoll_fd);
                    printf("User %s sent: %s\n", msg.username, msg.message);
                    BroadcastMsg(buff, connected, fd, &clients);
                }
            }
        }
    }

    DisconnectAll(connected, &clients);
}

DisconnectAll(struct connection connected[MAX_CLIENT_COUNT], int* clients)
{
    for (int i = 0; i < MAX_CLIENT_COUNT; i++)
    {
        (*clients)--;
        close(connected[i].clientFd);
    }
}

void manageClients(int serverSocket, int* clients, struct connection* connected, char* key, struct epoll_event* ev, int epoll_fd)
{
    // accept the new client
    int clientSocket = add_new_client(serverSocket);

    if (clientSocket < 0)
        ERR("accept()");

    // Get the authentication message
    char buffer[BUFF_SIZE];
    struct Message authMsg;
    ReceiveMsg(buffer, &authMsg, clientSocket, clients, connected, epoll_fd);
    
    printf("Uzytkownik: %s probuje nawiazac polaczenie. Podane haslo: %s\n", authMsg.username, authMsg.message);
    
    // if it's already packed throw the client trying to connect out
    if (*clients >= MAX_CLIENT_COUNT)
    {
        printf("Uzytkownik %s zostal odrzucony - za duzo klientow\n", authMsg.username);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, clientSocket, NULL) < 0)
            ERR("epoll_ctl");
        close(clientSocket);
        return;
    }

    // Check if the password is correct
    if (strcmp(key, authMsg.message) != 0) // strcmp returns 0 if they're the same
    {
        printf("Uzytkownik: %s zostal odrzucony - zle haslo\n", authMsg.username);
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, clientSocket, NULL) < 0)
            ERR("epoll_ctl");
        close(clientSocket);
        return;
    }

    // Keep the client 
    (*clients)++;
    printf("Uzytkownik: %s nawiazal polaczenie. Liczba polaczonych klientow: %d\n", authMsg.username, *clients);
   
    setAsActive(connected, clientSocket);
    
    // Send back the auth. message
    bulk_write(clientSocket, buffer, BUFF_SIZE);

    // give epoll the info about the event associated with the new client
    (*ev).data.fd = clientSocket;
    (*ev).events = EPOLLIN;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, clientSocket, ev) < 0)
        ERR("epoll_ctl");
}

void ReceiveMsg(char buffer[BUFF_SIZE], struct Message* authMsg, int clientSocket, int* clients, struct connection connected[MAX_CLIENT_COUNT], int epoll_fd)
{
    // Receive the authentication message
    int size;
    if (size = bulk_read(clientSocket, buffer, BUFF_SIZE) == -1 && errno == ECONNRESET)
    {
        (*clients)--;
        setAsInactive(connected, clientSocket);
        close(clientSocket);
    }

    if (size == 0)
    {   //client disconnected
        (*clients)--;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, clientSocket, NULL) < 0)
            ERR("epoll_ctl");
        setAsInactive(connected, clientSocket);
        return;
    }
    memcpy((*authMsg).username, buffer + NAME_OFFSET, NAME_SIZE);
    memcpy((*authMsg).message, buffer + MESSAGE_OFFSET, MESSAGE_SIZE);
    (*authMsg).username[NAME_SIZE - 1] = '\0';
    (*authMsg).message[MESSAGE_SIZE - 1] = '\0';
}

void setAsInactive(struct connection connected[MAX_CLIENT_COUNT], int fd)
{
    for (int i = 0; i < MAX_CLIENT_COUNT; i++)
    {
        if (fd == connected[i].clientFd)
            connected[i].active = 0;
    }
}

void setAsActive(struct connection connected[MAX_CLIENT_COUNT], int fd)
{
    for (int i = 0; i < MAX_CLIENT_COUNT; i++)
    {
        if (connected[i].active == 0)
        {
            connected[i].clientFd = fd;
            connected[i].active = 1;
            return;
        }
    }
}

void BroadcastMsg(char msg[BUFF_SIZE], struct connection connected[MAX_CLIENT_COUNT], int self_fd, int* clients)
{
    for (int i = 0; i < MAX_CLIENT_COUNT; i++)
    {
        if (connected[i].active == 0)
            continue;
        if (connected[i].clientFd == self_fd)
            continue;
        if (bulk_write(connected[i].clientFd, msg, BUFF_SIZE)== -1 && (errno == EPIPE || errno == ECONNRESET))
        {
            (*clients)--;
            setAsInactive(connected, connected[i].clientFd);
            close(connected[i].clientFd);
        }

    }
}

int set_nonblock(int desc) {
  int oldflags = fcntl(desc, F_GETFL, 0);
  if (oldflags == -1)
    return -1;
  oldflags |= O_NONBLOCK;
  return fcntl(desc, F_SETFL, oldflags);
}