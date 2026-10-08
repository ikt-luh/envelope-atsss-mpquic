# Femto-QUIC

A lightweight library that provides simplified abstractions for PicoQUIC, enabling easy exploration of QUIC concepts. Designed for research, learning, and prototyping, it offers a user-friendly API. Ideal for rapid experimentation and understanding the fundamentals of QUIC.

The library handles packets using an internal queue mechanism. When data is added to the queue, PicoQUIC's internal network thread is triggered to process the queued content. This thread sets up the necessary configurations for sending the data, copies the data to a new buffer and forwards the information to PicoQUIC's scheduler. From there, PicoQUIC processes the data over the just in time API through its callback mechanism, ensuring seamless integration with its event-driven architecture. There is potential to drastically improve the current memory handling by reducing the amount of memcopies!


## Example Usage

```bash
# Build the example
mkdir build && cd build

export PICOQUIC_DIR=/home/vagrant/picoquic
export PICOTLS_DIR=/home/vagrant/picotls

cmake ..
make

# Create a server certificate and key
openssl genrsa -out key.pem 2048
openssl req -x509 -new -nodes -key key.pem -sha256 -days 365 -out cert.pem

# Run the server
./femtoquic_example server 4443 server_cert.pem server_key.pem

# Run the client in a new terminal
./femtoquic_example client 127.0.0.1:4443

########### MULTIPATHING MODE

# Run the server
./femtoquic_example mpserver 4443,4444 server_cert.pem server_key.pem

# Run the client in a new terminal
./femtoquic_example mpclient 127.0.0.1:4443,127.0.0.1:4444
```


## Multipathing

Femto supports the following types of QUIC multipathing:

1. The "official" multipathing (QUIC-MP) as specified in `draft-ieft-quic-multipath` using `picoquic_probe_new_path_ex`/`picoquic_set_stream_path_affinity` (defined in `femto.h`) - *WIP*
2. A custom multipathing implementation which uses multiple, independent PicoQUIC instances (defined in `femto_mp.h`).


### MP-QUIC (IETF) Example

**Setup**: Host `h1` has the IPs `10.0.1.1/24` (if=2) and `10.0.2.1/24` (if=3). Host `h2` has the IPs `10.0.1.2/24` (if=2) and `10.0.2.2/24` (if=3).

```bash
h2$ ./femtoquic_example server 4443 cert.pem key.pem 1
h1$ ./femtoquic_example client 10.0.1.2:4443,10.0.2.1:3
```


## Code Samples

Take a look at `example.c` for detailed examples.

### Very simple client

```c
#include "femto.h"

#define SERVER "127.0.0.1:4443"  // multiple server will be supported with draft-ietf-quic-multipath
#define ALPN "fetmo_example"
#define SNI "test.example.com"

void *client_callback(char *data, size_t len, void *user_ctx) {
    printf("Received a response from the server (len=%lu): <%s>\n", len, data);
}

void main() {
    femto_client_t client;

    create_femto_client(&client, SERVER, ALPN, SNI, &client_callback, NULL);
    // create_femto_client_bind_if(&client, SERVER, ALPN, SNI, &client_callback, "eth0");  // bind interface (unfinished)

    send_data(&client, "Hello World!", 13, 0);

    usleep(10000);
    destroy_femto_client(&client);
}
```

### Very simple server

```c
#include "femto.h"

#define PORT 4443
#define ALPN "fetmo_example"
#define CERT_FILE "server_cert.pem"
#define KEY_FILE "server_key.pem"

void *server_callback(char *receive_buffer, size_t receive_len, void *user_ctx,
                      char *response_buffer, size_t *response_len) {
    printf("Received data from client (len=%lu): <%s>. Responding...\n", receive_len, receive_buffer);

    strcpy(response_buffer, "This is a response!");
    *response_len = strlen(response_buffer);
}

void main() {
    femto_server_t server;

    create_femto_server(&server, port, 0, ALPN, CERT_FILE, KEY_FILE, &server_callback, NULL);

    start_femto_server_loop(&server);

    printf("Press Enter to exit...\n");
    getchar();

    destroy_femto_server(&server);
}
```

