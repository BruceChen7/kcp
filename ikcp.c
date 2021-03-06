//=====================================================================
//
// KCP - A Better ARQ Protocol Implementation
// skywind3000 (at) gmail.com, 2010-2011
//
// Features:
// + Average RTT reduce 30% - 40% vs traditional ARQ like tcp.
// + Maximum RTT reduce three times vs tcp.
// + Lightweight, distributed as a single source file.
//
//=====================================================================
#include "ikcp.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>



//=====================================================================
// KCP BASIC
//=====================================================================
const IUINT32 IKCP_RTO_NDL = 30;        // no delay min rto
const IUINT32 IKCP_RTO_MIN = 100;        // normal min rto
const IUINT32 IKCP_RTO_DEF = 200;
const IUINT32 IKCP_RTO_MAX = 60000;
const IUINT32 IKCP_CMD_PUSH = 81;        // cmd: push data
const IUINT32 IKCP_CMD_ACK  = 82;        // cmd: ack
const IUINT32 IKCP_CMD_WASK = 83;        // cmd: window probe (ask)
const IUINT32 IKCP_CMD_WINS = 84;        // cmd: window size (tell)
const IUINT32 IKCP_ASK_SEND = 1;        // need to send IKCP_CMD_WASK
const IUINT32 IKCP_ASK_TELL = 2;        // need to send IKCP_CMD_WINS
const IUINT32 IKCP_WND_SND = 32;
const IUINT32 IKCP_WND_RCV = 128;       // must >= max fragment size
const IUINT32 IKCP_MTU_DEF = 1400;
const IUINT32 IKCP_ACK_FAST    = 3;
const IUINT32 IKCP_INTERVAL    = 100;
const IUINT32 IKCP_OVERHEAD = 24;
const IUINT32 IKCP_DEADLINK = 20;
const IUINT32 IKCP_THRESH_INIT = 2;
const IUINT32 IKCP_THRESH_MIN = 2;
const IUINT32 IKCP_PROBE_INIT = 7000;        // 7 secs to probe window size
const IUINT32 IKCP_PROBE_LIMIT = 120000;    // up to 120 secs to probe window
const IUINT32 IKCP_FASTACK_LIMIT = 5;        // max times to trigger fastack


//---------------------------------------------------------------------
// encode / decode
//---------------------------------------------------------------------

/* encode 8 bits unsigned int */
static inline char *ikcp_encode8u(char *p, unsigned char c)
{
    *(unsigned char*)p++ = c;
    return p;
}

/* decode 8 bits unsigned int */
static inline const char *ikcp_decode8u(const char *p, unsigned char *c)
{
    *c = *(unsigned char*)p++;
    return p;
}

/* encode 16 bits unsigned int (lsb) */
static inline char *ikcp_encode16u(char *p, unsigned short w)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *(unsigned char*)(p + 0) = (w & 255);
    *(unsigned char*)(p + 1) = (w >> 8);
#else
    //  小端结构
    memcpy(p, &w, 2);
#endif
    p += 2;
    return p;
}

/* decode 16 bits unsigned int (lsb) */
static inline const char *ikcp_decode16u(const char *p, unsigned short *w)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *w = *(const unsigned char*)(p + 1);
    *w = *(const unsigned char*)(p + 0) + (*w << 8);
#else
    memcpy(w, p, 2);
#endif
    p += 2;
    return p;
}

/* encode 32 bits unsigned int (lsb) */
static inline char *ikcp_encode32u(char *p, IUINT32 l)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *(unsigned char*)(p + 0) = (unsigned char)((l >>  0) & 0xff);
    *(unsigned char*)(p + 1) = (unsigned char)((l >>  8) & 0xff);
    *(unsigned char*)(p + 2) = (unsigned char)((l >> 16) & 0xff);
    *(unsigned char*)(p + 3) = (unsigned char)((l >> 24) & 0xff);
#else
    memcpy(p, &l, 4);
#endif
    p += 4;
    return p;
}

/* decode 32 bits unsigned int (lsb) */
static inline const char *ikcp_decode32u(const char *p, IUINT32 *l)
{
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    *l = *(const unsigned char*)(p + 3);
    *l = *(const unsigned char*)(p + 2) + (*l << 8);
    *l = *(const unsigned char*)(p + 1) + (*l << 8);
    *l = *(const unsigned char*)(p + 0) + (*l << 8);
#else
    memcpy(l, p, 4);
#endif
    p += 4;
    return p;
}

static inline IUINT32 _imin_(IUINT32 a, IUINT32 b) {
    return a <= b ? a : b;
}

static inline IUINT32 _imax_(IUINT32 a, IUINT32 b) {
    return a >= b ? a : b;
}

static inline IUINT32 _ibound_(IUINT32 lower, IUINT32 middle, IUINT32 upper)
{
    return _imin_(_imax_(lower, middle), upper);
}

static inline long _itimediff(IUINT32 later, IUINT32 earlier)
{
    return ((IINT32)(later - earlier));
}

//---------------------------------------------------------------------
// manage segment
//---------------------------------------------------------------------
typedef struct IKCPSEG IKCPSEG;

static void* (*ikcp_malloc_hook)(size_t) = NULL;
static void (*ikcp_free_hook)(void *) = NULL;

// internal malloc
static void* ikcp_malloc(size_t size) {
    if (ikcp_malloc_hook)
        return ikcp_malloc_hook(size);
    return malloc(size);
}

// internal free
static void ikcp_free(void *ptr) {
    if (ikcp_free_hook) {
        ikcp_free_hook(ptr);
    }    else {
        free(ptr);
    }
}

// redefine allocator
void ikcp_allocator(void* (*new_malloc)(size_t), void (*new_free)(void*))
{
    ikcp_malloc_hook = new_malloc;
    ikcp_free_hook = new_free;
}

// allocate a new kcp segment
static IKCPSEG* ikcp_segment_new(ikcpcb *kcp, int size)
{
    return (IKCPSEG*)ikcp_malloc(sizeof(IKCPSEG) + size);
}

