/*
 * swlink_protocol.h
 * SwLink USB 通信协议 —— Switch sysmodule 与 macOS 客户端共享
 *
 * 物理通道:一个 USB 设备,两个 vendor-specific 接口
 *   接口 0 (流):   EP OUT 握手 / EP IN  视频+音频复用包流
 *   接口 1 (文件): EP OUT 请求 / EP IN  响应
 * 两端均为小端 (Switch ARM64 / macOS x86_64|ARM64)
 */
#pragma once

#include <stdint.h>

#pragma pack(push, 1)

#ifdef __cplusplus
extern "C" {
#endif

#define SWLINK_PROTO_VERSION "01"

/* USB 设备标识 (社区通用 VID 0x16C0,自定义 PID) */
#define SWLINK_USB_VID 0x16C0
#define SWLINK_USB_PID 0x27DA

/* ---------------- 串流握手 ---------------- */

/* 主机插入后,console 主动往流接口 IN EP 写的字符串 (含 NUL,共 10 字节) */
#define SWLINK_HELLO "SwLink|" SWLINK_PROTO_VERSION

#define SWLINK_HANDSHAKE_MAGIC 0xAAAA1234u

/* host -> console,16 字节 */
typedef struct {
    uint32_t magic;          /* SWLINK_HANDSHAKE_MAGIC */
    uint16_t proto_ver;      /* '0','1' 两个 ASCII 字节 */
    uint8_t  want_video;
    uint8_t  want_audio;
    uint8_t  audio_batching; /* 0..SWLINK_AUDIO_MAX_BATCH,默认 3 */
    uint8_t  reserved[7];
} SwlinkHandshakeRequest;

#define SWLINK_HANDSHAKE_REQUEST_SIZE 16

enum {
    SWLINK_HANDSHAKE_OK          = 0,
    SWLINK_HANDSHAKE_BAD_MAGIC   = 1,
    SWLINK_HANDSHAKE_BAD_VERSION = 2,
    SWLINK_HANDSHAKE_BAD_SIZE    = 3,
    SWLINK_HANDSHAKE_BAD_REQUEST = 4,
};

/* console -> host,8 字节 */
typedef struct {
    uint32_t code;     /* 0 = OK */
    uint32_t reserved;
} SwlinkHandshakeResponse;

/* ---------------- 串流数据包 ---------------- */

#define SWLINK_PKT_MAGIC 0x534C5753u /* "SWLS" */

typedef enum {
    SwlinkStream_Video = 1,
    SwlinkStream_Audio = 2,
} SwlinkStreamType;

/* 18 字节包头,后接 data_size 字节负载 */
typedef struct {
    uint32_t magic;      /* SWLINK_PKT_MAGIC */
    uint32_t data_size;
    uint64_t timestamp;  /* grc 时间戳,单位 us */
    uint8_t  type;       /* SwlinkStreamType */
    uint8_t  flags;      /* bit0: 负载以 SPS/PPS 开头 (视频)
                            bit1: 错误包,负载为 u32 Result */
} SwlinkStreamHeader;

#define SWLINK_STREAM_FLAG_SPSPPS 0x01
#define SWLINK_STREAM_FLAG_ERROR  0x02

/* 视频:Annex-B H.264 NAL(720p30,grc 默认),单包一个 NAL(或 SPSPPS+NAL)
   音频:PCM s16le 48kHz 立体声,0x1000 字节整数倍 */
#define SWLINK_VIDEO_BUF_SIZE   0x54000
#define SWLINK_AUDIO_CHUNK      0x1000
#define SWLINK_AUDIO_MAX_BATCH  5
#define SWLINK_AUDIO_BUF_SIZE   (SWLINK_AUDIO_CHUNK * (1 + SWLINK_AUDIO_MAX_BATCH))

/* ---------------- 文件传输 ---------------- */

#define SWLINK_FT_MAGIC 0x464C5753u /* "SWLF" */

typedef enum {
    SwlinkFt_Ping   = 0, /* 响应 payload = 协议版本字符串 */
    SwlinkFt_List   = 1, /* 请求 payload: 路径;  响应 payload: 条目序列 */
    SwlinkFt_Stat   = 2, /* 请求 payload: 路径;  响应 payload: [u8 type][u64 size] */
    SwlinkFt_Read   = 3, /* offset/size 在 header;响应 payload: 文件数据 */
    SwlinkFt_Write  = 4, /* offset/size 在 header;请求 payload: 文件数据 */
    SwlinkFt_Mkdir  = 5,
    SwlinkFt_Delete = 6, /* offset 低 1 位: 1 = 递归删除目录 */
    SwlinkFt_Rename = 7, /* payload: [u16 len][旧路径][u16 len][新路径] */
} SwlinkFtCmd;

/* 40 字节,请求与响应共用;响应中 status 非 0 即失败 */
typedef struct {
    uint32_t magic;       /* SWLINK_FT_MAGIC */
    uint32_t cmd;         /* SwlinkFtCmd */
    uint32_t seq;         /* 请求方递增,响应回显 */
    uint32_t status;      /* 响应:0 成功,否则为错误码 */
    uint64_t offset;      /* READ/WRITE 文件偏移;DELETE 作 is_dir 标志 */
    uint64_t size;        /* READ/WRITE 字节数 */
    uint32_t payload_len;
    uint32_t reserved;
} SwlinkFtHeader;

#define SWLINK_FT_CHUNK       0x80000u /* 单次 READ/WRITE 最大 512KB */
#define SWLINK_FT_PATH_MAX    768
#define SWLINK_FT_LIST_BATCH  64       /* 单个 LIST 响应最多条目数 */

/* 路径 payload 格式:[u16 len][path bytes]
   LIST 响应条目格式:[u8 type(1=dir,2=file)][u64 size][u16 name_len][name bytes] */

enum {
    SWLINK_FT_OK              = 0,
    SWLINK_FT_ERR_IO          = 1,
    SWLINK_FT_ERR_BAD_REQUEST = 2,
    SWLINK_FT_ERR_NOT_FOUND   = 3,
    SWLINK_FT_ERR_PATH_TOO_LONG = 4,
};

#ifdef __cplusplus
}
#endif

#pragma pack(pop)

/* 编译期尺寸断言(两端编译器均支持 C11/C++11) */
#ifdef __cplusplus
static_assert(sizeof(SwlinkHandshakeRequest) == 16, "handshake request size");
static_assert(sizeof(SwlinkHandshakeResponse) == 8, "handshake response size");
static_assert(sizeof(SwlinkStreamHeader) == 18, "stream header size");
static_assert(sizeof(SwlinkFtHeader) == 40, "ft header size");
#else
_Static_assert(sizeof(SwlinkHandshakeRequest) == 16, "handshake request size");
_Static_assert(sizeof(SwlinkHandshakeResponse) == 8, "handshake response size");
_Static_assert(sizeof(SwlinkStreamHeader) == 18, "stream header size");
_Static_assert(sizeof(SwlinkFtHeader) == 40, "ft header size");
#endif
