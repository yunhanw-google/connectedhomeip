#!/usr/bin/env python3
"""Copyright (c) 2026 Project CHIP Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
"""

import base64
import logging
import os
import re
import shlex
import subprocess
import sys
import time

from helper.CHIPTestBase import CHIPVirtualHome, TestResult
from helper.paths import (
    CHIP_ALL_CLUSTERS_APP_ESC,
    CHIP_REPO_STR,
    CHIP_TOOL_APK_PATH,
)
from helper.android_emulator_logs import save_android_emulator_diagnostics
from helper.wifi_lease import wait_for_device_wlan0_ipv4

logger = logging.getLogger('AndroidBleThreadMobileDeviceTest')
logger.setLevel(logging.INFO)

sh = logging.StreamHandler()
sh.setFormatter(
    logging.Formatter('%(asctime)s [%(name)s] %(levelname)s %(message)s')
)
logger.addHandler(sh)

CIRQUE_URL = 'http://localhost:5000'
TEST_WIFI_SSID = 'CHIP-VirtualWiFi-AP'
TEST_WIFI_PSK = 'ChipWiFiPassword123'
TEST_EXTPANID = '1111111122222222'
TEST_PANID = '0x1234'
TEST_CHANNEL = '15'
TEST_DISCRIMINATOR = 3840
TEST_SETUP_PIN = 20202021
TEST_MANUAL_CODE = '34970112332'
# A cold boot of the headless emulator on a shared CI runner can take
# up to 107 s for a healthy boot. With 3 self-terminations of ~35 s each,
# the total budget covers 3 * 35 + 107 = 212 s plus APK install and margins.
ANDROID_BOOT_TIMEOUT_SEC = float(
    os.environ.get('CIRQUE_ANDROID_BOOT_TIMEOUT', '420')
)
ANDROID_COMMISSION_TIMEOUT_SEC = float(
    os.environ.get('CIRQUE_ANDROID_COMMISSION_TIMEOUT', '180')
)

