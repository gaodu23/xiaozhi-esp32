"""
xiaozhi-esp32 OTA 升级服务器
协议文档: 固件发送 POST / 携带设备信息 JSON，服务器返回固件版本和下载地址。
"""

import os
import json
import logging
from flask import Flask, request, jsonify, send_from_directory

app = Flask(__name__)
logging.basicConfig(level=logging.INFO, format='%(asctime)s %(levelname)s %(message)s')
logger = logging.getLogger(__name__)

FIRMWARE_DIR = os.environ.get('FIRMWARE_DIR', '/firmware')


def parse_version(v: str):
    """将版本字符串解析为整数列表，如 '1.2.3' -> [1, 2, 3]"""
    try:
        return [int(x) for x in str(v).strip().split('.')]
    except Exception:
        return [0]


def is_newer(current: str, candidate: str) -> bool:
    """若 candidate 版本高于 current 则返回 True"""
    c = parse_version(current)
    n = parse_version(candidate)
    for i in range(min(len(c), len(n))):
        if n[i] > c[i]:
            return True
        if n[i] < c[i]:
            return False
    return len(n) > len(c)


def load_firmware_config() -> dict | None:
    """从 firmware.json 读取最新固件信息"""
    path = os.path.join(FIRMWARE_DIR, 'firmware.json')
    if not os.path.isfile(path):
        logger.warning(f'firmware.json not found at {path}')
        return None
    try:
        with open(path, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception as e:
        logger.error(f'Failed to load firmware.json: {e}')
        return None


@app.route('/', methods=['GET', 'POST'])
def ota_check():
    """
    OTA 版本检查接口，与 xiaozhi-esp32 固件协议兼容。
    请求: POST / 或 GET /，Body 为设备信息 JSON
    响应示例:
      {
        "firmware": {
          "version": "1.2.0",
          "url": "http://mapuav.top:8090/firmware/xiaozhi_v1.2.0.bin"
        }
      }
    """
    current_version = ''

    if request.method == 'POST':
        try:
            device_info = request.get_json(force=True, silent=True) or {}
            app_info = device_info.get('application', {})
            current_version = app_info.get('version', '')
        except Exception:
            pass

    # 兜底：从 User-Agent 解析版本（格式 BoardName/version）
    if not current_version:
        ua = request.headers.get('User-Agent', '')
        if '/' in ua:
            current_version = ua.rsplit('/', 1)[-1].strip()

    mac = request.headers.get('Device-Id', 'unknown')
    logger.info(f'OTA check | device={mac} | current_version={current_version!r}')

    fw = load_firmware_config()
    result = {}

    if fw:
        latest_version = fw.get('version', '')
        bin_file = fw.get('bin', '')
        if bin_file and latest_version:
            if current_version and is_newer(current_version, latest_version):
                # 构造固件下载 URL（自动使用请求 host）
                host = request.headers.get('X-Forwarded-Host', request.host)
                firmware_url = f'http://{host}/firmware/{bin_file}'
                result['firmware'] = {
                    'version': latest_version,
                    'url': firmware_url,
                }
                logger.info(f'Update available | latest={latest_version} | url={firmware_url}')
            else:
                logger.info(f'No update needed | latest={latest_version} | current={current_version}')
        else:
            logger.info('firmware.json is empty or missing version/bin field')

    return jsonify(result)


@app.route('/firmware/<path:filename>')
def download_firmware(filename):
    """固件二进制文件下载接口"""
    logger.info(f'Firmware download request: {filename}')
    return send_from_directory(FIRMWARE_DIR, filename)


if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8090, debug=False)