// delete a segment
static void ikcp_segment_delete(ikcpcb *kcp, IKCPSEG *seg)
{
    ikcp_free(seg);
}

// write log
void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...)
{
    char buffer[1024];
    va_list argptr;
    if ((mask & kcp->logmask) == 0 || kcp->writelog == 0) return;
    va_start(argptr, fmt);
    vsprintf(buffer, fmt, argptr);
    va_end(argptr);
    kcp->writelog(buffer, kcp, kcp->user);
}

// check log mask
static int ikcp_canlog(const ikcpcb *kcp, int mask)
{
    if ((mask & kcp->logmask) == 0 || kcp->writelog == NULL) return 0;
    return 1;
}

// output segment
// 发送用户数据
static int ikcp_output(ikcpcb *kcp, const void *data, int size)
{
    assert(kcp);
    // 设置了用户层的回调
    assert(kcp->output);
    if (ikcp_canlog(kcp, IKCP_LOG_OUTPUT)) {
        ikcp_log(kcp, IKCP_LOG_OUTPUT, "[RO] %ld bytes", (long)size);
    }
    if (size == 0) return 0;
    // 直接调用用户上层自定义的发送调用， udp为sendto
    return kcp->output((const char*)data, size, kcp, kcp->user);
}

// output queue
void ikcp_qprint(const char *name, const struct IQUEUEHEAD *head)
{
#if 0
    const struct IQUEUEHEAD *p;
    printf("<%s>: [", name);
    for (p = head->next; p != head; p = p->next) {
        const IKCPSEG *seg = iqueue_entry(p, const IKCPSEG, node);
        printf("(%lu %d)", (unsigned long)seg->sn, (int)(seg->ts % 10000));
        if (p->next != head) printf(",");
    }
    printf("]\n");
#endif
}


//---------------------------------------------------------------------
// create a new kcpcb
//---------------------------------------------------------------------
ikcpcb* ikcp_create(IUINT32 conv, void *user)
{
    ikcpcb *kcp = (ikcpcb*)ikcp_malloc(sizeof(struct IKCPCB));
    if (kcp == NULL) return NULL;
    kcp->conv = conv;
    kcp->user = user;
    kcp->snd_una = 0;
    kcp->snd_nxt = 0;
    kcp->rcv_nxt = 0;
    kcp->ts_recent = 0;
    kcp->ts_lastack = 0;
    kcp->ts_probe = 0;
    kcp->probe_wait = 0;
    kcp->snd_wnd = IKCP_WND_SND; // 发送窗口
    kcp->rcv_wnd = IKCP_WND_RCV; //  接收窗口
    kcp->rmt_wnd = IKCP_WND_RCV;
    kcp->cwnd = 0;
    kcp->incr = 0;
    kcp->probe = 0;
    kcp->mtu = IKCP_MTU_DEF;

    // 除去头的最大数据单元
    kcp->mss = kcp->mtu - IKCP_OVERHEAD;
    kcp->stream = 0;

    // 设置kcp内部编解码使用
    kcp->buffer = (char*)ikcp_malloc((kcp->mtu + IKCP_OVERHEAD) * 3);
    if (kcp->buffer == NULL) {
        ikcp_free(kcp);
        return NULL;
    }

    iqueue_init(&kcp->snd_queue);
    iqueue_init(&kcp->rcv_queue);
    iqueue_init(&kcp->snd_buf);
    iqueue_init(&kcp->rcv_buf);
    kcp->nrcv_buf = 0;
    kcp->nsnd_buf = 0;
    kcp->nrcv_que = 0;
    kcp->nsnd_que = 0;
    kcp->state = 0;
    kcp->acklist = NULL;
    kcp->ackblock = 0;
    kcp->ackcount = 0;
    kcp->rx_srtt = 0;
    kcp->rx_rttval = 0;
    kcp->rx_rto = IKCP_RTO_DEF;
    kcp->rx_minrto = IKCP_RTO_MIN;
    kcp->current = 0;
    kcp->interval = IKCP_INTERVAL;
    kcp->ts_flush = IKCP_INTERVAL;
    kcp->nodelay = 0;
    kcp->updated = 0;
    kcp->logmask = 0;
    kcp->ssthresh = IKCP_THRESH_INIT;
    kcp->fastresend = 0;
    // 最多5次快速重传
    kcp->fastlimit = IKCP_FASTACK_LIMIT;
    kcp->nocwnd = 0;
    kcp->xmit = 0;
    kcp->dead_link = IKCP_DEADLINK;
    kcp->output = NULL;
    kcp->writelog = NULL;

    return kcp;
}


