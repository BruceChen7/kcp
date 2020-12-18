#ifndef __TEST_H__
#define __TEST_H__

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <string.h>

#include "ikcp.h"

#if defined(WIN32) || defined(_WIN32) || defined(WIN64) || defined(_WIN64)
#include <windows.h>
#elif !defined(__unix)
#define __unix
#endif

#ifdef __unix
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#endif

/* get system time */
static inline void itimeofday(long *sec, long *usec)
{
	#if defined(__unix)
	struct timeval time;
	gettimeofday(&time, NULL);
	if (sec) *sec = time.tv_sec;
	if (usec) *usec = time.tv_usec;
	#else
	static long mode = 0, addsec = 0;
	BOOL retval;
	static IINT64 freq = 1;
	IINT64 qpc;
	if (mode == 0) {
		retval = QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		freq = (freq == 0)? 1 : freq;
		retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
		addsec = (long)time(NULL);
		addsec = addsec - (long)((qpc / freq) & 0x7fffffff);
		mode = 1;
	}
	retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
	retval = retval * 2;
	if (sec) *sec = (long)(qpc / freq) + addsec;
	if (usec) *usec = (long)((qpc % freq) * 1000000 / freq);
	#endif
}

/* get clock in millisecond 64 */
static inline IINT64 iclock64(void)
{
	long s, u;
	IINT64 value;
	itimeofday(&s, &u);
	// 获取毫秒
	value = ((IINT64)s) * 1000 + (u / 1000);
	return value;
}

static inline IUINT32 iclock()
{
	// 取低32位
	return (IUINT32)(iclock64() & 0xfffffffful);
}

/* sleep in millisecond */
static inline void isleep(unsigned long millisecond)
{
	#ifdef __unix 	/* usleep( time * 1000 ); */
	struct timespec ts;
	ts.tv_sec = (time_t)(millisecond / 1000);
	ts.tv_nsec = (long)((millisecond % 1000) * 1000000);
	/*nanosleep(&ts, NULL);*/
	usleep((millisecond << 10) - (millisecond << 4) - (millisecond << 3));
	#elif defined(_WIN32)
	Sleep(millisecond);
	#endif
}

#ifdef __cplusplus
#include <list>
#include <vector>

// 带延迟的数据包
// 抽象的延迟包
class DelayPacket
{
public:
	virtual ~DelayPacket() {
		if (_ptr) delete _ptr;
		_ptr = NULL;
	}

	DelayPacket(int size, const void *src = NULL) {
		_ptr = new unsigned char[size];
		_size = size;
		if (src) {
			memcpy(_ptr, src, size);
		}
	}

	unsigned char* ptr() { return _ptr; }
	const unsigned char* ptr() const { return _ptr; }

	int size() const { return _size; }
	IUINT32 ts() const { return _ts; }
	void setts(IUINT32 ts) { _ts = ts; }

protected:
	unsigned char *_ptr;
	int _size;
	IUINT32 _ts;
};

// 均匀分布的随机数
class Random
{
public:
	Random(int size) {
		this->size = 0;
		seeds.resize(size);
	}

	int random() {
		int x, i;
		if (seeds.size() == 0) return 0;
		if (size == 0) { 
			for (i = 0; i < (int)seeds.size(); i++) {
				seeds[i] = i;
			}
			size = (int)seeds.size();
		}
		i = rand() % size;
		x = seeds[i];
		seeds[i] = seeds[--size];
		return x;
	}

protected:
	int size;
	std::vector<int> seeds;
};

// 网络延迟模拟器
class LatencySimulator
{
public:

	virtual ~LatencySimulator() {
		clear();
	}

	// lostrate: 往返一边丢包率的百分比，默认 10%
	// rttmin：rtt最小值，默认 60
	// rttmax：rtt最大值，默认 125
	// rtt是一个segment的往返延迟
	LatencySimulator(int lostrate = 10, int rttmin = 60, int rttmax = 125, int nmax = 1000): 
		r12(100), r21(100) {
		current = iclock();		
		this->lostrate = lostrate / 2;	// 上面数据是往返丢包率，单程除以2
		this->rttmin = rttmin / 2;
		this->rttmax = rttmax / 2;
		this->nmax = nmax;
		tx1 = tx2 = 0;
	}

	// 清除数据
	void clear() {
		DelayTunnel::iterator it;
		for (it = p12.begin(); it != p12.end(); it++) {
			delete *it;
		}
		for (it = p21.begin(); it != p21.end(); it++) {
			delete *it;
		}
		p12.clear();
		p21.clear();
	}

	// 发送数据
	// peer - 端点0/1，从0发送，从1接收；从1发送从0接收
	// 虚拟网络发送字节
	void send(int peer, const void *data, int size) {
		if (peer == 0) {
			// 发送次数 + 1
			tx1++;
			//  c从1到2的发送
			if (r12.random() < lostrate) return;
			if ((int)p12.size() >= nmax) return;
		}	else {
			// 从2到1的发送
			tx2++;
			if (r21.random() < lostrate) return;
			if ((int)p21.size() >= nmax) return;
		}
		// 新建一个延迟包
		DelayPacket *pkt = new DelayPacket(size, data);
		// 获取当前的事件戳
		current = iclock();
		IUINT32 delay = rttmin;
		// 书籍一个延迟
		if (rttmax > rttmin) delay += rand() % (rttmax - rttmin);
		pkt->setts(current + delay);
		// 添加到1到2的队列中
		if (peer == 0) {
			// push 到发送队列中
			p12.push_back(pkt);
		}	else {
			//  添加到2到1的独立额中
			p21.push_back(pkt);
		}
	}

	// 接收数据，返回接收到的paakets size
	int recv(int peer, void *data, int maxsize) {
		DelayTunnel::iterator it;
		// 从1中接收
		if (peer == 0) {
			it = p21.begin();
			if (p21.size() == 0) return -1;
		}	else {
			// 获取从1到2的udp包
			it = p12.begin();
			if (p12.size() == 0) return -1;
		}
		// 获取一个报文
		DelayPacket *pkt = *it;
		current = iclock();
		// 时间没有到，直接返回
		if (current < pkt->ts()) return -2;
		// 超过了最大的
		if (maxsize < pkt->size()) return -3;
		// 从对应的通道中，删除对应的包
		if (peer == 0) {
			p21.erase(it);
		}	else {
			// 
			p12.erase(it);
		}
		maxsize = pkt->size();
		// 拷贝数据到data中
		memcpy(data, pkt->ptr(), maxsize);
		delete pkt;
		return maxsize;
	}

public:
	int tx1;
	int tx2;

protected:
	IUINT32 current;
	int lostrate;
	int rttmin;
	int rttmax;
	int nmax;
	typedef std::list<DelayPacket*> DelayTunnel;
	DelayTunnel p12;
	DelayTunnel p21;
	Random r12;
	Random r21;
};

#endif

#endif


