#include <winsock2.h>
#include <stdio.h>
#include <WS2tcpip.h> 

#define SERVER_IP "192.168.43.251"
#define SERVER_PORT 55555
#define DEFAULT_PORT 80
#define REQUEST_BUF_SIZE 1024 * 1024
#define SLEEP_TIME 4000
#define CONFIG_FILE_PATH "Proxy-server.conf"
#define BLACKLIST_BUF_SIZE 1024
#define DEFAULT_ANSWER_1 "<h1>Unable to connect to server</h1>"
#define DEFAULT_ANSWER_2 " is in the blacklist</h2>"

#pragma comment(lib, "Ws2_32.lib")

typedef struct ThreadInfo
{
	SOCKET BrowserSocket;
	SOCKET ServerSocket;
	HANDLE hBrowserMutex;
	HANDLE hServerMutex;
	HANDLE hBrowserThread;
	HANDLE hServerThread;
} ThreadInfo;

typedef struct HTTPPacket
{
	char *StartingLine;
	int HeadersCount;
	char **Headers;
//	unsigned short Port;
} HTTPPacket;

typedef struct BlackListItem
{
	char *BlockedHost;
	struct BlackListItem *Next;
} BlackListItem;

typedef struct AfterAcceptInfo
{
	SOCKET AcceptSocket;
	BlackListItem *Header;
	BlackListItem *Last;
} AcceptInfo;

int IndexOf(char *Str, char *SubStr)
{
	char *CmpStr = (char *)calloc(1, strlen(SubStr)+1);
	int Index = 0, Len = strlen(Str);
	if (strlen(Str) >= strlen(SubStr))
	{
		do
		{
				memcpy(CmpStr, Str++, strlen(SubStr));
				Index++;
		}
		while (strcmp(CmpStr, SubStr) && strlen(Str) >= strlen(SubStr));
		free(CmpStr);
		return Index <= Len ? Index-1 : -1;
	}
	else
	{
		free(CmpStr);
		return -1;
	}
}

char *FindInHeadersList(HTTPPacket *Pack, char *FindValue)
{
	int i, Index;
	char *CurrentStr, *Result;
	if (Pack->HeadersCount > 0)
	{
		for (i = 0; i < Pack->HeadersCount; i++)
		{
			Index = IndexOf(*(Pack->Headers + i), ":");
			CurrentStr = (char *)calloc(1, Index + 1);
			memcpy(CurrentStr, *(Pack->Headers + i), Index);
			if (!strcmp(CurrentStr, FindValue))
			{
				Result = (char *)calloc(1, strlen(*(Pack->Headers + i)) - Index - 1);
				memcpy(Result, *(Pack->Headers + i) + Index + 2, strlen(*(Pack->Headers + i)) - Index - 2);
				free(CurrentStr);
				return Result;
			}
		}
		free(CurrentStr);
	}
	return NULL;
}

bool IsValidHTTP(char *StartStr)
{
	if (strlen(StartStr) > strlen("HTTP/1.0"))
	{
		char *BeginOfStr = (char *)calloc(1, strlen("HTTP/1.0") + 1), *EndOfStr = (char *)calloc(1, strlen("HTTP/1.0") + 1);
		memcpy(BeginOfStr, StartStr, strlen("HTTP/1.0"));
		memcpy(EndOfStr, StartStr + strlen(StartStr) - strlen("HTTP/1.0"), strlen("HTTP/1.0"));
		if (strcmp(BeginOfStr, "HTTP/1.0") && strcmp(BeginOfStr, "HTTP/1.1") && strcmp(EndOfStr, "HTTP/1.1") && strcmp(EndOfStr, "HTTP/1.0"))
			return false;
		else
			return true;
	}
	else
		return false;
}

HTTPPacket *ParseHTTP(char *ParsedValue)
{
	int Index;

	HTTPPacket *Result = (HTTPPacket *)calloc(1, sizeof(HTTPPacket));
	Index = IndexOf(ParsedValue, "\r\n");
	if (Index > 0)
	{
		Result->StartingLine = (char *)calloc(1, Index + 1);
		memcpy(Result->StartingLine, ParsedValue, Index);
		if (IsValidHTTP(Result->StartingLine))
		{
			ParsedValue += Index +  2;
			
			Result->HeadersCount = 0;
			while (*(ParsedValue) != '\r' && *(ParsedValue + 1) != '\n')
			{
				Index = IndexOf(ParsedValue, "\r\n");
				Result->Headers = (char **)realloc(Result->Headers, sizeof(char *) * (Result->HeadersCount + 1));
				*(Result->Headers + Result->HeadersCount) = (char *)calloc(1, Index + 1); 
				memcpy(*(Result->Headers + Result->HeadersCount), ParsedValue, Index);
				(Result->HeadersCount)++;			
				ParsedValue += Index + 2;
			}

			ParsedValue += 2;
		}
		else
		{
			free(Result->StartingLine);
			Result->StartingLine = NULL;
			Result->Headers = NULL;
			Result->HeadersCount = 0;
		}
	}
	else
	{
		Result->StartingLine = NULL;
		Result->Headers = NULL;
		Result->HeadersCount = 0;
	}
	return Result;
}

