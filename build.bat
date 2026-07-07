@echo off

if exist build rmdir /s /q build
mkdir build

set COMMON_FLAGS=/c /Iinclude /Isrc /O2

cl %COMMON_FLAGS% src\addp_protocol.cpp /Fo:build\addp_protocol.obj
cl %COMMON_FLAGS% src\cJSON.c /Fo:build\cJSON.obj
cl %COMMON_FLAGS% src\cJSON_Utils.c /Fo:build\cJSON_Utils.obj
cl %COMMON_FLAGS% src\csv_parser.c /Fo:build\csv_parser.obj
cl %COMMON_FLAGS% src\device.cpp /Fo:build\device.obj
cl %COMMON_FLAGS% src\ipv6_manager.c /Fo:build\ipv6_manager.obj
cl %COMMON_FLAGS% src\ipv6_server.cpp /Fo:build\ipv6_server.obj
cl %COMMON_FLAGS% src\log_manager.c /Fo:build\log_manager.obj
cl %COMMON_FLAGS% src\main_loop.cpp /Fo:build\main_loop.obj
cl %COMMON_FLAGS% src\modbus_client.c /Fo:build\modbus_client.obj
cl %COMMON_FLAGS% src\network.cpp /Fo:build\network.obj
cl %COMMON_FLAGS% src\opcua_server.c /Fo:build\opcua_server.obj

link /OUT:build\addp_controller.exe build\*.obj lib\libmodbus.a lib\libopen62541.a ws2_32.lib iphlpapi.lib

copy config.json build\
copy uploadtable.csv build\

echo Build complete: build\addp_controller.exe
