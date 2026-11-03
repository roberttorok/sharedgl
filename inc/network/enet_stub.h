/*
 * Stand-ins for ENet in builds without the network transport (the Windows XP
 * client). Every call reports failure, and nothing ever connects, so the
 * network paths in glimpl.c stay dead. lzav is not needed either.
 */
#ifndef _SGL_ENET_STUB_H_
#define _SGL_ENET_STUB_H_

#include <stddef.h>

typedef struct { int unused; } ENetHost;
typedef struct { int unused; } ENetPeer;
typedef struct { void *data; size_t dataLength; } ENetPacket;
typedef struct { unsigned host; unsigned short port; } ENetAddress;
typedef struct { int type; ENetPacket *packet; } ENetEvent;

enum { ENET_EVENT_TYPE_CONNECT = 1, ENET_EVENT_TYPE_DISCONNECT, ENET_EVENT_TYPE_RECEIVE };
#define ENET_PACKET_FLAG_RELIABLE 1

#define __enet_initialize() (-1)
#define __enet_deinitialize() ((void)0)
#define __enet_host_create(a, b, c, d, e) ((ENetHost*)NULL)
#define __enet_host_destroy(h) ((void)0)
#define __enet_host_connect(h, a, b, c) ((ENetPeer*)NULL)
#define __enet_host_service(h, e, t) (-1)
#define __enet_address_set_host(a, n) ((void)0)
#define __enet_packet_create(d, l, f) ((ENetPacket*)NULL)
#define __enet_packet_destroy(p) ((void)0)
#define __enet_peer_send(p, c, e) ((void)0)
#define __enet_peer_disconnect(p, d) ((void)0)
#define lzav_decompress(a, b, c, d) (0)

#endif
