// proxy.cpp
#include "proxy_parse.h"
#include <iostream>
#include <sstream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <pthread.h>
#include <semaphore.h>
#include <chrono>
using namespace std::chrono;
using namespace std;

#define MAX_BYTES 4096
#define MAX_CLIENTS 400
#define MAX_SIZE 200*(1<<20)
#define MAX_ELEMENT_SIZE 10*(1<<20)

typedef struct cache_element cache_element;

struct cache_element {
    string data;
    int len;
    string url;
    time_t lru_time_track;
    cache_element* next;
};

cache_element* find(char* url);
int add_cache_element(char* data, int size, char* url);
void remove_cache_element();

int port_number = 8080;
int proxy_socketId;
pthread_t tid[MAX_CLIENTS];
sem_t seamaphore;
pthread_mutex_t lock;

cache_element* head = nullptr;
int cache_size = 0;

// Cache stats
int cache_hits = 0;
int cache_misses = 0;

int sendErrorMessage(int socket, int status_code) {
    char str[1024];
    char currentTime[50];
    time_t now = time(0);

    struct tm data = *gmtime(&now);
    strftime(currentTime,sizeof(currentTime),"%a, %d %b %Y %H:%M:%S %Z", &data);

    switch(status_code) {
        case 400:
            snprintf(str, sizeof(str),
                "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Rqeuest</H1>\n</BODY></HTML>",
                currentTime);
            cout << "400 Bad Request" << endl;
            send(socket, str, strlen(str), 0);
            break;
        case 403:
            snprintf(str, sizeof(str),
                "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>",
                currentTime);
            cout << "403 Forbidden" << endl;
            send(socket, str, strlen(str), 0);
            break;
        case 404:
            snprintf(str, sizeof(str),
                "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>",
                currentTime);
            cout << "404 Not Found" << endl;
            send(socket, str, strlen(str), 0);
            break;
        case 500:
            snprintf(str, sizeof(str),
                "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>",
                currentTime);
            send(socket, str, strlen(str), 0);
            break;
        case 501:
            snprintf(str, sizeof(str),
                "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>404 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>",
                currentTime);
            cout << "501 Not Implemented" << endl;
            send(socket, str, strlen(str), 0);
            break;
        case 505:
            snprintf(str, sizeof(str),
                "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: %s\r\nServer: VaibhavN/14785\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>",
                currentTime);
            cout << "505 HTTP Version Not Supported" << endl;
            send(socket, str, strlen(str), 0);
            break;
        default:  return -1;
    }
    return 1;
}

