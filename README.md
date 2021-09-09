# 传输程序演示

该演示程序可以在两个镜像中间使用 DTP 协议一次一个地传输文件。

## 镜像构建

在根目录下运行类似 `docker build . -t ...` 的命令即可构建测试用镜像。

（也有已经编译好的私人镜像仓库 simonkorl0228/private_images:transport_demo）

## 测试方法

1. 使用该镜像创建两个容器，运行命令类似 `docker run -it --priviledged -n trans_server --network=my_net demo_image:0.0.1`
2. 查看服务端程序对应的虚拟网络地址。并且进入该容器运行类似下面的命令：`./server 172.18.0.2 9000`
3. 进入客户端程序对应的容器，可以创建生成一个测试用传输文件，比如：`echo "1234567890" > hello.txt`。之后运行客户端的发送程序，类似：`./client 172.18.0.2 9000 hello.txt hello.success.txt`。注意 IP 地址和端口号要和服务端相同。
4. 在**客户端程序运行结束（连接断开）以后**可以在服务端的容器中看到发送成功的文件`hello.success.txt`

## 客户端和服务端的使用

- 服务端需要指定数据的接收 IP 和端口。

```bash
./server 127.0.0.1 5555 2> server.log
```

- 运行 client：

```bash
./client 127.0.0.1 5555 2> client.log 本地文件路径  生成文件路径
```

例如:

```bash
./client 127.0.0.1 5555 2> client.log ./test_file_1  ../test_result.txt
```