//---------------------------------------------------------------------
// release a new kcpcb
//---------------------------------------------------------------------
void ikcp_release(ikcpcb *kcp)
{
    assert(kcp);
    if (kcp) {
        IKCPSEG *seg;
        while (!iqueue_is_empty(&kcp->snd_buf)) {
            seg = iqueue_entry(kcp->snd_buf.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        while (!iqueue_is_empty(&kcp->rcv_buf)) {
            seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        while (!iqueue_is_empty(&kcp->snd_queue)) {
            seg = iqueue_entry(kcp->snd_queue.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        while (!iqueue_is_empty(&kcp->rcv_queue)) {
            seg = iqueue_entry(kcp->rcv_queue.next, IKCPSEG, node);
            iqueue_del(&seg->node);
            ikcp_segment_delete(kcp, seg);
        }
        if (kcp->buffer) {
            ikcp_free(kcp->buffer);
        }
        if (kcp->acklist) {
            ikcp_free(kcp->acklist);
        }

        kcp->nrcv_buf = 0;
        kcp->nsnd_buf = 0;
        kcp->nrcv_que = 0;
        kcp->nsnd_que = 0;
        kcp->ackcount = 0;
        kcp->buffer = NULL;
        kcp->acklist = NULL;
        ikcp_free(kcp);
    }
}


//---------------------------------------------------------------------
// set output callback, which will be invoked by kcp
//---------------------------------------------------------------------
void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len,
    ikcpcb *kcp, void *user))
{
    kcp->output = output;
}


//---------------------------------------------------------------------
// user/upper level recv: returns size, returns below zero for EAGAIN
//---------------------------------------------------------------------
// 上层的recv函数，用来接受报文
int ikcp_recv(ikcpcb *kcp, char *buffer, int len)
{
    struct IQUEUEHEAD *p;
    int ispeek = (len < 0)? 1 : 0;
    int peeksize;
    int recover = 0;
    IKCPSEG *seg;
    assert(kcp);

    // 接收队列为空
    if (iqueue_is_empty(&kcp->rcv_queue))
        return -1;

    if (len < 0) len = -len;

    // 获取kcp中，接受数据的报文大小
    peeksize = ikcp_peeksize(kcp);

    if (peeksize < 0)
        return -2;

    // 队列中比传入的buffer
    if (peeksize > len)
        return -3;

    // 快速恢复模式
    if (kcp->nrcv_que >= kcp->rcv_wnd)
        recover = 1;

    // merge fragment
    for (len = 0, p = kcp->rcv_queue.next; p != &kcp->rcv_queue; ) {
        int fragment;
        seg = iqueue_entry(p, IKCPSEG, node);
        p = p->next;

        if (buffer) {
            // 将数据copy
            memcpy(buffer, seg->data, seg->len);
            buffer += seg->len;
        }

        len += seg->len;
        // 分片号为0
        fragment = seg->frg;

        if (ikcp_canlog(kcp, IKCP_LOG_RECV)) {
            ikcp_log(kcp, IKCP_LOG_RECV, "recv sn=%lu", (unsigned long)seg->sn);
        }

        if (ispeek == 0) {
            iqueue_del(&seg->node);
            // 直接释放节点
            ikcp_segment_delete(kcp, seg);
            kcp->nrcv_que--;
        }
        // 一整块，没有分片，直接退出
        if (fragment == 0)
            break;
    }

    assert(len == peeksize);

    // move available data from rcv_buf -> rcv_queue
    while (! iqueue_is_empty(&kcp->rcv_buf)) {
        seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
        // buffer中的报文能够匹配上期待的接收的报文编号
        if (seg->sn == kcp->rcv_nxt && kcp->nrcv_que < kcp->rcv_wnd) {
            iqueue_del(&seg->node);
            kcp->nrcv_buf--;
            // 移动到rcv_queue中
            iqueue_add_tail(&seg->node, &kcp->rcv_queue);
            kcp->nrcv_que++;
            // 更新窗口指针
            kcp->rcv_nxt++;
        }    else {
            break;
        }
    }

    // fast recover
    if (kcp->nrcv_que < kcp->rcv_wnd && recover) {
        // ready to send back IKCP_CMD_WINS in ikcp_flush
        // tell remote my window size
        kcp->probe |= IKCP_ASK_TELL;
    }

    return len;
}


//---------------------------------------------------------------------
// peek data size
//---------------------------------------------------------------------
int ikcp_peeksize(const ikcpcb *kcp)
{
    struct IQUEUEHEAD *p;
    IKCPSEG *seg;
    int length = 0;

    assert(kcp);

    if (iqueue_is_empty(&kcp->rcv_queue)) return -1;

    seg = iqueue_entry(kcp->rcv_queue.next, IKCPSEG, node);
    // 只有1片
    if (seg->frg == 0) return seg->len;

    if (kcp->nrcv_que < seg->frg + 1) return -1;

    for (p = kcp->rcv_queue.next; p != &kcp->rcv_queue; p = p->next) {
        seg = iqueue_entry(p, IKCPSEG, node);
        length += seg->len;
        // 到达了最后一个分片
        if (seg->frg == 0) break;
    }

    return length;
}


//---------------------------------------------------------------------
// user/upper level send, returns below zero for error
//---------------------------------------------------------------------
int ikcp_send(ikcpcb *kcp, const char *buffer, int len)
{
    IKCPSEG *seg;
    int count, i;

    assert(kcp->mss > 0);
    if (len < 0) return -1;

    // append to previous segment in streaming mode (if possible)
    // 在流模式下进行append
    if (kcp->stream != 0) {
        if (!iqueue_is_empty(&kcp->snd_queue)) {
            IKCPSEG *old = iqueue_entry(kcp->snd_queue.prev, IKCPSEG, node);
            if (old->len < kcp->mss) {
                int capacity = kcp->mss - old->len;
                int extend = (len < capacity)? len : capacity;
                seg = ikcp_segment_new(kcp, old->len + extend);
                assert(seg);
                if (seg == NULL) {
                    return -2;
                }
                iqueue_add_tail(&seg->node, &kcp->snd_queue);
                memcpy(seg->data, old->data, old->len);
                if (buffer) {
                    memcpy(seg->data + old->len, buffer, extend);
                    buffer += extend;
                }
                seg->len = old->len + extend;
                seg->frg = 0;
                len -= extend;
                iqueue_del_init(&old->node);
                ikcp_segment_delete(kcp, old);
            }
        }
        if (len <= 0) {
            return 0;
        }
    }

     // 小于mss, 1个segment
    if (len <= (int)kcp->mss) count = 1;
    else count = (len + kcp->mss - 1) / kcp->mss;

    // 如果大于最大接收的分段数，直接返回失败
    if (count >= (int)IKCP_WND_RCV) return -2;

    if (count == 0) count = 1;

    // fragment
    for (i = 0; i < count; i++) {
        // 进行数据的分片
        int size = len > (int)kcp->mss ? (int)kcp->mss : len;
        // 创建一个seg
        seg = ikcp_segment_new(kcp, size);
        assert(seg);
        if (seg == NULL) {
            return -2;
        }
        if (buffer && len > 0) {
            // 数据的拷贝
            memcpy(seg->data, buffer, size);
        }
        seg->len = size;
        // 非流式协议，那么给段设计分段号
        seg->frg = (kcp->stream == 0)? (count - i - 1) : 0;
        iqueue_init(&seg->node);
        // 将节点加入到发送队列为
        iqueue_add_tail(&seg->node, &kcp->snd_queue);
        kcp->nsnd_que++;
        if (buffer) {
            // 移动buffer指针
            buffer += size;
        }
        // 减少用户的数据
        len -= size;
    }

    return 0;
}


//---------------------------------------------------------------------
// parse ack
//---------------------------------------------------------------------
static void ikcp_update_ack(ikcpcb *kcp, IINT32 rtt)
{
    IINT32 rto = 0;
    // https://tools.ietf.org/html/rfc2988
    // srtt smoothed round-trip time
    if (kcp->rx_srtt == 0) {
        // srtt直接更新成smooth round-trip time
        kcp->rx_srtt = rtt;
        // rttval之
        kcp->rx_rttval = rtt / 2;
    }    else {
        long delta = rtt - kcp->rx_srtt;
        if (delta < 0) delta = -delta;
        kcp->rx_rttval = (3 * kcp->rx_rttval + delta) / 4;
        kcp->rx_srtt = (7 * kcp->rx_srtt + rtt) / 8;
        if (kcp->rx_srtt < 1) kcp->rx_srtt = 1;
    }
    // 重传超时时间设置
    rto = kcp->rx_srtt + _imax_(kcp->interval, 4 * kcp->rx_rttval);
    kcp->rx_rto = _ibound_(kcp->rx_minrto, rto, IKCP_RTO_MAX);
}

static void ikcp_shrink_buf(ikcpcb *kcp)
{
    struct IQUEUEHEAD *p = kcp->snd_buf.next;
    // 如果buffer中有seg，也就是非空
    if (p != &kcp->snd_buf) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        // 更新下一待确认的序列号
        kcp->snd_una = seg->sn;
    }    else {
        kcp->snd_una = kcp->snd_nxt;
    }
}

static void ikcp_parse_ack(ikcpcb *kcp, IUINT32 sn)
{
    struct IQUEUEHEAD *p, *next;
    //  已经确认包的sn和超过待确认的序列号
    if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0)
        return;

    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        next = p->next;
        // 找到对应的sn的报文删除
        if (sn == seg->sn) {
            iqueue_del(p);
            ikcp_segment_delete(kcp, seg);
            kcp->nsnd_buf--;
            break;
        }
        if (_itimediff(sn, seg->sn) < 0) {
            break;
        }
    }
}

