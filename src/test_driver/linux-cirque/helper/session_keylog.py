# Lint as: python3
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

TEST-ONLY Matter session key log plumbing for the Android Cirque tests.

chip-all-clusters-app is built with chip_enable_session_keylog=true
(scripts/build/gn_gen_cirque.sh) and logs nothing unless
CHIP_SESSION_KEYLOG_FILE is set. These tests set it on CHIPEndDevice and copy
the log next to the captures as matter_session_keys.txt, which the flow
visualizer uses to decrypt the PASE (BTP) and CASE (UDP) messages.
"""

import os
from typing import Any, Iterable, Optional

SESSION_KEYLOG_DEVICE_PATH = '/tmp/matter_session_keys.log'
SESSION_KEYLOG_ENV = f'CHIP_SESSION_KEYLOG_FILE={SESSION_KEYLOG_DEVICE_PATH}'
RESET_SESSION_KEYLOG_CMD = f'sh -c ": > {SESSION_KEYLOG_DEVICE_PATH}"'


def _output_text(resp: Any) -> str:
  if isinstance(resp, dict):
    out = resp.get('output', '')
  else:
    out = getattr(resp, 'content', resp)
  if isinstance(out, (bytes, bytearray)):
    return out.decode('utf-8', errors='replace')
  return str(out or '')


def save_session_keylog(
    test_instance: Any, device_ids: Iterable[str]
) -> Optional[str]:
  """Copies the devices' session keylogs to <log dir>/matter_session_keys.txt.

  Returns the written path, or None when no device logged a session key.
  """
  out_dir = os.environ.get('CIRQUE_PCAP_DIR') or os.environ.get(
      'DEVICE_LOG_DIR'
  )
  if not out_dir:
    return None
  chunks = []
  for device_id in device_ids:
    try:
      chunks.append(
          _output_text(
              test_instance.execute_device_cmd(
                  device_id, f'cat {SESSION_KEYLOG_DEVICE_PATH}'
              )
          )
      )
    except Exception as e:  # pylint: disable=broad-except
      test_instance.logger.warning(
          'Failed to read the session keylog of %s: %s', device_id, e
      )
  try:
    # cirque is on sys.path only under scripts/tests/cirque_tests.sh.
    from cirque.protocols.matter.session import write_matter_session_key_file  # pylint: disable=import-outside-toplevel

    path = write_matter_session_key_file(out_dir, '\n'.join(chunks))
  except (ImportError, OSError, ValueError) as e:
    test_instance.logger.warning('No Matter session keys saved: %s', e)
    return None
  test_instance.logger.info('Saved TEST-ONLY Matter session keys to %s', path)
  return path