### Enabling MP-QUIC

To use the ietf draft multipathing, it needs to be enabled for the client and the server. On the server side that can be done by setting the `mp_enabled` parameter of `create_femto_server` to `1`. To enable multipathing for the client, simply specify a second path (IP of the client!) and an interface in the `servers` parameter. For example, `"10.0.1.2:4443,10.0.2.1:3"` (first path is `10.0.1.2` (server's IP) on port `4443`, the second path is `10.0.2.1` (client's IP) using interface `3`).

### Custom MP

Using the custom multipathing implementation works by simply using the functions from `femto_mp.h` instead of `femto.h`. Note that the user context in the callback functions now consists of a struct (`femto_mp_user_ctx_t`) that includes the original user context and the path id.

### Sending-feedback mechansim

In the currently implementation, adding data to the internal queue using `send_data` is non-blocking, even if congestion occurs. This means that it is possible to add more packets than PicoQUIC can send, which will add delay to the packets when they are waiting in the queue. Furthermore, crashes could be observed if too many streams were spawned.

To counteract this, a (hacky) method for determining the number of active streams was implemented. Use the following code to prevent the creation of too many streams:

```c
#include "femto.h"

#define SERVER "127.0.0.1:4443"
#define ALPN "fetmo_example"
#define SNI "test.example.com"
#define MAX_ACTIVE_STREAMS 3  // how many PicoQUIC streams may wait to be sent

void main() {
    femto_client_t client;
    create_femto_client(&client, SERVER, ALPN, SNI, &client_callback, NULL);

    while (1) {
        usleep(1000);  // try to send a packet every millisecond
        if (client->client_ctx->active_streams > MAX_ACTIVE_STREAMS) {
            continue;  // more than 3 streams are waiting to be send. Don't send more for now.
        }
        send_data(&client, "Hello World!", 13, 0);
    }
}
```



## Including Femto-QUIC in your project

To include Femto-QUIC in your project, add the following to your `CMakeLists.txt` file:

```c
# Femto-QUIC library
include_directories(${FEMTO_QUIC_DIR})
link_libraries(${FEMTO_QUIC_DIR}/build/libfemto.a)

# PicoQUIC loglib library
include_directories(${PICOQUIC_DIR}/loglib)
link_libraries(${PICOQUIC_DIR}/libpicoquic-log.a)

# PicoQUIC library
include_directories(${PICOQUIC_DIR}/picoquic)
link_libraries(${PICOQUIC_DIR}/libpicoquic-core.a)

# PicoTLS library
include_directories(${PICOTLS_DIR}/include)
link_libraries(${PICOTLS_DIR}/libpicotls-core.a)
link_libraries(${PICOTLS_DIR}/libpicotls-fusion.a)
link_libraries(${PICOTLS_DIR}/libpicotls-openssl.a)
link_libraries(${PICOTLS_DIR}/libpicotls-minicrypto.a)
```


## TODOs

- [ ] The client and server code is very similar. Merge it into a single code base that allows configuring its type (client/server).
- [ ] Add path affinity for femto server's `send_data_to_client` function
- [ ] Add path affinity for femto server's callback
- [ ] Optimize the memory handling: Currently, four memory copies are used (`send_data`, `enqueue`, `femto_client_create_stream`, `femto_client_callback`). This could be reduced to one memory copy by allocating the buffer in `send_data` and just passing the pointer in the queue. In theory, a callback based API forwarding PicoQUIC's just in time API calls could be implemented though it would not be "simple and friendly".
- [ ] Add feedback to the queue: Currently, it is possible to fill up the queue faster than PicoQUIC could ever send the data. Somehow implement feedback (e.g., `getQueueSize()` or `getActiveStreamsSize()`). Also, maybe a blocking API or callback once it is ready to send new data?
- [ ] Implement better stream handling. Like allow creating a stream, adding data to a stream, and closing a stream instead of creating, sending, and closing a new stream for every call of `send_data()`
- [ ] Implement QUIC Datagram support (RFC 9221)
