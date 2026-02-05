import sys

sys.path.append("build")  # 添加.so文件所在目录

import rdmapp_py


def main():
    if len(sys.argv) == 2:
        app = rdmapp_py.RDMAApp()
        # 服务器模式
        port = int(sys.argv[1])
        app.run_server(port)
    elif len(sys.argv) == 3:
        app = rdmapp_py.RDMAApp()
        # 客户端模式
        ip = sys.argv[1]
        port = int(sys.argv[2])
        app.run_client(ip, port)
    else:
        print(
            f"Usage: {sys.argv[0]} [port] for server and {sys.argv[0]} [server_ip] [port] for client"
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