// una未确认的序列号，来删除已经确认的报文
static void ikcp_parse_una(ikcpcb *kcp, IUINT32 una)
{
    struct IQUEUEHEAD *p, *next;
    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        next = p->next;
        if (_itimediff(una, seg->sn) > 0) {
            // 删除这些已经确认的报文
            iqueue_del(p);
            ikcp_segment_delete(kcp, seg);
            kcp->nsnd_buf--;
        }    else {
            break;
        }
    }
}

static void ikcp_parse_fastack(ikcpcb *kcp, IUINT32 sn, IUINT32 ts)
{
    struct IQUEUEHEAD *p, *next;

    // 如果接收到的sn非法
    if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0)
        return;

    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = next) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        next = p->next;
        // 不在更新fastack的值
        if (_itimediff(sn, seg->sn) < 0) {
            break;
        }
        else if (sn != seg->sn) {
        #ifndef IKCP_FASTACK_CONSERVE
            seg->fastack++;
        #else
            if (_itimediff(ts, seg->ts) >= 0)
                seg->fastack++;
        #endif
        }
    }
}


//---------------------------------------------------------------------
// ack append
//---------------------------------------------------------------------
static void ikcp_ack_push(ikcpcb *kcp, IUINT32 sn, IUINT32 ts)
{
    size_t newsize = kcp->ackcount + 1;
    IUINT32 *ptr;
    // 分配内存
    if (newsize > kcp->ackblock) {
        IUINT32 *acklist;
        size_t newblock;

        for (newblock = 8; newblock < newsize; newblock <<= 1);
        acklist = (IUINT32*)ikcp_malloc(newblock * sizeof(IUINT32) * 2);

        if (acklist == NULL) {
            assert(acklist != NULL);
            abort();
        }

        if (kcp->acklist != NULL) {
            size_t x;
            for (x = 0; x < kcp->ackcount; x++) {
                acklist[x * 2 + 0] = kcp->acklist[x * 2 + 0];
                acklist[x * 2 + 1] = kcp->acklist[x * 2 + 1];
            }
            ikcp_free(kcp->acklist);
        }

        kcp->acklist = acklist;
        kcp->ackblock = newblock;
    }
    // 直接加到对尾中
    ptr = &kcp->acklist[kcp->ackcount * 2];
    ptr[0] = sn;
    ptr[1] = ts;
    // 应答数 + 1
    kcp->ackcount++;
}

static void ikcp_ack_get(const ikcpcb *kcp, int p, IUINT32 *sn, IUINT32 *ts)
{
    // 交替的存储sn和ts
    if (sn) sn[0] = kcp->acklist[p * 2 + 0];
    if (ts) ts[0] = kcp->acklist[p * 2 + 1];
}


//---------------------------------------------------------------------
// parse data, IKCPSEG是在外面new出来的，在这里删除，这种设计不是很好
//---------------------------------------------------------------------
void ikcp_parse_data(ikcpcb *kcp, IKCPSEG *newseg)
{
    struct IQUEUEHEAD *p, *prev;
    IUINT32 sn = newseg->sn;
    int repeat = 0;
    // 序列号非法, 直接删除
    if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) >= 0 ||
        _itimediff(sn, kcp->rcv_nxt) < 0) {
        ikcp_segment_delete(kcp, newseg);
        return;
    }
    // 逆序的的找到对应的sn
    for (p = kcp->rcv_buf.prev; p != &kcp->rcv_buf; p = prev) {
        IKCPSEG *seg = iqueue_entry(p, IKCPSEG, node);
        prev = p->prev;
        // 如果找到了相同的序列号
        if (seg->sn == sn) {
            repeat = 1;
            break;
        }
        if (_itimediff(sn, seg->sn) > 0) {
            break;
        }
    }
    // 插入到
    if (repeat == 0) {
        iqueue_init(&newseg->node);
        // 这里保持有序了
        iqueue_add(&newseg->node, p);
        kcp->nrcv_buf++;
    }    else {
        // 重复发送，删除
        ikcp_segment_delete(kcp, newseg);
    }

