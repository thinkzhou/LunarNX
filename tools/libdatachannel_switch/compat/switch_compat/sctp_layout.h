#pragma once
#include <cstddef>
#include <cstdint>
#ifndef HAVE_SCONN_LEN
#error All Horizon SCTP consumers must use HAVE_SCONN_LEN
#endif
static_assert(offsetof(sockaddr_conn, sconn_len) == offsetof(sockaddr, sa_len));
static_assert(offsetof(sockaddr_conn, sconn_family) == offsetof(sockaddr, sa_family));
static_assert(sizeof(sockaddr_conn::sconn_family) == sizeof(sockaddr::sa_family));
static_assert(sizeof(sockaddr_conn) <= sizeof(sctp_sockstore));
static_assert(sizeof(sockaddr_conn) <= UINT8_MAX);