void AddToBlackList(BlackListItem **Current, char *Host)
{
	(*Current)->Next = (BlackListItem *)calloc(1, sizeof(BlackListItem));
	(*Current) = (*Current)->Next;
	(*Current)->Next = NULL;
	(*Current)->BlockedHost = Host;
}

bool IsInBlackList(BlackListItem *Item, char *Host)
{
	for (; Item != NULL; Item = Item->Next)
	{
		if (!strcmp(Item->BlockedHost, Host))
			return true;
	}
	return false;
}

DWORD WINAPI ProcessServerDataThread(LPVOID lpParam)
{
	char *RecvBuf = (char *)calloc(1, REQUEST_BUF_SIZE);
	HTTPPacket *Pack;
	int Res;
	while (Res = recv(((ThreadInfo *)lpParam)->ServerSocket, RecvBuf, REQUEST_BUF_SIZE, 0))
	{
		if (Res == SOCKET_ERROR)
		{
			WaitForSingleObject(((ThreadInfo *)lpParam)->hServerMutex, INFINITE);
			shutdown(((ThreadInfo *)lpParam)->ServerSocket, SD_BOTH);
			closesocket(((ThreadInfo *)lpParam)->ServerSocket);
			ReleaseMutex(((ThreadInfo *)lpParam)->hServerMutex);
			return 0;
		}
		else
		{
			Pack = ParseHTTP(RecvBuf);
			if (Pack->StartingLine != NULL)
			{
				char *Tmp = FindInHeadersList(Pack, "Host");
				printf("Server send: %s %s\n", Tmp == NULL ? "" : Tmp, Pack->StartingLine);
				if (Tmp != NULL)
					free(Tmp);
			}
			WaitForSingleObject(((ThreadInfo *)lpParam)->hBrowserMutex, INFINITE);
			if (send(((ThreadInfo *)lpParam)->BrowserSocket, RecvBuf, Res, 0) == SOCKET_ERROR)
			{
				shutdown(((ThreadInfo *)lpParam)->ServerSocket, SD_BOTH);
				closesocket(((ThreadInfo *)lpParam)->ServerSocket);
				return 0;
			}
			ReleaseMutex(((ThreadInfo *)lpParam)->hBrowserMutex);
		}
	}
	WaitForSingleObject(((ThreadInfo *)lpParam)->hServerMutex, INFINITE);
	shutdown(((ThreadInfo *)lpParam)->ServerSocket, SD_BOTH);
	closesocket(((ThreadInfo *)lpParam)->ServerSocket);
	ReleaseMutex(((ThreadInfo *)lpParam)->hServerMutex);
	return 0;
}