#if 0
    ikcp_qprint("rcvbuf", &kcp->rcv_buf);
    printf("rcv_nxt=%lu\n", kcp->rcv_nxt);
#endif

    // move available data from rcv_buf -> rcv_queue
    while (! iqueue_is_empty(&kcp->rcv_buf)) {
        IKCPSEG *seg = iqueue_entry(kcp->rcv_buf.next, IKCPSEG, node);
        if (seg->sn == kcp->rcv_nxt && kcp->nrcv_que < kcp->rcv_wnd) {
            iqueue_del(&seg->node);
            kcp->nrcv_buf--;
            iqueue_add_tail(&seg->node, &kcp->rcv_queue);
            kcp->nrcv_que++;
            kcp->rcv_nxt++;
        }    else {
            break;
        }
    }

#if 0
    ikcp_qprint("queue", &kcp->rcv_queue);
    printf("rcv_nxt=%lu\n", kcp->rcv_nxt);
#endif

#if 1
//    printf("snd(buf=%d, queue=%d)\n", kcp->nsnd_buf, kcp->nsnd_que);
//    printf("rcv(buf=%d, queue=%d)\n", kcp->nrcv_buf, kcp->nrcv_que);
#endif
}


//---------------------------------------------------------------------
// input data
//---------------------------------------------------------------------
// 从网络层到kcp层
int ikcp_input(ikcpcb *kcp, const char *data, long size)
{
    IUINT32 prev_una = kcp->snd_una;
    IUINT32 maxack = 0, latest_ts = 0;
    int flag = 0;

    if (ikcp_canlog(kcp, IKCP_LOG_INPUT)) {
        ikcp_log(kcp, IKCP_LOG_INPUT, "[RI] %d bytes", (int)size);
    }
     // 输入数据没有或者size不对
    if (data == NULL || (int)size < (int)IKCP_OVERHEAD) return -1;

    while (1) {
        IUINT32 ts, sn, len, una, conv;
        IUINT16 wnd;
        IUINT8 cmd, frg;
        IKCPSEG *seg;
        // 剩余的包不完整，那么直接跳出循环
        if (size < (int)IKCP_OVERHEAD) break;
        // 获取连接号
        data = ikcp_decode32u(data, &conv);
        // 不是同一个连接发送的数据，直接出错
        if (conv != kcp->conv) return -1;
        // 解码kcp的头
        data = ikcp_decode8u(data, &cmd);
        // 解码段
        data = ikcp_decode8u(data, &frg);
        // 接收窗口大小
        data = ikcp_decode16u(data, &wnd);
        // s获取当前时间戳
        data = ikcp_decode32u(data, &ts);
        // 对方的发送序列号
        data = ikcp_decode32u(data, &sn);
        data = ikcp_decode32u(data, &una);
        // 数据长度
        data = ikcp_decode32u(data, &len);

        size -= IKCP_OVERHEAD;
        // 剩余size < 接收到的数据，错误
        if ((long)size < (long)len || (int)len < 0) return -2;
        // 非法的命令
        if (cmd != IKCP_CMD_PUSH && cmd != IKCP_CMD_ACK &&
            cmd != IKCP_CMD_WASK && cmd != IKCP_CMD_WINS)
            return -3;
        // 对方接收窗口的大小更新
        kcp->rmt_wnd = wnd;
        // 根据未确认报文，删除已经确认的报文
        ikcp_parse_una(kcp, una);
        // 更新下一个待确认的报文
        ikcp_shrink_buf(kcp);
        // 收到对方发送过来的ack
        if (cmd == IKCP_CMD_ACK) {   // 对方发送的ack报文
            if (_itimediff(kcp->current, ts) >= 0) {
                // 用来更新该kcp的rx_rttval时间
                // 和重传rto时间
                ikcp_update_ack(kcp, _itimediff(kcp->current, ts));
            }
            // 找到对应的sn，在发送队列中删除
            ikcp_parse_ack(kcp, sn);
            ikcp_shrink_buf(kcp);
            if (flag == 0) { // 接受第一个包的时候，更新maxack，latest_ts中
                flag = 1;
                maxack = sn;
                latest_ts = ts;
            }    else {
                if (_itimediff(sn, maxack) > 0) {
                #ifndef IKCP_FASTACK_CONSERVE
                    maxack = sn;
                    latest_ts = ts;
                #else
                    if (_itimediff(ts, latest_ts) > 0) {
                        maxack = sn;
                        latest_ts = ts;
                    }
                #endif
                }
            }
            // 打印日志
            if (ikcp_canlog(kcp, IKCP_LOG_IN_ACK)) {
                ikcp_log(kcp, IKCP_LOG_IN_ACK,
                    "input ack: sn=%lu rtt=%ld rto=%ld", (unsigned long)sn,
                    (long)_itimediff(kcp->current, ts),
                    (long)kcp->rx_rto);
            }
        }
        else if (cmd == IKCP_CMD_PUSH) {  // 收到数据包，需要快速push
            if (ikcp_canlog(kcp, IKCP_LOG_IN_DATA)) {
                ikcp_log(kcp, IKCP_LOG_IN_DATA,
                    "input psh: sn=%lu ts=%lu", (unsigned long)sn, (unsigned long)ts);
            }
            // 在窗口范围内
            if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) < 0) {
                // 给kcp添加一个ack报文
                ikcp_ack_push(kcp, sn, ts);
                //  接收到之后的数据
                if (_itimediff(sn, kcp->rcv_nxt) >= 0) {
                    // 创建一个segment
                    seg = ikcp_segment_new(kcp, len);
                    seg->conv = conv;
                    seg->cmd = cmd;
                    seg->frg = frg;
                    seg->wnd = wnd;
                    seg->ts = ts;
                    seg->sn = sn;
                    seg->una = una;
                    seg->len = len;

                    if (len > 0) {
                        memcpy(seg->data, data, len);
                    }
                    // 将接收到的报文，直接copy到kcp维持的rcv_buf和rcv_queue中
                    ikcp_parse_data(kcp, seg);
                }
            }
        }
        else if (cmd == IKCP_CMD_WASK) { // 用来告知对方自己的窗口大小
            // ready to send back IKCP_CMD_WINS in ikcp_flush
            // tell remote my window size
            kcp->probe |= IKCP_ASK_TELL; // 用来告知下次kcp发送的时候，需要告知自己的窗口大小
            if (ikcp_canlog(kcp, IKCP_LOG_IN_PROBE)) {
                ikcp_log(kcp, IKCP_LOG_IN_PROBE, "input probe");
            }
        }
        else if (cmd == IKCP_CMD_WINS) {
            // do nothing
            if (ikcp_canlog(kcp, IKCP_LOG_IN_WINS)) {
                ikcp_log(kcp, IKCP_LOG_IN_WINS,
                    "input wins: %lu", (unsigned long)(wnd));
            }
        }
        else {
            return -3;
        }
        // 移动指针
        data += len;
        size -= len;
    }
    // 说明有acksegment
    if (flag != 0) {
        ikcp_parse_fastack(kcp, maxack, latest_ts);
    }
    // 有确认序号报文产生，那么更新发送窗口的大小
    if (_itimediff(kcp->snd_una, prev_una) > 0) {
        // 如果发送窗口大小小于接受窗口大小
        if (kcp->cwnd < kcp->rmt_wnd) {
            IUINT32 mss = kcp->mss;
            if (kcp->cwnd < kcp->ssthresh) {
                // 窗口大小增加
                kcp->cwnd++;
                // 增加一个mss的流量
                kcp->incr += mss;
            }    else {
                if (kcp->incr < mss) kcp->incr = mss;
                kcp->incr += (mss * mss) / kcp->incr + (mss / 16);
                if ((kcp->cwnd + 1) * mss <= kcp->incr) {
                #if 1
                    kcp->cwnd = (kcp->incr + mss - 1) / ((mss > 0)? mss : 1);
                #else
                    kcp->cwnd++;
                #endif
                }
            }
            if (kcp->cwnd > kcp->rmt_wnd) {
                kcp->cwnd = kcp->rmt_wnd;
                kcp->incr = kcp->rmt_wnd * mss;
            }
        }
    }

    return 0;
}


