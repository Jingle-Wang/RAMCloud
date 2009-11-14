#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include <shared/rcrpc.h>
#include <shared/net.h>

#include <server/server.h>

namespace RAMCloud {

Server::Server() : net(0)
{
    memset(blobs, 0, sizeof(blobs));
    net = new DefaultNet(true);
}

Server::~Server()
{
    delete net;
}

void
Server::handleRPC()
{
    struct rcrpc *rcrpc;
    struct rcrpc *resp = 0;
    struct rcrpc buf;

    if (net->recvRPC(&rcrpc) == 0) {
        printf("got rpc type: 0x%08x, len 0x%08x\n", rcrpc->type, rcrpc->len);

        switch (rcrpc->type) {
        case RCRPC_PING_REQUEST:
            resp = &buf;
            resp->type = RCRPC_PING_RESPONSE;
            resp->len  = (uint32_t) RCRPC_PING_RESPONSE_LEN;
            break;

        case RCRPC_READ100_REQUEST:
            printf("read100 from key %d\n", rcrpc->read100_request.key);
            resp = &blobs[rcrpc->read100_request.key];
            resp->type = RCRPC_READ100_RESPONSE;
            resp->len  = (uint32_t) RCRPC_READ100_RESPONSE_LEN;
            printf("resp key: %d\n", resp->read100_request.key);
            break;

        case RCRPC_READ1000_REQUEST:
            resp = &blobs[rcrpc->read1000_request.key];
            resp->type = RCRPC_READ1000_RESPONSE;
            resp->len  = (uint32_t) RCRPC_READ1000_RESPONSE_LEN;
            break;

        case RCRPC_WRITE100_REQUEST:
            printf("write100 to key %d, val = %s\n", rcrpc->write100_request.key, rcrpc->write100_request.buf);
            memcpy(&blobs[rcrpc->write100_request.key], rcrpc, RCRPC_WRITE100_REQUEST_LEN);
            printf("post copy key: %d\n", blobs[rcrpc->write100_request.key].write100_request.key);
            resp = &buf;
            resp->type = RCRPC_WRITE1000_RESPONSE;
            resp->len  = (uint32_t) RCRPC_WRITE1000_RESPONSE_LEN;
            break;

        case RCRPC_WRITE1000_REQUEST:
            memcpy(&blobs[rcrpc->write1000_request.key], rcrpc, RCRPC_WRITE1000_REQUEST_LEN);
            resp = &buf;
            resp->type = RCRPC_WRITE1000_RESPONSE;
            resp->len  = (uint32_t) RCRPC_WRITE1000_RESPONSE_LEN;
            break;

        default:
            fprintf(stderr, "received unknown RPC type 0x%08x\n", rcrpc->type);
            exit(1);
        }
    }

    net->sendRPC(resp);
}

} // namespace RAMCloud
