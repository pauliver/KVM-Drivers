// websocket_server_async.h - Factory API for AsyncWebSocketServer
// Use these C++ functions to create/start/stop the async WebSocket server
// without needing to include the full class definition.
#pragma once

// maxClients: runtime connection cap, clamped to [1, WS_MAX_CLIENTS=32].
// Pass AppSettings.WsMaxClients here; defaults to 10 if omitted.
void* WsAsync_Create(int port, int maxClients = 10);
bool  WsAsync_Start(void* srv);
void  WsAsync_Stop(void* srv);
void  WsAsync_Destroy(void* srv);
