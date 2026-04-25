"# testControl-1" 

git 命令

# 1. 初始化Git（只做一次）
git init

# 2. 添加整个目录 + 所有子目录（核心命令）
git add .

# 3. 提交到本地仓库
git commit -m "添加整个项目目录和所有子目录"

# 4. 推送到GitHub
git push -u origin main


Windows open62541 的编译

cd build_mingw
cmake -G "MinGW Makefiles" .. -DUA_ENABLE_AMALGAMATION=ON

.

#编译
.
.
mingw32-make -j4 #或cmake --build . --config release
.
Ipv6 的opcua 的连接 opc.tcp://[2001:eaca:101:0:1e:cd00:ee0f:0]:4840



#程序的编译：
D:\Interest_Group\project1ai_testControl\testControl-1>mingw32-make

