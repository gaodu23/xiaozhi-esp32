"""
xiaozhi-esp32 OTA 升级服务器（代理模式）
工作方式：
  1. 将设备请求转发到上游 api.tenclass.net（保留 MQTT/WebSocket 配置）
  2. 若本地 firmware.json 中有更高版本，覆盖响应中的 firmware 字段
  3. 若本地无更新，直接透传上游响应（设备仍可获取 MQTT 服务配置）
"""

import os
import json
import logging
import requests
from flask import Flask, request, jsonify, send_from_directory

app = Flask(__name__)
logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
logger = logging.getLogger(__name__)

FIRMWARE_DIR = os.environ.get('FIRMWARE_DIR', '/firmware')
UPSTREAM_URL = os.environ.get('UPSTREAM_URL', 'https://api.tenclass.net/xiaozhi/ota/')
# 上游请求超时（秒）
UPSTREAM_TIMEOUT = int(os.environ.get('UPSTREAM_TIMEOUT', '10'))


def parse_version(v: str):
    try:
        return [int(x) for x in str(v).strip().split('.')]
    except Exception:
        return [0]


def is_newer(current: str, candidate: str) -> bool:
    c = parse_version(current)
    n = parse_version(candidate)
    for i in range(min(len(c), len(n))):
        if n[i] > c[i]:
            return True
        if n[i] < c[i]:
            return False
    return len(n) > len(c)


def load_firmware_config() -> dict | None:
    path = os.path.join(FIRMWARE_DIR, 'firmware.json')
    if not os.path.isfile(path):
        return None
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception as e:
        logger.error(f'Failed to load firmware.json: {e}')
        return None


def forward_to_upstream(body: bytes, headers: dict) -> dict | None:
    """将请求转发到上游服务器，返回解析后的 JSON 或 None"""
    forward_headers = {}
    # 转发设备身份相关头部
    for key in ('User-Agent', 'Device-Id', 'Client-Id', 'Serial-Number',
                'Activation-Version', 'Accept-Language', 'Content-Type'):
        val = headers.get(key)
        if val:
            forward_headers[key] = val

    try:
        resp = requests.post(
            UPSTREAM_URL,
            data=body,
            headers=forward_headers,
            timeout=UPSTREAM_TIMEOUT,
            verify=True,
        )
        if resp.status_code == 200:
            return resp.json()
        else:
            logger.warning(f'Upstream returned status {resp.status_code}')
    except Exception as e:
        logger.warning(f'Upstream request failed: {e}')
    return None


@app.route('/', methods=['GET', 'POST'])
def ota_check():
    """
    OTA 版本检查接口（代理 + 固件注入）
    """
    raw_body = request.get_data()
    mac = request.headers.get('Device-Id', 'unknown')

    # 解析当前版本
    current_version = ''
    try:
        body_json = json.loads(raw_body) if raw_body else {}
        current_version = body_json.get('application', {}).get('version', '')
    except Exception:
        pass
    if not current_version:
        ua = request.headers.get('User-Agent', '')
        if '/' in ua:
            current_version = ua.rsplit('/', 1)[-1].strip()

    logger.info(f'OTA check | device={mac} | current_version={current_version!r}')

    # 1. 转发到上游，拿完整配置（MQTT / WebSocket / activation / server_time）
    upstream_result = forward_to_upstream(raw_body, dict(request.headers))
    result = upstream_result if upstream_result else {}

    # 2. 检查本地是否有更新固件
    fw = load_firmware_config()
    if fw:
        latest_version = fw.get('version', '')
        bin_file = fw.get('bin', '')
        if bin_file and latest_version and current_version:
            if is_newer(current_version, latest_version):
                host = request.headers.get('X-Forwarded-Host', request.host)
                firmware_url = f'http://{host}/firmware/{bin_file}'
                result['firmware'] = {
                    'version': latest_version,
                    'url': firmware_url,
                }
                logger.info(f'Local update available | {current_version} -> {latest_version} | {firmware_url}')
            else:
                logger.info(f'No local update | latest={latest_version} | current={current_version}')

    return jsonify(result)


@app.route('/firmware/<path:filename>')
def download_firmware(filename):
    logger.info(f'Firmware download: {filename}')
    return send_from_directory(FIRMWARE_DIR, filename)


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8090, debug=False)



if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8090, debug=False)