//---------------------------------------------------------------------
// ikcp_encode_seg
//---------------------------------------------------------------------
static char *ikcp_encode_seg(char *ptr, const IKCPSEG *seg)
{
    // 4个字节存放会话编号
    ptr = ikcp_encode32u(ptr, seg->conv);
    // 接下来一个字节存放命令字
    ptr = ikcp_encode8u(ptr, (IUINT8)seg->cmd);
    // 接下来存放分片号
    ptr = ikcp_encode8u(ptr, (IUINT8)seg->frg);
    // 自己可用的窗口大小
    ptr = ikcp_encode16u(ptr, (IUINT16)seg->wnd);
    // 设置时间戳
    ptr = ikcp_encode32u(ptr, seg->ts);
    // 设置序列号
    ptr = ikcp_encode32u(ptr, seg->sn);
    // 设置una
    ptr = ikcp_encode32u(ptr, seg->una);
    // 设置报文大小
    ptr = ikcp_encode32u(ptr, seg->len);
    return ptr;
}

static int ikcp_wnd_unused(const ikcpcb *kcp)
{
    if (kcp->nrcv_que < kcp->rcv_wnd) {
        return kcp->rcv_wnd - kcp->nrcv_que;
    }
    return 0;
}


//---------------------------------------------------------------------
// ikcp_flush
//---------------------------------------------------------------------
void ikcp_flush(ikcpcb *kcp)
{
    IUINT32 current = kcp->current;
    char *buffer = kcp->buffer;
    char *ptr = buffer;
    int count, size, i;
    IUINT32 resent, cwnd;
    IUINT32 rtomin;
    struct IQUEUEHEAD *p;
    int change = 0;
    int lost = 0;
    IKCPSEG seg;

    // 'ikcp_update' haven't been called.
    if (kcp->updated == 0) return;
    // 设置报文的会话编号
    seg.conv = kcp->conv;
    //  应答报文
    seg.cmd = IKCP_CMD_ACK;
    seg.frg = 0;
    // 计算可用的发送窗口大小
    seg.wnd = ikcp_wnd_unused(kcp);
    // 设置未应答的ack
    seg.una = kcp->rcv_nxt;
    seg.len = 0;
    seg.sn = 0;
    seg.ts = 0;

    // flush acknowledges
    count = kcp->ackcount;
    for (i = 0; i < count; i++) {
        // 第一次这里size为0，因为ptr就指向了buffer
        size = (int)(ptr - buffer);

        // 大于一个mtu，那么执行一次output发出去
        // 发送多个ack报文
        if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
            // 执行下层协议栈的网络发送函数
            ikcp_output(kcp, buffer, size);
            // 再次复用buffer
            ptr = buffer;
        }
        // 设置序列号，设置时间戳
        ikcp_ack_get(kcp, i, &seg.sn, &seg.ts);
        // 这里移动了ptr
        ptr = ikcp_encode_seg(ptr, &seg);
    }
    //  对ack报文清0，因为已经将ack报文发送了
    kcp->ackcount = 0;

    // probe window size (if remote window size equals zero)
    // 如果对方的接收窗口为0，那么要进行对方窗口设置
    if (kcp->rmt_wnd == 0) {
        //  第一次进行窗口探测，设置窗口探测时间
        if (kcp->probe_wait == 0) {
            // 设置等待时间
            kcp->probe_wait = IKCP_PROBE_INIT;
            kcp->ts_probe = kcp->current + kcp->probe_wait;
        }
        else {
            // 当前时间大于上次probe的时间
            if (_itimediff(kcp->current, kcp->ts_probe) >= 0) {
                if (kcp->probe_wait < IKCP_PROBE_INIT)
                    kcp->probe_wait = IKCP_PROBE_INIT;
                // 如果多次进行窗口探测，那么动态更新等待时间增长
                kcp->probe_wait += kcp->probe_wait / 2;

                if (kcp->probe_wait > IKCP_PROBE_LIMIT)
                    kcp->probe_wait = IKCP_PROBE_LIMIT;
                kcp->ts_probe = kcp->current + kcp->probe_wait;
                // 设置立即发送的状态
                kcp->probe |= IKCP_ASK_SEND;
            }
        }
    }    else {
        // 探测时间和等待时间清0
        kcp->ts_probe = 0;
        kcp->probe_wait = 0;
    }

    // flush window probing commands
    // 将窗口探测的报文直接放到发送
    if (kcp->probe & IKCP_ASK_SEND) {
        // 将命令设置为Window ask
        seg.cmd = IKCP_CMD_WASK;
        size = (int)(ptr - buffer);
        if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
            ikcp_output(kcp, buffer, size);
            ptr = buffer;
        }
        ptr = ikcp_encode_seg(ptr, &seg);
    }

    // flush window probing commands
    // 如果要将自己的窗口大小发出去，那么直接发出去
    if (kcp->probe & IKCP_ASK_TELL) {
        seg.cmd = IKCP_CMD_WINS;
        size = (int)(ptr - buffer);
        if (size + (int)IKCP_OVERHEAD > (int)kcp->mtu) {
            ikcp_output(kcp, buffer, size);
            ptr = buffer;
        }
        ptr = ikcp_encode_seg(ptr, &seg);
    }
    // 标志位重置
    kcp->probe = 0;
    //  计算发送窗口和对端的接收窗口，选择较小的窗口
    // calculate window size
    cwnd = _imin_(kcp->snd_wnd, kcp->rmt_wnd);
    // 不进行流量控制，设置较小的窗口大小
    if (kcp->nocwnd == 0) cwnd = _imin_(kcp->cwnd, cwnd);

    // move data from snd_queue to snd_buf
    // 要发送的序号在窗口中
    // 从snd_queue移动到snd_buffer
    while (_itimediff(kcp->snd_nxt, kcp->snd_una + cwnd) < 0) {
        IKCPSEG *newseg;
        if (iqueue_is_empty(&kcp->snd_queue)) break;

        newseg = iqueue_entry(kcp->snd_queue.next, IKCPSEG, node);

        iqueue_del(&newseg->node);
        // 放到send buffer
        iqueue_add_tail(&newseg->node, &kcp->snd_buf);
        kcp->nsnd_que--;
        kcp->nsnd_buf++;

        newseg->conv = kcp->conv;
        newseg->cmd = IKCP_CMD_PUSH;
        newseg->wnd = seg.wnd;
        newseg->ts = current;
        // 序号加一
        newseg->sn = kcp->snd_nxt++;
        newseg->una = kcp->rcv_nxt;
        newseg->resendts = current;
        // 超时时间
        newseg->rto = kcp->rx_rto;
        // 设置没有fast ack过
        newseg->fastack = 0;
        newseg->xmit = 0;
    }

    // calculate resent
    // 触发快速重传的重复ACKc个数
    resent = (kcp->fastresend > 0)? (IUINT32)kcp->fastresend : 0xffffffff;
    // 打开了nodelay，重传超时时间为0
    rtomin = (kcp->nodelay == 0)? (kcp->rx_rto >> 3) : 0;

    // flush data segments
    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
        IKCPSEG *segment = iqueue_entry(p, IKCPSEG, node);
        int needsend = 0;
        //xmit为发送次数，第一次发送，设置超时时间
        if (segment->xmit == 0) {
            needsend = 1;
            // 传输次数+1
            segment->xmit++;
            // 设置重传超时时间
            segment->rto = kcp->rx_rto;
            // 设置重新发送时间
            segment->resendts = current + segment->rto + rtomin;
        }
        else if (_itimediff(current, segment->resendts) >= 0) {
            // 不是第一次发送，那么就是重传，到了重传的时间
            needsend = 1;
            segment->xmit++;
            // 增加一次这个会话重传次数
            kcp->xmit++;
            // 没有打开no_delay
            if (kcp->nodelay == 0) {
                // 设置重传等待时间
                segment->rto += _imax_(segment->rto, (IUINT32)kcp->rx_rto);
            }    else {
                // nodelay为1，就是rto
                IINT32 step = (kcp->nodelay < 2)?
                    ((IINT32)(segment->rto)) : kcp->rx_rto;
                segment->rto += step / 2;
            }
            // 设置下一次的重传时间
            segment->resendts = current + segment->rto;
            lost = 1;  // 确认这个segment之前lost
        } // 该报文被ack跳过的次数大于等于触发快速重传的次数
        else if (segment->fastack >= resent) { // 没有到达重传时间，但是也是需要重传
            // 没有超过快速重传次数
            if ((int)segment->xmit <= kcp->fastlimit ||
                kcp->fastlimit <= 0) {
                needsend = 1;
                segment->xmit++;
                // fastack清0
                segment->fastack = 0;
                // 设置重传时间
                segment->resendts = current + segment->rto;
                change++;
            }
        }
        // 需要现在发送
        if (needsend) {
            int need;
            segment->ts = current;
            segment->wnd = seg.wnd;
            // 每个报文会发送una
            segment->una = kcp->rcv_nxt;

            size = (int)(ptr - buffer);
            need = IKCP_OVERHEAD + segment->len;
            // 大于一个MTU直接发送
            if (size + need > (int)kcp->mtu) {
                // 使用协议层来传输
                ikcp_output(kcp, buffer, size);
                ptr = buffer;
            }

            // 将segment进行编码
            ptr = ikcp_encode_seg(ptr, segment);

            if (segment->len > 0) {
                memcpy(ptr, segment->data, segment->len);
                ptr += segment->len;
            }

            if (segment->xmit >= kcp->dead_link) {
                // 设置为-1
                kcp->state = (IUINT32)-1;
            }
        }
    }

    // flash remain segments
    size = (int)(ptr - buffer);
    if (size > 0) {
        ikcp_output(kcp, buffer, size);
    }

    // update ssthresh
    if (change) {
        // 当前未收到确认，但是已发送出去的报文数
        IUINT32 inflight = kcp->snd_nxt - kcp->snd_una;
        // 发送窗口上限改变
        kcp->ssthresh = inflight / 2;
        // 发送窗口没有和tcp一样, 立即为0
        if (kcp->ssthresh < IKCP_THRESH_MIN)
            kcp->ssthresh = IKCP_THRESH_MIN;
        // 当前的发送窗口改变
        kcp->cwnd = kcp->ssthresh + resent;
        // 可发送的最大数据量改变
        kcp->incr = kcp->cwnd * kcp->mss;
    }
    // 有数据包丢失，发送窗口改变
    if (lost) {
        //  发送窗口上限更新
        kcp->ssthresh = cwnd / 2;
        if (kcp->ssthresh < IKCP_THRESH_MIN)
            kcp->ssthresh = IKCP_THRESH_MIN;
        // 变成1
        kcp->cwnd = 1;
        kcp->incr = kcp->mss;
    }

    if (kcp->cwnd < 1) {
        // 至少一个报文
        kcp->cwnd = 1;
        kcp->incr = kcp->mss;
    }
}