DWORD WINAPI ProcessSingleRequestThread(LPVOID lpParam)
{
	ThreadInfo Info;
	Info.hBrowserMutex = CreateMutex(NULL, false, NULL);
	Info.BrowserSocket = ((AcceptInfo *)lpParam)->AcceptSocket;
	char *RecvBuf = (char *)calloc(1, REQUEST_BUF_SIZE);
	HTTPPacket *Pack;

	int Res = recv(Info.BrowserSocket, RecvBuf, REQUEST_BUF_SIZE, 0);
	
	if (Res == SOCKET_ERROR)
	{
		printf("BrowserSocket return error: %d\n", WSAGetLastError());
		Sleep(SLEEP_TIME);
		return 1;
	}

	Pack = ParseHTTP(RecvBuf);

	if (Pack->StartingLine != NULL)
	{	
		struct addrinfo *ServerAddressInfo, *ResultAddresses = NULL;
		ServerAddressInfo = (addrinfo *)calloc(1, sizeof(addrinfo));
		ServerAddressInfo->ai_family = AF_INET;
		ServerAddressInfo->ai_socktype = SOCK_STREAM;
		ServerAddressInfo->ai_protocol = IPPROTO_TCP;

		sockaddr_in *ServerAddress;
		
		char *HostName = FindInHeadersList(Pack, "Host");

		if (!IsInBlackList(((AcceptInfo *)lpParam)->Header->Next, HostName))
		{
			if (getaddrinfo(FindInHeadersList(Pack, "Host"), NULL, ServerAddressInfo, &ResultAddresses) == 0)
			{
				Info.hServerMutex = CreateMutex(NULL, false, NULL);
				Info.ServerSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
				if (Info.ServerSocket == INVALID_SOCKET)
				{
					printf("ServerSocket: socket return error: %d\n", WSAGetLastError());
				}

				sockaddr_in ConnectAddress;
				ConnectAddress.sin_addr = ((sockaddr_in *)ResultAddresses->ai_addr)->sin_addr;
				ConnectAddress.sin_family = AF_INET;
				ConnectAddress.sin_port = htons(DEFAULT_PORT);
				
				if (connect(Info.ServerSocket, (sockaddr *)&ConnectAddress, sizeof(sockaddr_in)) == SOCKET_ERROR)
				{
					printf("ServerSocket: connect return error: %d\n", WSAGetLastError());
				}
	
				if (send(Info.ServerSocket, RecvBuf, strlen(RecvBuf), 0) == SOCKET_ERROR)
				{
					printf("BrowserThread: send return error: %d\n", WSAGetLastError());
				}
				else
				{
					char *Tmp = FindInHeadersList(Pack, "Host");
					printf("Browser send: %s %s\n", Tmp == NULL ? "" : Tmp, Pack->StartingLine);
					if (Tmp != NULL)
						free(Tmp);
				}

				Info.hServerThread = CreateThread(NULL, 0, &ProcessServerDataThread, &Info, 0, 0);
			
				while (Res = recv(Info.BrowserSocket, RecvBuf, REQUEST_BUF_SIZE, 0))
				{
					if (Res == SOCKET_ERROR)
					{
						WaitForSingleObject(Info.hBrowserMutex, INFINITE);
						shutdown(Info.BrowserSocket, SD_BOTH);
						closesocket(Info.BrowserSocket);
						ReleaseMutex(Info.hBrowserMutex);
					}
					else
					{
						WaitForSingleObject(Info.hServerMutex, INFINITE);
						if (send(Info.ServerSocket, RecvBuf, Res, 0) == SOCKET_ERROR)
						{
							shutdown(Info.BrowserSocket, SD_BOTH);
							closesocket(Info.BrowserSocket);
							return 0;
						}
						ReleaseMutex(Info.hServerMutex);
					}
				}
				WaitForSingleObject(Info.hBrowserMutex, INFINITE);
				shutdown(Info.BrowserSocket, SD_BOTH);
				closesocket(Info.BrowserSocket);
				ReleaseMutex(Info.hBrowserMutex);
				WaitForSingleObject(Info.hServerThread, INFINITE);
			}
			else
			{
				return 0;
			}
		}
		else
		{
			char *Answer = (char *)calloc(1, strlen(DEFAULT_ANSWER_1) + 4 + strlen(HostName) + strlen(DEFAULT_ANSWER_2) + 1);
			strcpy(Answer, DEFAULT_ANSWER_1);
			strcat(Answer, "<h2>");
			strcat(Answer, HostName);
			strcat(Answer, DEFAULT_ANSWER_2);
			if (send(Info.BrowserSocket, Answer, strlen(Answer), 0) == SOCKET_ERROR) 
				printf("Answer: send return error: %d\n", WSAGetLastError());
			shutdown(Info.BrowserSocket, SD_BOTH);
			closesocket(Info.BrowserSocket);
		}
	}
	return 0;
}

int main(int argc, char **argv)
{
	WSADATA wsaData;

	if (WSAStartup(MAKEWORD(2,2), &wsaData) != 0)
	{
		printf("WSAStartup return error: %d\n", WSAGetLastError());
		Sleep(SLEEP_TIME);
		return 1;
	}

	SOCKET ListeningSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (ListeningSocket == INVALID_SOCKET)
	{
		printf("ListeningSocket: socket return error: %d\n", WSAGetLastError());
		Sleep(SLEEP_TIME);
		return 1;
	}

	sockaddr_in ListeningAddress;
	ListeningAddress.sin_family = AF_INET;
	ListeningAddress.sin_port = htons(SERVER_PORT);
	ListeningAddress.sin_addr.s_addr = inet_addr(SERVER_IP);

	if (bind(ListeningSocket, (SOCKADDR *)&ListeningAddress, sizeof(ListeningAddress)) == SOCKET_ERROR)
	{
		printf("ListeningSocket: bind return error: %d\n", WSAGetLastError());
		Sleep(SLEEP_TIME);
		return 1;
	}

	if (listen(ListeningSocket, SOMAXCONN) == SOCKET_ERROR)
	{
		printf("ListeningSocket: listen return error: %d\n", WSAGetLastError());
		Sleep(SLEEP_TIME);
		return 1;
	}

	printf("Waiting for request\n");

	AcceptInfo *AccInfo = (AcceptInfo *)calloc(1, sizeof(AcceptInfo));

	AccInfo->Header = (BlackListItem *)calloc(1, sizeof(AcceptInfo));
	AccInfo->Last = AccInfo->Header;
	AccInfo->Last->BlockedHost = "";
	AccInfo->Last->Next = NULL;

	FILE *ConfFile;

	ConfFile = fopen(CONFIG_FILE_PATH, "rt");
	if (ConfFile != NULL)
	{
		while (!feof(ConfFile))
		{
			char *BlackListBuf = (char *)calloc(1, BLACKLIST_BUF_SIZE);
			fscanf(ConfFile, "%s", BlackListBuf);
			AddToBlackList(&AccInfo->Last, BlackListBuf);
		}
		fclose(ConfFile);
	}

	while (AccInfo->AcceptSocket = accept(ListeningSocket, NULL, 0))
	{
		if (AccInfo->AcceptSocket != INVALID_SOCKET)
		{
			CreateThread(NULL, 0, &ProcessSingleRequestThread, &AccInfo->AcceptSocket, 0, 0);
		}
		else
		{
			printf("accept return error: %d\n", WSAGetLastError());
		}
	}
}