int connectRemoteServer(char* host_addr, int port_num) {
    int remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(remoteSocket < 0) {
        cout << "Error in Creating Socket." << endl;
        return -1;
    }
    struct hostent *host = gethostbyname(host_addr);
    if(host == NULL) {
        cerr << "No such host exists." << endl;
        return -1;
    }
    struct sockaddr_in server_addr;
    memset((char*)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    memcpy((char *)&server_addr.sin_addr.s_addr, (char *)host->h_addr, host->h_length);
    if(connect(remoteSocket, (struct sockaddr*)&server_addr, (socklen_t)sizeof(server_addr)) < 0) {
        cerr << "Error in connecting !" << endl;
        return -1;
    }
    return remoteSocket;
}

int handle_request(int clientSocket, ParsedRequest *request, char *tempReq) {
    auto remote_start = chrono::high_resolution_clock::now();

    string sendReq;
    sendReq.reserve(MAX_BYTES);
    sendReq.append("GET ");
    sendReq.append(request->path ? request->path : "");
    sendReq.append(" ");
    sendReq.append(request->version ? request->version : "");
    sendReq.append("\r\n");

    if (ParsedHeader_set(request, "Connection", "close") < 0){
        cout << "set header key not work" << endl;
    }
    if(ParsedHeader_get(request, "Host") == NULL) {
        if(ParsedHeader_set(request, "Host", request->host) < 0){
            cout << "Set \"Host\" header key not working" << endl;
        }
    }
    char header_buf[MAX_BYTES];
    memset(header_buf, 0, sizeof(header_buf));
    size_t len = sendReq.size();
    ParsedRequest_unparse_headers(request, header_buf, (size_t)MAX_BYTES - len);
    sendReq.append(header_buf);

    int server_port = 80;
    if(request->port != NULL)
        server_port = atoi(request->port);

    int remoteSocketID = connectRemoteServer(request->host, server_port);
    if(remoteSocketID < 0)
        return -1;

    send(remoteSocketID, sendReq.c_str(), (size_t)sendReq.size(), 0);

    char recv_buf[MAX_BYTES];
    memset(recv_buf, 0, sizeof(recv_buf));

    ssize_t bytes_recv = recv(remoteSocketID, recv_buf, MAX_BYTES-1, 0);
    string temp_buffer;
    temp_buffer.reserve(1024);

    while(bytes_recv > 0) {
        send(clientSocket, recv_buf, (size_t)bytes_recv, 0);
        temp_buffer.append(recv_buf, (size_t)bytes_recv);
        memset(recv_buf, 0, sizeof(recv_buf));
        bytes_recv = recv(remoteSocketID, recv_buf, MAX_BYTES-1, 0);
    }
    if(!temp_buffer.empty()) {
        add_cache_element(const_cast<char*>(temp_buffer.c_str()), (int)temp_buffer.size(), tempReq);
    }

   auto remote_end = chrono::high_resolution_clock::now();
cout << "[REMOTE FETCH TIME] "
     << chrono::duration_cast<chrono::microseconds>(remote_end - remote_start).count()
     << " µs" << endl;


    close(remoteSocketID);
    return 0;
}

cache_element* find(char* url) {
    auto cache_start = chrono::high_resolution_clock::now();
    cache_element* site = NULL;
    int temp_lock_val = pthread_mutex_lock(&lock);
    if(head != NULL){
        site = head;
        while (site != NULL){
            if(!site->url.compare(url)){
                cache_hits++;
                cout << "[CACHE HIT] URL found in cache" << endl;
                site->lru_time_track = time(NULL);
                break;
            }
            site = site->next;
        }
    }
    if(site == NULL){
        cache_misses++;
        cout << "[CACHE MISS] URL not found in cache" << endl;
    }
    pthread_mutex_unlock(&lock);
    auto cache_end = chrono::high_resolution_clock::now();
cout << "[CACHE LOOKUP TIME] "
     << chrono::duration_cast<chrono::microseconds>(cache_end - cache_start).count()
     << " µs" << endl;

    cout << "[Cache Stats] Hits: " << cache_hits << " | Misses: " << cache_misses << endl;
    return site;
}

int checkHTTPversion(char *msg)
{
    int version = -1;

    if(msg == nullptr) return -1;
    if(strncmp(msg, "HTTP/1.1", 8) == 0)
    {
        version = 1;
    }
    else if(strncmp(msg, "HTTP/1.0", 8) == 0)
    {
        version = 1;                                        // Handling this similar to version 1.1
    }
    else
        version = -1;

    return version;
}


void* thread_fn(void* socketNew)
{
    sem_wait(&seamaphore);
    int p;
    sem_getvalue(&seamaphore,&p);
    cout << "semaphore value:" << p << endl;

    int* t= (int*)(socketNew);
    int socket=*t;           // Socket is socket descriptor of the connected Client
    int bytes_send_client,len;      // Bytes Transferred


    char *buffer = (char*)calloc(MAX_BYTES,sizeof(char));    // Creating buffer of 4kb for a client


    memset(buffer, 0, MAX_BYTES);                             // Making buffer zero
    bytes_send_client = recv(socket, buffer, MAX_BYTES, 0); // Receiving the Request of client by proxy server

    while(bytes_send_client > 0)
    {
        len = (int)strlen(buffer);
        //loop until u find "\r\n\r\n" in the buffer
        if(strstr(buffer, "\r\n\r\n") == NULL)
        {
            bytes_send_client = recv(socket, buffer + len, MAX_BYTES - len, 0);
        }
        else{
            break;
        }
    }

    // copy buffer into a C-style tempReq (because we keep ParsedRequest expecting C-strings)
    // But we will keep it as a std::string where possible.
    string bufferStr;
    bufferStr.assign(buffer, strlen(buffer)); // copy only meaningful part

    // tempReq to pass to cache functions (they expect char*)
    // We will store it in std::string and pass c_str() where needed.
    string tempReqStr = bufferStr;

    //checking for the request in cache
    cache_element* temp = find(const_cast<char*>(tempReqStr.c_str()));

    if( temp != NULL){
        //request found in cache, so sending the response to client from proxy's cache
        int size=temp->len/sizeof(char);
        int pos=0;
        char response[MAX_BYTES];
        while(pos<size){
            memset(response,0,MAX_BYTES);
            int tosend = 0;
            for(int i=0;i<MAX_BYTES && pos < size;i++){
                response[i]= temp->data[pos];
                pos++;
                tosend++;
            }
            send(socket,response,tosend,0);
        }
        cout << "Data retrived from the Cache" << endl << endl;
        // print last chunk in response variable (same as original)
        cout << response << endl << endl;
        shutdown(socket, SHUT_RDWR);
        close(socket);
        free(buffer);
        sem_post(&seamaphore);
        sem_getvalue(&seamaphore,&p);
        cout << "Semaphore post value:" << p << endl;
        return NULL;
    }

    else if(bytes_send_client > 0)
    {
        len = (int)strlen(buffer);
        //Parsing the request
        ParsedRequest* request = ParsedRequest_create();

        //ParsedRequest_parse returns 0 on success and -1 on failure.On success it stores parsed request in
        // the request
        if (ParsedRequest_parse(request, buffer, len) < 0)
        {
            cout << "Parsing failed" << endl;
        }
        else
        {
            memset(buffer, 0, MAX_BYTES);
            if(!strcmp(request->method,"GET"))
            {
                if( request->host && request->path && (checkHTTPversion(request->version) == 1) )
                {
                    bytes_send_client = handle_request(socket, request, const_cast<char*>(tempReqStr.c_str()));        // Handle GET request
                    if(bytes_send_client == -1)
                    {
                        sendErrorMessage(socket, 500);
                    }
                }
                else
                    sendErrorMessage(socket, 500);            // 500 Internal Error
            }
            else
            {
                cout << "This code doesn't support any method other than GET" << endl;
            }
        }
        //freeing up the request pointer
        ParsedRequest_destroy(request);

    }

    else if( bytes_send_client < 0)
    {
        perror("Error in receiving from client.\n");
    }
    else if(bytes_send_client == 0)
    {
        cout << "Client disconnected!" << endl;
    }

    shutdown(socket, SHUT_RDWR);
    close(socket);
    free(buffer);
    sem_post(&seamaphore);

    sem_getvalue(&seamaphore,&p);
    cout << "Semaphore post value:" << p << endl;
    return NULL;
}


int main(int argc, char * argv[]) {

    int client_socketId, client_len; // client_socketId == to store the client socket id
    struct sockaddr_in server_addr, client_addr; // Address of client and server to be assigned

    sem_init(&seamaphore,0,MAX_CLIENTS); // Initializing seamaphore and lock
    pthread_mutex_init(&lock,NULL); // Initializing lock for cache


    if(argc == 2)        //checking whether two arguments are received or not
    {
        port_number = atoi(argv[1]);
    }
    else
    {
        cout << "Too few arguments" << endl;
        exit(1);
    }

    cout << "Setting Proxy Server Port : " << port_number << endl;

    //creating the proxy socket
    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);

    if( proxy_socketId < 0)
    {
        perror("Failed to create socket.\n");
        exit(1);
    }

    int reuse =1;
    if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
        perror("setsockopt(SO_REUSEADDR) failed\n");

    memset((char*)&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_number); // Assigning port to the Proxy
    server_addr.sin_addr.s_addr = INADDR_ANY; // Any available adress assigned

    // Binding the socket
    if( bind(proxy_socketId, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0 )
    {
        perror("Port is not free\n");
        exit(1);
    }
    cout << "Binding on port: " << port_number << endl;

    // Proxy socket listening to the requests
    int listen_status = listen(proxy_socketId, MAX_CLIENTS);

    if(listen_status < 0 )
    {
        perror("Error while Listening !\n");
        exit(1);
    }

    int i = 0; // Iterator for thread_id (tid) and Accepted Client_Socket for each thread
    int Connected_socketId[MAX_CLIENTS];   // This array stores socket descriptors of connected clients

    // Infinite Loop for accepting connections
    while(1)
    {

        memset((char*)&client_addr, 0, sizeof(client_addr));            // Clears struct client_addr
        client_len = sizeof(client_addr);

        // Accepting the connections
        client_socketId = accept(proxy_socketId, (struct sockaddr*)&client_addr,(socklen_t*)&client_len);    // Accepts connection
        if(client_socketId < 0)
        {
            cerr << "Error in Accepting connection !" << endl;
            exit(1);
        }
        else{
            Connected_socketId[i] = client_socketId; // Storing accepted client into array
        }

        // Getting IP address and port number of client
        struct sockaddr_in* client_pt = (struct sockaddr_in*)&client_addr;
        struct in_addr ip_addr = client_pt->sin_addr;
        char str[INET_ADDRSTRLEN];                                        // INET_ADDRSTRLEN: Default ip address size
        inet_ntop( AF_INET, &ip_addr, str, INET_ADDRSTRLEN );
        cout << "Client is connected with port number: " << ntohs(client_addr.sin_port) << " and ip address: " << str << " " << endl;
        //cout << "Socket values of index " << i << " in main function is " << client_socketId << endl;
        pthread_create(&tid[i],NULL,thread_fn, (void*)&Connected_socketId[i]); // Creating a thread for each client accepted
        i++;
    }
    close(proxy_socketId);                                    // Close socket
    return 0;
}

//////////////////////////////////////////////////////////////////////////
// Cache implementation
//////////////////////////////////////////////////////////////////////////

// cache_element* find(char* url){
//     // Checks for url in the cache if found returns pointer to the respective cache element or else returns NULL
//     cache_element* site=NULL;
//     //sem_wait(&cache_lock);
//     int temp_lock_val = pthread_mutex_lock(&lock);
//     cout << "Remove Cache Lock Acquired " << temp_lock_val << endl;
//     if(head!=NULL){
//         site = head;
//         while (site!=NULL)
//         {
//             if(!site->url.compare(url)){
//                 cout << "LRU Time Track Before : " << site->lru_time_track;
//                 cout << endl << "url found" << endl;
//                 // Updating the time_track
//                 site->lru_time_track = time(NULL);
//                 cout << "LRU Time Track After : " << site->lru_time_track << endl;
//                 break;
//             }
//             site=site->next;
//         }
//     }
//     else {
//         cout << endl << "url not found" << endl;
//     }
//     //sem_post(&cache_lock);
//     temp_lock_val = pthread_mutex_unlock(&lock);
//     cout << "Remove Cache Lock Unlocked " << temp_lock_val << endl;
//     return site;
// }

void remove_cache_element(){
    // If cache is not empty searches for the node which has the least lru_time_track and deletes it
    cache_element * p ;    // Cache_element Pointer (Prev. Pointer)
    cache_element * q ;        // Cache_element Pointer (Next Pointer)
    cache_element * temp;    // Cache element to remove
    //sem_wait(&cache_lock);
    int temp_lock_val = pthread_mutex_lock(&lock);
    cout << "Remove Cache Lock Acquired " << temp_lock_val << endl;
    if( head != NULL) { // Cache != empty
        for (q = head, p = head, temp =head ; q -> next != NULL;
            q = q -> next) { // Iterate through entire cache and search for oldest time track
            if(( (q -> next) -> lru_time_track) < (temp -> lru_time_track)) {
                temp = q -> next;
                p = q;
            }
        }
        if(temp == head) {
            head = head -> next; /*Handle the base case*/
        } else {
            p->next = temp->next;
        }
        cache_size = cache_size - (temp -> len) - (int)sizeof(cache_element) -
        (int)temp->url.length() - 1;     //updating the cache size
        // free(temp->data);
        // free(temp->url); // Free the removed element
        delete temp;
    }
    //sem_post(&cache_lock);
    temp_lock_val = pthread_mutex_unlock(&lock);
    cout << "Remove Cache Lock Unlocked " << temp_lock_val << endl;
}

int add_cache_element(char* data,int size,char* url){
    // Adds element to the cache
    // sem_wait(&cache_lock);
    int temp_lock_val = pthread_mutex_lock(&lock);
    cout << "Add Cache Lock Acquired " << temp_lock_val << endl;
    int element_size=size+1+(int)strlen(url)+(int)sizeof(cache_element); // Size of the new element which will be added to the cache
    if(element_size>MAX_ELEMENT_SIZE){
        //sem_post(&cache_lock);
        // If element size is greater than MAX_ELEMENT_SIZE we don't add the element to the cache
        temp_lock_val = pthread_mutex_unlock(&lock);
        cout << "Add Cache Lock Unlocked " << temp_lock_val << endl;
        return 0;
    }
    else
    {
        while(cache_size+element_size>MAX_SIZE){
            // We keep removing elements from cache until we get enough space to add the element
            remove_cache_element();
        }
        cache_element* element = new cache_element(); // Allocating memory for the new cache element
        element->data.assign(data, size); // copy response into string
        element->url = string(url);
        element->lru_time_track = time(NULL);    // Updating the time_track
        element->next = head;
        element->len = size;
        head = element;
        cache_size += element_size;
        temp_lock_val = pthread_mutex_unlock(&lock);
        cout << "Add Cache Lock Unlocked " << temp_lock_val << endl;
        return 1;
    }
    return 0;
}