//---------------------------------------------------------------------
// update state (call it repeatedly, every 10ms-100ms), or you can ask
// ikcp_check when to call it again (without ikcp_input/_send calling).
// 'current' - current timestamp in millisec.
//---------------------------------------------------------------------
void ikcp_update(ikcpcb *kcp, IUINT32 current)
{
    IINT32 slap;

    // 获取kcp当前时间
    kcp->current = current;

    if (kcp->updated == 0) {
        kcp->updated = 1;
        // 设置这一次flush的时间
        kcp->ts_flush = kcp->current;
    }
    // 当前时间大于等于上次flush时间
    slap = _itimediff(kcp->current, kcp->ts_flush);

    // 时间间隔太大，超过10s
    if (slap >= 10000 || slap < -10000) {
        kcp->ts_flush = kcp->current;
        slap = 0;
    }

    if (slap >= 0) {
        kcp->ts_flush += kcp->interval;
        if (_itimediff(kcp->current, kcp->ts_flush) >= 0) {
            // 设置下一次更新时间设置
            kcp->ts_flush = kcp->current + kcp->interval;
        }
        ikcp_flush(kcp);
    }
}


//---------------------------------------------------------------------
// Determine when should you invoke ikcp_update:
// returns when you should invoke ikcp_update in millisec, if there
// is no ikcp_input/_send calling. you can call ikcp_update in that
// time, instead of call update repeatly.
// Important to reduce unnacessary ikcp_update invoking. use it to
// schedule ikcp_update (eg. implementing an epoll-like mechanism,
// or optimize ikcp_update when handling massive kcp connections)
//---------------------------------------------------------------------
IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current)
{
    IUINT32 ts_flush = kcp->ts_flush;
    IINT32 tm_flush = 0x7fffffff;
    IINT32 tm_packet = 0x7fffffff;
    IUINT32 minimal = 0;
    struct IQUEUEHEAD *p;

    if (kcp->updated == 0) {
        return current;
    }

    if (_itimediff(current, ts_flush) >= 10000 ||
        _itimediff(current, ts_flush) < -10000) {
        ts_flush = current;
    }

    if (_itimediff(current, ts_flush) >= 0) {
        return current;
    }

    tm_flush = _itimediff(ts_flush, current);

    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
        const IKCPSEG *seg = iqueue_entry(p, const IKCPSEG, node);
        IINT32 diff = _itimediff(seg->resendts, current);
        if (diff <= 0) {
            return current;
        }
        if (diff < tm_packet) tm_packet = diff;
    }

    minimal = (IUINT32)(tm_packet < tm_flush ? tm_packet : tm_flush);
    if (minimal >= kcp->interval) minimal = kcp->interval;

    return current + minimal;
}