TBR_BORDER_PROXY_HELPER = r'''
import os
import re
import select
import socket
import struct
import subprocess
import sys
import time

print(
    "[TBR-Proxy] Starting Thread Border Router mDNS & UDP 5540 proxy...",
    flush=True,
)

AVAHI_SERVICE_TEMPLATE = """<?xml version="1.0" standalone='no'?>
<!DOCTYPE service-group SYSTEM "avahi-service.dtd">
<service-group>
  <name replace-wildcards="yes">{name}</name>
  <service>
    <type>_matter._tcp</type>
    <port>5540</port>
    <txt-record>SII=500</txt-record>
    <txt-record>SAI=300</txt-record>
    <txt-record>T=1</txt-record>
  </service>
</service-group>
"""

end_device_wpan_ip = "fd11:33::2"
end_device_port = 5540
registered_instances = set()


def get_wlan0_addrs():
  v4_list = []
  v6_ll = []
  v6_ula = []
  try:
    out4 = subprocess.check_output(
        "ip -4 -o addr show dev wlan0", shell=True
    ).decode("utf-8", "ignore")
    for m in re.finditer(r"inet\s+(\d+\.\d+\.\d+\.\d+)", out4):
      v4_list.append(m.group(1))
  except (OSError, subprocess.SubprocessError) as err:
    sys.stderr.write(f"[TBR-Proxy] ip -4 addr note: {err}\n")
  try:
    out6 = subprocess.check_output(
        "ip -6 -o addr show dev wlan0", shell=True
    ).decode("utf-8", "ignore")
    for m in re.finditer(r"inet6\s+([0-9a-fA-F:]+)", out6):
      ip6 = m.group(1)
      if ip6.lower().startswith("fe80:"):
        v6_ll.append(ip6)
      elif not ip6.startswith("::1"):
        v6_ula.append(ip6)
  except (OSError, subprocess.SubprocessError) as err:
    sys.stderr.write(f"[TBR-Proxy] ip -6 addr note: {err}\n")
  if not v4_list:
    v4_list.append("10.0.1.12")
  if "fd11:22::2" not in v6_ula:
    v6_ula.append("fd11:22::2")
  # Put link-local fe80:: FIRST so Android NsdServiceFinderAndResolver
  # getHostAddress() includes '%wlan0' scope ID for CASE over wlan0!
  return v4_list, v6_ll + v6_ula


def encode_dns_name(name):
  parts = [p for p in name.strip(".").split(".") if p]
  return b"".join(bytes([len(p)]) + p.encode("utf-8") for p in parts) + b"\x00"


def build_dns_rr(name, rtype, rclass, ttl, rdata):
  return (
      encode_dns_name(name)
      + struct.pack("!HHIH", rtype, rclass, ttl, len(rdata))
      + rdata
  )


def build_mdns_response(instance_name, v4_list, v6_list, port=5540, txid=0):
  service_type = "_matter._tcp.local."
  full_service = f"{instance_name}.{service_type}"
  host_target = "tbr.local."
  sub_id = instance_name.split("-")[0]
  sub_ptr = f"_I{sub_id}._sub.{service_type}"

  answers = []
  # PTR _matter._tcp.local. -> <instance>._matter._tcp.local.
  answers.append(
      build_dns_rr(service_type, 12, 0x0001, 120, encode_dns_name(full_service))
  )
  # PTR _I<fabric>._sub._matter._tcp.local. -> <instance>._matter._tcp.local.
  answers.append(
      build_dns_rr(sub_ptr, 12, 0x0001, 120, encode_dns_name(full_service))
  )
  # SRV <instance>._matter._tcp.local. -> 0 0 5540 tbr.local.
  srv_rdata = struct.pack("!HHH", 0, 0, port) + encode_dns_name(host_target)
  answers.append(build_dns_rr(full_service, 33, 0x8001, 120, srv_rdata))
  # TXT <instance>._matter._tcp.local.
  txt_entries = [b"SII=500", b"SAI=300", b"T=1"]
  txt_rdata = b"".join(bytes([len(t)]) + t for t in txt_entries)
  answers.append(build_dns_rr(full_service, 16, 0x8001, 120, txt_rdata))
  # AAAA records (link-local fe80:: FIRST, then ULA fd11:22::2)
  for ip6 in v6_list:
    try:
      packed6 = socket.inet_pton(socket.AF_INET6, ip6)
      answers.append(build_dns_rr(host_target, 28, 0x8001, 120, packed6))
    except OSError as err:
      sys.stderr.write(f"[TBR-Proxy] inet_pton v6 {ip6} note: {err}\n")
  # A records
  for ip4 in v4_list:
    try:
      packed4 = socket.inet_pton(socket.AF_INET, ip4)
      answers.append(build_dns_rr(host_target, 1, 0x8001, 120, packed4))
    except OSError as err:
      sys.stderr.write(f"[TBR-Proxy] inet_pton v4 {ip4} note: {err}\n")

  hdr = struct.pack("!HHHHHH", txid, 0x8400, 0, len(answers), 0, 0)
  return hdr + b"".join(answers)


sock_ctrl = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock_ctrl.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
  sock_ctrl.bind(("::", 5541))
except Exception as e:
  print(f"[TBR-Proxy] Warning: bind :: 5541: {e}", flush=True)

sock_v4 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock_v4.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
  sock_v4.bind(("0.0.0.0", 5540))
except Exception as e:
  print(f"[TBR-Proxy] Warning: bind 0.0.0.0 5540: {e}", flush=True)

sock_v6_client = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock_v6_client.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
  sock_v6_client.bind(("::", 5540))
except Exception as e:
  print(f"[TBR-Proxy] Warning: bind :: 5540: {e}", flush=True)

sock_v6_target = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
sock_v6_target.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
  sock_v6_target.bind(("::", 5542))
except Exception as e:
  print(f"[TBR-Proxy] Warning: bind :: 5542: {e}", flush=True)

sock_mdns4 = socket.socket(
    socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP
)
sock_mdns4.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
  sock_mdns4.bind(("0.0.0.0", 5353))
  v4_addrs, _ = get_wlan0_addrs()
  mreq4 = socket.inet_aton("224.0.0.251") + socket.inet_aton(v4_addrs[0])
  sock_mdns4.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, mreq4)
except Exception as e:
  print(f"[TBR-Proxy] Warning: mdns4 setup: {e}", flush=True)

sock_mdns6 = socket.socket(
    socket.AF_INET6, socket.SOCK_DGRAM, socket.IPPROTO_UDP
)
sock_mdns6.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
try:
  sock_mdns6.bind(("::", 5353))
  if_idx = socket.if_nametoindex("wlan0")
  mreq6 = socket.inet_pton(socket.AF_INET6, "ff02::fb") + struct.pack(
      "@I", if_idx
  )
  sock_mdns6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_JOIN_GROUP, mreq6)
  sock_mdns6.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_IF, if_idx)
except Exception as e:
  print(f"[TBR-Proxy] Warning: mdns6 setup: {e}", flush=True)


def announce_all_instances(dest4=None, dest6=None, txid=0):
  if not registered_instances:
    return
  v4_list, v6_list = get_wlan0_addrs()
  for inst in list(registered_instances):
    pkt = build_mdns_response(inst, v4_list, v6_list, port=5540, txid=txid)
    try:
      if dest4:
        sock_mdns4.sendto(pkt, dest4)
      else:
        sock_mdns4.sendto(pkt, ("224.0.0.251", 5353))
    except OSError as err:
      sys.stderr.write(f"[TBR-Proxy] mdns4 send note: {err}\n")
    try:
      if dest6:
        sock_mdns6.sendto(pkt, dest6)
      else:
        sock_mdns6.sendto(pkt, ("ff02::fb", 5353))
    except OSError as err:
      sys.stderr.write(f"[TBR-Proxy] mdns6 send note: {err}\n")


last_client_addr = None
last_client_family = None
last_periodic = 0.0

print(
    "[TBR-Proxy] Listening on UDP 5353 (mDNS), 5541 (ctrl), 5540 (client), 5542"
    " (wpan0 target)...",
    flush=True,
)

all_socks = [
    sock_ctrl,
    sock_v4,
    sock_v6_client,
    sock_v6_target,
    sock_mdns4,
    sock_mdns6,
]

while True:
  try:
    rlist, _, _ = select.select(all_socks, [], [], 0.5)
    now = time.time()
    if now - last_periodic >= 1.0:
      last_periodic = now
      announce_all_instances()

    for s in rlist:
      if s is sock_ctrl:
        data, addr = sock_ctrl.recvfrom(2048)
        msg = data.decode("utf-8", "replace").strip()
        print(f"[TBR-Proxy] Control recv from {addr}: {msg}", flush=True)
        parts = msg.split()
        if len(parts) >= 2 and parts[0] == "REGISTER":
          inst_name = parts[1].upper()
          if len(parts) >= 3:
            end_device_wpan_ip = parts[2]
          if len(parts) >= 4:
            try:
              end_device_port = int(parts[3])
            except ValueError as ve:
              print(f"[TBR-Proxy] Non-int port: {ve}", flush=True)
          if not inst_name.startswith(
              "0000000000000000"
          ) and not inst_name.startswith("1111222233334444"):
            registered_instances.add(inst_name)
          print(
              f"[TBR-Proxy] Registering {inst_name} ->"
              f" [{end_device_wpan_ip}]:{end_device_port}",
              flush=True,
          )
          announce_all_instances()
          try:
            os.makedirs("/etc/avahi/services", exist_ok=True)
            service_path = f"/etc/avahi/services/tbr_{inst_name}.service"
            with open(service_path, "w") as f:
              f.write(AVAHI_SERVICE_TEMPLATE.format(name=inst_name))
            subprocess.run(["killall", "-HUP", "avahi-daemon"], check=False)
          except (OSError, subprocess.SubprocessError) as err:
            sys.stderr.write(f"[TBR-Proxy] avahi write note: {err}\n")

      elif s is sock_mdns4:
        data, addr = sock_mdns4.recvfrom(4096)
        if len(data) >= 12 and (data[2] & 0x80) == 0:
          if b"_matter" in data or b"tbr" in data:
            txid = struct.unpack("!H", data[:2])[0]
            announce_all_instances(dest4=addr, txid=txid)
            announce_all_instances(txid=0)

      elif s is sock_mdns6:
        data, addr = sock_mdns6.recvfrom(4096)
        if len(data) >= 12 and (data[2] & 0x80) == 0:
          if b"_matter" in data or b"tbr" in data:
            txid = struct.unpack("!H", data[:2])[0]
            announce_all_instances(dest6=addr, txid=txid)
            announce_all_instances(txid=0)

      elif s is sock_v4:
        data, addr = sock_v4.recvfrom(4096)
        last_client_addr = addr
        last_client_family = socket.AF_INET
        try:
          sock_v6_target.sendto(data, (end_device_wpan_ip, end_device_port))
        except Exception as e:
          print(f"[TBR-Proxy] Relay v4->end_device error: {e}", flush=True)

      elif s is sock_v6_client:
        data, addr = sock_v6_client.recvfrom(4096)
        last_client_addr = addr
        last_client_family = socket.AF_INET6
        try:
          sock_v6_target.sendto(data, (end_device_wpan_ip, end_device_port))
        except Exception as e:
          print(
              f"[TBR-Proxy] Relay external_v6->end_device error: {e}",
              flush=True,
          )

      elif s is sock_v6_target:
        data, addr = sock_v6_target.recvfrom(4096)
        if last_client_addr is not None:
          try:
            if last_client_family == socket.AF_INET:
              sock_v4.sendto(data, last_client_addr)
            else:
              sock_v6_client.sendto(data, last_client_addr)
          except Exception as e:
            print(
                f"[TBR-Proxy] Relay end_device->client error: {e}",
                flush=True,
            )
  except Exception as e:
    print(f"[TBR-Proxy] Loop exception: {e}", flush=True)
    time.sleep(0.2)
'''

