if (Test-Path "build") {
    Remove-Item -Path "build" -Recurse -Force
}

New-Item -ItemType Directory -Path "build" -Force

$commonFlags = @("-Wall", "-Wextra", "-std=c++11", "-O2", "-static", "-static-libgcc", "-static-libstdc++", "-g", "-Iinclude", "-Isrc")

g++ @commonFlags -c src\addp_protocol.cpp -o build\addp_protocol.o
g++ @commonFlags -c src\device.cpp -o build\device.o
g++ @commonFlags -c src\ipv6_server.cpp -o build\ipv6_server.o
g++ @commonFlags -c src\main_loop.cpp -o build\main_loop.o
g++ @commonFlags -c src\network.cpp -o build\network.o

g++ @commonFlags -c src\cJSON.c -o build\cJSON.o
g++ @commonFlags -c src\cJSON_Utils.c -o build\cJSON_Utils.o
g++ @commonFlags -c src\csv_parser.c -o build\csv_parser.o
g++ @commonFlags -c src\ipv6_manager.c -o build\ipv6_manager.o
g++ @commonFlags -c src\log_manager.c -o build\log_manager.o
g++ @commonFlags -c src\modbus_client.c -o build\modbus_client.o
g++ @commonFlags -c src\opcua_server.c -o build\opcua_server.o

$objects = Get-ChildItem -Path "build" -Filter "*.o" | ForEach-Object { $_.FullName }
$linkFlags = @("-Wall", "-Wextra", "-std=c++11", "-O2", "-static", "-static-libgcc", "-static-libstdc++", "-g")
g++ @linkFlags @objects -o build\addp_controller.exe -lws2_32 -liphlpapi "-Wl,-Bstatic" -lstdc++ -lpthread "-Wl,-Bdynamic" lib\libmodbus.a lib\libopen62541.a

Copy-Item -Path "config.json" -Destination "build\"
Copy-Item -Path "uploadtable.csv" -Destination "build\"

Write-Host "Build complete: build\addp_controller.exe"