int ikcp_setmtu(ikcpcb *kcp, int mtu)
{
    char *buffer;
    if (mtu < 50 || mtu < (int)IKCP_OVERHEAD)
        return -1;
    // 设置buffer缓存大小
    buffer = (char*)ikcp_malloc((mtu + IKCP_OVERHEAD) * 3);
    if (buffer == NULL)
        return -2;
    kcp->mtu = mtu;
    kcp->mss = kcp->mtu - IKCP_OVERHEAD;
    ikcp_free(kcp->buffer);
    kcp->buffer = buffer;
    return 0;
}

int ikcp_interval(ikcpcb *kcp, int interval)
{
    if (interval > 5000) interval = 5000;
    else if (interval < 10) interval = 10;
    kcp->interval = interval;
    return 0;
}

int ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc)
{
    if (nodelay >= 0) {
        kcp->nodelay = nodelay;
        if (nodelay) {
            kcp->rx_minrto = IKCP_RTO_NDL;
        }
        else {
            // rto重传超时时间
            kcp->rx_minrto = IKCP_RTO_MIN;
        }
    }
    if (interval >= 0) {
        // 最大5s，最小10ms
        if (interval > 5000) interval = 5000;
        else if (interval < 10) interval = 10;
        kcp->interval = interval;
    }
    // 是否快速重传
    if (resend >= 0) {
        kcp->fastresend = resend;
    }
    if (nc >= 0) {
        kcp->nocwnd = nc;
    }
    return 0;
}


int ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd)
{
    if (kcp) {
        if (sndwnd > 0) {
            kcp->snd_wnd = sndwnd;
        }
        if (rcvwnd > 0) {   // must >= max fragment size
            kcp->rcv_wnd = _imax_(rcvwnd, IKCP_WND_RCV);
        }
    }
    return 0;
}

int ikcp_waitsnd(const ikcpcb *kcp)
{
    return kcp->nsnd_buf + kcp->nsnd_que;
}


// read conv
IUINT32 ikcp_getconv(const void *ptr)
{
    IUINT32 conv;
    ikcp_decode32u((const char*)ptr, &conv);
    return conv;
}