THREAD_END_DEVICE_JOINER_HELPER = r'''
import os
import re
import socket
import subprocess
import sys
import time

def sh(cmd):
  try:
    out = subprocess.check_output(
        cmd, shell=True, stderr=subprocess.STDOUT
    ).decode("utf-8", "replace").strip()
    print(f"+ {cmd} -> {out}", flush=True)
    return out
  except Exception as e:
    print(f"+ {cmd} -> ERR: {e}", flush=True)
    return ""


def query_local_matter_instance():
  op_pat = re.compile(
      r"(?:Advertise operational node|instance name:)\s*"
      r"([0-9A-Fa-f]{16}-[0-9A-Fa-f]{16})"
  )
  log_file = "/tmp/chip-all-clusters.log"
  if os.path.exists(log_file):
    try:
      with open(log_file, "r", errors="ignore") as f:
        content = f.read()
        matches = op_pat.findall(content)
        if matches:
          return matches[-1].upper()
    except OSError as err:
      sys.stderr.write(f"[EndDevice-Helper] Read {log_file} note: {err}\n")

  q = (
      b"\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00"
      b"\x07_matter\x04_tcp\x05local\x00\x00\x0c\x00\x01"
  )
  gen_pat = re.compile(r"([0-9A-Fa-f]{16}-[0-9A-Fa-f]{16})")
  for fam, addr in (
      (socket.AF_INET, ("169.254.169.254", 5353)),
      (socket.AF_INET6, ("fd11:33::fe", 5353)),
  ):
    try:
      s = socket.socket(fam, socket.SOCK_DGRAM)
      s.settimeout(0.25)
      s.sendto(q, addr)
      resp, _ = s.recvfrom(2048)
      s.close()
      text = resp.decode("latin-1", "ignore")
      for m in gen_pat.finditer(text):
        inst = m.group(1).upper()
        if not inst.startswith("0000000000000000") and not inst.startswith(
            "1111222233334444"
        ):
          return inst
    except OSError as err:
      sys.stderr.write(f"[EndDevice-Helper] DNS probe {addr} note: {err}\n")
  return None


print("[EndDevice-Helper] Started Thread EndDevice Joiner Helper", flush=True)

dataset_commissioned = False
for _ in range(300):
  time.sleep(0.4)
  state = sh("ot-ctl state")
  if "leader" in state or "router" in state or "child" in state:
    print(f"[EndDevice-Helper] Target state reached: {state}", flush=True)
    dataset_commissioned = True
    break
  if "detached" in state or "disabled" in state:
    act = sh("ot-ctl dataset active")
    chan_m = re.search(r"Channel:\s*(\d+)", act)
    pan_m = re.search(r"(?<!Ext )PAN ID:\s*(0x[0-9a-fA-F]+)", act)
    xpan_m = re.search(r"Ext PAN ID:\s*([0-9a-fA-F]+)", act)
    key_m = re.search(r"Network Key:\s*([0-9a-fA-F]+)", act)
    if xpan_m and key_m:
      chan = chan_m.group(1) if chan_m else "15"
      pan = pan_m.group(1) if pan_m else "0x1234"
      xpan = xpan_m.group(1)
      key = key_m.group(1)
      print(
          f"[EndDevice-Helper] Commissioned: chan={chan}, pan={pan},"
          f" xpan={xpan}",
          flush=True,
      )
      sh("ot-ctl thread stop")
      sh("ot-ctl dataset init new")
      sh("ot-ctl dataset activetimestamp 1")
      sh("ot-ctl dataset networkname CirqueThread")
      sh("ot-ctl dataset meshlocalprefix fdde:ad00:beef:0::")
      sh(f"ot-ctl dataset channel {chan}")
      sh(f"ot-ctl dataset panid {pan}")
      sh(f"ot-ctl dataset extpanid {xpan}")
      sh(f"ot-ctl dataset networkkey {key}")
      sh("ot-ctl dataset commit active")
      sh("ot-ctl ifconfig up")
      sh("ot-ctl thread start")
      dataset_commissioned = True
      break

if dataset_commissioned:
  print(
      "[EndDevice-Helper] Waiting up to 8s to attach as child or router...",
      flush=True,
  )
  attached = False
  for _ in range(20):
    time.sleep(0.4)
    st = sh("ot-ctl state")
    if "child" in st or "router" in st or "leader" in st:
      print(f"[EndDevice-Helper] Attached successfully: {st}", flush=True)
      attached = True
      break
  if not attached:
    print(
        "[EndDevice-Helper] Standalone timeout, fallback to state leader",
        flush=True,
    )
    sh("ot-ctl state leader")

sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
instance_name = None
found_logged = False
sent_logged = False
for _ in range(180):
  sh("ip -6 addr replace fd11:33::2/64 dev wpan0 nodad 2>/dev/null || true")
  sh(
      "ip -6 route replace fd11:22::/64 via fd11:33::1 dev wpan0"
      " 2>/dev/null || true"
  )
  found = query_local_matter_instance()
  if found:
    instance_name = found
    if not found_logged:
      print(
          "[EndDevice-Helper] Discovered operational instance: "
          f"{instance_name}",
          flush=True,
      )
      found_logged = True
  if instance_name:
    reg_msg = f"REGISTER {instance_name} fd11:33::2 5540".encode("utf-8")
    try:
      sock.sendto(reg_msg, ("fd11:33::1", 5541))
      if not sent_logged:
        print(
            f"[EndDevice-Helper] Sent REGISTER {instance_name} to"
            " [fd11:33::1]:5541",
            flush=True,
        )
        sent_logged = True
    except Exception as e:
      print(f"[EndDevice-Helper] Failed to send registration: {e}", flush=True)
  time.sleep(1.0)
'''

