#pragma once

#include <winsock2.h>
#include <windows.h>

#include <cstdint>

// Copie UDP vers le relais. Inactif tant que TransportStart n'a pas reussi.
// N'est pas appele quand transport=0 : aucune copie, aucune reecriture.

// 0 = transport complet. 1 = pas de depot dans le recv. 2 = pas de copie WSASendTo.
// 3 = pas de thread de heartbeat. Une seule valeur par binaire.
constexpr int kTransportCut = 0;

using TransportSendFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using TransportRecvFn = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using TransportLogFn = void (*)(const char*);
using TransportCompleteFn = void(CALLBACK*)(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);

bool TransportActive();
bool TransportSending();
bool TransportStart(const char* host, unsigned short port, TransportSendFn send_fn, TransportRecvFn recv_fn,
                    TransportLogFn log_fn, uint32_t local_ip_be = 0);
// Le thread s'arrete : plus de HEARTBEAT. Les sockets restent ouverts.
void TransportStop();
// Arret du thread, puis fermeture des sockets. Appele au dechargement.
void TransportShutdown();
void TransportCopyPayload(unsigned short game_port, const void* data, int len);
// Copie differee : Hook_WSASendTo ne doit pas appeler sendto dans le fil du jeu.
void TransportEnqueuePayload(unsigned short game_port, const void* data, int len);
void TransportNoteSocket(SOCKET sock);
void TransportNoteOutboundTarget(unsigned short game_port, const sockaddr* to, int tolen);
bool TransportTakePeer(SOCKET sock, const sockaddr* observed, int observed_len, sockaddr_in* peer);
bool TransportRewriteFrom(SOCKET sock, sockaddr* from, int* fromlen);

// Memes octets, sockaddr = adresse du pair fournie par l'appelant (l'echange du relais).
extern "C" int ScctCoopNet_Deposit(const void* payload, int payload_len, const sockaddr* peer, int peer_len, void* buf,
                                  int buf_cap, sockaddr* from, int* fromlen);
// Adresse du pair deja apprise sur ce canal (endpoint de donnees). 1 si connue.
extern "C" int ScctCoopNet_Peer(unsigned short game_port, sockaddr* out, int* out_len);
TransportCompleteFn TransportPrepareRecv(SOCKET sock, sockaddr* from, int* fromlen, LPWSAOVERLAPPED overlapped,
                                         TransportCompleteFn user_complete);
void TransportRecvDone(SOCKET sock, LPWSAOVERLAPPED overlapped, int result, bool pending);
bool TransportCompleteOverlapped(SOCKET sock, LPWSAOVERLAPPED overlapped);
