FROM python:3.11-slim

WORKDIR /app

RUN pip install --no-cache-dir flask==3.1.0 \
    -i https://mirrors.tuna.tsinghua.edu.cn/pypi/web/simple \
    --trusted-host mirrors.tuna.tsinghua.edu.cn

COPY app.py .

# 固件存放目录（通过 volume 挂载）
VOLUME ["/firmware"]

EXPOSE 8090

CMD ["python", "app.py"]