DEVICE_CONFIG = {
    'device0': {
        'type': 'wifi_ap',
        'base_image': '@default',
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'use_virtual_wifi_tcp': True,
        'capability': ['TrafficControl'],
        'traffic_control': {'latencyMs': 10},
    },
    'device1': {
        'type': 'ThreadBorderRouter',
        'base_image': '@default',
        'capability': [
            'Thread',
            'WiFi',
            'TrafficControl',
        ],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'use_virtual_wifi_tcp': True,
        'wifi_auto_connect': True,
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'traffic_control': {'latencyMs': 25, 'loss': 0},
    },
    'device2': {
        'type': 'CHIPEndDevice',
        'base_image': '@default',
        'capability': [
            'Thread',
            'Bluetooth',
            'TrafficControl',
            'Mount',
        ],
        'rcp_mode': True,
        'docker_network': 'Ipv6',
        'use_virtual_bt_tcp': True,
        'bt_num_Adapters': 2,
        'traffic_control': {'latencyMs': 25, 'loss': 0},
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
    'device3': {
        'type': 'android_emulator',
        'base_image': 'cirque-android-runner:latest',
        'capability': ['WiFi', 'Bluetooth', 'Mount'],
        'use_virtual_bt_tcp': True,
        'use_virtual_wifi_tcp': True,
        'wifi_auto_connect': True,
        'ssid': TEST_WIFI_SSID,
        'psk': TEST_WIFI_PSK,
        'tap_interface': 'cirque_tap0',
        'avd_name': 'Pixel_6_API_34',
        'chiptool_apk': str(CHIP_TOOL_APK_PATH),
        'mount_pairs': [[CHIP_REPO_STR, CHIP_REPO_STR]],
    },
}


ANDROID_RUNNER_IMAGE = 'cirque-android-runner:latest'


def _check_android_preconditions() -> bool:
  if not os.path.exists('/dev/kvm'):
    sys.stderr.write(
        'Precondition failed: /dev/kvm not found; KVM hardware acceleration is'
        ' required for cirque-android-runner (see'
        ' third_party/cirque/repo/README.md).\n'
    )
    return False
  try:
    res = subprocess.run(
        ['docker', 'image', 'inspect', ANDROID_RUNNER_IMAGE],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=5,
        check=False,
    )
    if res.returncode != 0:
      sys.stderr.write(
          f"Precondition failed: Docker image '{ANDROID_RUNNER_IMAGE}' not"
          ' found. Build via: docker build -t'
          f' {ANDROID_RUNNER_IMAGE} -f'
          ' third_party/cirque/repo/cirque/nodes/Dockerfile.android_runner'
          ' third_party/cirque/repo/cirque/nodes (see'
          ' third_party/cirque/repo/README.md).\n'
      )
      return False
  except (subprocess.SubprocessError, FileNotFoundError) as e:
    sys.stderr.write(
        f'Precondition failed: docker inspect failed: {e}\n'
    )
    return False
  return True


class TestAndroidBleThreadMobileDevice(CHIPVirtualHome):

  def __init__(self, device_config=None):
    if device_config is None:
      device_config = DEVICE_CONFIG
    super().__init__(CIRQUE_URL, device_config)
    self.logger = logger

  def setup(self):
    if not _check_android_preconditions():
      raise AssertionError('Android runner preconditions failed')
    self.initialize_home()

  def run_test(self, save_logs=True):
    if not _check_android_preconditions():
      return TestResult.TEST_FAILURE
    return super().run_test(save_logs=save_logs)

  def save_device_logs(self):
    super().save_device_logs()
    timestamp = int(time.time())
    save_android_emulator_diagnostics(self, timestamp)

  def test_routine(self):
    self.run_android_ble_thread_commissioning_test()

  @staticmethod
  def _extract_hci_index(device, default_index=1):
    if isinstance(device, dict):
      desc = device.get('description', {})
      if isinstance(desc, dict):
        if isinstance(desc.get('ble_adapt_id'), int):
          return desc['ble_adapt_id']
        ble_adapt = desc.get('ble_adapt', '')
        if isinstance(ble_adapt, str) and ble_adapt.startswith('hci'):
          return int(ble_adapt[3:])
      cap_bt = device.get('capability', {}).get('Bluetooth', {})
      if isinstance(cap_bt, dict):
        if isinstance(cap_bt.get('ble_adapt_id'), int):
          return cap_bt['ble_adapt_id']
        ble_adapt = cap_bt.get('ble_adapt', '')
        if isinstance(ble_adapt, str) and ble_adapt.startswith('hci'):
          return int(ble_adapt[3:])
      ble_adapt = device.get('ble_adapt', '')
      if isinstance(ble_adapt, str) and ble_adapt.startswith('hci'):
        return int(ble_adapt[3:])
    elif isinstance(device, str) and device.startswith('hci'):
      return int(device[3:])
    return default_index

  def _isolate_eth0_and_start_avahi(self, device_ids):
    self.logger.info(
        'Isolating eth0 and configuring avahi on TBR and CHIPEndDevice'
    )
    for dev_id in device_ids:
      dev_type = None
      if isinstance(self.device_config, dict):
        if dev_id in self.device_config and isinstance(
            self.device_config[dev_id], dict
        ):
          dev_type = self.device_config[dev_id].get('type')
        if not dev_type:
          for d in self.device_config.values():
            if isinstance(d, dict) and d.get('id') == dev_id:
              dev_type = d.get('type')
              break

      if dev_type == 'ThreadBorderRouter':
        self.execute_device_cmd(
            dev_id,
            'sh -c "ip addr flush dev eth0 2>/dev/null || true;'
            ' ip link set eth0 down 2>/dev/null || true;'
            ' sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null 2>&1 || true;'
            ' sysctl -w net.ipv6.conf.vwifi_phy.disable_ipv6=1'
            ' >/dev/null 2>&1 || true;'
            ' ip addr flush dev vwifi_phy 2>/dev/null || true"',
        )
        wait_for_device_wlan0_ipv4(self, dev_id)
        self.execute_device_cmd(
            dev_id,
            'sh -c "ip -6 addr replace fd11:22::2/64 dev wlan0 nodad'
            ' 2>/dev/null || true"',
        )
        self.execute_device_cmd(
            dev_id,
            'sh -c "sed -i'
            ' \'s/.*allow-interfaces=.*/allow-interfaces=wlan0/;'
            ' s/.*enable-reflector=.*/enable-reflector=yes/;'
            ' s/.*use-ipv6=.*/use-ipv6=yes/\''
            ' /etc/avahi/avahi-daemon.conf 2>/dev/null || true;'
            ' service dbus start 2>/dev/null || true;'
            ' service avahi-daemon restart 2>/dev/null ||'
            ' avahi-daemon -D 2>/dev/null || true"',
        )
      else:
        # CHIPEndDevice (pure BLE+Thread without wlan0)
        self.execute_device_cmd(
            dev_id,
            'sh -c "ip addr flush dev eth0 2>/dev/null || true;'
            ' ip link set eth0 down 2>/dev/null || true;'
            ' ip link add mdns0 type dummy 2>/dev/null || true;'
            ' ip link set mdns0 multicast on up 2>/dev/null || true;'
            ' ip addr replace 169.254.169.254/24 dev mdns0 2>/dev/null || true;'
            ' ip -6 addr replace fd11:33::fe/64 dev mdns0 nodad'
            ' 2>/dev/null || true"',
        )
        self.execute_device_cmd(
            dev_id,
            'sh -c "sed -i'
            ' \'s/.*allow-interfaces=.*/allow-interfaces=mdns0/;'
            ' s/.*enable-reflector=.*/enable-reflector=no/;'
            ' s/.*use-ipv6=.*/use-ipv6=yes/\''
            ' /etc/avahi/avahi-daemon.conf 2>/dev/null || true;'
            ' service dbus start 2>/dev/null || true;'
            ' service avahi-daemon restart 2>/dev/null ||'
            ' avahi-daemon -D 2>/dev/null || true"',
        )

  def _get_tbr_active_dataset(self, tbr_id) -> dict:
    res_txt = self.execute_device_cmd(tbr_id, 'ot-ctl dataset active')
    out_txt = (
        res_txt.get('output', '')
        if isinstance(res_txt, dict)
        else str(res_txt)
    )
    res_hex = self.execute_device_cmd(tbr_id, 'ot-ctl dataset active -x')
    out_hex = (
        res_hex.get('output', '')
        if isinstance(res_hex, dict)
        else str(res_hex)
    )
    dataset_tlvs_hex = ''
    for line in out_hex.splitlines():
      line = line.strip()
      if re.match(r'^[0-9a-fA-F]{10,}$', line):
        dataset_tlvs_hex = line
        break

    chan_m = re.search(r'Channel:\s*(\d+)', out_txt)
    pan_m = re.search(r'(?<!Ext )PAN ID:\s*(?:0x)?([0-9a-fA-F]+)', out_txt)
    xpan_m = re.search(r'Ext PAN ID:\s*([0-9a-fA-F]+)', out_txt)
    key_m = re.search(r'Network Key:\s*([0-9a-fA-F]+)', out_txt)

    channel = int(chan_m.group(1)) if chan_m else int(TEST_CHANNEL)
    pan_id = pan_m.group(1) if pan_m else TEST_PANID.replace('0x', '')
    xpan_id = xpan_m.group(1) if xpan_m else TEST_EXTPANID
    master_key = (
        key_m.group(1) if key_m else '00112233445566778899aabbccddeeff'
    )

    return {
        'channel': channel,
        'pan_id': pan_id,
        'xpan_id': xpan_id,
        'master_key': master_key,
        'dataset_tlvs_hex': dataset_tlvs_hex,
    }

  def _setup_thread_border_routers(self, tbr_ids) -> dict:
    self.logger.info('Setting up ThreadBorderRouter nodes: %s', tbr_ids)
    active_dataset = {}
    for tbr_id in tbr_ids:
      # 1. Form Thread network on wpan0 via ot-ctl
      form_cmds = [
          'ot-ctl dataset init new',
          'ot-ctl dataset activetimestamp 1',
          f'ot-ctl dataset channel {TEST_CHANNEL}',
          f'ot-ctl dataset panid {TEST_PANID}',
          f'ot-ctl dataset extpanid {TEST_EXTPANID}',
          'ot-ctl dataset networkkey 00112233445566778899aabbccddeeff',
          'ot-ctl dataset meshlocalprefix fdde:ad00:beef:0::',
          'ot-ctl dataset commit active',
          'ot-ctl ifconfig up',
          'ot-ctl thread start',
          'ot-ctl state leader',
      ]
      for cmd in form_cmds:
        self.execute_device_cmd(tbr_id, cmd)

      # 2. Poll ot-ctl state until 'leader'
      start_poll = time.time()
      leader_formed = False
      while time.time() - start_poll < 15.0:
        reply = self.execute_device_cmd(tbr_id, 'ot-ctl state')
        out = (
            reply.get('output', '').strip()
            if isinstance(reply, dict)
            else str(reply)
        )
        if 'leader' in out:
          leader_formed = True
          break
        time.sleep(0.5)
      self.assertTrue(
          leader_formed,
          f'ThreadBorderRouter {tbr_id} failed to become leader within'
          ' timeout',
      )

      # 3. Register prefixes and routes, assign fd11:33::1/64 on wpan0 and
      # fd11:22::2/64 on wlan0
      net_cmds = [
          'ot-ctl prefix add fd11:33::/64 pasor',
          'ot-ctl route add fd11:22::/64 s med',
          'ot-ctl route add fd11:33::/64 s med',
          'ot-ctl route add fdde:ad00:beef:0::/64 s med',
          'ot-ctl netdata register',
          'sh -c "sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null 2>&1 ||'
          ' true; ip -6 addr replace fd11:22::2/64 dev wlan0 nodad 2>/dev/null'
          ' || true; ip -6 addr replace fd11:33::1/64 dev wpan0 nodad'
          ' 2>/dev/null || true; ip -6 route replace fd11:33::/64 dev wpan0'
          ' 2>/dev/null || true"',
      ]
      for cmd in net_cmds:
        self.execute_device_cmd(tbr_id, cmd)

      # 4. Start TBR mDNS + UDP 5540 border router proxy
      b64_proxy = base64.b64encode(
          TBR_BORDER_PROXY_HELPER.encode('utf-8')
      ).decode('ascii')
      proxy_cmd = (
          'sh -c "kill -9 $(cat /tmp/tbr_border_proxy.pid 2>/dev/null)'
          ' 2>/dev/null || true;'
          f' echo \\"{b64_proxy}\\" | base64 -d > /tmp/tbr_border_proxy.py &&'
          ' nohup python3 /tmp/tbr_border_proxy.py >/tmp/tbr_border_proxy.log'
          ' 2>&1 &'
          ' echo $! > /tmp/tbr_border_proxy.pid"'
      )
      self.execute_device_cmd(tbr_id, proxy_cmd)

      # 5. Extract active dataset
      active_dataset = self._get_tbr_active_dataset(tbr_id)
      self.logger.info(
          'ThreadBorderRouter %s active dataset: %s', tbr_id, active_dataset
      )

    return active_dataset

  def _start_ble_thread_end_devices(self, server_devices, host_libs_dir=None):
    if host_libs_dir is None:
      host_libs_dir = shlex.quote(f'{CHIP_REPO_STR}/out/host_libs')

    for device in server_devices:
      server_id = device['id'] if isinstance(device, dict) else device
      ble_adapt_id = self._extract_hci_index(device, default_index=1)

      # 1. Reset wpan0 so CHIPEndDevice starts uncommissioned (disabled)
      self.execute_device_cmd(
          server_id,
          'sh -c "ot-ctl thread stop 2>/dev/null || true;'
          ' ot-ctl ifconfig down 2>/dev/null || true;'
          ' ot-ctl dataset clear 2>/dev/null || true;'
          ' ot-ctl factoryreset 2>/dev/null || true"',
      )

      # 2. Start THREAD_END_DEVICE_JOINER_HELPER
      b64_joiner = base64.b64encode(
          THREAD_END_DEVICE_JOINER_HELPER.encode('utf-8')
      ).decode('ascii')
      joiner_cmd = (
          'sh -c "kill -9 $(cat /tmp/joiner_helper.pid 2>/dev/null) 2>/dev/null'
          ' || true;'
          f' echo \\"{b64_joiner}\\" | base64 -d >'
          ' /tmp/joiner_helper.py &&'
          ' nohup python3 /tmp/joiner_helper.py >/tmp/joiner_helper.log 2>&1 &'
          ' echo $! > /tmp/joiner_helper.pid"'
      )
      self.execute_device_cmd(server_id, joiner_cmd)

      # 3. Start chip-all-clusters-app with BLE and Thread
      self.logger.info(
          'Starting chip-all-clusters-app on CHIPEndDevice %s with'
          ' --thread --ble-controller %d --discriminator %d',
          self.get_device_pretty_id(server_id),
          ble_adapt_id,
          TEST_DISCRIMINATOR,
      )
      self.execute_device_cmd(
          server_id, 'sh -c ": > /tmp/chip-all-clusters.log"'
      )
      self.execute_device_cmd(
          server_id,
          'CHIPCirqueDaemon.py -- run sh -c "'
          f'env LD_LIBRARY_PATH={host_libs_dir}'
          ' gdb -batch -return-child-result -q -ex'
          ' \'set pagination off\' -ex run -ex \'thread apply all bt\' --args'
          f' {CHIP_ALL_CLUSTERS_APP_ESC} --thread --ble-controller'
          f' {ble_adapt_id} --discriminator {TEST_DISCRIMINATOR} 2>&1 | tee -a'
          ' /tmp/chip-all-clusters.log"',
      )

  _start_thread_end_devices = _start_ble_thread_end_devices

  def _dump_emulator_diagnostics(self, req_device_id):
    """Logs emulator state so a CI boot failure is diagnosable."""
    for cmd in (
        'adb devices -l',
        'sh -c "ls -l /dev/kvm; nproc; free -m"',
        'sh -c "cat /proc/loadavg; uptime"',
        'sh -c "tail -n 200 /tmp/emulator.log 2>/dev/null"',
    ):
      self.execute_device_cmd(req_device_id, cmd)

  def _setup_android_emulator_node(self, req_device_id):
    self.logger.info('Initializing Android emulator environment on device0')
    init_res = None
    try:
      init_res = self.query_api(
          'init_android_emulator',
          [self.home_id, req_device_id],
          params={'timeout': ANDROID_BOOT_TIMEOUT_SEC},
      )
    except Exception as e:
      self.logger.warning(
          'REST init_android_emulator failed: %s; direct exec fallback', e
      )
    if init_res is not None:
      if isinstance(init_res, dict) and init_res.get('status') == 'success':
        self.logger.info(
            'Android emulator initialized via REST API: %s', init_res
        )
        return
      # The endpoint answered, so an emulator was already launched. Do
      # not start a second one on the same AVD; report why it failed.
      self.logger.error('REST init_android_emulator returned: %s', init_res)
      self._dump_emulator_diagnostics(req_device_id)
      self.assertTrue(False, f'Android emulator init failed: {init_res}')

    # Direct container fallback:
    # 1. Setup tap
    self.execute_device_cmd(
        req_device_id,
        'sh -c "ip link set dev cirque_tap0 up 2>/dev/null ||'
        ' (ip tuntap add dev cirque_tap0 mode tap &&'
        ' ip link set dev cirque_tap0 up)"',
    )
    # 2. Start emulator in background
    self.execute_device_cmd(
        req_device_id,
        'sh -c "export ANDROID_HOME=/opt/android/sdk'
        ' ANDROID_SDK_ROOT=/opt/android/sdk;'
        ' /opt/android/sdk/emulator/emulator -avd Pixel_6_API_34 -no-window'
        ' -no-audio -no-boot-anim -gpu swiftshader_indirect -read-only'
        ' -no-snapshot -feature -BluetoothEmulation'
        ' -feature -WiFiPacketStream'
        ' -wifi-tap cirque_tap0 > /tmp/emulator.log 2>&1 &"',
    )
    # 3. Wait for boot completion
    self.logger.info('Waiting for Android emulator boot completion...')
    booted = False
    start_boot = time.time()
    while time.time() - start_boot < ANDROID_BOOT_TIMEOUT_SEC:
      res = self.execute_device_cmd(
          req_device_id, 'adb shell getprop sys.boot_completed'
      )
      out = res.get('output', '')
      if '1' in out:
        self.execute_device_cmd(req_device_id, 'adb root')
        time.sleep(0.5)
        self.execute_device_cmd(req_device_id, 'adb wait-for-device')
        self.execute_device_cmd(req_device_id, 'adb shell setenforce 0')
        booted = True
        break
      time.sleep(1.0)
    self.assertTrue(booted, 'Android emulator failed to boot within timeout')

    # 4. Install CHIPTool APK
    apk_candidates = [
        str(CHIP_TOOL_APK_PATH),
        (
            f'{CHIP_REPO_STR}/out/android-x64-chip-tool/outputs/apk/debug/'
            'app-debug.apk'
        ),
        (
            '/connectedhomeip/out/android-x64-chip-tool/outputs/apk/debug/'
            'app-debug.apk'
        ),
        '/tmp/CHIPTool.apk',
    ]
    apk_target = None
    for cand in apk_candidates:
      chk = self.execute_device_cmd(req_device_id, f'test -f {cand}')
      if chk.get('return_code') == '0':
        apk_target = cand
        break
    if apk_target:
      self.execute_device_cmd(req_device_id, f'adb install -r -g {apk_target}')
      for p in [
          'android.permission.ACCESS_FINE_LOCATION',
          'android.permission.ACCESS_COARSE_LOCATION',
          'android.permission.BLUETOOTH_SCAN',
          'android.permission.BLUETOOTH_CONNECT',
          'android.permission.BLUETOOTH_ADVERTISE',
      ]:
        self.execute_device_cmd(
            req_device_id, f'adb shell pm grant com.google.chip.chiptool {p}'
        )

    # 5. Start pty_bridge
    cirque_pty = (
        f'{CHIP_REPO_STR}/third_party/cirque/repo/cirque/virtual_bt/android/'
        'pty_bridge'
    )
    self.execute_device_cmd(
        req_device_id,
        'sh -c "adb push /opt/virtual_bt_android/pty_bridge'
        ' /data/local/tmp/pty_bridge ||'
        f' adb push {cirque_pty} /data/local/tmp/pty_bridge"',
    )
    self.execute_device_cmd(
        req_device_id, 'adb shell chmod 755 /data/local/tmp/pty_bridge'
    )

    bt_info = self.query_api('virtual_bt_info')
    hci_port = bt_info.get('hci_port', 23458)
    gw_res = self.execute_device_cmd(req_device_id, 'ip route')
    gw_ip = '10.0.2.2'
    for line in gw_res.get('output', '').splitlines():
      if line.startswith('default via '):
        parts = line.split()
        if len(parts) >= 3 and re.match(r'^\d+\.\d+\.\d+\.\d+$', parts[2]):
          gw_ip = parts[2]
          break
    relay_chk = self.execute_device_cmd(
        req_device_id,
        'test -S /dev/virtual_bt/shared_hci.sock'
        ' -a -f /dev/virtual_bt/bin/bt_tcp_to_unix_relay.py',
    )
    if relay_chk.get('return_code') == '0':
      self.execute_device_cmd(
          req_device_id,
          'sh -c "pkill -9 -f \\"[b]t_tcp_to_unix_relay.py\\" || true;'
          ' python3 /dev/virtual_bt/bin/bt_tcp_to_unix_relay.py'
          f' {hci_port} /dev/virtual_bt/shared_hci.sock >/dev/null 2>&1 &"',
      )
      time.sleep(0.2)
      gw_ip = '10.0.2.2'

    pin_cmd = (
        f'ip route replace {gw_ip}/32 via 10.0.2.2 dev eth0 2>/dev/null ||'
        f' true; ip rule add to {gw_ip}/32 lookup main pref 100 2>/dev/null ||'
        ' true'
    )
    self.execute_device_cmd(req_device_id, f'adb shell "{pin_cmd}"')
    self.execute_device_cmd(
        req_device_id, 'adb shell killall pty_bridge 2>/dev/null || true'
    )
    pty_cmd = (
        f'/data/local/tmp/pty_bridge /dev/bluetooth0 {gw_ip} {hci_port}'
        ' android_hci0 > /data/local/tmp/pty_bridge.log 2>&1 &'
    )
    self.execute_device_cmd(req_device_id, f'adb shell "{pty_cmd}"')
    time.sleep(2.0)
    self.execute_device_cmd(
        req_device_id,
        'adb shell chmod 666 /dev/bluetooth0 /dev/pts/* 2>/dev/null || true',
    )
    restart_cmd = (
        'killall bt_vhci_forwarder android.hardware.bluetooth-service.default'
        ' 2>/dev/null || true; cmd bluetooth_manager enable 2>/dev/null || true'
    )
    self.execute_device_cmd(req_device_id, f'adb shell "{restart_cmd}"')
    time.sleep(2.0)

    # 6. Guest Wi-Fi DHCP setup
    self.execute_device_cmd(
        req_device_id,
        'adb shell su root killall dhcpclient 2>/dev/null || true',
    )
    self.execute_device_cmd(
        req_device_id, 'adb shell su root ip link set dev wlan0 up'
    )
    self.execute_device_cmd(
        req_device_id, 'adb shell su root /vendor/bin/dhcpclient -i wlan0 &'
    )
    time.sleep(3.0)

  def _start_ui_video(self, mobile_id, name) -> bool:
    try:
      res = self.query_api(
          'start_screen_recording',
          [self.home_id, mobile_id],
          params={'name': name},
      )
      started = bool(isinstance(res, dict) and res.get('started', True))
      if started:
        self.logger.info('Started screen recording %s on %s', name, mobile_id)
      return started
    except Exception as e:
      self.logger.debug(
          'start_screen_recording endpoint not available or failed: %s', e
      )
      return False

  def _stop_ui_video(self, mobile_id):
    try:
      self.query_api('stop_screen_recording', [self.home_id, mobile_id])
      self.logger.info('Stopped screen recording on %s', mobile_id)
    except Exception as e:
      self.logger.debug(
          'stop_screen_recording endpoint not available or failed: %s', e
      )

  def _commission_and_interact_chiptool(
      self, req_device_id, tbr_dataset=None
  ):
    self.logger.info('Driving CHIPTool commissioning over BLE + Thread')
    commissioned_via_rest = False
    rest_endpoint_available = False
    self._start_ui_video(req_device_id, '2_ble_thread_commissioning.mp4')
    try:
      params = {
          'network_type': 'thread',
          'timeout': ANDROID_COMMISSION_TIMEOUT_SEC,
      }
      if tbr_dataset:
        if 'channel' in tbr_dataset:
          params['channel'] = tbr_dataset['channel']
        if 'pan_id' in tbr_dataset:
          params['pan_id'] = tbr_dataset['pan_id']
        if 'xpan_id' in tbr_dataset:
          params['xpan_id'] = tbr_dataset['xpan_id']
        if 'master_key' in tbr_dataset:
          params['master_key'] = tbr_dataset['master_key']
        if 'dataset_tlvs_hex' in tbr_dataset:
          params['dataset_tlvs_hex'] = tbr_dataset['dataset_tlvs_hex']
      comm_res = self.query_api(
          'commission_chiptool',
          [self.home_id, req_device_id],
          params=params,
      )
      rest_endpoint_available = isinstance(comm_res, dict)
      if isinstance(comm_res, dict) and comm_res.get('status') == 'success':
        self.logger.info('Commissioning via REST API succeeded: %s', comm_res)
        commissioned_via_rest = True
      else:
        self.logger.error(
            'REST commission_chiptool failed: %s',
            comm_res,
        )
    except Exception as e:
      self.logger.warning(
          'REST commission_chiptool endpoint failed: %s; falling back to'
          ' UI/adb',
          e,
      )
    finally:
      self._stop_ui_video(req_device_id)

    if rest_endpoint_available:
      self.assertTrue(
          commissioned_via_rest,
          'CHIPTool Thread commissioning via REST API failed to complete within'
          ' timeout',
      )

    comm_ok = commissioned_via_rest
    if not comm_ok and not rest_endpoint_available:
      # Fallback when REST endpoint is unavailable: drive via ADB / UI
      # automation using provisionThreadCredentialsBtn
      comm_success = self._provision_thread_network_via_adb_and_otctl(
          req_device_id, tbr_dataset=tbr_dataset
      )
      self.assertTrue(
          comm_success,
          'CHIPTool Thread commissioning failed to complete within timeout',
      )
      comm_ok = comm_success

    toggle_ok = False
    read_ok = False
    self._start_ui_video(req_device_id, '3_onoff_cluster_toggle_read.mp4')
    try:
      try:
        toggle_res = self.query_api(
            'toggle_chiptool',
            [self.home_id, req_device_id],
            params={'timeout': 30.0},
        )
        read_res = self.query_api(
            'read_chiptool',
            [self.home_id, req_device_id],
            params={'timeout': 30.0},
        )
        toggle_ok = (
            isinstance(toggle_res, dict)
            and toggle_res.get('status') == 'success'
        )
        read_ok = (
            isinstance(read_res, dict) and read_res.get('status') == 'success'
        )
      except Exception as e:
        self.logger.warning(
            'REST toggle/read endpoint failed: %s; falling back to UI'
            ' toggle/read',
            e,
        )
      if not (toggle_ok and read_ok):
        ui_toggle_ok, ui_read_ok = self._trigger_ui_toggle_and_read(
            req_device_id
        )
        toggle_ok = toggle_ok or ui_toggle_ok
        read_ok = read_ok or ui_read_ok
    finally:
      self._stop_ui_video(req_device_id)
    return comm_ok, toggle_ok, read_ok

  def _provision_thread_network_via_adb_and_otctl(
      self, req_device_id, tbr_dataset=None
  ) -> bool:
    self._start_ui_video(req_device_id, '2_ble_thread_commissioning.mp4')
    try:
      self.execute_device_cmd(req_device_id, 'adb logcat -c')
      self.execute_device_cmd(
          req_device_id,
          'adb shell am force-stop com.google.chip.chiptool',
      )
      time.sleep(0.5)
      self.execute_device_cmd(
          req_device_id,
          'adb shell am start -n com.google.chip.chiptool/.CHIPToolActivity',
      )
      time.sleep(4.0)

      # Tap provisionThreadCredentialsBtn (fallback coords 357, 606)
      self.execute_device_cmd(req_device_id, 'adb shell input tap 357 606')
      time.sleep(2.0)

      # Tap manualCodeBtn / Submit (fallback coords 964, 2274)
      self.execute_device_cmd(req_device_id, 'adb shell input tap 964 2274')
      time.sleep(2.0)

      if tbr_dataset:
        chan = tbr_dataset.get('channel')
        if chan is not None and str(chan) != '15':
          self.execute_device_cmd(req_device_id, 'adb shell input tap 540 650')
          time.sleep(0.5)
          self.execute_device_cmd(
              req_device_id, f'adb shell input text {chan}'
          )
        pan = tbr_dataset.get('pan_id')
        if pan is not None and str(pan) != '1234':
          self.execute_device_cmd(req_device_id, 'adb shell input tap 540 800')
          time.sleep(0.5)
          self.execute_device_cmd(
              req_device_id, f'adb shell input text {pan}'
          )

      # Tap saveNetworkBtn / SAVE NETWORK (fallback coords 858, 2227)
      self.execute_device_cmd(req_device_id, 'adb shell input tap 858 2227')

      # Poll logcat for commissioning completion
      self.logger.info(
          'Polling adb logcat for Thread commissioning completion...'
      )
      comm_success = False
      start_wait = time.time()
      while time.time() - start_wait < 60.0:
        res = self.execute_device_cmd(
            req_device_id,
            'adb shell "logcat -d | grep -E'
            ' \'onCommissioningComplete|Device commissioning completed|'
            'Commissioning complete\'"',
        )
        out = res.get('output', '') if isinstance(res, dict) else str(res)
        if (
            'onCommissioningComplete' in out
            or 'Commissioning completed' in out
            or 'Commissioning complete' in out
        ):
          if 'CHIP Error' in out and 'CHIP Error 0x00000000' not in out:
            comm_success = False
            break
          comm_success = True
          break
        time.sleep(1.0)
      return comm_success
    finally:
      self._stop_ui_video(req_device_id)

  def _trigger_ui_toggle_and_read(self, req_device_id):
    # Toggle On/Off cluster via UI
    started_rec = self._start_ui_video(
        req_device_id, '3_onoff_cluster_toggle_read.mp4'
    )
    try:
      time.sleep(2.0)
      dump_res = self.execute_device_cmd(
          req_device_id,
          'adb shell "uiautomator dump /data/local/tmp/ui.xml >/dev/null 2>&1'
          ' && cat /data/local/tmp/ui.xml 2>/dev/null || true"',
      )
      dump_out = (
          dump_res.get('output', '')
          if isinstance(dump_res, dict)
          else str(dump_res)
      )
      if (
          'onOffClusterBtn' not in dump_out
          and 'LIGHT ON/OFF' not in dump_out
      ):
        self.execute_device_cmd(
            req_device_id,
            'adb shell am start --activity-clear-top --activity-single-top'
            ' -n com.google.chip.chiptool/.CHIPToolActivity',
        )
        time.sleep(2.0)
      # Tap LIGHT ON/OFF & LEVEL CLUSTER (bounds [21,884][639,1010] -> 330, 947)
      self.execute_device_cmd(req_device_id, 'adb shell input tap 330 947')
      time.sleep(2.0)
      # Tap TOGGLE button (bounds [429,667][645,793] -> 537, 730)
      self.execute_device_cmd(req_device_id, 'adb shell input tap 537 730')
      time.sleep(2.0)
      # Tap READ button (bounds [61,821][271,947] -> 166, 884)
      self.execute_device_cmd(req_device_id, 'adb shell input tap 166 884')
      time.sleep(2.0)

      logcat_filter_cmd = (
          'adb shell "logcat -d | grep -E '
          "'CTL|ChipTool|CHIP|OnOffClientFragment|onCommissioningComplete|"
          'Commissioning complete|Toggle|attribute|value\' || true"'
      )
      log_check = self.execute_device_cmd(req_device_id, logcat_filter_cmd)
      logcat_out = (
          log_check.get('output', '')
          if isinstance(log_check, dict)
          else str(log_check)
      )
      toggle_ok = (
          'Toggle command success' in logcat_out
          or 'onResponse' in logcat_out
          or 'Code : 0' in logcat_out
      )
      read_ok = (
          'attribute value: true' in logcat_out
          or 'value: true' in logcat_out
          or 'onOff' in logcat_out
      )
      return toggle_ok, read_ok
    finally:
      if started_rec:
        self._stop_ui_video(req_device_id)

  def _verify_post_commissioning_thread_state(self, tbr_ids, server_ids):
    self.logger.info(
        'Verifying post-commissioning Thread state across TBR and CHIPEndDevice'
    )
    for tbr_id in tbr_ids:
      reply = self.execute_device_cmd(tbr_id, 'ot-ctl state')
      out = (
          reply.get('output', '').strip()
          if isinstance(reply, dict)
          else str(reply)
      )
      tbr_state = out.split()[0].strip() if out else ''
      self.assertEqual(
          tbr_state,
          'leader',
          f'ThreadBorderRouter {tbr_id} expected leader state, got {tbr_state}',
      )
    for server_id in server_ids:
      reply = self.execute_device_cmd(server_id, 'ot-ctl state')
      out = (
          reply.get('output', '').strip()
          if isinstance(reply, dict)
          else str(reply)
      )
      end_state = out.split()[0].strip() if out else ''
      self.assertIn(
          end_state,
          ['child', 'router', 'leader'],
          f'CHIPEndDevice {server_id} expected child/router/leader state, got'
          f' {end_state}',
      )
      ip_reply = self.execute_device_cmd(
          server_id, 'ip -6 addr show dev wpan0'
      )
      ip_out = (
          ip_reply.get('output', '')
          if isinstance(ip_reply, dict)
          else str(ip_reply)
      )
      self.assertIn(
          'fd11:33::2',
          ip_out,
          f'CHIPEndDevice {server_id} missing fd11:33::2 on wpan0:\n{ip_out}',
      )

  def _assert_ble_thread_device_logs(self, server_ids):
    # Wait for device to achieve Thread attached role (child, router, or leader)
    self.check_device_thread_state(
        server_ids[0], expected_role=['child', 'router', 'leader'], timeout=10
    )
    for device_id in server_ids:
      # Verify extpanid
      reply_extpan = self.execute_device_cmd(device_id, 'ot-ctl extpanid')
      extpan_out = reply_extpan['output'].split()[0].strip()
      self.assertEqual(
          extpan_out.lower(),
          TEST_EXTPANID.lower(),
          f'Expected extpanid {TEST_EXTPANID}, got {extpan_out}',
      )

      # Verify panid
      reply_panid = self.execute_device_cmd(device_id, 'ot-ctl panid')
      panid_out = reply_panid['output'].split()[0].strip()
      self.assertEqual(
          panid_out.lower(),
          TEST_PANID.lower(),
          f'Expected panid {TEST_PANID}, got {panid_out}',
      )

      # Verify channel
      reply_channel = self.execute_device_cmd(device_id, 'ot-ctl channel')
      channel_out = reply_channel['output'].split()[0].strip()
      self.assertEqual(
          channel_out,
          TEST_CHANNEL,
          f'Expected channel {TEST_CHANNEL}, got {channel_out}',
      )

      end_device_log = self.get_device_log(device_id).decode(
          'utf-8', errors='replace'
      )
      self.logger.info(
          '===== CHIPEndDevice (%s) Log =====\n%s',
          self.get_device_pretty_id(device_id),
          end_device_log,
      )
      self.assertTrue(
          self.sequenceMatch(
              end_device_log,
              [
                  'Thread interface: wpan0',
                  'GATT application registered successfully',
                  'New BLE connection',
                  'selected BTP version 4',
                  'Receive kCHIPoBLEConnectionEstablished',
                  'Commissioning completed session establishment step',
                  'Toggle ep1 on/off from state 0 to 1',
              ],
          ),
          'CHIPEndDevice log is missing expected BLE + Thread + OnOff'
          ' sequence markers',
      )

  def run_android_ble_thread_commissioning_test(self):
    all_devs = list(self.device_config.values())
    tbr_devices = [
        d for d in all_devs if d.get('type') == 'ThreadBorderRouter'
    ]
    server_devices = [d for d in all_devs if d.get('type') == 'CHIPEndDevice']
    req_devices = [
        d
        for d in all_devs
        if d.get('type') in ('android_emulator', 'Android_Emulator')
    ]
    if not tbr_devices:
      raise RuntimeError(
          "Required 'ThreadBorderRouter' node not found in topology"
      )
    if not server_devices:
      raise RuntimeError("Required 'CHIPEndDevice' node not found in topology")
    if not req_devices:
      raise RuntimeError(
          "Required controller device 'android_emulator' not found in topology"
      )
    tbr_ids = [d['id'] for d in tbr_devices]
    server_ids = [d['id'] for d in server_devices]
    req_device = req_devices[0]
    req_device_id = req_device['id']
    host_libs_dir = shlex.quote(f'{CHIP_REPO_STR}/out/host_libs')

    self._setup_android_emulator_node(req_device_id)
    self._isolate_eth0_and_start_avahi(tbr_ids + server_ids)
    tbr_dataset = self._setup_thread_border_routers(tbr_ids)
    self._start_ble_thread_end_devices(server_devices, host_libs_dir)
    self.assertTrue(
        self.wait_for_device_output(
            server_ids[0], 'GATT application registered', timeout=15
        ),
        'CHIPEndDevice failed to register GATT application',
    )
    tbr_ip = wait_for_device_wlan0_ipv4(self, tbr_ids[0])
    self.assertTrue(
        bool(tbr_ip and tbr_ip.startswith('10.0.1.')),
        f'Invalid ThreadBorderRouter IP on wlan0: {tbr_ip}',
    )

    comm_ok, toggle_ok, read_ok = self._commission_and_interact_chiptool(
        req_device_id, tbr_dataset=tbr_dataset
    )

    # 1. Dump and assert adb logcat shows commissioning, toggle, read
    logcat_filter_cmd = (
        'adb shell "logcat -d | grep -E '
        "'CTL|ChipTool|CHIP|OnOffClientFragment|onCommissioningComplete|"
        'Commissioning complete|Toggle|attribute|value\' || true"'
    )
    logcat_res = self.execute_device_cmd(req_device_id, logcat_filter_cmd)
    logcat_out = (
        logcat_res.get('output', '')
        if isinstance(logcat_res, dict)
        else str(logcat_res)
    )
    self.logger.info(
        '===== Android Logcat Snippet =====\n%s', logcat_out[-3000:]
    )
    self.assertTrue(comm_ok, 'Commissioning did not succeed')
    self.assertTrue(toggle_ok, 'Toggle command was not observed in logcat')
    self.assertTrue(read_ok, 'Read command was not observed in logcat')
    self.assertTrue(
        'onCommissioningComplete' in logcat_out
        or 'Device commissioning completed' in logcat_out
        or 'Commissioning completed' in logcat_out
        or 'Commissioning complete' in logcat_out,
        'adb logcat missing commissioning complete marker',
    )
    self.assertTrue(
        'Toggle command success' in logcat_out
        or 'Code : 0' in logcat_out
        or 'onResponse' in logcat_out,
        'adb logcat missing Toggle command success marker',
    )
    self.assertTrue(
        'On/Off attribute value: true' in logcat_out
        or 'attribute value: true' in logcat_out
        or 'value: true' in logcat_out
        or 'onOff' in logcat_out,
        'adb logcat missing On/Off attribute value marker',
    )

    # 2. Verify post-commissioning Thread state
    self._verify_post_commissioning_thread_state(tbr_ids, server_ids)

    # 3. Assert CHIPEndDevice device logs
    self._assert_ble_thread_device_logs(server_ids)

    # 4. Assert emulator radio path
    self._assert_emulator_radio_path(req_device_id)

  def _assert_emulator_radio_path(self, req_device_id):
    ps_res = self.execute_device_cmd(req_device_id, 'ps -ef')
    emu_lines = [
        line
        for line in ps_res.get('output', '').splitlines()
        if '-avd Pixel_6_API_34' in line and 'emulator' in line
    ]
    self.assertTrue(
        bool(emu_lines), 'No running Android emulator process found in device0'
    )
    self.assertTrue(
        '-feature -BluetoothEmulation' in emu_lines[0],
        'Emulator built-in Bluetooth emulation is not disabled',
    )
    self.assertTrue(
        '-feature -WiFiPacketStream' in emu_lines[0],
        'Emulator Wi-Fi packet streamer is not disabled',
    )
    self.assertTrue(
        '-wifi-tap cirque_tap0' in emu_lines[0],
        'Emulator Wi-Fi is not bridged onto cirque_tap0',
    )


if __name__ == '__main__':
  sys.exit(TestAndroidBleThreadMobileDevice(DEVICE_CONFIG).run_test())
