# Linux build

This folder is a standalone Linux-oriented copy. It does not modify the original Windows project.

## Third-party files you need to provide

Put Linux versions of the OPC UA and Modbus headers/libraries here:

```text
third_party/include/open62541.h
third_party/include/modbus.h
third_party/lib/libopen62541.a
third_party/lib/libmodbus.a
```

If your libraries are shared objects, place them in `third_party/lib` and adjust `LDLIBS` or `LD_LIBRARY_PATH` as needed.

## Build

```bash
cd linux_func
make clean
make
```

## Run

Edit `config.json` first. `ipv6_multicast.nic_index` must be a Linux interface index. You can check it with:

```bash
ip link
```

The program adds/removes IPv6 addresses with `ip -6 addr`, so running usually needs root privileges:

```bash
cd build
sudo ./addp_controller
```

## Notes

- `config.json` and `uploadtable.csv` are copied to `build/` by `make`.
- Linux IPv6 address add/delete uses `ip -6 addr add/del <addr>/<prefix> dev <ifname>`.
- The interface name is resolved from `nic_index` with `if_indextoname()`.
- Joining the IPv6 multicast group is required. Startup fails if `IPV6_JOIN_GROUP` fails.
- If the generated IPv6 address already exists on the interface, it is treated as success.
- `open62541.h` must be generated/built for POSIX/Linux, not the Windows `UA_ARCHITECTURE_WIN32` header from the original project.
- `modbus.h` and `libmodbus.a` must be Linux builds.
- `addp_protocol.cpp` and `device.cpp` are retained for reference/reuse, but the Makefile builds the current Linux entry path with `main_loop.cpp` directly.
