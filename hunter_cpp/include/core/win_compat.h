#pragma once

// Windows compatibility layer for Linux
// Provides Windows socket types and functions on non-Windows platforms

#ifndef _WIN32

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <time.h>

// Type aliases
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)

// Windows types
typedef unsigned long DWORD;
typedef unsigned long u_long;
typedef int HANDLE;
#define INVALID_HANDLE_VALUE (-1)
#define NO_ERROR 0

// Function mappings
inline int closesocket(int fd) { return ::close(fd); }
inline void Sleep(unsigned long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, nullptr);
}

// ioctlsocket -> fcntl non-blocking mode
#define FIONBIO 1
inline int ioctlsocket(int fd, int cmd, u_long* mode) {
    if (cmd == FIONBIO && *mode) {
        return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    }
    return 0;
}

// ICMP stubs (Linux uses raw sockets instead)
inline HANDLE IcmpCreateFile() { return -1; }
inline void IcmpCloseHandle(HANDLE) {}
inline DWORD IcmpSendEcho(HANDLE, unsigned int, const void*, int, const void*, void*, DWORD, DWORD) { return 0; }

// IP forwarding stubs
struct MIB_IPFORWARDROW {
    DWORD dwForwardDest;
    DWORD dwForwardMask;
    DWORD dwForwardPolicy;
    DWORD dwForwardNextHop;
    DWORD dwForwardIfIndex;
    DWORD dwForwardType;
    DWORD dwForwardProto;
    DWORD dwForwardAge;
    DWORD dwForwardNextHopAS;
    DWORD dwForwardMetric1;
    DWORD dwForwardMetric2;
    DWORD dwForwardMetric3;
    DWORD dwForwardMetric4;
    DWORD dwForwardMetric5;
};
inline DWORD CreateIpForwardEntry(MIB_IPFORWARDROW*) { return NO_ERROR; }
inline DWORD SetIpForwardEntry(MIB_IPFORWARDROW*) { return NO_ERROR; }
inline DWORD DeleteIpForwardEntry(MIB_IPFORWARDROW*) { return NO_ERROR; }
inline DWORD GetBestRoute(DWORD, DWORD, MIB_IPFORWARDROW* row) {
    if (row) memset(row, 0, sizeof(*row));
    return 0; // NO_ERROR
}

// ICMP echo reply stub
struct ICMP_ECHO_REPLY { DWORD Address; DWORD Status; unsigned long RoundTripTime; };
struct IP_OPTION_INFORMATION { unsigned char Tos; unsigned char Ttl; unsigned char Flags; unsigned char OptionsSize; unsigned char* OptionsData; };
typedef ICMP_ECHO_REPLY* PICMP_ECHO_REPLY;
#define IP_SUCCESS 0
#define IP_TTL_EXPIRED_TRANSIT 11000
#define IP_TTL_EXPIRED_REASSEM 11001

// Error codes
#define ERROR_SUCCESS 0L
#define ERROR_INVALID_PARAMETER 87L
#define ERROR_BAD_ARGUMENTS 160L
#define ERROR_ACCESS_DENIED 5L
#define ERROR_ALREADY_EXISTS 183L
#define ERROR_NOT_FOUND 1168L
#define ERROR_BUFFER_OVERFLOW 111L
#define ERROR_OBJECT_ALREADY_EXISTS 183L

// Network adapter stubs
typedef unsigned long ULONG;
typedef ULONG IPAddr;
typedef struct _IP_ADAPTER_INFO IP_ADAPTER_INFO;
typedef IP_ADAPTER_INFO* PIP_ADAPTER_INFO;
struct _IP_ADAPTER_INFO { IP_ADAPTER_INFO* Next; };
inline DWORD GetAdaptersInfo(PIP_ADAPTER_INFO, ULONG*) { return ERROR_NOT_FOUND; }
struct IP_ADAPTER_ADDRESSES;
typedef IP_ADAPTER_ADDRESSES* PIP_ADAPTER_ADDRESSES;
#define GAA_FLAG_INCLUDE_PREFIX 0
inline DWORD GetAdaptersAddresses(ULONG, ULONG, void*, PIP_ADAPTER_ADDRESSES, ULONG*) { return ERROR_NOT_FOUND; }
inline DWORD SendARP(IPAddr, DWORD, void*, ULONG*) { return 1; }

typedef int BOOL;

// Routing table stub
struct MIB_IPFORWARDTABLE { DWORD dwNumEntries; MIB_IPFORWARDROW table[1]; };
typedef MIB_IPFORWARDTABLE* PMIB_IPFORWARDTABLE;
inline DWORD GetIpForwardTable(PMIB_IPFORWARDTABLE, ULONG*, BOOL) { return ERROR_NOT_FOUND; }

// Misc Windows functions
inline DWORD GetLastError() { return 0; }
#define MIB_IPROUTE_TYPE_INDIRECT 4
#define MIB_IPPROTO_NETMGMT 3

// IPPROTO constants are already defined in Linux headers

#endif // _WIN32
