// websocket_server_async.h - Factory API for AsyncWebSocketServer
// Use these C++ functions to create/start/stop the async WebSocket server
// without needing to include the full class definition.
#pragma once

void* WsAsync_Create(int port);
bool  WsAsync_Start(void* srv);
void  WsAsync_Stop(void* srv);
void  WsAsync_Destroy(void* srv);
