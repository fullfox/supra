#include <stdint.h>

#define MAX_NACK 350 // 4 byte per seq, NackPacket about 1408 bytes

typedef enum {
    INIT,
    FILE_CHUNK,
    CHECK,
    NACK,
    SLOWDOWN
} PacketType;

typedef struct {
    PacketType type;
} Packet;

typedef struct {
    PacketType type;
    uint32_t seq_num;
    uint32_t data_len;
    // Data follows
} ChunkPacketHeader;

typedef struct {
    PacketType type;
    uint64_t file_size;
    uint32_t frame_size;
} InitPacket;

typedef struct {
    PacketType type;
    uint32_t count;
    uint32_t missing[MAX_NACK];
} NackPacket;

typedef struct {
    PacketType type;
    uint64_t bitrate;
} SlowdownPacket;